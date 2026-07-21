# Layer 1: Accelerator HAL (IAccelerator)

> Generic memory allocation, DMA mapping, stream/event lifecycle, kernel launch.  
> Replaces the generic half of `IMemorySubsystem`.

## Current State: IMemorySubsystem Split

`IMemorySubsystem` (`memory/include/memory_subsystem.h`) conflates three concerns:

| Concern | Example Methods | New Owner |
|---|---|---|
| **Generic HAL** | `allocate_*`, `register_*`, DMA-map, `lookup` | **IAccelerator** |
| **NVMe descriptor build** | `descriptor_slice`, `ensure_prp_pages_resident`, `set_descriptor_format` | **NVMe Backend** |
| **IO-slice orchestration** | `register_tensor`, `lookup_io_slice`, `list_io_slices` | **IO Engine** |

The concrete implementation `HostDeviceMemorySubsystem` includes `<cuda_runtime.h>` directly and uses CUDA types in public headers (constraint violation).

---

## Proposed API: IAccelerator

```cpp
// tutti/accel/iaccel.h
#pragma once
#include "tutti/accel/accel_types.h"   // AccelStream, AccelEvent, MemoryRegion, IpcHandle
#include "tutti/accel/memory_kind.h"   // MemoryKind enum
#include <cstddef>
#include <cstdint>

namespace tutti {

// Vendor-neutral accelerator runtime interface.
// Concrete implementations: CudaAccelerator, RocmAccelerator, SyclAccelerator, etc.
class IAccelerator {
public:
    virtual ~IAccelerator() = default;

    //==========================================================================
    // Identity
    //==========================================================================
    
    virtual const char* vendor_name() const = 0;    // "CUDA", "ROCm", "SYCL", "CANN"
    virtual int         device_count() const = 0;
    virtual bool        set_device(int device_id) = 0;
    virtual int         get_device() const = 0;

    //==========================================================================
    // Memory Allocation
    //==========================================================================
    
    // Allocate host memory.
    // kind: HOST (pageable) or PINNED_HOST (page-locked)
    virtual void* allocate_host(size_t size, MemoryKind kind) = 0;
    
    // Allocate device memory.
    // kind: DEVICE (on-GPU) or MANAGED (unified memory)
    virtual void* allocate_device(size_t size, MemoryKind kind, int device_id) = 0;
    
    // Free memory allocated by this allocator.
    virtual void free(void* ptr, MemoryKind kind) = 0;

    //==========================================================================
    // MemoryRegion Registry
    //
    // Register caller-owned buffers and hand back a stable MemoryRegion*.
    // The registry is the runtime's "we know about this memory" source of truth.
    //==========================================================================
    
    // Register host memory (pageable or pinned).
    virtual MemoryRegion* register_host(void* host_ptr, size_t size) = 0;
    
    // Register device memory (on-GPU or managed).
    virtual MemoryRegion* register_device(void* device_ptr, size_t size, int device_id) = 0;
    
    // Register external memory (IPC import, host fd mapping, etc.).
    // host_ptr and/or device_ptr may be nullptr depending on source.
    virtual MemoryRegion* register_external(
        void* host_ptr, 
        void* device_ptr,
        size_t size,
        const ExternalMemorySpec& spec) = 0;
    
    // Unregister and invalidate the region. Caller must ensure no outstanding IO.
    virtual void unregister(MemoryRegion* region) = 0;
    
    // Lookup by host pointer, device pointer, or region ID.
    virtual MemoryRegion* lookup(const void* ptr) const = 0;
    virtual MemoryRegion* lookup_by_id(uint64_t region_id) const = 0;

    //==========================================================================
    // DMA Mapping
    //
    // Explicit DMA-map operation separate from registration.
    // Produces per-page bus addresses (ioaddrs) that NVMe/RDMA controllers use.
    //==========================================================================
    
    // Map a region for controller DMA access.
    // out_ioaddrs: pointer to the array of bus addresses (one per page)
    // out_count: number of pages
    // Returns false on mapping failure.
    virtual bool dma_map(
        MemoryRegion* region,
        int device_id,
        const uint64_t** out_ioaddrs,
        size_t* out_count) = 0;
    
    // Unmap DMA addresses. Safe to call multiple times.
    virtual void dma_unmap(MemoryRegion* region, int device_id) = 0;

    //==========================================================================
    // Host ↔ Device Pointer Translation
    //==========================================================================
    
    // Translate a pinned-host pointer to its device-visible address.
    // Returns nullptr if ptr is not pinned or not mappable.
    virtual void* device_pointer_for(const void* host_ptr) = 0;

    //==========================================================================
    // Stream Lifecycle
    //==========================================================================
    
    virtual AccelStream create_stream() = 0;
    virtual void        destroy_stream(AccelStream stream) = 0;
    virtual void        synchronize_stream(AccelStream stream) = 0;

    //==========================================================================
    // Event Lifecycle
    //==========================================================================
    
    virtual AccelEvent create_event() = 0;
    virtual void       destroy_event(AccelEvent event) = 0;
    virtual void       record_event(AccelEvent event, AccelStream stream) = 0;
    virtual void       wait_event(AccelStream stream, AccelEvent event) = 0;
    virtual bool       query_event(AccelEvent event) = 0;  // true = complete

    //==========================================================================
    // Transfer
    //==========================================================================
    
    // Async memcpy (host→device, device→host, device→device).
    // Direction inferred from src/dst pointers.
    virtual bool memcpy_async(
        void* dst,
        const void* src,
        size_t size,
        AccelStream stream) = 0;

    //==========================================================================
    // Kernel Launch
    //
    // Launches a caller-provided kernel entry on a stream.
    // Backends call this rather than writing <<<>>> directly.
    //==========================================================================
    
    virtual void launch(
        void* kernel_func,      // function pointer (cast from __global__ void (*)(Args...))
        dim3 grid,
        dim3 block,
        size_t shared_mem_bytes,
        AccelStream stream,
        void** kernel_args) = 0;  // array of pointers to kernel arguments

    //==========================================================================
    // IPC (Cross-Process Memory Sharing)
    //==========================================================================
    
    // Export a device region for import by another process.
    virtual bool ipc_export(MemoryRegion* region, IpcHandle* out_handle) = 0;
    
    // Import a region exported by another process.
    // Returns a new MemoryRegion* that must be unregistered when done.
    virtual MemoryRegion* ipc_import(const IpcHandle& handle, int device_id) = 0;
};

} // namespace tutti
```

---

## Supporting Types

### MemoryKind

```cpp
// tutti/accel/memory_kind.h
#pragma once

namespace tutti {

enum class MemoryKind : uint32_t {
    HOST         = 0,  // Pageable host memory
    PINNED_HOST  = 1,  // Page-locked host memory (cudaHostAlloc)
    DEVICE       = 2,  // GPU device memory (cudaMalloc)
    MANAGED      = 3,  // Unified memory (cudaMallocManaged)
    EXTERNAL     = 4,  // IPC import, fd mapping, etc.
};

} // namespace tutti
```

### MemoryRegion (moved from memory/)

```cpp
// tutti/accel/memory_region.h
#pragma once
#include <cstdint>
#include <cstddef>
#include "memory_kind.h"

namespace tutti {

// Immutable runtime handle for registered memory.
// Created by IAccelerator::register_*, destroyed by unregister().
struct MemoryRegion {
    // Identity
    uint64_t    region_id;      // Unique across the runtime
    MemoryKind  kind;
    int         cuda_device;    // Which GPU owns this (or -1 for host-only)
    
    // Address views
    void*       host_ptr;       // nullptr if device-only
    void*       device_ptr;     // nullptr if host-only
    size_t      size;
    
    // External memory metadata (valid only if kind == EXTERNAL)
    ExternalMemorySpec external;
    
    // Backend registration metadata (opaque to HAL)
    // - DMA ioaddrs (NVMe, RDMA)
    // - RDMA keys
    // Set by dma_map(), not by register_*()
    void* backend_private;
};

struct ExternalMemorySpec {
    enum class Source {
        APP_MANAGED,  // Caller allocated, HAL just tracks it
        CUDA_IPC,     // cudaIpcOpenMemHandle
        HOST_SHM,     // shm_open + mmap
        HOST_FD_MAP,  // fd + mmap
    } source;
    
    union {
        struct { /* empty */ } app_managed;
        struct { IpcHandle handle; } cuda_ipc;
        struct { int shm_fd; } host_shm;
        struct { int fd; off_t offset; } host_fd;
    };
};

} // namespace tutti
```

---

## What Moves OUT of IMemorySubsystem

### To IO Engine

| Method | Why |
|---|---|
| `register_tensor(TensorRegistrationSpec)` | Combines alloc + dma_map + IO-slice precompute across all devices; IO Engine orchestrates this using IAccelerator primitives |
| `lookup_io_slice(region, slice_addr)` | IO-slice tables are PRP/SGL-specific, not a HAL concern |
| `list_io_slices(region)` | Same |

### To NVMe Backend

| Method | Why |
|---|---|
| `ensure_prp_pages_resident(regions, stream)` | PRP-list page eviction/promotion is a backend-private two-tier cache (PrpPageCache) |
| `descriptor_slice(region, device, offset, length, out, count)` | Pure NVMe descriptor formatting (PRP/SGL build), not generic HAL |
| `set_descriptor_format()` / `descriptor_format()` | PRP-vs-SGL negotiation is transport-specific |

### To Coordinator Config

| Method | Why |
|---|---|
| `bind_devices(devices)` — MDTS/caps aspect | IO Engine needs device caps for fan-out planning; not a HAL responsibility |

---

## Key Constraint: No cuda_runtime.h Above HAL

The following 8 headers currently violate this and **must be fixed**:

| File | Current Issue | Fix |
|---|---|---|
| `memory/include/memory_subsystem.h` | `cudaStream_t` in `ensure_prp_pages_resident` | Method moves to backend; drop `#include <cuda_runtime.h>` |
| `io_engine/include/io_engine.h` | `cudaStream_t` in `submit_batch` / `submit_batch_async` | Replace with `AccelStream` |
| `io_engine/include/local_nvme/launch_batch.h` | `cudaStream_t`, `cudaError_t` | Replace with `AccelStream`; return `bool` |
| `io_engine/include/local_nvme/local_nvme_io_engine.h` | Override signatures with `cudaStream_t` | Replace with `AccelStream` |
| `coordinator/include/coordinator.h` | `cudaStream_t` throughout IO APIs | Replace with `AccelStream` |
| `memory/include/gpu_slot_pool.h` | `cudaStream_t` / `cudaEvent_t` in slot pool API | Replace with `AccelStream` / `AccelEvent` |
| `memory/include/host_slot_pool.h` | `cudaMallocHost` / `cudaFreeHost` direct calls | Move into HAL concrete impl |
| `io_engine/include/backend_provider.h` | `cudaStream_t` in SPI methods | Replace with `AccelStream` |

**Allowed exception**: Concrete `.cu` implementation files and private headers (e.g., `cuda_accelerator_impl.cuh`) may include `cuda_runtime.h`.

---

## Concrete Implementation: CudaAccelerator

```cpp
// tutti/accel/cuda/cuda_accelerator.h
#pragma once
#include "tutti/accel/iaccel.h"
#include <cuda_runtime.h>  // OK in concrete impl header
#include <unordered_map>
#include <mutex>

namespace tutti {

class CudaAccelerator : public IAccelerator {
public:
    CudaAccelerator();
    ~CudaAccelerator() override;
    
    // IAccelerator implementation
    const char* vendor_name() const override { return "CUDA"; }
    int device_count() const override;
    bool set_device(int device_id) override;
    // ... all other methods ...

private:
    struct RegionEntry {
        MemoryRegion region;
        // CUDA-specific state
        cudaStream_t pinning_stream;  // for async pin operations
    };
    
    std::unordered_map<uint64_t, RegionEntry> regions_;
    std::mutex registry_mutex_;
    uint64_t next_region_id_ = 1;
};

} // namespace tutti
```

Implementation notes:
- `allocate_host(PINNED_HOST)` → `cudaHostAlloc`
- `allocate_device(DEVICE)` → `cudaMalloc`
- `dma_map` → `nvm_dma_map_data_device` (libnvm call, but HAL knows about it)
- `device_pointer_for` → `cudaHostGetDevicePointer`
- `launch` → `cudaLaunchKernel` (not `<<<>>>`, this is runtime API)

---

## Migration Path

### Step 1: Create HAL Headers (no code changes yet)

```
tutti/accel/accel_types.h       — AccelStream, AccelEvent, IpcHandle
tutti/accel/memory_kind.h       — MemoryKind enum
tutti/accel/memory_region.h     — MemoryRegion struct
tutti/accel/iaccel.h            — IAccelerator interface
```

### Step 2: Create Concrete CUDA Implementation

```
tutti/accel/cuda/cuda_accelerator.h
tutti/accel/cuda/cuda_accelerator.cu
```

### Step 3: Fix Constraint Violations (8 headers)

Replace `cudaStream_t` → `AccelStream` in all public headers listed above.

### Step 4: Update Coordinator

Replace `HostDeviceMemorySubsystem` with `CudaAccelerator` in `Coordinator`:

```cpp
// Before:
std::unique_ptr<IMemorySubsystem> memory_;

// After:
std::unique_ptr<IAccelerator> accel_;
```

### Step 5: Move Methods to New Owners

- `register_tensor` → IO Engine
- `ensure_prp_pages_resident` → NVMe Backend
- `descriptor_slice` → NVMe Backend

---

## Validation Criteria

✅ HAL is correct when:

1. **No cuda_runtime.h above HAL** — all 8 violating headers fixed
2. **AccelStream is opaque** — no `cudaStream_t` in public headers
3. **MemoryRegion registry works** — register/lookup/unregister roundtrip succeeds
4. **DMA mapping produces ioaddrs** — backend can call `dma_map` and get valid bus addresses
5. **Stream/event lifecycle works** — create/destroy/sync/record/wait all succeed
6. **Kernel launch works** — backends can call `accel->launch(...)` instead of `<<<>>>`

---

## Open Questions

See `10-open-questions.md` for:
- **Q4**: Should `dma_map` be part of `register_*` or a separate call?  
  **Current decision**: Separate (explicit DMA-map after registration).
- **Q5**: Should `MemoryRegion` live in HAL or in a shared `tutti/types/` directory?  
  **Current decision**: HAL owns it (`tutti/accel/memory_region.h`).
