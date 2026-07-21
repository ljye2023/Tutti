# Implementation Sequence

> Recommended build order for the new directory, targeting 5.15 kernel + CUDA.

## Principles

1. **Bottom-up** — foundation layers before consumers
2. **Shared types first** — value types before interfaces
3. **Validate early** — unit test each layer before moving up
4. **No big-bang** — incremental cutover from v0.1 code

---

## Phase 0: Foundations (No Dependencies)

These have zero dependencies and can be built in parallel:

```
tutti/abstraction/accel.h              — Macros (TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, ...)
tutti/accel/accel_types.h              — AccelStream, AccelEvent, IpcHandle (type aliases)
tutti/types/storage_target.h           — StorageTarget enum + union
tutti/types/io_types.h                 — IoRequest, SubSliceInfo
```

**Validation**:
- ✅ Headers compile in isolation (host-only and nvcc builds)
- ✅ No `#include` cycles
- ✅ `sizeof(StorageTarget)` reasonable (≤ 128 bytes)

**Estimated effort**: 1 day (mostly design verification, code is straightforward)

---

## Phase 1: Accelerator HAL

The HAL sits on top of Phase 0 types and is consumed by all higher layers.

### 1.1 Interface + Supporting Types

```
tutti/accel/memory_kind.h              — MemoryKind enum (HOST, PINNED_HOST, DEVICE, MANAGED, EXTERNAL)
tutti/accel/memory_region.h            — MemoryRegion struct + ExternalMemorySpec
tutti/accel/iaccel.h                   — IAccelerator interface
```

**Validation**:
- ✅ Headers compile in isolation
- ✅ `MemoryRegion` layout matches current `memory/include/memory_region.h` (for migration)

### 1.2 Concrete CUDA Implementation

```
tutti/accel/cuda/cuda_accelerator.h    — CudaAccelerator class
tutti/accel/cuda/cuda_accelerator.cu   — Implementation (alloc, dma_map, stream, launch, ...)
```

**Key methods**:
- `allocate_host(PINNED_HOST)` → `cudaHostAlloc`
- `allocate_device(DEVICE)` → `cudaMalloc`
- `dma_map` → `nvm_dma_map_data_device` (libnvm wrapper, but HAL-owned)
- `device_pointer_for` → `cudaHostGetDevicePointer`
- `create_stream` → `cudaStreamCreate`
- `memcpy_async` → `cudaMemcpyAsync`
- `launch` → `cudaLaunchKernel` (runtime API, not `<<<>>>`)

**Validation**:
- ✅ Unit test: allocate → register → dma_map → lookup → unregister → free
- ✅ Unit test: stream create → memcpy_async → event → sync
- ✅ Unit test: kernel launch via `IAccelerator::launch()` (not `<<<>>>`)
- ✅ No `cuda_runtime.h` in `iaccel.h` (only in `cuda_accelerator.cu`)

**Estimated effort**: 3-4 days

---

## Phase 2: Device Manager

DM provides queue virtualization and device-side helpers. Depends on HAL (for `cudaMalloc` in `NvmeQueueGroup`).

### 2.1 VDevice + IVirtualNvme

```
device_manager/include/vdevice.h                   — VDevice struct
device_manager/include/virtual_nvme.h              — IVirtualNvme interface
device_manager/src/local_nvme_virtual.h            — LocalNvmeVirtualRegistry impl
device_manager/src/local_nvme_virtual.cpp          — Level-2 allocator (contiguous-first-fit)
```

**Validation**:
- ✅ Unit test: open_vdevice(quota=8) → VDevice{d_qps=..., queue_quota=8}
- ✅ Unit test: close_vdevice → QPs returned to pool
- ✅ Unit test: exhaust pool → open_vdevice returns nullptr
- ✅ `VDevice::d_qps` points into `NvmeQueueGroup::d_qps_[]` (address validation)

### 2.2 Move Queue Helpers

```
device_manager/include/queue_acquire_helper.cuh    — MOVED FROM nvme_storage/
device_manager/src/queue_acquire_helper_impl.cuh   — Inline __device__ implementations
```

**Update references**:
- `nvme_storage/include/nvme_storage_device.cuh:8` — change include path

**Validation**:
- ✅ Compile nvme_storage/ after move (ensure no broken includes)
- ✅ Device-side test: `acquire_queue(d_qps, 8)` returns valid qid ∈ [0,7]

### 2.3 Existing Components (No Changes)

```
device_manager/include/device_registry.h           — IDeviceRegistry (unchanged)
device_manager/include/local_nvme_device.h         — LocalNvmeDevice (unchanged)
device_manager/include/nvme_queue_group.h          — NvmeQueueGroup (unchanged)
device_manager/include/lease_manager.h             — ILeaseManager (unchanged)
```

**Estimated effort**: 2-3 days

---

## Phase 3: Backends SPI + NVMe Backend

Backends consume VDevice from DM and present the SPI to IO Engine.

### 3.1 Update IBackendProvider SPI

```
io_engine/include/backend_provider.h               — MODIFY
  - Remove: #include <cuda_runtime.h>
  - Add: #include "tutti/accel/accel_types.h"
  - Add: #include "tutti/types/storage_target.h"
  - Change: initialize() → initialize(VDevice* vdev)
  - Change: cudaStream_t → AccelStream
  - Add: acquire_target_handle(StorageTarget) / release_target_handle(void*)
  - Remove: acquire_queue() / release_queue()
```

**Validation**:
- ✅ Header compiles without `cuda_runtime.h`
- ✅ No breaking changes to method signatures consumers don't use yet

### 3.2 NVMe Backend Implementation

Merge `nvme_storage/` → `backends/local_nvme/`:

```
backends/local_nvme/include/local_nvme_backend.h   — Concrete IBackendProvider
backends/local_nvme/include/nvme_backend_device.cuh — Backend-private __device__ helpers
backends/local_nvme/src/local_nvme_backend.cpp     — initialize(VDevice*), acquire_target_handle, ...
backends/local_nvme/src/prp_builder.cu             — PRP/SGL descriptor build (MOVED from memory/)
backends/local_nvme/src/prp_page_cache.cu          — PRP-page cache (MOVED from memory/)
backends/local_nvme/src/submit_kernel.cu           — GPU-stream batch submit kernel
```

**Key changes**:
- `initialize(VDevice* vdev)` — store `vdev_` member
- `acquire_target_handle(StorageTarget)` — build `NvmeFileDeviceHandle` or `NvmeRawDeviceHandle`
- `launch_batch_gpu_stream(AccelStream stream, ...)` — cast to `cudaStream_t` inside `.cu` file
- Device kernel: `acquire_queue(vdev_->d_qps, vdev_->queue_quota)` → use DM helpers

**Validation**:
- ✅ Unit test: initialize(vdev) succeeds, vdev_ != nullptr
- ✅ Unit test: acquire_target_handle(StorageTarget{NVME_FILE}) → non-null handle
- ✅ Unit test: prepare_descriptors → BufferDescriptor[] with valid PRP addresses
- ✅ Integration test: launch kernel on test stream, verify doorbell ring (via libnvm trace)

**Estimated effort**: 5-6 days (includes PRP/SGL logic migration from memory/)

---

## Phase 4: IO Engine

IO Engine orchestrates fan-out and delegates to backends.

### 4.1 Update IIoEngine

```
io_engine/include/io_engine.h                      — MODIFY
  - Remove: #include <cuda_runtime.h>
  - Remove: #include "local_nvme/nvme_batch.h"
  - Add: #include "tutti/accel/accel_types.h"
  - Add: #include "tutti/types/io_types.h"
  - Change: NvmeBatchInputTensor → IoRequest
  - Change: cudaStream_t → AccelStream
  - Add: slice_fanout(MemoryRegion*) method
```

### 4.2 IO Engine Implementation

```
io_engine/src/io_engine_impl.h                     — Concrete IIoEngine
io_engine/src/io_engine_impl.cpp                   — submit_batch fan-out logic (MOVED from memory/)
io_engine/include/local_nvme/launch_batch.h        — MODIFY: cudaStream_t → AccelStream
```

**Key logic** (moved from `IMemorySubsystem::register_tensor`):

```cpp
bool IoEngineImpl::submit_batch(const std::vector<IoRequest>& requests, bool is_read, AccelStream stream) {
    size_t max_io = backend_->max_io_size();
    
    std::vector<SubSliceInfo> slices;
    for (const IoRequest& req : requests) {
        for (size_t off = 0; off < req.byte_length; off += max_io) {
            slices.push_back({
                .region_byte_offset = req.byte_offset + off,
                .byte_length = std::min(max_io, req.byte_length - off),
                .ioaddr_index = (req.byte_offset + off) / PAGE_SIZE
            });
        }
    }
    
    std::vector<BufferDescriptor> descs(slices.size());
    backend_->prepare_descriptors(req.region->dma_ioaddrs, slices.data(), slices.size(), descs.data());
    
    accel_->memcpy_async(d_descs_, descs.data(), descs.size() * sizeof(BufferDescriptor), stream);
    backend_->launch_batch_gpu_stream(stream, req.target_handle, d_descs_, descs.size(), is_read);
    accel_->synchronize_stream(stream);
    
    return true;
}
```

**Validation**:
- ✅ Unit test: `slice_fanout(region{size=1GiB})` with `max_io=128KiB` → 8192
- ✅ Integration test: submit_batch({IoRequest}) → kernel launches → completes
- ✅ No NVMe-specific headers in `io_engine/include/` (only SPI)

**Estimated effort**: 3-4 days

---

## Phase 5: Top Interfaces (Block Storage + Raw Device)

### 5.1 Update IBlockStorage

```
block_storage/include/block_storage.h              — MODIFY
  - Change: using GpuStreamHandle = void*;
  - To: using AccelStream = tutti::AccelStream;
```

**Validation**:
- ✅ Compile check (minimal change, mostly type alias rename)

### 5.2 Create IRawDevice

```
raw_device/include/raw_device.h                    — NEW: IRawDevice interface
raw_device/src/raw_device_impl.h                   — NEW: Concrete implementation
raw_device/src/raw_device_impl.cpp                 — NEW
raw_device/src/raw_target_handle_impl.h            — NEW: RawTargetHandle internals
```

**Key methods**:
- `acquire_raw_target(device, ns_id, extents, stream)` → build `StorageTarget{NVME_RAW}` → call `backend_->acquire_target_handle()`
- `release_raw_target(handle, stream)` → call `backend_->release_target_handle()`

**Validation**:
- ✅ Unit test: acquire_raw_target → non-null handle
- ✅ Integration test: submit_batch with raw target → kernel completes
- ✅ Verify raw and file paths produce identical `IoRequest` to IO Engine

**Estimated effort**: 2-3 days

---

## Phase 6: Coordinator Integration

Wire all layers together:

```
coordinator/include/coordinator.h                  — MODIFY
  - Add: #include "raw_device/include/raw_device.h"
  - Add: std::unique_ptr<IRawDevice> raw_device_;
  - Change: cudaStream_t → AccelStream throughout
  - Remove: #include "io_engine/include/local_nvme/nvme_batch.h"
  
coordinator/src/coordinator.cpp                    — MODIFY
  - Replace: NvmeBatchInputTensor → IoRequest
  - Add: VDevice allocation via IVirtualNvme
  - Add: backend_->initialize(vdev) at bootstrap
```

**Bootstrap sequence**:

```cpp
bool Coordinator::bootstrap() {
    // 1. Device registry
    device_registry_->Open();
    
    // 2. Level-2 allocator
    virtual_nvme_ = std::make_unique<LocalNvmeVirtualRegistry>(device_registry_.get());
    
    // 3. Allocate vDevice for backend
    VDevice* vdev = virtual_nvme_->open_vdevice(/*phys_id=*/0, /*quota=*/8);
    
    // 4. Initialize backend with vDevice
    backend_->initialize(vdev);
    
    // 5. Wire IO Engine
    io_engine_ = std::make_unique<LocalNvmeIoEngine>(backend_.get(), accel_.get());
    
    // 6. Top interfaces
    block_storage_->bootstrap(devices_);
    raw_device_->bootstrap(devices_);
    
    return true;
}
```

**Validation**:
- ✅ End-to-end test: bootstrap → open_gpu_file → register_device → submit_batch → verify data
- ✅ End-to-end test: bootstrap → acquire_raw_target → register_device → submit_batch → verify data
- ✅ No cuda_runtime.h in `coordinator.h`

**Estimated effort**: 2-3 days

---

## Phase 7: Cutover + Validation

### 7.1 Existing Tests

Run all existing tests against the new stack:

```bash
cd build
ctest --output-on-failure
```

**Expected**:
- ✅ `block_storage_smoke` passes (uses IBlockStorage, unchanged API)
- ✅ `block_storage_gpu_smoke` passes (GPU kernel submit)
- ⚠️ Any test using `NvmeBatchInputTensor` directly → needs update to `IoRequest`

### 7.2 New Tests

Add tests for new components:

```
tests/vdevice_test.cpp                — IVirtualNvme allocation / exhaustion
tests/raw_device_test.cpp             — IRawDevice acquire / release
tests/storage_target_test.cpp         — StorageTarget union size / layout
tests/accel_stream_test.cpp           — AccelStream opaque cast roundtrip
```

### 7.3 Performance Validation

**No regression on v0.1 workloads**:

| Metric | v0.1 Baseline | New Stack Target |
|---|---|---|
| Single 1 GiB GPU-stream read | ~X ms | ≤ X + 5% ms |
| Batch of 100×10 MiB reads | ~Y ms | ≤ Y + 5% ms |
| Queue acquire latency (device-side) | ~Z ns | ≤ Z + 10% ns |

**Estimated effort**: 3-4 days

---

## Total Estimated Timeline

| Phase | Effort | Dependencies |
|---|---|---|
| Phase 0: Foundations | 1 day | None |
| Phase 1: Accelerator HAL | 3-4 days | Phase 0 |
| Phase 2: Device Manager | 2-3 days | Phase 1 |
| Phase 3: Backends | 5-6 days | Phase 0, 1, 2 |
| Phase 4: IO Engine | 3-4 days | Phase 1, 3 |
| Phase 5: Top Interfaces | 2-3 days | Phase 3, 4 |
| Phase 6: Coordinator | 2-3 days | All prior |
| Phase 7: Cutover + Validation | 3-4 days | All prior |

**Total**: ~21-27 days (4-5.5 weeks) for one engineer, assuming no blockers.

**Parallelization**: Phases 0-2 can be partially parallelized if multiple engineers available.

---

## Risk Mitigation

### Risk: PRP/SGL Migration from memory/ to backends/

**Mitigation**: Keep old `memory/` code intact initially, copy logic to backend, run both in parallel with comparison checks, then delete old code once validated.

### Risk: ioaddr Index Arithmetic Mismatch

**Mitigation**: Add debug assertions in `SubSliceInfo` generation:
```cpp
assert(slice.ioaddr_index < region->dma_ioaddr_count);
assert(slice.region_byte_offset % PAGE_SIZE == 0);  // alignment
```

### Risk: VDevice d_qps Pointer Arithmetic Wrong

**Mitigation**: Unit test that verifies `vdev->d_qps + i` is within `[NvmeQueueGroup::d_qps_, d_qps_ + n_qps_)`.

### Risk: AccelStream Cast Breaks on Non-CUDA Platforms

**Mitigation**: For v0.1 (CUDA-only), this is not a concern. For future ROCm/SYCL, add runtime vendor check before cast.

---

## Rollback Plan

If Phase 6 integration uncovers a fundamental design flaw:

1. **Phase 0-5 are self-contained** — they don't touch existing v0.1 code
2. Coordinator can temporarily hold **both** old `IMemorySubsystem` and new `IAccelerator`
3. Gradually migrate call sites one at a time
4. If blocked, pause at Phase 5 and reassess design decisions in `10-open-questions.md`

**Key**: The new stack is built **alongside** v0.1, not **in place of** it, until Phase 6 cutover.
