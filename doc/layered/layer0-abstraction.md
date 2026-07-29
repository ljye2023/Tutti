# Layer 0: Abstraction (Macro Layer)

**Version:** 1.0  
**Date:** 2026-07-22  
**Status:** Implemented  
**Location:** `tutti/abstraction/accel.h`

---

## Overview

Layer 0 provides **compile-time vendor abstraction** for GPU programming through preprocessor macros. It enables writing vendor-neutral device code that compiles to CUDA, ROCm, SYCL, or host-only targets without runtime overhead.

**Key Properties:**
- Header-only, no library target
- Zero runtime cost (pure preprocessor)
- Single canonical header: `tutti/abstraction/accel.h`
- Vendor selected by the `TUTTI_ACCEL_*` macro that is compiled in (see below)
- No vtables, no dynamic dispatch

**Design Philosophy:**  
Abstraction should be **invisible at runtime**. Layer 0 is purely a compile-time translation layer that maps portable syntax to vendor-specific intrinsics. Generated code is identical to hand-written vendor code.

---

## Primary Functionality

### 1. Function Qualifiers

Portable annotations for where functions execute (host, device, or both).

| Macro | Purpose | CUDA Mapping | Host-only Mapping |
|-------|---------|--------------|-------------------|
| `TUTTI_DEVICE` | Device-only function | `__device__` | Empty (becomes regular function) |
| `TUTTI_GLOBAL` | Kernel entry point | `__global__` | Empty (cannot call in host-only) |
| `TUTTI_HOST` | Host-only function | `__host__` | Empty |
| `TUTTI_HOST_DEVICE` | Dual-compilation function + force inline | `__host__ __device__ __forceinline__` | `inline` |
| `TUTTI_FORCEINLINE` | Force inlining | `__forceinline__` | `__attribute__((always_inline)) inline` |

**Usage Example:**
```cpp
// Device-only helper
TUTTI_DEVICE
uint32_t compute_offset(uint32_t idx, uint32_t stride) {
    return idx * stride;
}

// Kernel entry point
TUTTI_GLOBAL
void my_kernel(int* data, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        data[idx] = compute_offset(idx, 4);
    }
}

// Works on both host and device
TUTTI_HOST_DEVICE
uint64_t hash_combine(uint64_t a, uint64_t b) {
    return a ^ (b + 0x9e3779b9 + (a << 6) + (a >> 2));
}
```

**Design Note:** `TUTTI_HOST_DEVICE` bundles `__forceinline__` because dual-compilation functions are typically small utility functions that should always be inlined to avoid code bloat.

---

### 2. Atomic Types

Portable atomic variables with explicit memory scope control.

| Macro | Purpose | CUDA Mapping | Host-only Mapping |
|-------|---------|--------------|-------------------|
| `TUTTI_ATOMIC_U32_DEV` | 32-bit atomic, device scope | `cuda::atomic<uint32_t, cuda::thread_scope_device>` | `uint32_t __attribute__((aligned(4)))` |
| `TUTTI_ATOMIC_U32_SYS` | 32-bit atomic, system scope | `cuda::atomic<uint32_t, cuda::thread_scope_system>` | `uint32_t __attribute__((aligned(4)))` |
| `TUTTI_ATOMIC_U64_DEV` | 64-bit atomic, device scope | `cuda::atomic<uint64_t, cuda::thread_scope_device>` | `uint64_t __attribute__((aligned(8)))` |
| `TUTTI_ATOMIC_U64_SYS` | 64-bit atomic, system scope | `cuda::atomic<uint64_t, cuda::thread_scope_system>` | `uint64_t __attribute__((aligned(8)))` |

**Memory Scopes:**
- **Device scope** (`_DEV`): Atomic operations are visible only within the same GPU. Use for intra-GPU synchronization (queues, locks, counters).
- **System scope** (`_SYS`): Atomic operations are visible across GPUs and to host. Use for cross-device coordination, DMA completion flags, or host-device signaling.

**Usage Example:**
```cpp
struct DeviceQueue {
    TUTTI_ATOMIC_U32_DEV head;  // Only GPU threads access
    TUTTI_ATOMIC_U32_DEV tail;
    void* slots[256];
};

struct DmaCompletionFlag {
    TUTTI_ATOMIC_U32_SYS ready;  // NVMe controller writes, GPU reads
};

TUTTI_DEVICE
void enqueue(DeviceQueue* q, void* item) {
    uint32_t slot = q->tail.fetch_add(1, cuda::memory_order_relaxed);
    q->slots[slot % 256] = item;
}

TUTTI_DEVICE
bool poll_dma(DmaCompletionFlag* flag) {
    return flag->ready.load(cuda::memory_order_acquire) == 1;
}
```

**Design Note:** Host-only builds fall back to aligned integers because most host-side code is single-threaded (tests, utilities). Multi-threaded host code should use `std::atomic<>` directly.

---

### 3. Kernel Launch

Portable kernel invocation across vendor syntaxes.

| Macro | Purpose | CUDA Mapping | ROCm Mapping |
|-------|---------|--------------|--------------|
| `TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...)` | Launch kernel with arguments | `kernel<<<grid, block, shmem, stream>>>(...)` | `hipLaunchKernelGGL(kernel, grid, block, shmem, stream, ...)` |

**Parameters:**
- `kernel`: Function pointer to `TUTTI_GLOBAL` function
- `grid`: Grid dimensions (`dim3` or `Dim3`)
- `block`: Block dimensions (`dim3` or `Dim3`)
- `shmem`: Shared memory bytes per block
- `stream`: Stream handle (`cudaStream_t`, `hipStream_t`, or `AccelStream`)
- `...`: Variadic kernel arguments

**Usage Example:**
```cpp
TUTTI_GLOBAL
void vector_add(int* a, int* b, int* c, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        c[idx] = a[idx] + b[idx];
    }
}

void launch_example(int* d_a, int* d_b, int* d_c, int n, cudaStream_t stream) {
    dim3 grid((n + 255) / 256);
    dim3 block(256);
    
    TUTTI_LAUNCH_KERNEL(vector_add, grid, block, 0, stream, d_a, d_b, d_c, n);
}
```

**SYCL Incompatibility:**  
SYCL uses `queue.submit([&](handler& h) { h.parallel_for(...); })` which is fundamentally incompatible with a call-expression macro. SYCL support is deferred post-v0.1 and currently blocked with `#error`.

---

### 4. Memory Fencing

Portable system-wide memory fence for cross-device synchronization.

| Macro | Purpose | CUDA Mapping | Host-only Mapping |
|-------|---------|--------------|-------------------|
| `TUTTI_THREADFENCE_SYSTEM()` | System-wide memory fence | `__threadfence_system()` | `((void)0)` (no-op) |

**Use Cases:**
- Ensure GPU writes are visible to NVMe controller (DMA)
- Ensure GPU writes are visible to remote RDMA NICs
- Synchronize across multiple GPUs via PCIe coherence

**Usage Example:**
```cpp
TUTTI_DEVICE
void signal_dma_ready(DmaDescriptor* desc, void* buffer, size_t size) {
    desc->buffer_addr = (uint64_t)buffer;
    desc->size = size;
    
    TUTTI_THREADFENCE_SYSTEM();  // Ensure descriptor write visible to NVMe
    
    desc->ready_flag.store(1, cuda::memory_order_release);
}
```

**Design Note:** `__threadfence_system()` is expensive (PCIe round-trip). Only use when coordinating with external devices (NVMe, RDMA) or other GPUs.

---

## Vendor Support Matrix

| Vendor | Flag | Status | Notes |
|--------|------|--------|-------|
| **CUDA (NVIDIA)** | `-DTUTTI_ACCEL_CUDA` | ✅ Implemented | CUDA 12.6+ required, full feature parity |
| **ROCm (AMD)** | `-DTUTTI_ACCEL_ROCM` | ⚠️ Stub | HIP runtime included, atomics need proper types |
| **SYCL (Intel)** | `-DTUTTI_ACCEL_SYCL` | ❌ Blocked | `#error` due to kernel launch incompatibility |
| **CANN (Huawei)** | `-DTUTTI_ACCEL_CANN` | ❌ Not implemented | `#error`, placeholder only |
| **Host-only** | (no flag) | ✅ Implemented | For unit tests, gRPC services, host utilities |

**Build-time Vendor Selection:**

There is **no** CMake `option()` or cache variable for the vendor. The `tutti_accel`
target hardcodes `TUTTI_ACCEL_CUDA` as a `PUBLIC` compile definition
(`tutti/accel/CMakeLists.txt:64-67`), which propagates transitively to every
consumer. Passing `-DTUTTI_ACCEL_CUDA=ON`/`=OFF` on the CMake command line does
**nothing** — it sets an unused cache entry and the target still compiles with
`TUTTI_ACCEL_CUDA` defined.

To build another vendor (or host-only) today you must edit the
`target_compile_definitions` in `tutti/accel/CMakeLists.txt` and rebuild. A
proper `option()`/cache-driven selector is not yet implemented.

```cmake
# tutti/accel/CMakeLists.txt (current, hardcoded)
target_compile_definitions(tutti_accel
    PUBLIC
        TUTTI_ACCEL_CUDA)
```

**Important:** The `Flag` column in the table above lists the *macro that must be
defined*, not a `-D...=ON/OFF` CMake option. Selecting a vendor other than CUDA
requires editing the CMake definition directly.

**Multiple-flag behavior:** `accel.h` is a plain `#if`/`#elif`/`#else` chain with
no arity check. Defining more than one `TUTTI_ACCEL_*` macro does **not** trigger
a compile error — the first matching branch (`TUTTI_ACCEL_CUDA`, tested first)
silently wins and the others are ignored. It is the build's responsibility to
define at most one; the header does not enforce it.

---

## Complete API Reference

### Function Qualifiers (5 macros)

```cpp
TUTTI_DEVICE          // Device-only function
TUTTI_GLOBAL          // Kernel entry point
TUTTI_HOST            // Host-only function (rarely needed explicitly)
TUTTI_HOST_DEVICE     // Dual-compilation + force inline
TUTTI_FORCEINLINE     // Force inline (any target)
```

### Atomic Types (4 macros)

```cpp
TUTTI_ATOMIC_U32_DEV  // 32-bit atomic, device scope
TUTTI_ATOMIC_U32_SYS  // 32-bit atomic, system scope
TUTTI_ATOMIC_U64_DEV  // 64-bit atomic, device scope
TUTTI_ATOMIC_U64_SYS  // 64-bit atomic, system scope
```

### Operations (2 macros)

```cpp
TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...)  // Launch kernel
TUTTI_THREADFENCE_SYSTEM()                                     // System-wide fence
```

**Total: 11 macros**

---

## Design Decisions

### Decision 1: Single Canonical Header

**Choice:** All abstractions in `tutti/abstraction/accel.h`, vendor selected by build flag.

**Rationale:**
- Prevents macro redefinition conflicts
- Single source of truth for all vendor mappings
- Easy to audit (one file, 97 lines)
- Matches CMake's vendor selection model

**Rejected Alternative:** Per-vendor headers (`accel_cuda.h`, `accel_hip.h`) would require complex include logic and risk inconsistent definitions.

---

### Decision 2: Bundle `__forceinline__` into `TUTTI_HOST_DEVICE`

**Choice:** `TUTTI_HOST_DEVICE` → `__host__ __device__ __forceinline__`

**Rationale:**
- Dual-compilation functions are typically small utilities (hash functions, bit manipulation, address arithmetic)
- Failing to inline them causes code bloat (function appears in both host and device object files)
- Sites wanting dual-compilation without inlining can use `TUTTI_HOST TUTTI_DEVICE` separately
- Matches existing usage in `tutti/block_storage/include/gpu_file_resolve.cuh`

**Rejected Alternative:** Separate `TUTTI_HOST_DEVICE` and `TUTTI_INLINE` would require two annotations at every call site, increasing verbosity.

---

### Decision 3: Explicit Memory Scopes for Atomics

**Choice:** Provide both `_DEV` (device-scope) and `_SYS` (system-scope) variants.

**Rationale:**
- Device-scope atomics are faster (no PCIe coherence traffic)
- System-scope atomics are required for DMA coordination (NVMe, RDMA)
- Making scope explicit prevents accidental use of slow system-scope where device-scope suffices
- CUDA's `cuda::atomic<T, Scope>` already exposes this distinction

**Rejected Alternative:** Single `TUTTI_ATOMIC_U32` defaulting to system-scope would be safe but slow. Device-scope is ~10× faster for intra-GPU atomics.

---

### Decision 4: Host-only Atomics as Aligned Integers

**Choice:** Host-only builds map atomics to `uint32_t __attribute__((aligned(4)))`.

**Rationale:**
- Host-only builds are for unit tests, gRPC services, and utilities (mostly single-threaded)
- Multi-threaded host code should use `std::atomic<>` directly, not device abstractions
- Alignment ensures no miscompile issues on x86-64 (natural atomicity for aligned 32/64-bit)
- Matches existing pattern in libnvm (`nvm_types.h`)

**Rejected Alternative:** Mapping to `std::atomic<>` would add C++ standard library dependency to device headers, blocking nvcc compilation.

---

### Decision 5: Defer SYCL Support

**Choice:** Block SYCL with `#error` until post-v0.1.

**Rationale:**
- SYCL's `queue.submit([&](handler) { parallel_for(...); })` is structurally incompatible with a call-expression macro
- Options (template wrapper, separate `.sycl.cpp` units, C++20 concepts) all require design decisions beyond macro dispatch
- v0.1 targets CUDA only; no SYCL hardware available for testing
- Explicit `#error` prevents accidental misconfiguration

**Future Path:** SYCL support likely requires:
1. Template wrapper: `tutti::launch_kernel<Kernel>(queue, grid, block, args...)`
2. Or separate compilation model: `.cu` for CUDA, `.sycl.cpp` for SYCL

---

## Usage Guidelines

### When to Use Each Qualifier

| Scenario | Use | Example |
|----------|-----|---------|
| Kernel entry point | `TUTTI_GLOBAL` | `TUTTI_GLOBAL void my_kernel(...)` |
| Helper called only from device | `TUTTI_DEVICE` | `TUTTI_DEVICE uint32_t compute_hash(uint64_t key)` |
| Small utility (host + device) | `TUTTI_HOST_DEVICE` | `TUTTI_HOST_DEVICE uint32_t round_up(uint32_t x, uint32_t align)` |
| Large function (host + device) | `TUTTI_HOST TUTTI_DEVICE` (no inline) | Rare, only for debug builds |
| Force inline (any target) | `TUTTI_FORCEINLINE` | `TUTTI_FORCEINLINE void critical_path(...)` |

### When to Use Each Atomic Scope

| Scenario | Use | Rationale |
|----------|-----|-----------|
| Queue head/tail (GPU-only) | `TUTTI_ATOMIC_U32_DEV` | No PCIe traffic, faster |
| Completion counter (GPU-only) | `TUTTI_ATOMIC_U64_DEV` | No PCIe traffic, faster |
| DMA ready flag (GPU ↔ NVMe) | `TUTTI_ATOMIC_U32_SYS` | Must be visible to PCIe device |
| RDMA completion flag | `TUTTI_ATOMIC_U64_SYS` | Must be visible to NIC |
| Multi-GPU barrier | `TUTTI_ATOMIC_U32_SYS` | Must be visible across GPUs |

**Rule of Thumb:** Default to `_DEV` for GPU-only data structures. Only use `_SYS` when coordinating with external devices or other GPUs.

---

## Migration from Raw CUDA

### Before (Direct CUDA)
```cpp
#include <cuda_runtime.h>
#include <cuda/atomic>

__device__ __forceinline__
uint32_t hash_function(uint64_t key) {
    return key % 1024;
}

__global__
void process_kernel(uint64_t* keys, uint32_t* hashes, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        hashes[idx] = hash_function(keys[idx]);
    }
}

void launch(uint64_t* d_keys, uint32_t* d_hashes, int n, cudaStream_t stream) {
    dim3 grid((n + 255) / 256);
    dim3 block(256);
    process_kernel<<<grid, block, 0, stream>>>(d_keys, d_hashes, n);
}
```

### After (Tutti Layer 0)
```cpp
#include "tutti/abstraction/accel.h"

TUTTI_DEVICE TUTTI_FORCEINLINE
uint32_t hash_function(uint64_t key) {
    return key % 1024;
}

TUTTI_GLOBAL
void process_kernel(uint64_t* keys, uint32_t* hashes, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        hashes[idx] = hash_function(keys[idx]);
    }
}

void launch(uint64_t* d_keys, uint32_t* d_hashes, int n, cudaStream_t stream) {
    dim3 grid((n + 255) / 256);
    dim3 block(256);
    TUTTI_LAUNCH_KERNEL(process_kernel, grid, block, 0, stream, d_keys, d_hashes, n);
}
```

**Benefits:**
- Same generated code (zero overhead)
- Compiles on both CUDA and host-only
- Future ROCm port requires only recompilation, no code changes

---

## Testing Strategy

### Validation Criteria

✅ Layer 0 is correct when:

1. **No direct vendor syntax above Layer 0** - All device code uses `TUTTI_*` macros
2. **Host-only build compiles** - Test with no vendor flags
3. **CUDA build compiles** - Test with `-DTUTTI_ACCEL_CUDA`
4. **Generated code is identical** - PTX/SASS matches hand-written CUDA
5. **No local fallback guards** - All `#ifndef __device__` removed

### Test Coverage

**Current Status:** ❌ No dedicated Layer 0 tests

**Justification:** Layer 0 is validated implicitly through Layer 1+ usage:
- Layer 1 tests (`layer1_smoke_test.cu`) use `TUTTI_GLOBAL` for kernel launch
- Backend kernels use all qualifiers and atomics
- Host-only builds (e.g., gRPC services) validate fallback path

**Future:** Consider adding:
1. Compile-only test suite (verify all macro expansions are syntactically valid)
2. Host-only build CI job (catch host fallback breakage)
3. PTX comparison test (ensure macro expansion matches raw CUDA)

---

## Known Limitations

### 1. SYCL Kernel Launch

**Issue:** `TUTTI_LAUNCH_KERNEL` is a call-expression macro, incompatible with SYCL's `queue.submit()` lambda model.

**Impact:** SYCL builds blocked with `#error`.

**Workaround:** Future SYCL support requires template wrapper or separate compilation units.

---

### 2. Shared and Constant Memory

**Issue:** No abstraction for `__shared__` or `__constant__` memory qualifiers.

**Impact:** Device code must use raw `__shared__` and `__constant__`.

**Rationale:** These qualifiers are rarely used in Tutti (most state is in device memory or registers). Adding `TUTTI_SHARED` / `TUTTI_CONSTANT` increases macro count for minimal benefit.

**Future:** If shared/constant memory usage increases, add macros in v0.2.

---

### 3. Warp-level Primitives

**Issue:** No abstraction for warp shuffle, warp vote, or cooperative groups.

**Impact:** Code using warp intrinsics is CUDA-specific.

**Rationale:** Warp primitives are CUDA-specific and don't map cleanly to other vendors (AMD has wavefronts with different sizes, SYCL has sub-groups). Abstracting them requires vendor-specific codepaths anyway.

**Future:** Use `#if defined(TUTTI_ACCEL_CUDA)` guards for warp-specific optimizations.

---

## Implementation Status

| Component | Status | Tested |
|-----------|--------|--------|
| CUDA branch (all 11 macros) | Complete — expands to real CUDA intrinsics (`accel.h:6-31`) | Implicitly via L1+ usage; no dedicated L0 tests |
| Host-only fallback | Complete — qualifiers empty, `TUTTI_HOST_DEVICE`→`inline`, atomics→aligned plain ints, `TUTTI_LAUNCH_KERNEL`→`static_assert(false)`, fence→`((void)0)` (`accel.h:72-96`) | Implicitly (gRPC services, host utilities compile against it) |
| ROCm branch — qualifiers / launch / fence | Complete — `__device__`/`__global__`/`__host__`, `hipLaunchKernelGGL`, `__threadfence_system()` (`accel.h:39-43,51-54`) | Not implemented (build) — no target defines `TUTTI_ACCEL_ROCM`, so this branch is never compiled |
| ROCm branch — atomics | Stub — placeholder plain `unsigned int` / `unsigned long long`, marked `FIXME` (`accel.h:45-49`); no HIP atomic types, so no actual atomicity | Not tested |
| SYCL branch | Intentionally-unimplemented — `#error` (`accel.h:56-64`) | — |
| CANN branch | Intentionally-unimplemented — `#error` (`accel.h:66-70`) | — |
| CMake vendor selector | Not implemented — `TUTTI_ACCEL_CUDA` hardcoded PUBLIC on `tutti_accel`; no `option()`/cache var (`tutti/accel/CMakeLists.txt:64-67`) | — |

---

## Known Issues & Gaps

- **No arity check on vendor macros.** `accel.h` is a plain `#if`/`#elif`/`#else` chain (`accel.h:6,33,56,66,72`). Defining two `TUTTI_ACCEL_*` macros at once does *not* produce a compile error — the first branch tested (`TUTTI_ACCEL_CUDA`) silently wins and the rest are ignored. A future guard (e.g. a summed-arity `static_assert`/`#error`) is needed to make misconfiguration fail loudly.
- **No build-time vendor selector.** `tutti/accel/CMakeLists.txt:64-67` hardcodes `TUTTI_ACCEL_CUDA` as a `PUBLIC` compile definition. There is no `option()` or cache variable, so `-DTUTTI_ACCEL_CUDA=ON/OFF` (and the analogous ROCm flag) on the CMake command line have no effect. Building host-only or ROCm requires editing the CMakeLists by hand.
- **ROCm atomics are non-atomic placeholders.** `TUTTI_ATOMIC_*` map to bare `unsigned int` / `unsigned long long` under `TUTTI_ACCEL_ROCM` (`accel.h:45-49`, marked `FIXME`). Any DMA/completion flag or queue pointer built on these would lose the atomic/scope semantics the CUDA branch provides. Must be replaced with real HIP atomics before the ROCm path is usable — but note the branch is never compiled today.
- **No dedicated Layer 0 tests.** Macro expansions are only validated implicitly through Layer 1+ consumers. There is no compile-only test suite asserting every branch (especially host-only and ROCm) expands to syntactically valid code, so breakage in an unbuilt branch would go unnoticed until that branch is first compiled.
- **No abstraction for `__shared__` / `__constant__` memory or warp-level primitives.** Device code needing these must drop to raw CUDA syntax, defeating the single-source-portability goal for those constructs (see Known Limitations §2, §3).

---

## Dependencies

### Layer 0 Depends On:
- **None** - Pure preprocessor, no dependencies

### Layers That Depend on Layer 0:
- **Layer 1** (Accelerator HAL) - Uses `TUTTI_GLOBAL` for test kernels
- **Layer 3** (Backends) - Uses all qualifiers and atomics in device code
- **Layer 4** (IO Engine) - Uses atomics for completion tracking
- **Layer 5** (Block Storage) - Uses `TUTTI_HOST_DEVICE` for file resolution

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-07-22 | Initial implementation (CUDA + host-only) |

---

## References

- **Implementation:** `tutti/abstraction/accel.h`
- **Architecture:** `doc/layered/architecture-overview.md` (L0–5 connective overview)
- **Layer 1 (Next layer):** `doc/layered/layer1-accelerator-hal.md`
- **CUDA Programming Guide:** https://docs.nvidia.com/cuda/cuda-c-programming-guide/
- **HIP Programming Guide:** https://rocmdocs.amd.com/en/latest/Programming_Guides/HIP-GUIDE.html
