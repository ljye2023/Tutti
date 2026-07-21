# Layer 3: Backends SPI (IBackendProvider)

> Thin transport adapters: turn (ioaddrs + StorageTarget) into device-issuable IO commands.  
> Backends consume VDevices from Device Manager; they don't own queues.

## Key Changes from v0.1

| Aspect | Current (v0.1) | New Design |
|---|---|---|
| **Queue ownership** | Backend has `acquire_queue()` / `release_queue()` methods | Backend receives `VDevice*` at `initialize()`; holds queues for lifetime |
| **Initialization** | `initialize()` — no args | `initialize(VDevice* vdev)` — receives queue slice from DM |
| **Target handle** | Implicit or NVMe-specific | `acquire_target_handle(StorageTarget)` — backend-neutral input |
| **Stream types** | `cudaStream_t` directly in SPI | `AccelStream` (opaque `void*`) |
| **CUDA leaks** | `#include <cuda_runtime.h>` in `backend_provider.h` | No cuda_runtime.h in public SPI headers |

---

## Revised IBackendProvider SPI

```cpp
// io_engine/include/backend_provider.h (revised)
#pragma once
#include "tutti/accel/accel_types.h"     // AccelStream — no cuda_runtime.h
#include "tutti/types/storage_target.h"  // StorageTarget
#include "tutti/types/io_types.h"        // SubSliceInfo
#include "buffer_descriptor.h"           // BufferDescriptor
#include "backend_type.h"                // BackendType enum

namespace tutti {

struct VDevice;  // device_manager/include/vdevice.h

class IBackendProvider {
public:
    virtual ~IBackendProvider() = default;

    //==========================================================================
    // Lifecycle
    //
    // initialize() receives the backend's VDevice at bootstrap.
    // For NVMe backends: vdev != nullptr, contains queue slice + namespace view.
    // For non-NVMe backends (GDS, RDMA): vdev == nullptr, backend ignores it.
    //==========================================================================
    
    virtual bool initialize(VDevice* vdev) = 0;
    virtual void cleanup() = 0;

    //==========================================================================
    // Descriptor Build (Memory Layer → Backend)
    //
    // Translate raw DMA ioaddrs + sub-slice layout into transport command bytes.
    // For NVMe: PRP/SGL construction.
    // For RDMA: scatter-gather list.
    // For GDS: cuFile descriptor.
    //==========================================================================
    
    virtual bool prepare_descriptors(
        const uint64_t* ioaddrs,      // per-page bus addresses from IAccelerator::dma_map
        const SubSliceInfo* slices,   // IO-slice layout (IO Engine fan-out)
        uint32_t n_slices,
        BufferDescriptor* out_descs) = 0;  // output: transport command bytes

    //==========================================================================
    // Target Handle (StorageTarget → Device-Resident Handle)
    //
    // Build a GPU-resident handle the submit kernel can dereference.
    // For NVMe: produces NvmeFileDeviceHandle (extents + vDevice ref).
    // For RDMA: produces remote QP + rkey.
    // Returns opaque void* — backend-private type.
    //==========================================================================
    
    virtual void* acquire_target_handle(const StorageTarget& target) = 0;
    virtual void  release_target_handle(void* handle) = 0;

    //==========================================================================
    // Submission Modes
    //
    // Mode 1 (REQUIRED): BATCH_GPU_STREAM
    //   CPU stages descriptors; GPU kernel rings device doorbell.
    //   Completion is stream-ordered.
    //
    // Mode 2 (REQUIRED): BATCH_CPU_SYNC
    //   CPU prepares + submits + blocks. No GPU kernel.
    //
    // Mode 3 (OPTIONAL): BATCH_CPU_ASYNC
    //   CPU submits, returns immediately. Caller polls/waits later.
    //
    // Mode 4 (OPTIONAL): COOP
    //   Shared SQ/CQ between CPU and GPU. Direction per channel.
    //==========================================================================
    
    // Mode 1: GPU-stream submit (primary high-throughput path)
    virtual void launch_batch_gpu_stream(
        AccelStream stream,              // NOT cudaStream_t — opaque void*
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;
    
    // Mode 2: CPU sync submit (bootstrap / metadata / tests)
    virtual bool submit_batch_cpu_sync(
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;
    
    // Mode 3: CPU async submit (optional — may return false if unsupported)
    virtual bool submit_batch_cpu_async(
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;
    
    // Mode 4: COOP channel setup (optional — may return false if unsupported)
    virtual bool setup_coop_channel(void* channel_params) = 0;

    //==========================================================================
    // Metadata
    //==========================================================================
    
    virtual BackendType backend_type() const = 0;   // NVME_LOCAL, GDS, RDMA, ...
    virtual const char* backend_name() const = 0;   // "local_nvme", "gds", "rdma"
    virtual size_t      max_io_size() const = 0;    // bytes (from vDevice MDTS or backend limit)
};

} // namespace tutti
```

---

## Key Changes Explained

### 1. `initialize(VDevice* vdev)` Replaces `initialize()`

**Before (v0.1)**:
```cpp
bool LocalNvmeBackend::initialize() {
    // Backend discovers its queues through libnvm directly
    ctrl_ = /* some global or passed-in nvm_ctrl_t* */;
    return true;
}
```

**After (new design)**:
```cpp
bool LocalNvmeBackend::initialize(VDevice* vdev) {
    if (vdev == nullptr) {
        return false;  // NVMe backend requires a vDevice
    }
    vdev_ = vdev;  // Hold the queue slice
    // No need to call acquire_queue() — we already have vdev->d_qps
    return true;
}
```

**For non-NVMe backends**:
```cpp
bool GdsBackend::initialize(VDevice* vdev) {
    // vdev is nullptr for GDS — ignore it
    // Initialize cuFile handles instead
    return cufile_driver_open() == 0;
}
```

### 2. Remove `acquire_queue()` / `release_queue()` from SPI

**v0.1 has**:
```cpp
virtual IQueue* acquire_queue(uint32_t required_caps) = 0;
virtual void release_queue(IQueue* queue) = 0;
```

**Why remove**: These were **hot-path per-IO calls**. In the new design:
- Backends receive their queue slice once at `initialize()`
- Device-side kernels call `acquire_queue(vdev->d_qps, vdev->queue_quota)` directly (DM helper)
- No runtime DM calls after bootstrap

### 3. `acquire_target_handle(StorageTarget)` — Backend-Neutral Input

**Before**: Implicit or NVMe-specific `NvmeFile*` passed around.

**After**: Backends receive a `StorageTarget` (convergence noun):

```cpp
void* LocalNvmeBackend::acquire_target_handle(const StorageTarget& target) {
    if (target.kind != StorageTargetKind::NVME_FILE &&
        target.kind != StorageTargetKind::NVME_RAW) {
        return nullptr;  // This backend only handles NVMe targets
    }
    
    // Build NvmeFileDeviceHandle (GPU-resident)
    auto* handle = new NvmeFileDeviceHandle();
    if (target.kind == StorageTargetKind::NVME_FILE) {
        // Populate from target.nvme_file
        handle->file_id = target.nvme_file.file_id;
        // ... load extents ...
    } else {
        // Populate from target.nvme_raw
        handle->extents = target.nvme_raw.extents;
    }
    handle->d_qps = vdev_->d_qps;
    handle->num_d_qps = vdev_->queue_quota;
    
    return handle;  // Opaque void* to IO Engine
}
```

### 4. `AccelStream` Replaces `cudaStream_t`

**Before**:
```cpp
#include <cuda_runtime.h>

virtual void launch_batch_gpu_stream(
    ...,
    cudaStream_t stream,  // ← CUDA leak
    ...);
```

**After**:
```cpp
#include "tutti/accel/accel_types.h"  // AccelStream = void*

virtual void launch_batch_gpu_stream(
    ...,
    AccelStream stream,  // ← Opaque, no CUDA dependency
    ...);
```

Concrete backends cast back:
```cpp
void LocalNvmeBackend::launch_batch_gpu_stream(AccelStream stream, ...) {
    cudaStream_t cuda_stream = (cudaStream_t)stream;  // Safe cast in .cu file
    my_kernel<<<grid, block, 0, cuda_stream>>>(...);
}
```

---

## StorageTarget Definition

```cpp
// tutti/types/storage_target.h
#pragma once
#include <cstdint>
#include <vector>

namespace tutti {

struct Device;  // coordinator/include/device.h

enum class StorageTargetKind : uint32_t {
    NVME_FILE   = 0,  // File on NVMe namespace (Block Storage produces this)
    NVME_RAW    = 1,  // Raw LBA range (raw device produces this)
    RDMA_REMOTE = 2,  // Remote RDMA memory region (future)
    GDS_FILE    = 3,  // GPUDirect Storage cuFile (future)
};

struct LbaExtent {
    uint64_t start_lba;
    uint64_t length_blocks;
};

struct StorageTarget {
    StorageTargetKind kind;
    
    union {
        struct {
            uint64_t      file_id;
            const Device* device;
            uint32_t      shard_index;
        } nvme_file;
        
        struct {
            const Device*           device;
            uint32_t                namespace_id;
            std::vector<LbaExtent>* extents;  // pointer to avoid union size explosion
        } nvme_raw;
        
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint32_t lkey;
        } rdma_remote;
        
        struct {
            void* cufile_handle;  // CUfileHandle_t (opaque)
        } gds_file;
    };
    
    uint64_t logical_size;
};

} // namespace tutti
```

**Usage**: Block Storage and raw device both produce `StorageTarget`; backends consume it.

---

## NVMe Backend Private API

The NVMe backend has backend-private device-side helpers (NOT in the SPI):

```cpp
// backends/local_nvme/include/nvme_backend_device.cuh
#pragma once
#include "tutti/abstraction/accel.h"
#include "device_manager/include/queue_acquire_helper.cuh"  // DM helpers

namespace tutti {

struct NvmeFileDeviceHandle;  // backend-private GPU-resident handle

// Resolve logical byte offset → (LBA, nblocks) by walking extents.
TUTTI_DEVICE TUTTI_FORCEINLINE
bool resolve_lba(
    const NvmeFileDeviceHandle* handle,
    uint64_t logical_offset,
    uint64_t nbytes,
    uint64_t* out_lba,
    uint32_t* out_nblocks);

// Submit one read via the vDevice's queue slice.
TUTTI_DEVICE TUTTI_FORCEINLINE
void submit_read_one(
    const NvmeFileDeviceHandle* handle,
    uint64_t prp1,
    uint64_t prp2,
    uint64_t offset,
    uint64_t nbytes);

// Submit one write via the vDevice's queue slice.
TUTTI_DEVICE TUTTI_FORCEINLINE
void submit_write_one(
    const NvmeFileDeviceHandle* handle,
    uint64_t prp1,
    uint64_t prp2,
    uint64_t offset,
    uint64_t nbytes);

} // namespace tutti
```

These call **down** into DM's `acquire_queue` / `issue_nvme_cmd` / `poll` helpers.

---

## CUDA Leak Fixes

### File: `io_engine/include/backend_provider.h`

**Before** (line 35):
```cpp
#include <cuda_runtime.h>
```

**After**:
```cpp
#include "tutti/accel/accel_types.h"  // AccelStream, AccelEvent
```

**Before** (line 163):
```cpp
virtual void launch_batch_gpu_stream(
    IQueue* queue,
    cudaStream_t stream,  // ← CUDA leak
    ...);
```

**After**:
```cpp
virtual void launch_batch_gpu_stream(
    AccelStream stream,  // ← Opaque
    void* target_handle,
    ...);
```

---

## Migration Checklist

### Files to Modify

```
io_engine/include/backend_provider.h
  - Remove #include <cuda_runtime.h>
  - Add #include "tutti/accel/accel_types.h"
  - Change initialize() → initialize(VDevice* vdev)
  - Change cudaStream_t → AccelStream
  - Add acquire_target_handle(StorageTarget)
  - Add release_target_handle(void*)
  - Remove acquire_queue() / release_queue()
```

### Files to Create

```
tutti/types/storage_target.h                (NEW — StorageTarget convergence noun)
backends/local_nvme/include/nvme_backend_device.cuh  (NEW — backend-private device helpers)
```

### Concrete Backend Updates

Each concrete backend (e.g., `LocalNvmeBackend`) must:
1. Update `initialize()` signature to accept `VDevice*`
2. Hold `vdev_` member variable
3. Implement `acquire_target_handle(StorageTarget)`
4. Cast `AccelStream` back to `cudaStream_t` in `.cu` files only

---

## Validation Criteria

✅ Backends SPI is correct when:

1. **No cuda_runtime.h in SPI header** — `backend_provider.h` includes only opaque types
2. **AccelStream is opaque** — no `cudaStream_t` in public interface
3. **VDevice passed at initialize** — backends receive queue slice once, not per-IO
4. **StorageTarget works** — backends can consume both NVME_FILE and NVME_RAW
5. **No acquire_queue in SPI** — hot-path queue acquisition is device-side only
6. **Target handle roundtrip works** — `acquire_target_handle` → submit → `release_target_handle`

---

## Open Questions

See `10-open-questions.md` for:
- **Q8**: Should `initialize(VDevice*)` be `initialize(BackendInitParams)` with a tagged union?  
  **Current decision**: Direct `VDevice*` for v0.1 (non-NVMe backends receive nullptr).
- **Q9**: Should StorageTarget extents be inline or pointer?  
  **Current decision**: Pointer (avoids union size explosion for multi-extent files).
