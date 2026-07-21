# Layer 0: Abstraction (Macro Layer)

> Headers-only compile-time vendor dispatch for device code.  
> No runtime object, no vtable.

## Current State

**Only one semantic `TUTTI_` macro exists** in the entire codebase:

```cpp
// /home/zfw/refact/Tutti/block_storage/include/gpu_file_resolve.h:83
#ifdef __CUDACC__
  #define TUTTI_HOST_DEVICE __host__ __device__ __forceinline__
#else
  #define TUTTI_HOST_DEVICE inline
#endif
```

All other `TUTTI_` tokens are include-guard fragments (`__TUTTI_IO_ENGINE_IO_ENGINE_H__`).

**Direct CUDA annotations are used everywhere:**
- `__device__`, `__global__`, `__forceinline__` appear directly in `.cuh` headers
- `cuda::atomic<uint32_t, cuda::thread_scope_system>` appears in `nvm_types.h`, `ctrl.h`, `nvm_parallel_queue.h`, `coop_channel.h`
- `<<<grid, block, shmem, stream>>>` appears directly in backend kernels

**Local fallback guards** exist in libnvm headers:
```cpp
// bafs_ptr.h, nvm_queue.h, host_util.h each have:
#ifndef __device__
  #define __device__
#endif
```

These should be replaced with the canonical abstraction header.

---

## Proposed API

### Single Canonical Header

All abstraction macros live in **`tutti/abstraction/accel.h`**. Vendor selection (`TUTTI_ACCEL_CUDA` / `_ROCM` / `_SYCL` / `_CANN`) is defined exactly once by the build system via `-D`.

```cpp
// tutti/abstraction/accel.h
#pragma once

// Exactly one of these must be defined by the build system:
// -DTUTTI_ACCEL_CUDA / -DTUTTI_ACCEL_ROCM / -DTUTTI_ACCEL_SYCL / -DTUTTI_ACCEL_CANN

#if defined(TUTTI_ACCEL_CUDA)
  //--------------------------------------------------------------------------
  // CUDA (NVIDIA)
  //--------------------------------------------------------------------------
  #include <cuda_runtime.h>
  #include <cuda/atomic>

  // Function qualifiers
  #define TUTTI_DEVICE          __device__
  #define TUTTI_GLOBAL          __global__
  #define TUTTI_HOST            __host__
  #define TUTTI_HOST_DEVICE     __host__ __device__ __forceinline__
  #define TUTTI_FORCEINLINE     __forceinline__

  // Atomic types
  #define TUTTI_ATOMIC_U32_DEV  cuda::atomic<uint32_t, cuda::thread_scope_device>
  #define TUTTI_ATOMIC_U32_SYS  cuda::atomic<uint32_t, cuda::thread_scope_system>
  #define TUTTI_ATOMIC_U64_DEV  cuda::atomic<uint64_t, cuda::thread_scope_device>
  #define TUTTI_ATOMIC_U64_SYS  cuda::atomic<uint64_t, cuda::thread_scope_system>

  // Kernel launch
  #define TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...) \
    kernel<<<grid, block, shmem, stream>>>(__VA_ARGS__)

  // Memory fence
  #define TUTTI_THREADFENCE_SYSTEM() __threadfence_system()

#elif defined(TUTTI_ACCEL_ROCM)
  //--------------------------------------------------------------------------
  // ROCm (AMD)
  //--------------------------------------------------------------------------
  #include <hip/hip_runtime.h>
  
  #define TUTTI_DEVICE          __device__
  #define TUTTI_GLOBAL          __global__
  #define TUTTI_HOST            __host__
  #define TUTTI_HOST_DEVICE     __host__ __device__ __forceinline__
  #define TUTTI_FORCEINLINE     __forceinline__

  // ROCm atomics (placeholder — needs actual ROCm atomic types)
  #define TUTTI_ATOMIC_U32_DEV  unsigned int  // FIXME: use proper HIP atomics
  #define TUTTI_ATOMIC_U32_SYS  unsigned int
  #define TUTTI_ATOMIC_U64_DEV  unsigned long long
  #define TUTTI_ATOMIC_U64_SYS  unsigned long long

  #define TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...) \
    hipLaunchKernelGGL(kernel, grid, block, shmem, stream, __VA_ARGS__)

  #define TUTTI_THREADFENCE_SYSTEM() __threadfence_system()

#elif defined(TUTTI_ACCEL_SYCL)
  //--------------------------------------------------------------------------
  // SYCL (Intel / DPC++)
  //--------------------------------------------------------------------------
  // SYCL requires fundamentally different kernel launch model.
  // TUTTI_LAUNCH_KERNEL cannot be a simple macro — needs template wrapper.
  // This is flagged as an OPEN QUESTION (see 10-open-questions.md).
  
  #error "SYCL support requires design decisions beyond simple macro dispatch"

#elif defined(TUTTI_ACCEL_CANN)
  //--------------------------------------------------------------------------
  // CANN (Huawei Ascend)
  //--------------------------------------------------------------------------
  #error "CANN support not yet implemented"

#else
  //--------------------------------------------------------------------------
  // Host-only build (no accelerator)
  //--------------------------------------------------------------------------
  // Used for: gRPC services, unit tests, host-side utilities
  
  #define TUTTI_DEVICE
  #define TUTTI_GLOBAL
  #define TUTTI_HOST
  #define TUTTI_HOST_DEVICE     inline
  #define TUTTI_FORCEINLINE     __attribute__((always_inline)) inline

  // Atomics fall back to plain aligned integers (matches libnvm pattern)
  #define TUTTI_ATOMIC_U32_DEV  uint32_t __attribute__((aligned(4)))
  #define TUTTI_ATOMIC_U32_SYS  uint32_t __attribute__((aligned(4)))
  #define TUTTI_ATOMIC_U64_DEV  uint64_t __attribute__((aligned(8)))
  #define TUTTI_ATOMIC_U64_SYS  uint64_t __attribute__((aligned(8)))

  #define TUTTI_LAUNCH_KERNEL(...) \
    static_assert(false, "TUTTI_LAUNCH_KERNEL unavailable in host-only build")

  #define TUTTI_THREADFENCE_SYSTEM() ((void)0)

#endif
```

---

## Design Decisions

### A. `TUTTI_HOST_DEVICE` bundles `__forceinline__`

**Decision**: Keep the bundle as shown above.

**Rationale**:
- The existing `gpu_file_resolve.h:83` macro already bundles them
- Dual-compilation functions (`__host__ __device__`) are almost always small and should be inlined
- Sites that want inlining without dual-compilation can use `TUTTI_FORCEINLINE` separately
- Do NOT bundle `__forceinline__` into `TUTTI_DEVICE` alone — single-target device functions may be large

### B. Non-accelerator fallback for atomics

**Decision**: Plain aligned integers in the `#else` branch.

**Rationale**:
- Matches the existing pattern in `nvm_types.h` (libnvm already does this)
- Host-only builds (gRPC services, unit tests) need to compile without nvcc/hipcc
- Alignment ensures no miscompile issues on x86-64
- Atomic operations in host-only builds are typically single-threaded anyway

### C. `TUTTI_ATOMIC_U64_*` addition

**Decision**: Add 64-bit atomics to the macro set.

**Rationale**:
- Some device code (e.g., completion tracking) needs 64-bit atomics
- `cuda::atomic<uint64_t, ...>` exists but isn't currently abstracted
- Cost: zero (macro definition, no runtime overhead)

### D. SYCL `TUTTI_LAUNCH_KERNEL` incompatibility

**Decision**: Deferred to post-v0.1. Block compilation with `#error` for now.

**Rationale**:
- SYCL's `queue.submit([&](handler& cgh) { cgh.parallel_for(...); })` model is structurally incompatible with a call-expression macro
- Options:
  1. Template wrapper function (loses `<<<>>>` syntax)
  2. Separate `.sycl.cpp` compilation units (duplicate code)
  3. C++20 concepts + overload resolution (requires C++20)
- None of these can be decided without a concrete SYCL target
- v0.1 targets CUDA only — SYCL support is explicitly out of scope

---

## Migration Plan

### Files to Create

```
tutti/abstraction/accel.h  (new)
```

### Files to Modify

| File | Current Issue | Fix |
|---|---|---|
| `block_storage/include/gpu_file_resolve.h:83` | Local `TUTTI_HOST_DEVICE` definition | Delete local def, add `#include "tutti/abstraction/accel.h"` |
| `backends/local/nvme/libnvm/include/bafs_ptr.h` | `#ifndef __device__ #define __device__` fallback | Delete fallback, add `#include "tutti/abstraction/accel.h"` |
| `backends/local/nvme/libnvm/include/nvm_queue.h` | Same | Same |
| `backends/local/nvme/libnvm/include/host_util.h` | Same | Same |
| `nvme_storage/include/queue_acquire_helper.cuh` | `__device__ __forceinline__` | Replace with `TUTTI_DEVICE TUTTI_FORCEINLINE` |
| `nvme_storage/include/nvme_storage_device.cuh` | `__device__ __forceinline__` | Replace with `TUTTI_DEVICE TUTTI_FORCEINLINE` |
| `backends/local/nvme/libnvm/include/nvm_types.h` | `cuda::atomic<uint32_t, ...>` | Replace with `TUTTI_ATOMIC_U32_SYS` |
| `backends/local/nvme/libnvm/include/ctrl.h` | `cuda::atomic<uint32_t, ...>` | Replace with `TUTTI_ATOMIC_U32_DEV` or `_SYS` |
| `backends/local/nvme/libnvm/include/nvm_parallel_queue.h` | `cuda::atomic<uint64_t, ...>` | Replace with `TUTTI_ATOMIC_U64_SYS` |
| `io_engine/include/coop_channel.h` | `cuda::atomic<uint32_t, ...>` | Replace with `TUTTI_ATOMIC_U32_SYS` |

### Kernel Launch Sites

All sites using `<<<grid, block, shmem, stream>>>` must:
1. Include `tutti/abstraction/accel.h`
2. Replace `kernel<<<...>>>` with `TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, args...)`

**Example**:
```cpp
// Before:
my_kernel<<<grid, block, 0, stream>>>(arg1, arg2, arg3);

// After:
TUTTI_LAUNCH_KERNEL(my_kernel, grid, block, 0, stream, arg1, arg2, arg3);
```

**Known sites** (from workflow analysis):
- `io_engine/src/local_nvme/*.cu`
- `backends/local/nvme/*.cu`
- `block_storage/src/*.cu`
- `nvme_storage/src/*.cu`

---

## Validation Criteria

✅ The abstraction layer is correct when:

1. **No direct CUDA annotations above this layer** — all `.h` and `.cuh` files use `TUTTI_*` macros
2. **No local fallback guards** — the canonical header is the single source of truth
3. **Host-only build compiles** — test with `-DTUTTI_ACCEL_CUDA` undefined
4. **nvcc build succeeds** — test with `-DTUTTI_ACCEL_CUDA`
5. **No semantic change** — generated PTX/SASS is identical for CUDA builds (can verify with `cuobjdump`)

---

## Example: Before and After

### Before (gpu_file_resolve.h:83)

```cpp
#ifdef __CUDACC__
  #define TUTTI_HOST_DEVICE __host__ __device__ __forceinline__
#else
  #define TUTTI_HOST_DEVICE inline
#endif

TUTTI_HOST_DEVICE
inline uint32_t gpu_file_resolve(
    uint64_t global_byte_offset,
    uint64_t tensor_size,
    uint32_t num_shards,
    uint32_t* out_shard_idx,
    uint64_t* out_shard_byte_offset) {
    // ...
}
```

### After

```cpp
#include "tutti/abstraction/accel.h"

TUTTI_HOST_DEVICE
uint32_t gpu_file_resolve(
    uint64_t global_byte_offset,
    uint64_t tensor_size,
    uint32_t num_shards,
    uint32_t* out_shard_idx,
    uint64_t* out_shard_byte_offset) {
    // ...
}
```

---

## Open Questions

See `10-open-questions.md` for:
- **Q1**: SYCL kernel launch wrapper design
- **Q2**: Whether to add `TUTTI_SHARED` for `__shared__` memory
- **Q3**: Whether to add `TUTTI_CONSTANT` for `__constant__` memory

These don't block v0.1 (CUDA-only).
