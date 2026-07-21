# Layer 5: Block Storage + Raw Device (Top Interfaces)

> Two symmetric data interfaces that converge on `StorageTarget` before dispatching to IO Engine.

## Architecture: Two Top Interfaces, Not One

**v0.1**: `IBlockStorage` (GPUFile) is the only way in.

**New design**: Two peer interfaces:
1. **IBlockStorage** — named, striped, persistent (GPUFile)
2. **IRawDevice** — fileless (namespace + LBA range)

Both produce `StorageTarget` → IO Engine → Backends.

```
┌──────────────────────┐        ┌──────────────────────┐
│  IBlockStorage       │        │  IRawDevice          │
│  (GPUFile)           │        │  (ns + LBA)          │
└─────────┬────────────┘        └─────────┬────────────┘
          │                               │
          └───────────┬───────────────────┘
                      ▼
              StorageTarget (convergence noun)
                      │
                      ▼
                 IIoEngine
                      │
                      ▼
              IBackendProvider
```

---

## 5.1 IBlockStorage (Exists, Minor Changes)

### Current State

`block_storage/include/block_storage.h` is largely correct. Changes needed:

1. **Stream type**: `GpuStreamHandle = void*` → `AccelStream` (rename for consistency)
2. **No new methods**: API is already well-designed

### Proposed API (Revised)

```cpp
// block_storage/include/block_storage.h (revised)
#pragma once
#include "tutti/accel/accel_types.h"  // AccelStream
#include <cstdint>
#include <string>
#include <vector>

namespace tutti {

using GpuFileId = uint32_t;

enum GpuFileOpenFlags : uint32_t {
    GPU_FILE_OPEN_EXISTING  = 0,
    GPU_FILE_OPEN_CREATE    = 1u << 0,
    GPU_FILE_OPEN_EXCL      = 1u << 1,
    GPU_FILE_OPEN_NO_PERSIST = 1u << 2,
};

constexpr uint32_t kGpuFileMaxShards = 4;

struct GpuFileSpec {
    std::string name;
    uint64_t    total_size;
    uint64_t    tensor_shape[3];
    int         shard_placement[kGpuFileMaxShards];  // device IDs
};

struct GpuFile {
    GpuFileId   id;
    std::string name;
    uint64_t    total_size;
    uint64_t    tensor_shape[3];
    uint32_t    num_shards;
    // ... shard metadata ...
};

struct GpuFileHandle {
    const GpuFile* file;
    uint64_t       tensor_size;
    uint32_t       num_shards;
    void*          d_shards_host;  // host-pinned shard pointer array
    void*          d_shards_dev;   // device-resident shard pointer array
};

class IBlockStorage {
public:
    virtual ~IBlockStorage() = default;

    //==========================================================================
    // Lifecycle
    //==========================================================================
    
    virtual bool bootstrap(const std::vector<const Device*>& devices) = 0;
    virtual bool shutdown() = 0;

    //==========================================================================
    // Directory (single + batch, persistent)
    //==========================================================================
    
    virtual GpuFile* open_gpu_file(
        const GpuFileSpec& spec,
        uint32_t flags = GPU_FILE_OPEN_EXISTING) = 0;
    
    virtual bool close_gpu_file(GpuFile* file) = 0;
    
    virtual bool delete_gpu_file(GpuFile* file, bool persist_now = true) = 0;
    
    // Batch variants (for bulk-init optimization)
    virtual std::vector<GpuFile*> open_gpu_files_batch(
        const GpuFileSpec* specs,
        uint32_t count,
        uint32_t flags) = 0;
    
    virtual bool delete_gpu_files_batch(
        GpuFile* const* files,
        uint32_t count,
        bool* out_ok) = 0;
    
    virtual bool flush_metadata() = 0;
    
    virtual std::vector<std::string> list_gpu_file_names() const = 0;
    virtual std::vector<GpuFile*> list_open_gpu_files() const = 0;

    //==========================================================================
    // GPU Handle Acquisition → Produces StorageTarget
    //
    // acquire_device_handle brings a file's shards to GPU and produces
    // a GpuFileHandle. Internally, this builds a StorageTarget for each
    // shard and hands it to the backend via acquire_target_handle().
    //==========================================================================
    
    virtual GpuFileHandle* acquire_device_handle(
        GpuFile* file,
        AccelStream stream) = 0;  // ← Was GpuStreamHandle, now AccelStream
    
    virtual void release_device_handle(
        GpuFileHandle* handle,
        AccelStream stream) = 0;
    
    // Batch variant
    virtual bool acquire_device_handles_batch(
        GpuFile* const* files,
        uint32_t count,
        AccelStream stream,
        GpuFileHandle** out_handles) = 0;

    //==========================================================================
    // Durability
    //==========================================================================
    
    virtual bool sync_file(GpuFileId id, AccelStream stream) = 0;
};

} // namespace tutti
```

### Changes from v0.1

| Item | v0.1 | New |
|---|---|---|
| Stream type | `GpuStreamHandle = void*` (local typedef) | `AccelStream` (shared type from `tutti/accel/accel_types.h`) |
| cuda_runtime.h | Not included (already clean) | Still clean ✓ |
| API shape | Correct | No changes ✓ |

**Migration**: Replace `using GpuStreamHandle = void*;` with `using AccelStream = tutti::AccelStream;` or just use `AccelStream` directly.

---

## 5.2 IRawDevice (NEW — Must Be Created)

### Purpose

Expose fileless (namespace + LBA range) access for:
- Databases that manage their own block allocation
- Raw KV stores
- Block-oriented workloads where a file is pure overhead

### Proposed API

```cpp
// raw_device/include/raw_device.h (NEW)
#pragma once
#include "tutti/accel/accel_types.h"     // AccelStream
#include "tutti/types/storage_target.h"  // StorageTarget, LbaExtent
#include <cstdint>
#include <vector>

namespace tutti {

struct Device;  // coordinator/include/device.h

// Opaque handle to a raw device target.
// Internally wraps a StorageTarget + backend-produced target_handle.
struct RawTargetHandle;

class IRawDevice {
public:
    virtual ~IRawDevice() = default;

    //==========================================================================
    // Lifecycle
    //==========================================================================
    
    virtual bool bootstrap(const std::vector<const Device*>& devices) = 0;
    virtual bool shutdown() = 0;

    //==========================================================================
    // Acquire Raw Target → Produces StorageTarget{NVME_RAW}
    //
    // Given (namespace, LBA range), produce a RawTargetHandle.
    // No FIEMAP, no persistent log, no directory — pure LBA passthrough.
    //
    // The caller is responsible for:
    //   - LBA range validity (start_lba + total_blocks ≤ namespace size)
    //   - Block alignment (start_lba % blk_size == 0)
    //   - Not overlapping with other allocations
    //==========================================================================
    
    virtual RawTargetHandle* acquire_raw_target(
        const Device* device,
        uint32_t namespace_id,
        const std::vector<LbaExtent>& extents,
        AccelStream stream) = 0;
    
    virtual void release_raw_target(
        RawTargetHandle* handle,
        AccelStream stream) = 0;

    //==========================================================================
    // Metadata Query
    //==========================================================================
    
    // Total namespace capacity in blocks.
    virtual uint64_t namespace_capacity_blocks(
        const Device* device,
        uint32_t namespace_id) const = 0;
    
    // Block size in bytes.
    virtual uint32_t block_size(
        const Device* device,
        uint32_t namespace_id) const = 0;
};

} // namespace tutti
```

### Implementation Notes

`RawTargetHandle` internally wraps:
1. A `StorageTarget` with `kind = NVME_RAW`
2. The backend-produced `target_handle` (from `IBackendProvider::acquire_target_handle`)

```cpp
// raw_device/src/raw_target_handle_impl.h
struct RawTargetHandle {
    StorageTarget target;       // kind = NVME_RAW, extents filled
    void* backend_handle;       // from IBackendProvider::acquire_target_handle(target)
    const Device* device;
    uint32_t namespace_id;
    uint64_t logical_size;      // sum of extent lengths
};
```

### Usage Example

```cpp
// Application: raw KV store managing 1 GiB at LBA 0x100000
std::vector<LbaExtent> extents = {{0x100000, 0x40000}};  // 256K blocks × 4096 bytes = 1 GiB

RawTargetHandle* raw = raw_device->acquire_raw_target(
    device, /*ns_id=*/1, extents, stream);

MemoryRegion* region = accel->register_device(my_buffer, 1ULL << 30, device_id);
accel->dma_map(region, device_id, &ioaddrs, &count);

// Submit IO via IO Engine (same API as GpuFile)
IoRequest req = {
    .region = region,
    .target_handle = raw->backend_handle,  // opaque to application
    .byte_offset = 0,
    .byte_length = 1ULL << 30
};
io_engine->submit_batch({req}, /*is_read=*/true, stream);

raw_device->release_raw_target(raw, stream);
```

---

## 5.3 StorageTarget Convergence

Both interfaces produce a `StorageTarget` before dispatching to IO Engine.

### Block Storage → StorageTarget

```cpp
// block_storage/src/host_fs_backed_block_storage.cpp

GpuFileHandle* HostFsBackedBlockStorage::acquire_device_handle(GpuFile* file, AccelStream stream) {
    // For each shard, produce a StorageTarget
    for (uint32_t i = 0; i < file->num_shards; ++i) {
        StorageTarget target = {
            .kind = StorageTargetKind::NVME_FILE,
            .nvme_file = {
                .file_id = file->id,
                .device = file->shards[i].device,
                .shard_index = i
            },
            .logical_size = file->shards[i].size
        };
        
        // Backend builds device handle from StorageTarget
        void* backend_handle = backend_->acquire_target_handle(target);
        // ... store in GpuFileHandle ...
    }
}
```

### Raw Device → StorageTarget

```cpp
// raw_device/src/raw_device_impl.cpp

RawTargetHandle* RawDeviceImpl::acquire_raw_target(
    const Device* device,
    uint32_t namespace_id,
    const std::vector<LbaExtent>& extents,
    AccelStream stream) {
    
    StorageTarget target = {
        .kind = StorageTargetKind::NVME_RAW,
        .nvme_raw = {
            .device = device,
            .namespace_id = namespace_id,
            .extents = new std::vector<LbaExtent>(extents)  // heap copy
        },
        .logical_size = compute_total_size(extents, device)
    };
    
    void* backend_handle = backend_->acquire_target_handle(target);
    
    auto* handle = new RawTargetHandle{target, backend_handle, device, namespace_id, target.logical_size};
    return handle;
}
```

---

## Migration Checklist

### Files to Modify

```
block_storage/include/block_storage.h
  - Change: using GpuStreamHandle = void*;
  - To:     using AccelStream = tutti::AccelStream;
  - Or just: use AccelStream directly throughout
```

### Files to Create

```
raw_device/include/raw_device.h                (NEW — IRawDevice interface)
raw_device/src/raw_device_impl.h               (NEW — concrete implementation)
raw_device/src/raw_device_impl.cpp             (NEW)
raw_device/src/raw_target_handle_impl.h        (NEW — RawTargetHandle internals)
```

### Coordinator Integration

```cpp
// coordinator/include/coordinator.h

class Coordinator {
public:
    // ...existing methods...
    
    // NEW: raw device access
    IRawDevice* raw_device() { return raw_device_.get(); }

private:
    std::unique_ptr<IBlockStorage> block_storage_;
    std::unique_ptr<IRawDevice> raw_device_;      // NEW
    std::unique_ptr<IIoEngine> io_engine_;
    std::unique_ptr<IAccelerator> accel_;
    std::unique_ptr<IBackendProvider> backend_;
};
```

---

## Validation Criteria

✅ Top interfaces are correct when:

1. **IBlockStorage uses AccelStream** — no local `GpuStreamHandle` typedef
2. **IRawDevice exists and works** — can acquire/release raw LBA ranges
3. **Both produce StorageTarget** — no direct NVMe types leak to IO Engine
4. **Coordinator holds both** — `block_storage_` + `raw_device_` peer interfaces
5. **IO Engine is target-agnostic** — submits IoRequests the same way regardless of source
6. **No cuda_runtime.h in top headers** — all use AccelStream opaque type

---

## Example: End-to-End Flow

### GPUFile Path

```
App: open_gpu_file("model.safetensors", CREATE)
  → IBlockStorage: produce GpuFile (multi-shard metadata)
  → App: acquire_device_handle(file, stream)
    → IBlockStorage: for each shard, build StorageTarget{NVME_FILE}
      → IBackendProvider: acquire_target_handle(target) → GPU-resident NvmeFileDeviceHandle*
  → App: submit_batch({IoRequest{region, handle, offset, size}}, READ, stream)
    → IIoEngine: fan-out, prepare_descriptors, launch_batch_gpu_stream
      → IBackendProvider: kernel uses VDevice::d_qps + NvmeFileDeviceHandle
```

### Raw Device Path

```
App: acquire_raw_target(device, ns=1, extents=[{lba=0x100000, len=0x40000}], stream)
  → IRawDevice: build StorageTarget{NVME_RAW}
    → IBackendProvider: acquire_target_handle(target) → GPU-resident NvmeRawDeviceHandle*
  → App: submit_batch({IoRequest{region, handle, offset, size}}, READ, stream)
    → IIoEngine: fan-out, prepare_descriptors, launch_batch_gpu_stream
      → IBackendProvider: kernel uses VDevice::d_qps + NvmeRawDeviceHandle
```

**Key observation**: IO Engine sees identical `IoRequest` in both paths. The `target_handle` is opaque — backend-produced, backend-consumed.

---

## Open Questions

See `10-open-questions.md` for:
- **Q12**: Should IRawDevice validate LBA ranges or trust the caller?  
  **Current decision**: Trust the caller (no validation for v0.1). Add optional debug-mode checks in post-v0.1.
- **Q13**: Should GpuFile and raw device share a common `IStorageInterface` base?  
  **Current decision**: No for v0.1 (YAGNI). Their APIs are different enough (directory vs. LBA passthrough).
