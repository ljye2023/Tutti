# Layer 4: IO Engine (IIoEngine)

> Batch data-plane brain: fan-out, descriptor staging, backend-neutral submission.  
> Owns the transfer schemes (batch GPU-stream, CPU sync, async, COOP).

## Key Changes from v0.1

| Aspect | Current (v0.1) | New Design |
|---|---|---|
| **Input type** | `NvmeBatchInputTensor` (NVMe-specific) | `IoRequest` (backend-neutral) |
| **Stream type** | `cudaStream_t` | `AccelStream` (opaque `void*`) |
| **Fan-out ownership** | `IMemorySubsystem::register_tensor()` | **IO Engine** (orchestrates using IAccelerator + IBackendProvider) |
| **Dependencies** | Direct NVMe headers (`nvme_batch.h`) | Only `IBackendProvider*` + `IAccelerator*` |
| **slice_fanout query** | Absent (data buried in MemoryRegion) | `slice_fanout(MemoryRegion*)` method added |

---

## Current State: NvmeBatchInputTensor Problem

**v0.1** `IIoEngine::submit_batch` signature:

```cpp
// io_engine/include/io_engine.h (current)
#include <cuda_runtime.h>
#include "local_nvme/nvme_batch.h"  // ← NVMe-specific

bool submit_batch(
    const std::vector<NvmeBatchInputTensor>& inputs,  // ← NVMe-specific
    bool is_read,
    cudaStream_t stream);  // ← CUDA leak
```

**What is `NvmeBatchInputTensor`?**

```cpp
// io_engine/include/local_nvme/nvme_batch.h
struct NvmeBatchInputTensor {
    void*         data_ptr;           // device buffer
    uint64_t      tensor_size;
    NvmeFile*     file;               // ← ties IO Engine to NVMe file abstraction
    uint64_t      file_byte_offset;
    MemoryRegion* region;
};
```

**Problems**:
1. Ties IO Engine to `NvmeFile*` (should be backend-neutral)
2. `data_ptr` duplicates `region` (redundant)
3. No support for `StorageTarget` (raw device, RDMA, GDS)
4. Coordinator includes `nvme_batch.h` (leaks NVMe specifics upward)

---

## Proposed API: Backend-Neutral IIoEngine

```cpp
// io_engine/include/io_engine.h (revised)
#pragma once
#include "tutti/accel/accel_types.h"  // AccelStream — no cuda_runtime.h
#include "tutti/types/io_types.h"     // IoRequest
#include <vector>
#include <cstdint>

namespace tutti {

struct MemoryRegion;  // tutti/accel/memory_region.h
class IBackendProvider;
class IAccelerator;

class IIoEngine {
public:
    virtual ~IIoEngine() = default;

    //==========================================================================
    // Batch Submit (Blocking)
    //
    // Submit one uniform-direction batch and block until complete.
    // Internally:
    //   1. Fan out: IoRequest → SubSliceInfo[] (using max_io_size from backend)
    //   2. Backend: prepare_descriptors(ioaddrs, slices) → BufferDescriptor[]
    //   3. HAL: memcpy_async(descs CPU→GPU, stream)
    //   4. Backend: launch_batch_gpu_stream(stream, target_handle, descs, n, is_read)
    //   5. HAL: synchronize_stream(stream)
    //==========================================================================
    
    virtual bool submit_batch(
        const std::vector<IoRequest>& requests,  // Backend-neutral
        bool is_read,
        AccelStream stream) = 0;

    //==========================================================================
    // Batch Submit (Async)
    //
    // Returns after kernel launch is queued on stream.
    // Caller responsible for stream-sync or event observation.
    // Kernel-side failures surface on caller's eventual stream-sync.
    //==========================================================================
    
    virtual bool submit_batch_async(
        const std::vector<IoRequest>& requests,
        bool is_read,
        AccelStream stream) = 0;

    //==========================================================================
    // Capacity / Planning
    //==========================================================================
    
    // Maximum entries one batch may flatten to after fan-out.
    // Callers must pack under this limit.
    virtual uint32_t max_entries_per_batch() const = 0;
    
    // How many sub-IOs a given MemoryRegion flattens to after fan-out.
    // Adapters use this to pack batches without exceeding max_entries_per_batch.
    // NEW METHOD (was buried in IMemorySubsystem::lookup_io_slice).
    virtual uint32_t slice_fanout(const MemoryRegion* region) const = 0;
};

} // namespace tutti
```

---

## New Input Type: IoRequest

```cpp
// tutti/types/io_types.h
#pragma once
#include <cstdint>

namespace tutti {

struct MemoryRegion;  // forward-decl

// Backend-neutral IO request (replaces NvmeBatchInputTensor).
struct IoRequest {
    MemoryRegion* region;         // Registered source/destination buffer
    void*         target_handle;  // Opaque, produced by IBackendProvider::acquire_target_handle
    uint64_t      byte_offset;    // Offset into the logical target (file offset or LBA offset)
    uint64_t      byte_length;    // Bytes to transfer
};

// Describes one transport-sized sub-IO within a fanned-out batch.
// IO Engine produces these when splitting a large IoRequest.
struct SubSliceInfo {
    uint64_t region_byte_offset;  // Offset within MemoryRegion
    uint64_t byte_length;         // Sub-IO size (≤ backend's max_io_size)
    uint32_t ioaddr_index;        // Index into MemoryRegion::dma_ioaddrs[]
};

} // namespace tutti
```

**Key differences from `NvmeBatchInputTensor`**:
- No `NvmeFile*` — uses opaque `target_handle` instead
- No redundant `data_ptr` — `MemoryRegion` already contains both host_ptr and device_ptr
- Works for NVMe files, NVMe raw, RDMA, GDS — completely backend-neutral

---

## IO Engine Orchestration (Fan-Out Logic)

The IO Engine owns the **tensor→sub-IO fan-out** that currently lives in `IMemorySubsystem::register_tensor()`.

### Current v0.1 Flow

```cpp
// memory/src/host_device_memory_subsystem.cpp
MemoryRegion* HostDeviceMemorySubsystem::register_tensor(...) {
    // 1. Allocate or wrap buffer
    // 2. DMA-map → ioaddrs
    // 3. Compute IO-slice fan-out (tensor_size / max_io_size)
    // 4. Store slices on MemoryRegion
}
```

**Problem**: HAL shouldn't know about `max_io_size` (backend concern) or slice tables (IO Engine concern).

### New Design Flow

```cpp
// io_engine/src/io_engine_impl.cpp
bool IoEngineImpl::submit_batch(const std::vector<IoRequest>& requests, ...) {
    size_t max_io = backend_->max_io_size();
    
    std::vector<SubSliceInfo> slices;
    for (const IoRequest& req : requests) {
        // Fan out this request into transport-sized sub-IOs
        for (size_t off = 0; off < req.byte_length; off += max_io) {
            SubSliceInfo slice = {
                .region_byte_offset = req.byte_offset + off,
                .byte_length = std::min(max_io, req.byte_length - off),
                .ioaddr_index = compute_ioaddr_index(req.region, off)
            };
            slices.push_back(slice);
        }
    }
    
    // Now call backend to build descriptors
    std::vector<BufferDescriptor> descs(slices.size());
    const uint64_t* ioaddrs = req.region->dma_ioaddrs;  // from HAL dma_map
    backend_->prepare_descriptors(ioaddrs, slices.data(), slices.size(), descs.data());
    
    // Stage descriptors CPU→GPU
    accel_->memcpy_async(d_descs_, descs.data(), descs.size() * sizeof(BufferDescriptor), stream);
    
    // Launch backend kernel
    backend_->launch_batch_gpu_stream(stream, req.target_handle, d_descs_, descs.size(), is_read);
    
    // Synchronize
    accel_->synchronize_stream(stream);
    return true;
}
```

---

## New Method: `slice_fanout(MemoryRegion*)`

**Purpose**: Let adapters query how many sub-IOs a region will produce **before** submitting, so they can pack batches correctly.

**Current v0.1**: This data exists but is buried in `MemoryRegion` private state, accessible only via `IMemorySubsystem::lookup_io_slice()`.

**New design**: IO Engine exposes a simple query:

```cpp
uint32_t IoEngineImpl::slice_fanout(const MemoryRegion* region) const {
    size_t max_io = backend_->max_io_size();
    return (region->size + max_io - 1) / max_io;  // ceil division
}
```

**Usage** (adapter):

```cpp
// adapters/kv_cache/src/kv_cache_io_adapter.cpp
void KvCacheIoAdapter::pack_batch(const std::vector<CacheBlock>& blocks) {
    uint32_t total_slices = 0;
    std::vector<IoRequest> batch;
    
    for (const CacheBlock& block : blocks) {
        uint32_t block_slices = io_engine_->slice_fanout(block.region);
        if (total_slices + block_slices > io_engine_->max_entries_per_batch()) {
            // Batch full — submit and start new batch
            io_engine_->submit_batch(batch, true, stream);
            batch.clear();
            total_slices = 0;
        }
        batch.push_back({block.region, block.target_handle, block.offset, block.size});
        total_slices += block_slices;
    }
    
    if (!batch.empty()) {
        io_engine_->submit_batch(batch, true, stream);
    }
}
```

---

## Dependencies: IO Engine Holds IBackendProvider + IAccelerator

**Current v0.1**: IO Engine includes NVMe-specific headers and calls into `memory/`.

**New design**: IO Engine holds:
- `IBackendProvider* backend_` — for descriptor build + launch
- `IAccelerator* accel_` — for memcpy_async, synchronize_stream
- Does **not** hold `IMemorySubsystem*` (that interface is being split)

```cpp
// io_engine/src/local_nvme_io_engine.h (revised)
class LocalNvmeIoEngine : public IIoEngine {
public:
    LocalNvmeIoEngine(IBackendProvider* backend, IAccelerator* accel);
    
    bool submit_batch(const std::vector<IoRequest>&, bool, AccelStream) override;
    bool submit_batch_async(const std::vector<IoRequest>&, bool, AccelStream) override;
    uint32_t max_entries_per_batch() const override;
    uint32_t slice_fanout(const MemoryRegion*) const override;

private:
    IBackendProvider* backend_;  // not owned
    IAccelerator* accel_;        // not owned
    
    BufferDescriptor* d_descs_;  // GPU-resident scratch for descriptors
    uint32_t max_batch_entries_;
};
```

---

## CUDA Leak Fixes

### File: `io_engine/include/io_engine.h`

**Before** (line 30):
```cpp
#include <cuda_runtime.h>
#include "local_nvme/nvme_batch.h"
```

**After**:
```cpp
#include "tutti/accel/accel_types.h"
#include "tutti/types/io_types.h"
```

**Before** (line 55):
```cpp
virtual bool submit_batch(
    const std::vector<NvmeBatchInputTensor>& inputs,
    bool is_read,
    cudaStream_t stream) = 0;
```

**After**:
```cpp
virtual bool submit_batch(
    const std::vector<IoRequest>& requests,
    bool is_read,
    AccelStream stream) = 0;
```

### File: `io_engine/include/local_nvme/launch_batch.h`

**Before**:
```cpp
#include <cuda_runtime.h>

cudaError_t launch_nvme_batch_xfer(..., cudaStream_t stream);
```

**After**:
```cpp
#include "tutti/accel/accel_types.h"

bool launch_nvme_batch_xfer(..., AccelStream stream);  // return bool, not cudaError_t
```

---

## Migration Checklist

### Files to Modify

```
io_engine/include/io_engine.h
  - Remove #include <cuda_runtime.h>
  - Remove #include "local_nvme/nvme_batch.h"
  - Add #include "tutti/accel/accel_types.h"
  - Add #include "tutti/types/io_types.h"
  - Change NvmeBatchInputTensor → IoRequest
  - Change cudaStream_t → AccelStream
  - Add slice_fanout(MemoryRegion*) method

io_engine/include/local_nvme/local_nvme_io_engine.h
  - Change cudaStream_t → AccelStream in overrides
  - Hold IBackendProvider* + IAccelerator* (not IMemorySubsystem*)

io_engine/include/local_nvme/launch_batch.h
  - Change cudaStream_t → AccelStream
  - Change cudaError_t → bool

io_engine/src/*.cpp
  - Update to use IoRequest
  - Fan-out logic moves from memory/ to here
```

### Files to Create

```
tutti/types/io_types.h  (IoRequest + SubSliceInfo)
```

### Coordinator Changes

```cpp
// coordinator/src/coordinator.cpp

// Before:
#include "io_engine/include/local_nvme/nvme_batch.h"
std::vector<NvmeBatchInputTensor> inputs;
inputs.push_back({data_ptr, size, nvme_file, offset, region});
io_engine_->submit_batch(inputs, true, stream);

// After:
#include "tutti/types/io_types.h"
void* target_handle = backend_->acquire_target_handle(storage_target);
std::vector<IoRequest> requests;
requests.push_back({region, target_handle, offset, size});
io_engine_->submit_batch(requests, true, stream);
backend_->release_target_handle(target_handle);
```

---

## Validation Criteria

✅ IO Engine is correct when:

1. **No cuda_runtime.h in public headers** — only `AccelStream` opaque type
2. **No NVMe specifics in IIoEngine** — no `NvmeFile*`, no `nvme_batch.h`
3. **IoRequest works for all backends** — NVMe file, NVMe raw, future RDMA/GDS
4. **Fan-out logic is in IO Engine** — not in HAL, not in backend
5. **slice_fanout() works** — adapters can query before packing
6. **Backend-neutral dependencies** — only `IBackendProvider*` + `IAccelerator*`

---

## Example: Before and After

### Before (v0.1)

```cpp
// Coordinator code
NvmeFile* file = nvme_storage_->open_file(...);
MemoryRegion* region = memory_->register_tensor(...);  // fan-out happens here

NvmeBatchInputTensor input = {
    .data_ptr = region->device_ptr,
    .tensor_size = size,
    .file = file,  // ← NVMe-specific
    .file_byte_offset = 0,
    .region = region
};

io_engine_->submit_batch({input}, true, stream);
```

### After (new design)

```cpp
// Coordinator code
GpuFile* gpu_file = block_storage_->open_gpu_file(...);
void* gpu_file_handle = block_storage_->acquire_device_handle(gpu_file, stream);

MemoryRegion* region = accel_->register_device(device_ptr, size, device_id);
accel_->dma_map(region, device_id, &ioaddrs, &count);

// Backend produces target_handle from StorageTarget
StorageTarget target = block_storage_->get_storage_target(gpu_file, shard_idx);
void* target_handle = backend_->acquire_target_handle(target);

IoRequest request = {
    .region = region,
    .target_handle = target_handle,  // ← backend-neutral
    .byte_offset = 0,
    .byte_length = size
};

io_engine_->submit_batch({request}, true, stream);

backend_->release_target_handle(target_handle);
block_storage_->release_device_handle(gpu_file_handle, stream);
```

---

## Open Questions

See `10-open-questions.md` for:
- **Q10**: Should IO-slice tables be cached on MemoryRegion or recomputed per-batch?  
  **Current decision**: Recompute per-batch (simpler, no stale state).
- **Q11**: Should `slice_fanout()` account for non-contiguous ioaddrs?  
  **Current decision**: No for v0.1 (assumes contiguous allocation). Revisit for RDMA scatter-gather.
