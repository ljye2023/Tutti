# Open Design Questions

> Decisions that need resolution before or during implementation.

These questions were surfaced during the API design analysis. Some are **blockers** for v0.1 (must be resolved before coding starts); others are **deferrable** (can be punted to post-v0.1 without breaking the design).

---

## Q1: SYCL Kernel Launch Wrapper Design ⚠️ **BLOCKER for SYCL support, NOT v0.1**

**Context**: SYCL's `queue.submit([&](handler& cgh) { cgh.parallel_for(...); })` model is structurally incompatible with a call-expression macro like `TUTTI_LAUNCH_KERNEL`.

**Options**:

1. **Template wrapper function** — lose `<<<>>>` syntax, gain portability:
   ```cpp
   template<typename Kernel, typename... Args>
   void tutti_launch(AccelStream stream, dim3 grid, dim3 block, size_t shmem, Kernel k, Args... args);
   ```
   - ✅ Works for CUDA, ROCm, SYCL
   - ❌ Loses CUDA `<<<>>>` familiarity
   - ❌ Requires C++17 fold expressions

2. **Separate `.sycl.cpp` compilation units** — duplicate device code:
   ```cpp
   // nvme_submit.cu (CUDA)
   __global__ void nvme_submit_kernel(...) { ... }
   
   // nvme_submit.sycl.cpp (SYCL)
   void nvme_submit_kernel(sycl::handler& cgh, ...) { cgh.parallel_for(...); }
   ```
   - ✅ Each platform uses native idioms
   - ❌ Code duplication
   - ❌ Maintenance burden

3. **C++20 concepts + overload resolution** — requires C++20:
   ```cpp
   template<AcceleratorConcept T>
   void launch(T& accel, ...);  // dispatch to CUDA or SYCL impl via concept
   ```
   - ✅ Single codebase
   - ❌ Requires C++20 (current Tutti is C++17)

**Current Decision**: **Defer to post-v0.1.** Block SYCL compilation with `#error` in `tutti/abstraction/accel.h`. v0.1 targets CUDA only; SYCL support is explicitly out of scope.

**When to revisit**: When a concrete SYCL target (Intel GPU or similar) becomes a requirement.

---

## Q2: TUTTI_SHARED and TUTTI_CONSTANT Macros ⚠️ **Deferrable**

**Context**: Should we abstract `__shared__` and `__constant__` memory qualifiers?

**Observation**: Current device code uses `__shared__` directly in 3 places:
- `backends/local/nvme/libnvm/src/queue.cu` — shared memory for SQ/CQ staging
- `io_engine/src/local_nvme/launch_batch.cu` — shared memory for descriptor blocks
- `block_storage/src/gpu_file_resolve.cu` — shared memory for extent cache

**Options**:

1. **Add to abstraction layer**:
   ```cpp
   #define TUTTI_SHARED   __shared__    // CUDA/ROCm
   #define TUTTI_CONSTANT __constant__  // CUDA/ROCm
   ```
   - ✅ Consistent with existing `TUTTI_DEVICE` pattern
   - ❌ SYCL doesn't have direct equivalents (uses local accessors, different API)

2. **Leave as-is** — `__shared__` is widely understood, SYCL port will need manual rewrite anyway
   - ✅ No extra abstraction cost
   - ❌ Inconsistent with the "no raw CUDA annotations" principle

**Current Decision**: **Defer to post-v0.1.** Leave `__shared__` and `__constant__` as-is for now. Add `TUTTI_SHARED` / `TUTTI_CONSTANT` if/when SYCL port starts and a clear need emerges.

**Rationale**: SYCL's local memory model is fundamentally different (accessors, not qualifiers). A macro won't bridge this gap cleanly.

---

## Q3: IO-Slice Table Caching ⚠️ **Deferrable**

**Context**: Should the IO Engine cache `SubSliceInfo[]` on `MemoryRegion`, or recompute per-batch?

**Current v0.1**: `IMemorySubsystem::register_tensor()` precomputes and caches IO-slice tables on `MemoryRegion`.

**New design options**:

1. **Cache on MemoryRegion** (v0.1 pattern):
   ```cpp
   struct MemoryRegion {
       // ...
       std::vector<SubSliceInfo> io_slices;  // cached at register time
   };
   ```
   - ✅ Zero per-batch computation
   - ❌ Stale if `max_io_size` changes (backend hot-swap scenario)
   - ❌ Memory overhead (256 MiB region @ 128 KiB max_io → 2048 entries × 24 bytes = 48 KiB)

2. **Recompute per-batch** (proposed):
   ```cpp
   for (size_t off = 0; off < req.byte_length; off += max_io) {
       slices.push_back({...});  // compute on-the-fly
   }
   ```
   - ✅ No stale state
   - ✅ No memory overhead
   - ❌ ~1-2 μs compute per batch (negligible compared to IO latency)

3. **Hybrid: cache only for "hot" regions** — complexity without clear benefit

**Current Decision**: **Recompute per-batch** for v0.1. If profiling shows this is a bottleneck (unlikely), revisit.

**Rationale**: IO latency is 10-100 μs; 1 μs of fan-out compute is <1% overhead. Simpler code wins.

---

## Q4: DMA Mapping — Part of register_* or Separate? ✅ **RESOLVED**

**Context**: Should `IAccelerator::register_host()` implicitly call `dma_map()`, or require a separate call?

**Options**:

1. **Implicit** — `register_*` does DMA-map automatically:
   ```cpp
   MemoryRegion* region = accel->register_device(ptr, size, device_id);
   // region->dma_ioaddrs already populated
   ```
   - ✅ Fewer API calls
   - ❌ Caller can't defer DMA-map to lazy (e.g., register early, map on first use)

2. **Explicit** (proposed) — separate calls:
   ```cpp
   MemoryRegion* region = accel->register_device(ptr, size, device_id);
   accel->dma_map(region, device_id, &ioaddrs, &count);  // explicit
   ```
   - ✅ Caller controls when DMA-map happens
   - ✅ Supports multi-device scenarios (map once, use on multiple GPUs)
   - ❌ Extra API call (minimal cost)

**Decision**: **Explicit `dma_map()` as a separate method.** This is how the design doc specifies it.

**Rationale**: Flexibility for multi-GPU and lazy-map scenarios outweighs the minor verbosity.

---

## Q5: MemoryRegion Ownership — HAL or Shared Types? ✅ **RESOLVED**

**Context**: Should `MemoryRegion` live in `tutti/accel/memory_region.h` (HAL-owned) or `tutti/types/memory_region.h` (shared types)?

**Observation**: `MemoryRegion` is created by HAL, consumed by backends (descriptor build) and IO Engine (fan-out).

**Decision**: **HAL-owned** (`tutti/accel/memory_region.h`). This is how the design doc specifies it.

**Rationale**: `MemoryRegion` is the HAL's registry token. Backends and IO Engine include HAL headers anyway (they depend on `IAccelerator`), so no circular dependency issue.

---

## Q6: VDevice Granularity — Static vs. Dynamic? ⚠️ **Deferrable**

**Context**: Should `IVirtualNvme` support dynamic borrow/return of QPs, or static bootstrap-only allocation?

**Current design**: Static allocation at bootstrap — `open_vdevice(quota)` carves a slice once, backend holds it for lifetime.

**Alternative**: Dynamic borrow/return:
```cpp
VDevice* vdev = virtual_nvme->borrow_vdevice(phys_id, quota, priority);
// ... use it ...
virtual_nvme->return_vdevice(vdev);  // Returns QPs to pool for other backends
```

**Pros of dynamic**:
- ✅ Better QP utilization if backends have bursty workloads
- ✅ Supports over-subscription (quota > physical QPs, time-shared)

**Cons of dynamic**:
- ❌ More complex (priority queues, contention, starvation risk)
- ❌ v0.1 workloads don't need it (single backend per process)

**Current Decision**: **Static for v0.1.** Defer dynamic allocation to post-v0.1 if multi-backend or over-subscription becomes a requirement.

---

## Q7: IVirtualNvme — Transport-Neutral or NVMe-Specific? ✅ **RESOLVED**

**Context**: Should the Level-2 allocator be generic (`IVirtualDevice`) or NVMe-specific (`IVirtualNvme`)?

**Decision**: **NVMe-specific** (`IVirtualNvme`). The name explicitly says "Nvme."

**Rationale**:
- GDS and RDMA backends don't use queue-pairs — no `VDevice` analog
- Forcing a generic `IVirtualDevice` would create an awkward `nullptr` path for 2/3 of backends
- YAGNI — no concrete non-NVMe use case for Level-2 virtualization yet

**Future**: If a new backend (e.g., FPGA DMA) needs similar virtualization, create a separate `IVirtualFpga` rather than over-generalizing.

---

## Q8: Backend initialize() — VDevice* or BackendInitParams Union? ⚠️ **Deferrable**

**Context**: `IBackendProvider::initialize(VDevice* vdev)` works for NVMe, but non-NVMe backends receive `nullptr`. Is this elegant?

**Alternative**: Tagged union:
```cpp
struct BackendInitParams {
    enum { NVME, GDS, RDMA } kind;
    union {
        VDevice* nvme;
        struct { void* cufile_driver; } gds;
        struct { ibv_context* ctx; } rdma;
    };
};

virtual bool initialize(const BackendInitParams& params) = 0;
```

**Pros**:
- ✅ Type-safe for each backend kind
- ✅ Extensible to new backends without SPI change

**Cons**:
- ❌ Over-engineering for v0.1 (only NVMe backend exists)
- ❌ Coordinator must know each backend's init params (couples layers)

**Current Decision**: **Direct `VDevice*` for v0.1.** Non-NVMe backends check `if (vdev != nullptr)` and log a warning. Revisit if GDS/RDMA backends are added.

---

## Q9: StorageTarget Extents — Inline or Pointer? ✅ **RESOLVED**

**Context**: `StorageTarget::nvme_raw.extents` is a pointer to avoid union size explosion. Is this the right choice?

**Decision**: **Pointer** (`std::vector<LbaExtent>*`). This is how the design doc specifies it.

**Rationale**:
- A single-extent file (common case) still has `sizeof(std::vector<LbaExtent>) = 24 bytes` inline
- Multi-extent files (fragmentation) would balloon the union to hundreds of bytes
- Pointer keeps `sizeof(StorageTarget) ≤ 128 bytes`

**Caveat**: Caller must manage the lifetime:
```cpp
std::vector<LbaExtent> extents = {{0x1000, 0x4000}};
StorageTarget target = {
    .kind = NVME_RAW,
    .nvme_raw = {.extents = &extents}  // pointer into caller's stack
};
backend->acquire_target_handle(target);  // backend copies extents to GPU
// extents can go out of scope after acquire_target_handle returns
```

---

## Q10: IO-Slice Fanout — Recompute or Cache? ✅ **RESOLVED** (see Q3)

**Decision**: **Recompute per-batch.** See Q3 for full analysis.

---

## Q11: slice_fanout() — Account for Non-Contiguous ioaddrs? ⚠️ **Deferrable**

**Context**: `IIoEngine::slice_fanout(MemoryRegion*)` currently assumes:
```cpp
return (region->size + max_io - 1) / max_io;  // simple division
```

But what if `region` has non-contiguous `dma_ioaddrs`? (e.g., two separate DMA ranges)

**Current Decision**: **Assume contiguous for v0.1.** CUDA `cudaMalloc` always produces contiguous physical memory. Non-contiguous scenarios only arise with:
- External memory (IPC imports with fragmented mappings)
- RDMA scatter-gather lists

**When to revisit**: When adding RDMA backend or supporting externally-fragmented buffers.

**Fix if needed**: Replace simple division with walk over `ioaddrs[]` to count actual descriptor boundaries.

---

## Q12: IRawDevice LBA Range Validation ⚠️ **Deferrable**

**Context**: Should `IRawDevice::acquire_raw_target()` validate that `start_lba + length ≤ namespace_capacity`?

**Options**:

1. **Validate** — reject invalid ranges:
   ```cpp
   if (start_lba + length_blocks > ns_capacity_blocks(device, ns_id)) {
       *error = "LBA range exceeds namespace capacity";
       return nullptr;
   }
   ```
   - ✅ Fail fast with clear error
   - ❌ Performance cost (extra ioctl to query namespace size)

2. **Trust caller** (proposed) — no validation:
   ```cpp
   // Caller responsible for valid LBA ranges
   ```
   - ✅ Zero overhead
   - ❌ Invalid LBA → NVMe controller error at submit time (harder to debug)

**Current Decision**: **Trust caller for v0.1.** Add optional debug-mode checks (`#ifdef TUTTI_DEBUG_RAW_DEVICE`) in post-v0.1 if field issues arise.

**Rationale**: Applications managing raw LBA allocation (databases, KV stores) already have validation logic. Duplicating it adds overhead for no benefit in production.

---

## Q13: GpuFile and Raw Device — Common Base Interface? ⚠️ **Deferrable**

**Context**: Should `IBlockStorage` and `IRawDevice` inherit from a common `IStorageInterface`?

**Observation**: Their APIs are quite different:
- `IBlockStorage`: directory, names, persistence, multi-shard
- `IRawDevice`: LBA passthrough, no names, no persistence

**Decision**: **No common base for v0.1** (YAGNI). Their APIs diverge enough that a common interface would be a thin shell with no reusable code.

**When to revisit**: If we find ourselves writing polymorphic code that switches on "file vs. raw" at runtime. Currently, applications choose one interface or the other at compile time.

---

## Summary Table

| Question | Status | Blocks v0.1? | Decision |
|---|---|---|---|
| Q1: SYCL launch wrapper | DEFERRED | ❌ No | `#error` for SYCL, revisit when needed |
| Q2: TUTTI_SHARED macro | DEFERRED | ❌ No | Leave `__shared__` as-is |
| Q3: IO-slice caching | ✅ RESOLVED | ❌ No | Recompute per-batch |
| Q4: DMA map explicit | ✅ RESOLVED | ❌ No | Separate `dma_map()` call |
| Q5: MemoryRegion ownership | ✅ RESOLVED | ❌ No | HAL-owned |
| Q6: VDevice dynamic alloc | DEFERRED | ❌ No | Static for v0.1 |
| Q7: IVirtualNvme scope | ✅ RESOLVED | ❌ No | NVMe-specific |
| Q8: initialize() signature | DEFERRED | ❌ No | Direct `VDevice*` |
| Q9: StorageTarget extents | ✅ RESOLVED | ❌ No | Pointer |
| Q10: slice_fanout cache | ✅ RESOLVED (→Q3) | ❌ No | Recompute |
| Q11: Non-contiguous ioaddrs | DEFERRED | ❌ No | Assume contiguous |
| Q12: Raw LBA validation | DEFERRED | ❌ No | Trust caller |
| Q13: Common storage base | DEFERRED | ❌ No | No common interface |

**Conclusion**: No open questions block v0.1 implementation. All blockers are for post-v0.1 features (SYCL, multi-backend, over-subscription).
