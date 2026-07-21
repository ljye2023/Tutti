# Layer 0 + Layer 1 Verification Result

> Scope: `tutti/abstraction/` (Layer 0) and `tutti/accel/` (Layer 1), verified
> against [layered-architecture-redesign.md](layered-architecture-redesign.md)
> §3.1, §3.2, §8.1, §8.2. DMA mapping is explicitly out of scope (not yet
> implemented). No other layer was built or modified.

## Build

Configured and built standalone via `tutti/CMakeLists.txt` (which currently
wires only Layer 0 + Layer 1 + `tests/`):

```
cmake ../tutti -DCMAKE_CUDA_ARCHITECTURES=89
make
```

Result: `libtutti_accel.a` and `tests/layer1_smoke_test` build clean, CUDA
12.8, GCC 11.4, on an NVIDIA L40S (sm_89). No other layer's CMake target was
added or built.

## Design conformance

**Layer 0 (`tutti/abstraction/accel.h`)** — matches §3.1 / §8.1: exactly one
`TUTTI_ACCEL_*` vendor macro gates the branch; qualifiers (`TUTTI_DEVICE`,
`TUTTI_GLOBAL`, `TUTTI_HOST_DEVICE`, `TUTTI_FORCEINLINE`), atomics, and
`TUTTI_LAUNCH_KERNEL` are defined for CUDA/ROCm; SYCL/CANN are correctly
stubbed as open questions per the doc. Not yet consumed anywhere in Layer 1
(expected — HAL device-side generic helpers aren't written yet).

**Layer 1 (`tutti/accel/`)** — the host-side `IAccelerator` surface matches
§3.2 / §8.2 capability-by-capability: identity, allocation (host / pinned /
device / managed via `MemoryKind`), `MemoryRegion` registry (register /
lookup by pointer and id / unregister), host↔device pointer translation,
stream/event lifecycle, `memcpy_async`, `launch`, and IPC export/import are
all present in `iaccel.h` and implemented in `cuda_accelerator.cu`.

**Key invariant held**: `cuda_runtime.h` is confined to
`accel/include/cuda/cuda_accelerator.h` and `accel/src/cuda/cuda_accelerator.cu`.
It does not appear in `accel/include/common/{accel_types,memory_kind,memory_region,iaccel}.h`.

DMA mapping (`dma_map`/`dma_unmap`, `ioaddrs`) is absent, as expected.

## Bug found and fixed

`CudaAccelerator::allocate_device(size, MemoryKind::DEVICE, device_id)`
computed a 64KB-aligned sub-pointer (needed later for
`nvm_dma_map_data_device`-style DMA mapping) but then discarded it and
returned the raw, unaligned `cudaMalloc` pointer instead — the alignment
logic was dead code.

Fix (`tutti/accel/src/cuda/cuda_accelerator.cu`,
`tutti/accel/include/cuda/cuda_accelerator.h`):
- `allocate_device()` now returns the actual aligned pointer.
- Since `cudaFree()` requires the exact pointer `cudaMalloc()` returned, added
  an `aligned_to_raw_` map that records aligned→raw on allocation; `free()`
  consults it to recover the raw pointer before calling `cudaFree()`.

## Verification of the fix

Added a regression test, `test_device_memory_alignment_and_leak`, to
`tests/layer1_smoke_test.cu`:
- asserts the pointer from `allocate_device(DEVICE)` is 64KB-aligned
- runs 200 alloc/free cycles and checks `cudaMemGetInfo` drift stays near
  zero (catches both the leak that the old discard-and-return-raw path would
  have caused and any regression in the new lookup path)

Results:

```
$ ./bin/layer1_smoke_test
...
[TEST] Device memory alignment + leak regression
  Free device memory before: 47102164992, after: 47102164992, drift: 0 bytes
  [PASS] test_device_memory_alignment_and_leak: Device memory alignment and leak regression succeeded
...
Passed: 11
Failed: 0
Total:  11
✓ All tests passed!
```

```
$ compute-sanitizer --tool memcheck --leak-check full ./bin/layer1_smoke_test
...
Passed: 11 / Failed: 0
========= LEAK SUMMARY: 0 bytes leaked in 0 allocations
========= ERROR SUMMARY: 0 errors
```

## Overall verdict

Layer 0 and Layer 1 (excluding DMA) faithfully implement the design doc.
The one implementation bug found (misaligned/leak-prone `allocate_device`)
is fixed and covered by a regression test. No other layer was touched.
