# Layer 1: Accelerator HAL

**Version:** 1.0  
**Date:** 2026-07-22  
**Status:** Implemented (CUDA)  
**Library:** `libtutti_accel`  
**Location:** `tutti/accel/`

---

## Overview

Layer 1 provides a **runtime hardware abstraction layer (HAL)** for GPU/accelerator operations. Where Layer 0 abstracts device *code* at compile time, Layer 1 abstracts device *runtime services* — memory allocation, streams, events, transfers, kernel dispatch, and IPC — behind a vendor-neutral C++ interface (`IAccelerator`).

**Key Properties:**
- Runtime abstraction via virtual interface (`IAccelerator`)
- Vendor implementation swappable at construction (currently `CudaAccelerator`)
- No vendor types leak into the public interface (opaque handles)
- Central memory registry with backend-extensible metadata
- Thread-safe (all public methods mutex-protected)
- 64KB-aligned device allocations (NVMe DMA requirement)

**Design Philosophy:**  
Higher layers (device_manager, backends, io_engine) must never call CUDA directly. They program against `IAccelerator`, so porting to ROCm/SYCL means providing a new implementation, not rewriting callers.

---

## Primary Functionality

Layer 1 groups its services into eight functional areas:

| Area | Responsibility |
|------|----------------|
| **Identity** | Enumerate and select accelerator devices |
| **Memory Allocation** | Allocate/free host, device, pinned, and managed memory |
| **Memory Registry** | Track memory regions with unified metadata |
| **Pointer Translation** | Map host pointers to device-visible addresses |
| **Stream Lifecycle** | Create/destroy/synchronize async execution queues |
| **Event Lifecycle** | Create/record/wait/query synchronization primitives |
| **Transfer** | Asynchronous memory copy (H↔D, D↔D) |
| **Kernel Launch** | Dispatch kernels via function pointer |
| **IPC** | Export/import memory for cross-process sharing |

The central design element is the **`MemoryRegion` registry**: every piece of memory the system uses is tracked in one place, with a stable `region_id` and an opaque `backend_private` slot where Layer 3+ backends attach DMA ioaddrs, RDMA keys, etc. — without requiring changes to the HAL.

---

## Core Types

### `AccelStream` — Opaque Stream Handle
**File:** `include/common/accel_types.h`

Wraps a vendor stream (`cudaStream_t`) as an opaque `void*`.

| Member | Type | Purpose |
|--------|------|---------|
| `handle` | `void*` | Underlying stream pointer |
| `is_valid()` | `bool` | True if handle is non-null |
| `operator==` / `operator!=` | `bool` | Handle comparison |

### `AccelEvent` — Opaque Event Handle
**File:** `include/common/accel_types.h`

Wraps a vendor event (`cudaEvent_t`) as an opaque `void*`. Same shape as `AccelStream`.

### `IpcHandle` — Cross-Process Memory Handle
**File:** `include/common/accel_types.h`

| Member | Type | Purpose |
|--------|------|---------|
| `MAX_HANDLE_SIZE` | `constexpr size_t = 64` | Matches `cudaIpcMemHandle_t` size |
| `data[64]` | `uint8_t[]` | Zero-initialized handle bytes |

### `Dim3` — Kernel Launch Dimensions
**File:** `include/common/accel_types.h`

Vendor-neutral 3D dimension (named `Dim3` to avoid conflict with CUDA's `dim3`).

| Member | Type | Default |
|--------|------|---------|
| `x, y, z` | `uint32_t` | (1, 1, 1) |

Constructors accept 1, 2, or 3 dimensions.

### `MemoryKind` — Allocation Type
**File:** `include/common/memory_kind.h`

| Value | Meaning |
|-------|---------|
| `HOST` | Pageable host RAM, not device-accessible |
| `PINNED_HOST` | Pinned host RAM, DMA-capable, device-accessible |
| `DEVICE` | GPU VRAM |
| `MANAGED` | Unified memory (auto-migrated) |
| `EXTERNAL` | Registered, not owned by the HAL |

### `MemoryAccessFlags` — Access Bits
**File:** `include/common/memory_kind.h`

| Flag | Value | Meaning |
|------|-------|---------|
| `ACCESS_NONE` | 0 | No access |
| `ACCESS_READ` | 1<<0 | Device may read |
| `ACCESS_WRITE` | 1<<1 | Device may write |
| `ACCESS_HOST_MAPPED` | 1<<2 | Device memory host-accessible |

### `MemoryRegion` — Unified Memory Handle
**File:** `include/common/memory_region.h`

The central tracking structure returned by all `register_*` calls.

| Field | Type | Purpose |
|-------|------|---------|
| `region_id` | `uint64_t` | Unique, stable across runtime |
| `kind` | `MemoryKind` | Allocation type |
| `device_id` | `int` | Owning accelerator device (-1 for host-only) |
| `host_ptr` | `void*` | Host address (nullptr if device-only) |
| `device_ptr` | `void*` | Device address (nullptr if host-only) |
| `size` | `size_t` | Region size in bytes |
| `external` | `ExternalMemorySpec*` | Set only if `kind == EXTERNAL` |
| `backend_private` | `void*` | Layer 3+ metadata (DMA ioaddrs, RDMA keys); set by `dma_map()`, not `register_*()` |

### `ExternalMemorySpec` — External Source Descriptor
**File:** `include/common/memory_region.h`

Tagged union describing memory the HAL tracks but does not own.

| `Source` | Backing | Union Payload |
|----------|---------|---------------|
| `APP_MANAGED` | Caller allocation | (empty) |
| `DEVICE_IPC` | `cudaIpcOpenMemHandle` | `uint8_t handle[64]` |
| `HOST_SHM` | `shm_open` + `mmap` | `int shm_fd` |
| `HOST_FD_MAP` | `fd` + `mmap` | `int fd; off_t offset` |

---

## IAccelerator Interface

**File:** `include/common/iaccel.h`  
The complete vendor-neutral API. 26 pure-virtual methods across 9 areas.

### Identity

| Method | Signature | Purpose |
|--------|-----------|---------|
| `vendor_name` | `const char* vendor_name() const` | Backend name (e.g. `"CUDA"`) |
| `device_count` | `int device_count() const` | Number of visible accelerators |
| `set_device` | `bool set_device(int device_id)` | Select active device |
| `get_device` | `int get_device() const` | Current active device |

### Memory Allocation

| Method | Signature | Purpose |
|--------|-----------|---------|
| `allocate_host` | `void* allocate_host(size_t size, MemoryKind kind)` | Allocate host memory (HOST / PINNED_HOST / MANAGED) |
| `allocate_device` | `void* allocate_device(size_t size, MemoryKind kind, int device_id)` | Allocate device memory, **64KB-aligned** |
| `free` | `void free(void* ptr, MemoryKind kind)` | Release allocation; `kind` selects the free path |

### Memory Registry

| Method | Signature | Purpose |
|--------|-----------|---------|
| `register_host` | `MemoryRegion* register_host(void* host_ptr, size_t size)` | Track/pin existing host memory |
| `register_device` | `MemoryRegion* register_device(void* device_ptr, size_t size, int device_id)` | Track existing device memory |
| `register_external` | `MemoryRegion* register_external(void* host_ptr, void* device_ptr, size_t size, const ExternalMemorySpec& spec)` | Track external memory (IPC / SHM / fd / app) |
| `unregister` | `void unregister(MemoryRegion* region)` | Remove from registry |
| `lookup` | `MemoryRegion* lookup(const void* ptr) const` | Find region by contained pointer |
| `lookup_by_id` | `MemoryRegion* lookup_by_id(uint64_t region_id) const` | Find region by ID |

### Pointer Translation

| Method | Signature | Purpose |
|--------|-----------|---------|
| `device_pointer_for` | `void* device_pointer_for(const void* host_ptr)` | Device-visible address of pinned host memory |

### Stream Lifecycle

| Method | Signature | Purpose |
|--------|-----------|---------|
| `create_stream` | `AccelStream create_stream()` | Create async execution queue |
| `destroy_stream` | `void destroy_stream(AccelStream stream)` | Destroy stream |
| `synchronize_stream` | `void synchronize_stream(AccelStream stream)` | Block until stream drains |

### Event Lifecycle

| Method | Signature | Purpose |
|--------|-----------|---------|
| `create_event` | `AccelEvent create_event()` | Create synchronization event |
| `destroy_event` | `void destroy_event(AccelEvent event)` | Destroy event |
| `record_event` | `void record_event(AccelEvent event, AccelStream stream)` | Record event on stream |
| `wait_event` | `void wait_event(AccelStream stream, AccelEvent event)` | Make stream wait for event |
| `query_event` | `bool query_event(AccelEvent event)` | Non-blocking completion check |

### Transfer

| Method | Signature | Purpose |
|--------|-----------|---------|
| `memcpy_async` | `bool memcpy_async(void* dst, const void* src, size_t size, AccelStream stream)` | Async copy; direction auto-detected |

### Kernel Launch

| Method | Signature | Purpose |
|--------|-----------|---------|
| `launch` | `void launch(void* kernel_func, const Dim3& grid, const Dim3& block, size_t shared_mem_bytes, AccelStream stream, void** kernel_args)` | Dispatch kernel by function pointer |

### IPC

| Method | Signature | Purpose |
|--------|-----------|---------|
| `ipc_export` | `bool ipc_export(MemoryRegion* region, IpcHandle* out_handle)` | Export device memory as IPC handle |
| `ipc_import` | `MemoryRegion* ipc_import(const IpcHandle& handle, int device_id)` | Import IPC handle as EXTERNAL region |

---

## CUDA Implementation (`CudaAccelerator`)

**File:** `include/cuda/cuda_accelerator.h`, `src/cuda/cuda_accelerator.cu`

The only implementation for v0.1. Maps each `IAccelerator` method to CUDA runtime calls.

### Internal State

```cpp
// Registry (protected by registry_mutex_)
std::unordered_map<uint64_t, RegionEntry> regions_by_id_;   // region_id -> MemoryRegion
std::unordered_map<uintptr_t, uint64_t>   ptr_to_region_id_;// ptr -> region_id
uint64_t next_region_id_;

// Device allocation tracking (protected by alloc_mutex_)
std::unordered_map<uintptr_t, void*> aligned_to_raw_;       // aligned_ptr -> cudaMalloc ptr
```

### Key Mechanism: 64KB Alignment

`allocate_device(DEVICE)` over-allocates by 64KB and returns an aligned sub-pointer. Because `cudaFree()` requires the exact pointer `cudaMalloc()` returned, `free()` recovers the raw pointer from `aligned_to_raw_`.

```cpp
void* CudaAccelerator::allocate_device(size_t size, MemoryKind kind, int device_id) {
    cudaSetDevice(device_id);
    void* raw;
    cudaMalloc(&raw, size + 65536);                  // over-allocate
    uintptr_t aligned = ((uintptr_t)raw + 65535) & ~65535ULL;
    aligned_to_raw_[aligned] = raw;                  // remember for free()
    return (void*)aligned;
}

void CudaAccelerator::free(void* ptr, MemoryKind kind) {
    if (kind == MemoryKind::DEVICE) {
        cudaFree(aligned_to_raw_[(uintptr_t)ptr]);   // free the raw allocation
        aligned_to_raw_.erase((uintptr_t)ptr);
    }
    // ... HOST -> free(), PINNED_HOST -> cudaFreeHost(), MANAGED -> cudaFree()
}
```

**Why 64KB:** NVMe DMA and PCIe TLP boundaries require 64KB-aligned buffers. This alignment is enforced at Layer 1 so higher layers get DMA-ready memory transparently.

### API → CUDA Summary

| Area | CUDA calls |
|------|-----------|
| Identity | `cudaGetDeviceCount`, `cudaSetDevice`, `cudaGetDevice` |
| Host alloc | `malloc` / `cudaMallocHost` / `cudaMallocManaged` |
| Device alloc | `cudaMalloc` (+ alignment) |
| Free | `free` / `cudaFreeHost` / `cudaFree` |
| Register host | `cudaHostRegister` / `cudaHostUnregister` |
| Pointer xlate | `cudaHostGetDevicePointer` |
| Streams | `cudaStreamCreate/Destroy/Synchronize` |
| Events | `cudaEventCreate/Destroy/Record/Query`, `cudaStreamWaitEvent` |
| Transfer | `cudaMemcpyAsync` (direction via `cudaPointerGetAttributes`) |
| Launch | `cudaLaunchKernel` |
| IPC | `cudaIpcGetMemHandle` / `cudaIpcOpenMemHandle` / `cudaIpcCloseMemHandle` |

> A full field-by-field mapping is in `doc/layered/layer1-cuda-mapping.md` (see project root `layer1-cuda-mapping.md`).

### Thread Safety

All public methods are thread-safe. The registry and allocation maps are shared mutable state, protected by `registry_mutex_` and `alloc_mutex_` respectively. CUDA runtime calls are themselves thread-safe per-device; Layer 1 adds protection for its own bookkeeping.

---

## Design Decisions

### Decision 1: Opaque `void*` Handles

**Choice:** `AccelStream` / `AccelEvent` wrap `void*`, not `cudaStream_t`.

**Rationale:** Keeps `cuda_runtime.h` out of the public interface. Higher layers include `iaccel.h` without pulling in CUDA headers, enabling host-only compilation of callers and future non-CUDA backends.

---

### Decision 2: Central `MemoryRegion` Registry

**Choice:** Every allocation/registration produces a tracked `MemoryRegion` with a stable `region_id`.

**Rationale:**
- CUDA has no unified allocation tracking; each layer would otherwise reinvent it.
- The `backend_private` slot lets Layer 3+ attach DMA ioaddrs and RDMA keys without changing the HAL interface.
- `lookup(ptr)` / `lookup_by_id()` enable reverse mapping needed by io_engine and backends.

---

### Decision 3: 64KB Alignment at the HAL

**Choice:** Enforce 64KB device alignment inside `allocate_device`, not in callers.

**Rationale:** NVMe/PCIe DMA alignment is a hardware constraint every backend needs. Centralizing it means higher layers never have to think about alignment, and there's exactly one place to audit for correctness (with a leak-regression test guarding the raw-pointer bookkeeping).

---

### Decision 4: `MemoryKind` on `free()`

**Choice:** `free(ptr, kind)` takes the kind explicitly rather than looking it up.

**Rationale:** The free path differs per kind (`free` / `cudaFreeHost` / `cudaFree`). Passing `kind` avoids a registry lookup on the hot path and keeps `free()` usable for raw allocations that were never registered.

---

### Decision 5: Explicit External Memory Sources

**Choice:** `ExternalMemorySpec` enumerates four concrete sources (APP_MANAGED, DEVICE_IPC, HOST_SHM, HOST_FD_MAP).

**Rationale:** These are the real ways memory enters the system from outside the HAL. Making them explicit lets `unregister()` do the right teardown (e.g. `cudaIpcCloseMemHandle` for IPC, `munmap` for mappings) and documents supported integration paths.

---

## Usage Example

```cpp
#include "iaccel.h"
#include "cuda_accelerator.h"
using namespace tutti;

IAccelerator* accel = new CudaAccelerator();

// Allocate DMA-ready pinned host + device buffers
const size_t bytes = 1 << 20;
void* h = accel->allocate_host(bytes, MemoryKind::PINNED_HOST);
void* d = accel->allocate_device(bytes, MemoryKind::DEVICE, 0);  // 64KB-aligned

// Track them
MemoryRegion* hr = accel->register_host(h, bytes);
MemoryRegion* dr = accel->register_device(d, bytes, 0);

// Async H->D copy on a stream
AccelStream s = accel->create_stream();
accel->memcpy_async(d, h, bytes, s);
accel->synchronize_stream(s);

// Cleanup
accel->destroy_stream(s);
accel->unregister(hr);
accel->unregister(dr);
accel->free(h, MemoryKind::PINNED_HOST);
accel->free(d, MemoryKind::DEVICE);
delete accel;
```

---

## Testing

**File:** `tests/layer1_smoke_test.cu` — 11 tests

| # | Test | Covers |
|---|------|--------|
| 1 | `test_device_management` | device_count, get/set_device |
| 2 | `test_host_memory` | HOST alloc/free + write |
| 3 | `test_device_memory` | DEVICE alloc/free |
| 4 | `test_pinned_memory` | PINNED_HOST alloc/free + write |
| 5 | `test_memory_registration` | register_host, lookup, lookup_by_id, unregister |
| 6 | `test_stream_lifecycle` | create/sync/destroy stream + memcpy |
| 7 | `test_event_lifecycle` | create/record/query/wait/destroy event |
| 8 | `test_memcpy_async` | H→D, D→H with verification |
| 9 | `test_memcpy_device_to_device` | D→D with verification |
| 10 | `test_device_memory_alignment_and_leak` | 64KB alignment + 200-iteration leak regression |
| 11 | `test_kernel_launch` | `launch()` with a vector-add kernel |

### Coverage Gaps

Not yet tested:
- `MemoryKind::MANAGED` and `MemoryKind::EXTERNAL` allocation paths
- `register_device()` and `register_external()` (all four sources)
- `device_pointer_for()`
- `ipc_export()` / `ipc_import()` (cross-process sharing)
- `wait_event()` cross-stream ordering (called but not asserted)
- Error handling (invalid device, null ptr, double free, unregister unknown)
- Concurrency (thread-safety is claimed but not exercised)
- Multi-GPU (only device 0 tested)

**Recommendation priority:** IPC and external-memory tests first (core to multi-process use cases), then error handling and thread-safety, then multi-GPU.

---

## Dependencies

### Layer 1 Depends On:
- **Layer 0** (`tutti/abstraction/accel.h`) — for `TUTTI_GLOBAL` test kernels
- **CUDA Toolkit** (`CUDA::cudart`)
- `tutti_types` (interface library, include paths)

### Layers That Depend on Layer 1:
- **Layer 2** (device_manager) — uses `IAccelerator` for memory and streams
- **Layer 3** (backends) — attaches DMA metadata via `backend_private`
- **Layer 4** (io_engine) — streams/events for async IO
- **Layer 5** (block_storage) — device buffers for striping

---

## Known Limitations

1. **CUDA-only** — ROCm/SYCL implementations not yet provided (`IAccelerator` is designed to accept them).
2. **No `memcpy` H↔H fast path** — `memcpy_async` handles it via `cudaMemcpyDefault` but a plain `memcpy` would be faster for host-to-host.
3. **`launch()` uses `cudaLaunchKernel`** — requires kernel function pointers; template kernels need explicit instantiation.
4. **No async allocation** — allocations are synchronous; no `cudaMallocAsync`/stream-ordered allocator yet.

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-07-22 | Initial CUDA implementation, 11 smoke tests |

---

## References

- **Interface:** `tutti/accel/include/common/iaccel.h`
- **CUDA impl:** `tutti/accel/include/cuda/cuda_accelerator.h`
- **CUDA mapping (detailed):** `layer1-cuda-mapping.md`
- **Previous layer:** `doc/layered/layer0-abstraction.md`
- **Architecture:** `doc/architecture/layered-architecture-redesign.md`
