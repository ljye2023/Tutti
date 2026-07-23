# Layer 1 Accelerator HAL - CUDA Mapping

**Date:** 2026-07-22  
**Purpose:** Complete mapping between Tutti Layer 1 APIs and CUDA runtime/driver APIs

---

## Type Mappings

### Core Handle Types

| Tutti Type | CUDA Type | Storage | Notes |
|------------|-----------|---------|-------|
| `AccelStream` | `cudaStream_t` | `void*` opaque handle | Wrapper around stream pointer |
| `AccelEvent` | `cudaEvent_t` | `void*` opaque handle | Wrapper around event pointer |
| `IpcHandle` | `cudaIpcMemHandle_t` | `uint8_t[64]` | CUDA uses 64 bytes, Tutti matches |
| `Dim3` | `dim3` | `{uint32_t x, y, z}` | Named `Dim3` to avoid conflict with CUDA's `dim3` |

### Memory Types

| Tutti `MemoryKind` | CUDA Allocation Function | Properties |
|-------------------|-------------------------|------------|
| `HOST` | `malloc()` or `new` | Pageable, CPU-only, not DMA-capable |
| `PINNED_HOST` | `cudaMallocHost()` or `cudaHostAlloc()` | Pinned, DMA-capable, device-accessible |
| `DEVICE` | `cudaMalloc()` | GPU VRAM, not host-accessible |
| `MANAGED` | `cudaMallocManaged()` | Unified memory, automatic migration |
| `EXTERNAL` | `cudaHostRegister()` / IPC | User-managed, registered with runtime |

### Memory Region Structure

| Tutti `MemoryRegion` Field | CUDA Equivalent | Notes |
|---------------------------|-----------------|-------|
| `region_id` | (none) | Tutti-specific registry ID |
| `kind` | (implicit in allocation type) | Tracked by Tutti |
| `device_id` | Device ID from context | From `cudaGetDevice()` |
| `host_ptr` | Host pointer | From `cudaMallocHost()` or user pointer |
| `device_ptr` | Device pointer | From `cudaMalloc()` or `cudaHostGetDevicePointer()` |
| `size` | (tracked by app) | CUDA doesn't expose size after allocation |
| `external` | (none) | Tutti-specific for external memory tracking |
| `backend_private` | (none) | Layer 3+ backend metadata (DMA ioaddrs, RDMA keys) |

### External Memory Sources

| Tutti `ExternalMemorySpec::Source` | CUDA/System Call | Purpose |
|-----------------------------------|------------------|---------|
| `APP_MANAGED` | (user allocation) | App owns lifecycle, Tutti tracks only |
| `DEVICE_IPC` | `cudaIpcOpenMemHandle()` | Cross-process GPU memory sharing |
| `HOST_SHM` | `shm_open()` + `mmap()` | POSIX shared memory |
| `HOST_FD_MAP` | `mmap()` with fd | File-backed mapping |

---

## API Mappings

### Identity APIs

| IAccelerator Method | CUDA API | Notes |
|-------------------|----------|-------|
| `vendor_name()` | (none) | Returns `"CUDA"` constant |
| `device_count()` | `cudaGetDeviceCount()` | Number of visible GPUs |
| `set_device(int id)` | `cudaSetDevice(id)` | Set active GPU |
| `get_device()` | `cudaGetDevice()` | Get current GPU |

---

### Memory Allocation APIs

| IAccelerator Method | CUDA API | Implementation Details |
|--------------------|----------|----------------------|
| `allocate_host(size, HOST)` | `malloc(size)` | Standard C allocation |
| `allocate_host(size, PINNED_HOST)` | `cudaMallocHost(&ptr, size)` | Pinned memory for fast H↔D transfer |
| `allocate_host(size, MANAGED)` | `cudaMallocManaged(&ptr, size)` | Unified memory |
| `allocate_device(size, DEVICE, dev)` | `cudaSetDevice(dev)` + `cudaMalloc(&ptr, size + 65536)` | **Over-allocates by 64KB**, returns aligned sub-pointer |
| `allocate_device(size, MANAGED, dev)` | `cudaSetDevice(dev)` + `cudaMallocManaged(&ptr, size)` | Unified memory on specific device |
| `free(ptr, HOST)` | `free(ptr)` | Standard C free |
| `free(ptr, PINNED_HOST)` | `cudaFreeHost(ptr)` | Unpin and free |
| `free(ptr, DEVICE)` | `cudaFree(aligned_to_raw_[ptr])` | **Maps aligned ptr → raw ptr**, then `cudaFree()` |
| `free(ptr, MANAGED)` | `cudaFree(ptr)` | Works for both device and managed |

**Key Implementation Detail:**
- `allocate_device(DEVICE)` over-allocates by 64KB and returns a 64KB-aligned sub-pointer
- `free(DEVICE)` must map the aligned pointer back to the original `cudaMalloc()` pointer via `aligned_to_raw_` map
- This ensures DMA operations land on 64KB boundaries (NVMe requirement)

---

### Memory Registration APIs

| IAccelerator Method | CUDA API | Notes |
|--------------------|----------|-------|
| `register_host(ptr, size)` | `cudaHostRegister(ptr, size, cudaHostRegisterDefault)` | Make existing host memory DMA-capable |
| `register_device(ptr, size, dev)` | (none - tracking only) | Wraps existing device allocation |
| `register_external(...)` | Varies by source | See External Memory Registration below |
| `unregister(region)` | `cudaHostUnregister(region->host_ptr)` (if HOST) | Only for registered host memory |
| `lookup(ptr)` | (none) | Tutti registry: `ptr_to_region_id_` map |
| `lookup_by_id(id)` | (none) | Tutti registry: `regions_by_id_` map |

#### External Memory Registration

| Source Type | CUDA/System APIs | Flow |
|-------------|------------------|------|
| `APP_MANAGED` | (none) | Track pointer only, no CUDA registration |
| `DEVICE_IPC` | `cudaIpcOpenMemHandle(handle, &ptr, flags)` | Import device pointer from another process |
| `HOST_SHM` | `shm_open()` → `mmap()` → `cudaHostRegister()` | Shared memory, then register |
| `HOST_FD_MAP` | `mmap(fd, offset)` → `cudaHostRegister()` | File-backed, then register |

---

### Pointer Translation APIs

| IAccelerator Method | CUDA API | Notes |
|--------------------|----------|-------|
| `device_pointer_for(host_ptr)` | `cudaHostGetDevicePointer(&dev_ptr, host_ptr, 0)` | Get device view of pinned host memory |

---

### Stream Lifecycle APIs

| IAccelerator Method | CUDA API | Notes |
|--------------------|----------|-------|
| `create_stream()` | `cudaStreamCreate(&stream)` | Returns wrapped `AccelStream` |
| `destroy_stream(stream)` | `cudaStreamDestroy((cudaStream_t)stream.handle)` | Unwrap and destroy |
| `synchronize_stream(stream)` | `cudaStreamSynchronize((cudaStream_t)stream.handle)` | Block until stream complete |

---

### Event Lifecycle APIs

| IAccelerator Method | CUDA API | Notes |
|--------------------|----------|-------|
| `create_event()` | `cudaEventCreate(&event)` | Returns wrapped `AccelEvent` |
| `destroy_event(event)` | `cudaEventDestroy((cudaEvent_t)event.handle)` | Unwrap and destroy |
| `record_event(event, stream)` | `cudaEventRecord((cudaEvent_t)event.handle, (cudaStream_t)stream.handle)` | Record event on stream |
| `wait_event(stream, event)` | `cudaStreamWaitEvent((cudaStream_t)stream.handle, (cudaEvent_t)event.handle, 0)` | Stream waits for event |
| `query_event(event)` | `cudaEventQuery((cudaEvent_t)event.handle) == cudaSuccess` | Returns `true` if complete |

---

### Transfer APIs

| IAccelerator Method | CUDA API | Direction Detection |
|--------------------|----------|-------------------|
| `memcpy_async(dst, src, size, stream)` | `cudaMemcpyAsync(dst, src, size, direction, stream)` | Auto-detect via `cudaPointerGetAttributes()` |

**Direction Detection Logic:**
1. Query `src` and `dst` pointer attributes
2. Determine direction:
   - Host → Device: `cudaMemcpyHostToDevice`
   - Device → Host: `cudaMemcpyDeviceToHost`
   - Device → Device: `cudaMemcpyDeviceToDevice`
   - Host → Host: `cudaMemcpyHostToHost` (rare)
   - Unknown: `cudaMemcpyDefault` (runtime figures it out)

---

### Kernel Launch APIs

| IAccelerator Method | CUDA Equivalent | Implementation |
|--------------------|-----------------|----------------|
| `launch(kernel_func, grid, block, shmem, stream, args)` | `cudaLaunchKernel(kernel_func, grid, block, args, shmem, stream)` | Direct mapping to CUDA driver API |

**Mapping Details:**
- `void* kernel_func` → CUDA device function pointer
- `Dim3 grid` → `dim3 gridDim`
- `Dim3 block` → `dim3 blockDim`
- `size_t shmem` → shared memory bytes
- `AccelStream stream` → `cudaStream_t`
- `void** args` → array of kernel argument pointers

**Alternative:** Could use `<<<grid, block, shmem, stream>>>` syntax, but `cudaLaunchKernel()` is more explicit and allows runtime function pointer dispatch.

---

### IPC APIs

| IAccelerator Method | CUDA API | Flow |
|--------------------|----------|------|
| `ipc_export(region, out_handle)` | `cudaIpcGetMemHandle((cudaIpcMemHandle_t*)out_handle->data, region->device_ptr)` | Export device pointer as IPC handle |
| `ipc_import(handle, device_id)` | `cudaSetDevice(device_id)` + `cudaIpcOpenMemHandle(&ptr, *(cudaIpcMemHandle_t*)handle.data, flags)` | Import and register as EXTERNAL region |

**IPC Handle Flow:**
1. **Process A**: `ipc_export()` → `cudaIpcGetMemHandle()` → 64-byte handle
2. **Inter-process**: Transfer handle via shared memory / socket / etc.
3. **Process B**: `ipc_import()` → `cudaIpcOpenMemHandle()` → device pointer
4. **Cleanup**: Process B calls `unregister()` → `cudaIpcCloseMemHandle()`

---

## Memory Hierarchy Summary

| Tutti Concept | CUDA Reality | Visibility |
|---------------|--------------|------------|
| HOST | CPU pageable RAM | CPU only |
| PINNED_HOST | CPU pinned RAM | CPU + GPU (via PCIe) |
| DEVICE | GPU VRAM | GPU only (unless HOST_MAPPED) |
| MANAGED | Unified memory pool | CPU + GPU (auto-migrated) |
| EXTERNAL (DEVICE_IPC) | GPU VRAM in another process | GPU only, shared via IPC |
| EXTERNAL (HOST_SHM) | CPU shared memory | CPU + GPU (if registered) |

---

## Key Design Choices

### 1. Opaque Handles
**Tutti:** `AccelStream { void* handle; }`  
**CUDA:** `cudaStream_t` (opaque pointer typedef)  
**Rationale:** Prevents vendor types leaking into public headers, enables future backends (ROCm, SYCL)

### 2. 64KB Alignment
**Tutti:** `allocate_device(DEVICE)` returns 64KB-aligned pointer  
**CUDA:** `cudaMalloc()` returns 256-byte aligned pointer  
**Rationale:** NVMe DMA requires 64KB-aligned buffers (PCIe TLP alignment)

### 3. MemoryRegion Registry
**Tutti:** Central registry with `region_id`, `kind`, `host_ptr`, `device_ptr`, `size`, `backend_private`  
**CUDA:** No unified tracking, app must track allocations  
**Rationale:** Enables Layer 3+ backends to attach DMA metadata (ioaddrs, RDMA keys) without requiring HAL changes

### 4. External Memory Types
**Tutti:** 4 explicit source types (APP_MANAGED, DEVICE_IPC, HOST_SHM, HOST_FD_MAP)  
**CUDA:** No equivalent abstraction  
**Rationale:** Unifies tracking of memory not owned by HAL (app buffers, IPC imports, shared memory, file mappings)

### 5. Thread Safety
**Tutti:** All `IAccelerator` methods protected by mutexes  
**CUDA:** Most runtime APIs are thread-safe per-device  
**Rationale:** Tutti registry (`regions_by_id_`, `ptr_to_region_id_`, `aligned_to_raw_`) is shared state requiring explicit protection

---

## Implementation Notes

### CudaAccelerator Internal State

```cpp
// Registry protection
std::mutex registry_mutex_;
std::unordered_map<uint64_t, RegionEntry> regions_by_id_;       // region_id → MemoryRegion
std::unordered_map<uintptr_t, uint64_t> ptr_to_region_id_;      // ptr → region_id
uint64_t next_region_id_;

// Allocation tracking
std::mutex alloc_mutex_;
std::unordered_map<uintptr_t, void*> aligned_to_raw_;           // aligned_ptr → raw_cudaMalloc_ptr
```

### Alignment Implementation

```cpp
void* CudaAccelerator::allocate_device(size_t size, MemoryKind kind, int device_id) {
    cudaSetDevice(device_id);
    
    if (kind == MemoryKind::DEVICE) {
        void* raw_ptr;
        cudaMalloc(&raw_ptr, size + 65536);  // Over-allocate by 64KB
        
        uintptr_t addr = (uintptr_t)raw_ptr;
        uintptr_t aligned = (addr + 65535) & ~65535ULL;  // Round up to 64KB
        void* aligned_ptr = (void*)aligned;
        
        aligned_to_raw_[aligned] = raw_ptr;  // Track for free()
        return aligned_ptr;
    }
    // ... MANAGED case ...
}

void CudaAccelerator::free(void* ptr, MemoryKind kind) {
    if (kind == MemoryKind::DEVICE) {
        void* raw_ptr = aligned_to_raw_[ptr];  // Lookup original pointer
        cudaFree(raw_ptr);                     // Free original allocation
        aligned_to_raw_.erase(ptr);
    }
    // ... other cases ...
}
```

---

## Future Backend Considerations

### ROCm (AMD)

| Tutti API | ROCm Equivalent | Notes |
|-----------|----------------|-------|
| `cudaStream_t` | `hipStream_t` | Direct equivalent |
| `cudaEvent_t` | `hipEvent_t` | Direct equivalent |
| `cudaMalloc()` | `hipMalloc()` | Direct equivalent |
| `cudaMemcpyAsync()` | `hipMemcpyAsync()` | Direct equivalent |
| `cudaIpcMemHandle_t` | `hipIpcMemHandle_t` | Direct equivalent |

**Porting Effort:** Low - HIP API is CUDA-compatible by design

### SYCL (Intel)

| Tutti Concept | SYCL Equivalent | Challenge |
|--------------|----------------|-----------|
| Stream | `sycl::queue` | Different semantics (queue vs stream) |
| Event | `sycl::event` | Different lifecycle (returned by submit) |
| Kernel launch | `queue.submit([&](handler& h) { ... })` | Lambda-based, not function pointer |
| Memory | USM (Unified Shared Memory) | Closer to MANAGED than discrete HOST/DEVICE |

**Porting Effort:** High - SYCL's programming model is fundamentally different

---

## Compliance Summary

✅ **Tutti Layer 1 API is a clean, minimal abstraction over CUDA runtime**  
✅ **No vendor types leak into public headers**  
✅ **64KB alignment requirement met**  
✅ **Memory registry enables backend metadata attachment**  
✅ **Thread-safe by design**  
✅ **ROCm portability: straightforward**  
⚠️ **SYCL portability: requires design changes (flagged in accel.h)**

