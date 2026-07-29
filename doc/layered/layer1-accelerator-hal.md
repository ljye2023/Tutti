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

> **Dead type:** `MemoryAccessFlags` is defined in `memory_kind.h` but is not referenced by any `IAccelerator` signature or by `CudaAccelerator`. It is reserved for future use and carries no behavior today.

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

| `Source` | Intended Backing | Union Payload |
|----------|------------------|---------------|
| `APP_MANAGED` | Caller allocation | (empty) |
| `DEVICE_IPC` | `cudaIpcOpenMemHandle` | `uint8_t handle[64]` |
| `HOST_SHM` | `shm_open` + `mmap` | `int shm_fd` |
| `HOST_FD_MAP` | `fd` + `mmap` | `int fd; off_t offset` |

> **Note:** the "Intended Backing" column documents what each source *represents*, not work the HAL performs. `register_external()` only records the caller-supplied pointers and a deep copy of the spec — it never calls `shm_open`, `mmap`, `cudaHostRegister`, or any IPC API, and `unregister()` performs no matching teardown. Bringing these pointers into existence is the caller's responsibility today (see Known Issues & Gaps).

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
| `allocate_host` | `void* allocate_host(size_t size, MemoryKind kind)` | Allocate host memory (HOST / PINNED_HOST only; MANAGED returns nullptr here — use `allocate_device`) |
| `allocate_device` | `void* allocate_device(size_t size, MemoryKind kind, int device_id)` | Allocate device memory, **64KB-aligned** |
| `free` | `void free(void* ptr, MemoryKind kind)` | Release allocation; `kind` selects the free path |

### Memory Registry

| Method | Signature | Purpose |
|--------|-----------|---------|
| `register_host` | `MemoryRegion* register_host(void* host_ptr, size_t size)` | Track existing host memory (bookkeeping only — does **not** pin; no `cudaHostRegister`) |
| `register_device` | `MemoryRegion* register_device(void* device_ptr, size_t size, int device_id)` | Track existing device memory |
| `register_external` | `MemoryRegion* register_external(void* host_ptr, void* device_ptr, size_t size, const ExternalMemorySpec& spec)` | Store the given pointers + a deep copy of `spec` (bookkeeping only — performs **no** `shm_open`/`mmap`/`cudaHostRegister`/IPC work) |
| `unregister` | `void unregister(MemoryRegion* region)` | Erase pointer maps + delete the `ExternalMemorySpec`; does **not** unpin, `munmap`, or close IPC handles |
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
| `memcpy_async` | `bool memcpy_async(void* dst, const void* src, size_t size, AccelStream stream)` | Async copy; issued with `cudaMemcpyDefault` (UVA infers direction) |

### Kernel Launch

| Method | Signature | Purpose |
|--------|-----------|---------|
| `launch` | `void launch(void* kernel_func, const Dim3& grid, const Dim3& block, size_t shared_mem_bytes, AccelStream stream, void** kernel_args)` | Dispatch kernel by function pointer |

### IPC

| Method | Signature | Purpose |
|--------|-----------|---------|
| `ipc_export` | `bool ipc_export(MemoryRegion* region, IpcHandle* out_handle)` | Export device memory as IPC handle |
| `ipc_import` | `MemoryRegion* ipc_import(const IpcHandle& handle, int device_id)` | **Non-functional** — opens the handle but calls `register_external(..., size=0)`, which rejects `size==0` and returns nullptr; the imported pointer leaks (see Known Issues) |

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
| Host alloc | `malloc` (HOST) / `cudaHostAlloc` (PINNED_HOST); MANAGED rejected here |
| Device alloc | `cudaMalloc` (+ alignment) for DEVICE; `cudaMallocManaged` for MANAGED |
| Free | `free` / `cudaFreeHost` / `cudaFree` |
| Register host | none — pure registry bookkeeping (no `cudaHostRegister`) |
| Pointer xlate | `cudaHostGetDevicePointer` |
| Streams | `cudaStreamCreate/Destroy/Synchronize` |
| Events | `cudaEventCreate/Destroy/Record/Query`, `cudaStreamWaitEvent` |
| Transfer | `cudaMemcpyAsync` with `cudaMemcpyDefault` (UVA infers direction) |
| Launch | `cudaLaunchKernel` |
| IPC | `cudaIpcGetMemHandle` (export) / `cudaIpcOpenMemHandle` (import); no `cudaIpcCloseMemHandle` on teardown |

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

**Rationale:** These are the real ways memory enters the system from outside the HAL. Making them explicit documents the supported integration paths and gives `unregister()` a place to hook the right teardown per source.

> **Current state:** the teardown is *not* implemented. `register_external()` stores pointers + a spec copy only, and `unregister()` deletes the spec and erases pointer maps — it does **not** call `cudaIpcCloseMemHandle`, `munmap`, or `cudaHostUnregister`. Imported IPC pointers and any host mappings are therefore leaked. See Known Issues & Gaps.

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

**Location:** `tests/accel/*.cu` — a GoogleTest suite (~35 `TEST_F` cases) split by functional area. The old single-file `layer1_smoke_test.cu` has been retired.

| File | Cases | Covers |
|------|-------|--------|
| `identity.cu` | 6 | device_count, get/set_device, invalid-device handling |
| `memory.cu` | 8 | HOST / PINNED_HOST / DEVICE / MANAGED alloc + free paths, 64KB alignment |
| `registry.cu` | 9 | register_host / register_device / register_external, lookup, lookup_by_id, unregister, null/zero-size guards |
| `stream_event.cu` | 4 | stream + event lifecycle incl. cross-stream `wait_event` ordering |
| `transfer.cu` | 3 | H→D, D→H, D→D `memcpy_async` with verification |
| `kernel.cu` | 2 | `launch()` via function pointer + 64KB-alignment/200-iteration leak regression |
| `ipc.cu` | 3 | `ipc_export` arg-validation; export→import roundtrip (see note) |

The shared fixture lives in `tests/accel/accel_test_fixture.h`; the build wiring is `tests/accel/CMakeLists.txt`.

### Coverage Notes

- **`ipc_import` is not verified end-to-end.** `ipc.cu`'s `ExportImportRoundtrip` calls `GTEST_SKIP()` when `ipc_export` is unsupported *or* when the in-process self-import returns nullptr — which it always does today because of the `size=0` bug (see Known Issues & Gaps). The metadata assertions after import are therefore never reached in-process.
- **External-source flows** (`HOST_SHM`, `HOST_FD_MAP`, `DEVICE_IPC` via `register_external`) are covered only at the bookkeeping level; the HAL performs no backing syscalls, so there is nothing further to assert.
- **Concurrency / multi-GPU** are still not exercised (thread-safety is claimed but not stress-tested; only device 0 is used).

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

## Implementation Status

| Component | Status | Tested |
|-----------|--------|--------|
| Identity (device_count / set / get) | Complete | Yes (`identity.cu`, incl. invalid-device) |
| Alloc HOST / PINNED_HOST | Complete | Yes (`memory.cu`) |
| Alloc DEVICE (64KB-aligned + raw-ptr free bookkeeping) | Complete | Yes (`memory.cu`, `kernel.cu` leak regression) |
| Alloc MANAGED (via `allocate_device`) | Complete | Yes (`memory.cu`) |
| Registry (register_host/device, lookup, lookup_by_id, unregister) | Complete | Yes (`registry.cu`) |
| Pointer translation (`device_pointer_for`) | Complete | Yes (`registry.cu`/`transfer.cu`) |
| Stream lifecycle | Complete | Yes (`stream_event.cu`) |
| Event lifecycle (incl. cross-stream `wait_event`) | Complete | Yes (`stream_event.cu`) |
| Transfer (`memcpy_async`, `cudaMemcpyDefault`) | Complete | Yes (`transfer.cu`) |
| Kernel launch (`cudaLaunchKernel`) | Complete | Yes (`kernel.cu`) |
| `ipc_export` | Complete | Yes — arg-validation (`ipc.cu`) |
| `ipc_import` | Stub (non-functional; `size=0` bug) | Test GTEST_SKIPs it |
| `register_external` backing (SHM / fd-map / IPC syscalls) | Not implemented (bookkeeping only) | N/A |
| External teardown in `unregister` (IPC close / munmap / unpin) | Not implemented | No |
| `register_host` pinning (`cudaHostRegister`) | Not implemented (bookkeeping only) | No |
| `MemoryAccessFlags` | Dead-reserved (defined, never referenced) | No |
| ROCm / SYCL backends | Not implemented | No |
| Async memset / stream query | Not implemented (no such method) | No |

---

## Known Issues & Gaps

- **`ipc_import` always returns nullptr and leaks the imported pointer.** After a successful `cudaIpcOpenMemHandle`, the method calls `register_external(nullptr, device_ptr, 0, spec)` (`src/cuda/cuda_accelerator.cu:495`). `register_external` rejects `size == 0` (`cuda_accelerator.cu:234`) and returns nullptr, so the opened device pointer is never registered and never closed — a leak on every call. The handle carries no size, so the fix requires threading a caller-supplied size into `ipc_import`. `tests/accel/ipc.cu` `GTEST_SKIP()`s past this rather than failing.
- **External memory teardown is missing.** `unregister` (`cuda_accelerator.cu:260`) only deletes the `ExternalMemorySpec` and erases pointer maps. It does not call `cudaIpcCloseMemHandle` (DEVICE_IPC), `munmap` (HOST_SHM / HOST_FD_MAP), or `cudaHostUnregister`. Any externally-imported memory leaks its OS/driver resource.
- **`register_external` performs no backing work.** It stores the supplied pointers and a deep copy of the spec (`cuda_accelerator.cu:228`). The documented flows (`shm_open`+`mmap` for HOST_SHM, `mmap` for HOST_FD_MAP, `cudaHostRegister`/IPC for EXTERNAL) are not executed; the caller must have already produced the pointers.
- **`register_host` does not pin.** It is pure registry bookkeeping (`cuda_accelerator.cu:180`) — no `cudaHostRegister`. Memory registered this way is not made DMA-capable by the HAL, contrary to earlier "track/pin" wording.
- **`allocate_host(MANAGED)` returns nullptr.** `allocate_host` handles only HOST and PINNED_HOST (`cuda_accelerator.cu:67`); MANAGED falls through to the error path. MANAGED memory is reachable only via `allocate_device(size, MANAGED, dev)`.
- **`MemoryAccessFlags` is dead.** Defined in `include/common/memory_kind.h` but referenced by no signature or implementation. Either wire it into `register_*`/`allocate_*` semantics or drop it.
- **No async memset and no stream-query method.** There is no `cudaMemset*` call anywhere in the backend and no stream-query entry point on `IAccelerator`; only `query_event` exists. Docs/READMEs that list these were aspirational.

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-07-22 | Initial CUDA implementation, single-file smoke test |
| 1.1 | 2026-07-29 | Docs reconciled with code; GoogleTest suite (`tests/accel/*.cu`, ~35 cases); documented `ipc_import` bug + external-teardown/pinning gaps |

---

## References

- **Interface:** `tutti/accel/include/common/iaccel.h`
- **CUDA impl:** `tutti/accel/include/cuda/cuda_accelerator.h`
- **CUDA mapping (detailed):** `layer1-cuda-mapping.md`
- **Previous layer:** `doc/layered/layer0-abstraction.md`
- **Architecture:** `doc/layered/architecture-overview.md` (L0–5 connective overview)
