# Layer 0: Abstraction Macro Mapping

**Source:** `tutti/abstraction/accel.h`

## CUDA Backend Mapping

When `TUTTI_ACCEL_CUDA` is defined:

| Tutti Macro | CUDA Equivalent | Purpose |
|-------------|-----------------|---------|
| `TUTTI_DEVICE` | `__device__` | Function callable only from device code |
| `TUTTI_GLOBAL` | `__global__` | Kernel entry point (callable from host) |
| `TUTTI_HOST` | `__host__` | Function callable only from host code |
| `TUTTI_HOST_DEVICE` | `__host__ __device__ __forceinline__` | Function callable from both host and device, force-inlined |
| `TUTTI_FORCEINLINE` | `__forceinline__` | Force function inlining |
| `TUTTI_ATOMIC_U32_DEV` | `cuda::atomic<uint32_t, cuda::thread_scope_device>` | Device-scope atomic 32-bit unsigned integer |
| `TUTTI_ATOMIC_U32_SYS` | `cuda::atomic<uint32_t, cuda::thread_scope_system>` | System-scope atomic 32-bit unsigned integer (visible across CPU/GPU) |
| `TUTTI_ATOMIC_U64_DEV` | `cuda::atomic<uint64_t, cuda::thread_scope_device>` | Device-scope atomic 64-bit unsigned integer |
| `TUTTI_ATOMIC_U64_SYS` | `cuda::atomic<uint64_t, cuda::thread_scope_system>` | System-scope atomic 64-bit unsigned integer (visible across CPU/GPU) |
| `TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...)` | `kernel<<<grid, block, shmem, stream>>>(...)` | CUDA kernel launch syntax |
| `TUTTI_THREADFENCE_SYSTEM()` | `__threadfence_system()` | Memory fence visible across entire system (CPU + all GPUs) |

## Required Headers (CUDA)

```cpp
#include <cuda_runtime.h>
#include <cuda/atomic>
```

## Atomic Scope Semantics

| Scope | CUDA Type | Visibility | Use Case |
|-------|-----------|------------|----------|
| `thread_scope_device` | `_DEV` suffix | Single GPU only | Intra-GPU coordination |
| `thread_scope_system` | `_SYS` suffix | All CPUs + all GPUs | Cross-process, DMA coordination, CPU-GPU shared state |

**System-scope atomics** are critical for:
- DMA queue head/tail pointers shared with NVMe controllers
- Multi-GPU coordination via PCIe peer-to-peer
- CPU-GPU producer-consumer queues in pinned memory

## Kernel Launch Macro Expansion

```cpp
// Tutti code:
TUTTI_LAUNCH_KERNEL(my_kernel, dim3(32,1,1), dim3(256,1,1), 0, stream, arg1, arg2);

// Expands to (CUDA):
my_kernel<<<dim3(32,1,1), dim3(256,1,1), 0, stream>>>(arg1, arg2);
```

## Host-Only Fallback Mapping

When no accelerator flag is defined:

| Tutti Macro | Host-Only Fallback | Notes |
|-------------|-------------------|-------|
| `TUTTI_DEVICE` | *(empty)* | Function becomes host-only |
| `TUTTI_GLOBAL` | *(empty)* | Function becomes host-only |
| `TUTTI_HOST` | *(empty)* | No-op |
| `TUTTI_HOST_DEVICE` | `inline` | Standard C++ inline |
| `TUTTI_FORCEINLINE` | `__attribute__((always_inline)) inline` | GCC/Clang force-inline |
| `TUTTI_ATOMIC_U32_DEV` | `uint32_t __attribute__((aligned(4)))` | Plain aligned integer (no atomic ops) |
| `TUTTI_ATOMIC_U32_SYS` | `uint32_t __attribute__((aligned(4)))` | Plain aligned integer (no atomic ops) |
| `TUTTI_ATOMIC_U64_DEV` | `uint64_t __attribute__((aligned(8)))` | Plain aligned integer (no atomic ops) |
| `TUTTI_ATOMIC_U64_SYS` | `uint64_t __attribute__((aligned(8)))` | Plain aligned integer (no atomic ops) |
| `TUTTI_LAUNCH_KERNEL(...)` | `static_assert(false, "...")` | Compile-time error |
| `TUTTI_THREADFENCE_SYSTEM()` | `((void)0)` | No-op |

**Purpose:** Allows host-side code (gRPC services, unit tests, utilities) to compile without CUDA dependency.

## ROCm Backend Mapping (Stub)

When `TUTTI_ACCEL_ROCM` is defined:

| Tutti Macro | ROCm Equivalent | Status |
|-------------|-----------------|--------|
| `TUTTI_DEVICE` | `__device__` | ✅ Ready |
| `TUTTI_GLOBAL` | `__global__` | ✅ Ready |
| `TUTTI_HOST` | `__host__` | ✅ Ready |
| `TUTTI_HOST_DEVICE` | `__host__ __device__ __forceinline__` | ✅ Ready |
| `TUTTI_FORCEINLINE` | `__forceinline__` | ✅ Ready |
| `TUTTI_ATOMIC_U32_DEV` | `unsigned int` | ⚠️ Placeholder (needs HIP atomics) |
| `TUTTI_ATOMIC_U32_SYS` | `unsigned int` | ⚠️ Placeholder (needs HIP atomics) |
| `TUTTI_ATOMIC_U64_DEV` | `unsigned long long` | ⚠️ Placeholder (needs HIP atomics) |
| `TUTTI_ATOMIC_U64_SYS` | `unsigned long long` | ⚠️ Placeholder (needs HIP atomics) |
| `TUTTI_LAUNCH_KERNEL(...)` | `hipLaunchKernelGGL(...)` | ✅ Ready |
| `TUTTI_THREADFENCE_SYSTEM()` | `__threadfence_system()` | ✅ Ready |

**Required Header:** `<hip/hip_runtime.h>`

## SYCL Backend

`TUTTI_ACCEL_SYCL` → **Compile error**

SYCL uses fundamentally different kernel launch model (queue.submit + parallel_for). Cannot be mapped via simple macros. Requires template wrappers and lambda-based kernels. Flagged as open design question.

## CANN Backend

`TUTTI_ACCEL_CANN` → **Compile error** ("not yet implemented")

Huawei Ascend NPU support is a future target.

## Design Philosophy

1. **Single-source portability**: Write once, compile for multiple backends
2. **Zero abstraction overhead**: Macros expand directly to vendor syntax
3. **Graceful degradation**: Host-only builds compile for testing/services
4. **Compile-time safety**: Invalid operations trigger static_assert or compiler errors

## Usage Example

Real consumers of `accel.h` include it as:

```cpp
#include "tutti/abstraction/accel.h"
```

for example `tutti/device_manager/nvme/include/queue_acquire_helper.cuh:18`
(pulls in `TUTTI_DEVICE`, `TUTTI_FORCEINLINE`) and
`tutti/block_storage/include/gpu_file_resolve.cuh:8`.

```cpp
// e.g. tutti/device_manager/nvme/include/queue_acquire_helper.cuh
#include "tutti/abstraction/accel.h"

// Device function (GPU-only)
TUTTI_DEVICE uint32_t sq_next_tail(uint32_t current, uint32_t size) {
    return (current + 1) % size;
}

// Host-device function (callable from both)
TUTTI_HOST_DEVICE bool is_power_of_two(uint64_t n) {
    return n && !(n & (n - 1));
}

// Kernel
TUTTI_GLOBAL void process_completions(
    TUTTI_ATOMIC_U32_SYS* cq_head,
    volatile uint32_t* completions,
    int count
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        // Process completion
        TUTTI_THREADFENCE_SYSTEM();  // Ensure visibility to CPU/NVMe
        cq_head->fetch_add(1, cuda::memory_order_release);
    }
}

// Launch from host
void submit_work(cudaStream_t stream) {
    TUTTI_LAUNCH_KERNEL(
        process_completions,
        dim3(4), dim3(256),
        0, stream,
        cq_head_ptr, completions_ptr, count
    );
}
```

## Build Configuration

```cmake
# tutti/accel/CMakeLists.txt
target_compile_definitions(${LAYER_NAME}
    PUBLIC
        TUTTI_ACCEL_CUDA  # Enable CUDA backend
)
```

Users of `tutti_accel` automatically get `TUTTI_ACCEL_CUDA` defined via transitive PUBLIC propagation.
