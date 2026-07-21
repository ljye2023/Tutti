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

  #include <cstdint>

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
