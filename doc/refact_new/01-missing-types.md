# Missing Shared Types

> These types must be created before any layer above Layer 1 can be implemented.  
> They are value types or type aliases, not interfaces.

## Summary Table

| Type | Status | Purpose | Suggested Header |
|---|---|---|---|
| `AccelStream` | **MISSING** | Opaque `void*` alias replacing `cudaStream_t` / `hipStream_t` / `sycl::queue*` | `tutti/accel/accel_types.h` |
| `AccelEvent` | **MISSING** | Opaque `void*` alias replacing `cudaEvent_t` | `tutti/accel/accel_types.h` |
| `StorageTarget` | **MISSING** | Convergence noun produced by IBlockStorage and IRawDevice; consumed by IBackendProvider and IIoEngine | `tutti/types/storage_target.h` |
| `VDevice` | **MISSING** | Queue-pair slice + namespace view + caps handed from IVirtualNvme to a backend at `initialize()` | `device_manager/include/vdevice.h` |
| `IVirtualNvme` | **MISSING** | Interface through which a backend pulls its `VDevice` from Device Manager | `device_manager/include/virtual_nvme.h` |
| `SubSliceInfo` | **MISSING** | Describes one transport-sized sub-IO within a fanned-out batch | `tutti/types/io_types.h` |
| `IpcHandle` | **MISSING** | Opaque cross-process IPC export token | `tutti/accel/accel_types.h` |

---

## AccelStream / AccelEvent

### Current State

Three independent definitions exist:
1. `GpuStreamHandle = void*` in `nvme_storage.h`
2. `GpuStreamHandle = void*` in `block_storage.h`
3. Raw `cudaStream_t` in 8 public headers (constraint violation)

`AccelEvent` doesn't exist anywhere.

### Proposed API

```cpp
// tutti/accel/accel_types.h
#pragma once

namespace tutti {

// Opaque stream handle — concrete type is vendor-specific:
//   CUDA: cudaStream_t
//   ROCm: hipStream_t
//   SYCL: sycl::queue*
//   CANN: aclrtStream
using AccelStream = void*;

// Opaque event handle — concrete type is vendor-specific:
//   CUDA: cudaEvent_t
//   ROCm: hipEvent_t
//   SYCL: sycl::event*
using AccelEvent = void*;

} // namespace tutti
```

### Migration

All headers above the HAL that currently use `cudaStream_t` must:
1. Remove `#include <cuda_runtime.h>`
2. Replace `cudaStream_t` with `AccelStream`
3. Add `#include "tutti/accel/accel_types.h"`

The two existing `GpuStreamHandle` definitions should be deleted and replaced with `using AccelStream = tutti::AccelStream`.

---

## StorageTarget

### Current State

**Completely absent.** The concept exists implicitly:
- Block Storage produces `NvmeFileDeviceHandle*` (NVMe-specific)
- No raw device interface exists
- IO Engine is tied to `NvmeBatchInputTensor` (NVMe-specific)

### Proposed API

```cpp
// tutti/types/storage_target.h
#pragma once
#include <cstdint>
#include <vector>

namespace tutti {

struct Device;  // coordinator/include/device.h

enum class StorageTargetKind : uint32_t {
    NVME_FILE   = 0,  // File on NVMe namespace (via Block Storage)
    NVME_RAW    = 1,  // Raw LBA range on NVMe namespace (via raw device)
    RDMA_REMOTE = 2,  // Remote RDMA memory region (future)
    GDS_FILE    = 3,  // GPUDirect Storage cuFile (future)
};

// LBA extent for raw NVMe access
struct LbaExtent {
    uint64_t start_lba;
    uint64_t length_blocks;
};

struct StorageTarget {
    StorageTargetKind kind;
    
    union {
        // NVME_FILE: references a GpuFile's shard
        struct {
            uint64_t      file_id;
            const Device* device;
            uint32_t      shard_index;  // which shard of a multi-device file
        } nvme_file;
        
        // NVME_RAW: direct (namespace, LBA range) access
        struct {
            const Device*           device;
            uint32_t                namespace_id;
            std::vector<LbaExtent>* extents;  // pointer to avoid union size explosion
        } nvme_raw;
        
        // RDMA_REMOTE: future
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint32_t lkey;
        } rdma_remote;
        
        // GDS_FILE: future
        struct {
            void* cufile_handle;  // CUfileHandle_t (opaque)
        } gds_file;
    };
    
    // Total logical size in bytes
    uint64_t logical_size;
};

} // namespace tutti
```

### Design Notes

- The union payload must remain small (no inline `std::vector`). For `nvme_raw.extents`, store a pointer.
- The `StorageTarget` is **ephemeral** — valid only for the duration of one IO operation. It's not a persistent handle.
- Backends call `IBackendProvider::acquire_target_handle(StorageTarget)` to produce a GPU-resident device handle from this.

---

## VDevice

### Current State

**Completely absent.** The concept is implicit in `NvmeQueueGroup` embedded in `LocalNvmeDevice`, but there's no "vDevice" type handed to backends.

### Proposed API

```cpp
// device_manager/include/vdevice.h
#pragma once
#include <cstdint>
#include <cstddef>

// Forward declaration — QueuePair defined in libnvm
struct nvm_queue_t;

namespace tutti {

// A virtual storage device: one backend's slice of a physical NVMe controller.
// Handed from IVirtualNvme to the backend at initialize() time.
struct VDevice {
    // Identity
    int32_t  phys_device_id;  // which LocalNvmeDevice this slices
    uint32_t vdev_id;         // dense index within IVirtualNvme
    
    // Level-2 allocation: the slice of d_qps[] this backend owns
    nvm_queue_t* d_qps;       // GPU-resident pointer into parent NvmeQueueGroup::d_qps_[slice_start]
    uint32_t     queue_quota; // number of QPs in this slice
    
    // Namespace view (from LocalNvmeDevice)
    uint32_t namespace_id;
    uint32_t blk_size;
    uint32_t blk_size_log;
    size_t   max_data_size;   // MDTS in bytes
    
    // Capabilities (bitmask)
    uint32_t caps;
    // bit 0: GPUDIRECT_CAPABLE
    // bit 1-31: reserved
};

} // namespace tutti
```

### Design Notes

- `VDevice` is a **view**, not ownership. The backend receives it from `IVirtualNvme::open_vdevice()` and must return it via `close_vdevice()` at shutdown.
- `d_qps` points into the parent `NvmeQueueGroup`'s GPU-resident array. The slice is `[d_qps, d_qps + queue_quota)`.
- Non-NVMe backends (GDS, RDMA) receive `nullptr` at `initialize(VDevice*)` and ignore it.

---

## IVirtualNvme

### Current State

**Completely absent.** Currently:
- `IBackendProvider::acquire_queue()` exists but is the wrong abstraction (hot-path per-IO call)
- Backends don't receive their queue slice at initialization — they discover it through libnvm directly

### Proposed API

```cpp
// device_manager/include/virtual_nvme.h
#pragma once
#include <cstdint>
#include <string>
#include "vdevice.h"

namespace tutti {

// Level-2 allocator: splits this process's queue-pair grant into per-backend slices.
class IVirtualNvme {
public:
    virtual ~IVirtualNvme() = default;
    
    // Carve a VDevice from phys_device_id's QP pool.
    // quota = number of QPs to reserve for this backend.
    // Returns nullptr if:
    //   - phys_id unknown
    //   - quota == 0
    //   - insufficient QPs remain in the pool
    // Error message written to *error if provided.
    virtual VDevice* open_vdevice(int32_t phys_id, uint32_t quota,
                                  std::string* error = nullptr) = 0;
    
    // Return the QP slice back to the pool. No-op on nullptr.
    virtual void close_vdevice(VDevice* vdev) = 0;
    
    // Remaining unallocated QPs for a given physical device.
    virtual uint32_t available_queues(int32_t phys_id) const = 0;
    
    // Capability bitmask of the underlying physical device (MDTS, page_size, etc.)
    virtual uint32_t caps(int32_t phys_id) const = 0;
};

} // namespace tutti
```

### Concrete Implementation

Create `device_manager/src/local_nvme_virtual.{h,cpp}`:

- `LocalNvmeVirtualRegistry` holds an `IDeviceRegistry*` (not owned)
- Walks to `LocalNvmeDevice::queue_group` for each physical device
- Manages a per-device free-list of `[0, n_qps)` contiguous slices
- Allocation: contiguous-first-fit
- Deallocation: return slice to free-list
- For v0.1: static allocation at bootstrap, no queue migration

---

## SubSliceInfo

### Current State

**Forward-declared but not defined.** Referenced in:
- `io_engine/include/backend_provider.h:76` — `const SubSliceInfo* slices` parameter
- Nowhere else

### Proposed API

```cpp
// tutti/types/io_types.h
#pragma once
#include <cstdint>

namespace tutti {

struct MemoryRegion;  // tutti/accel/accel_types.h

// Describes one transport-sized sub-IO within a fanned-out batch.
// The IO Engine produces these when splitting a large IoRequest into
// transport-sized chunks (each ≤ backend's max_io_size).
struct SubSliceInfo {
    uint64_t region_byte_offset;  // offset within the MemoryRegion
    uint64_t byte_length;         // sub-IO size (≤ max_io_size)
    uint32_t ioaddr_index;        // index into MemoryRegion::dma_ioaddrs[]
};

// Backend-neutral IO request (replaces NvmeBatchInputTensor).
struct IoRequest {
    MemoryRegion* region;         // registered source/destination buffer
    void*         target_handle;  // opaque, produced by IBackendProvider::acquire_target_handle
    uint64_t      byte_offset;    // offset into the logical target (file offset or LBA)
    uint64_t      byte_length;    // bytes to transfer
};

} // namespace tutti
```

### Usage

```cpp
// IO Engine fan-out pseudo-code:
for (const IoRequest& req : requests) {
    size_t max_io = backend->max_io_size();
    for (size_t off = 0; off < req.byte_length; off += max_io) {
        SubSliceInfo slice = {
            .region_byte_offset = req.byte_offset + off,
            .byte_length = std::min(max_io, req.byte_length - off),
            .ioaddr_index = compute_ioaddr_index(req.region, off)
        };
        // Call backend->prepare_descriptors(..., &slice, 1, &desc)
    }
}
```

---

## IpcHandle

### Current State

**Absent as a named type.** Currently stored as raw bytes in `MemoryRegion.external` (see `memory/include/memory_region.h:37`):

```cpp
struct CudaIpcImport {
    uint8_t ipc_handle[64];  // cudaIpcMemHandle_t
    // ...
};
```

### Proposed API

```cpp
// tutti/accel/accel_types.h
#pragma once
#include <cstdint>

namespace tutti {

// Opaque cross-process IPC memory handle.
// Size: large enough for any vendor's IPC token.
//   CUDA: cudaIpcMemHandle_t (64 bytes)
//   ROCm: hipIpcMemHandle_t (64 bytes)
//   SYCL: platform-dependent
struct IpcHandle {
    uint8_t data[128];  // 2× CUDA size for future headroom
};

} // namespace tutti
```

### Migration

Replace `CudaIpcImport::ipc_handle[64]` with `IpcHandle`. Update `IAccelerator::ipc_export()` and `ipc_import()` to use this type.

---

## Creation Order

These types have dependencies:

```
Layer 0 (no dependencies):
  - AccelStream, AccelEvent, IpcHandle  (tutti/accel/accel_types.h)

Layer 1 (depends on L0):
  - SubSliceInfo, IoRequest  (tutti/types/io_types.h) — needs MemoryRegion forward-decl
  - StorageTarget  (tutti/types/storage_target.h) — needs Device forward-decl

Layer 2 (depends on L0, used by DM):
  - VDevice  (device_manager/include/vdevice.h) — needs nvm_queue_t forward-decl
  - IVirtualNvme  (device_manager/include/virtual_nvme.h) — needs VDevice
```

**Recommended:** Create all L0 types first (one header), then L1, then L2. Each can be validated independently before moving to the next layer's implementation.
