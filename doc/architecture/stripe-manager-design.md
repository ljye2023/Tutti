# Stripe Manager Design: An IO-Time Logical→Physical Mapper

> **Status**: Design proposal (rev 2), extends
> [layered-architecture-redesign.md](layered-architecture-redesign.md).
> Supersedes the earlier `block-layer-design.md`. This revision replaces the
> "block layer" name with **stripe manager**, corrects the design to match the
> code that actually exists after the Layer 3 (`IBackend`) migration, and
> scopes the work to a single-backend / multi-vdevice model with a
> single-request-first rollout.

## 0. Scope and Assumptions (read this first)

This document is deliberately narrow. The decisions below are fixed for this
phase; everything outside them is out of scope.

1. **One backend.** The IO Engine talks to exactly **one** `IBackend`
   (concretely `NvmeBackend` today). We do not orchestrate across multiple
   backends. A single batch still targets a single backend.
2. **Multiple vdevices behind that backend.** Striping means distributing a
   logical IO across the **vdevices** owned by that one backend, selected by
   `VDeviceHandle` (a dense index). Striping is NOT across backends.
3. **Single request first.** We implement and validate `submit_one` (one
   logical `IoRequest` → 1..K vdevices) end-to-end before generalizing to
   multi-request batches.
4. **Block Storage adaptation is deferred.** The stripe manager lives at the
   IO Engine layer and reuses the *math* of `block_storage::StripeManager`, but
   we do NOT adapt Block Storage in this phase. Changes here MAY be breaking to
   Block Storage; that is accepted for now.
5. **IO Engine depends on the Backends layer.** We accept this explicitly. The
   engine will bind to the concrete `NvmeBackend` (see §4, Option A).
6. **Alignment and fanout-estimation are implementation details**, refined as
   the sharding logic matures. Not designed in full here.

## 1. Terminology and the Rename

The name "block layer library" is dropped. The new component is the
**stripe manager**. Note there are now **two** stripe managers with distinct
responsibilities and lifetimes — do not conflate them:

| | Allocation-time StripeManager (exists) | IO-time StripeManager (new) |
|---|---|---|
| **Location** | `block_storage/src/stripe_manager.{h,cpp}`, ns `block_storage` | `io_engine/`, ns `io_engine` |
| **When** | File creation | Every read/write |
| **Input** | file_size, stripe_size | logical offset + length on an open target |
| **Output** | `std::vector<FileShard>` (placement) | `std::vector<SubIo>` (which vdevice + physical offset + buffer slice) |
| **Direction** | write-side (decides where data goes) | read-side dual (finds where data already is) |
| **State** | per-namespace LBA allocators (mutable) | stateless mapper over a fixed shard geometry |

To keep the two unambiguous in code, the new one lives under the
`tutti::io_engine` namespace (`io_engine::StripeManager`). If a distinct name
is preferred later (e.g. `StripeMapper`), only this document and the new files
change — the allocation-time class is untouched.

**Critical correction from reading the code**: the existing allocation layout
is **contiguous-per-shard (linear concatenation)**, NOT interleaved
round-robin. Shard 0 holds logical bytes `[0, s0)`, shard 1 holds `[s0, s0+s1)`,
etc., where `s_i = min(stripe_size, remaining)`. Device placement is
**greedy least-loaded** (picks the namespace with the fewest `allocated_blocks`
at allocation time), evaluated per-shard and stateful across files. The
`next_device_index_` member exists but is unused; there is no `i % n`
round-robin. **Consequence: the IO-time mapper MUST read `shards[]` to learn
placement — it cannot recompute which vdevice a shard lives on by index
arithmetic.**

## 2. The Real Starting Point: IO Engine Does Not Compile (Phase 0)

Before any striping work, the IO Engine must be migrated off the deleted
`IBackendProvider` onto the new `IBackend`. This is not optional cleanup — the
engine cannot build today. Evidence (from reading the tree, not a build run):

- `io_engine_impl.h:8`, `io_engine_impl.cpp:8`, and
  `local_nvme/local_nvme_io_engine.h:9` all `#include
  "backends/include/backend_provider.h"` — **this file was deleted** in commit
  cc430c4. Fatal preprocessor error.
- `backends::IBackendProvider` is referenced ~10 places and **defined
  nowhere**. The SPI is now `backends::IBackend` + concrete
  `backends::nvme::NvmeBackend`.
- `backend_->max_io_size()` (called at `io_engine_impl.cpp:90,194,313`) **no
  longer exists**. Max IO size is now the field `metadata().max_io_size`
  (filled from `nvme_vdevices_[0]->max_data_size`, i.e. MDTS).
- `backends::BufferDescriptor` / `backends::SubSliceInfo` **moved** to
  `backends::nvme::` scope (`nvme_io_types.h`). Field layouts are unchanged;
  only the namespace moved.
- `prepare_descriptors` / `release_descriptors` / `launch_batch_gpu_stream` /
  `acquire_target_handle` are **not on `IBackend`** — they are non-virtual
  members of concrete `NvmeBackend` (backend.h deliberately keeps
  transport-specific submission off the SPI).
- `libtutti_io_engine.a` is dated 2026-07-25, predating the L3 migration — it
  was built against now-deleted symbols.

### 2.1 Phase 0 Migration Steps

1. **Delete the dead include, retype the pointer.** Remove the three
   `backend_provider.h` includes; include `backends/include/backend.h` and
   `backends/nvme/include/nvme_backend.h`. Replace every
   `backends::IBackendProvider*` with `backends::nvme::NvmeBackend*`. Drop the
   `class IBackendProvider;` forward-decls (`io_engine.h:16`,
   `io_engine_impl.h:18`).
2. **Bind to concrete `NvmeBackend*` (Option A).** Because
   `prepare_descriptors` / `launch_batch_gpu_stream` are not virtuals on
   `IBackend`, the engine binds to the concrete type for this phase. This
   re-couples Layer 4 to NVMe — accepted per §0.5. When a second backend
   appears, lift these onto a narrow `IBatchSubmitter` SPI (Option B, deferred).
3. **Fix type namespaces.** `backends::BufferDescriptor` →
   `backends::nvme::BufferDescriptor`; same for `SubSliceInfo`. Field names are
   unchanged, so no field edits.
4. **Cache `max_io_size`.** In the ctor, read `metadata().max_io_size` once into
   a member; replace the three `backend_->max_io_size()` calls. Keep the `== 0`
   guard (empty roster returns 0). Correct because MDTS is fixed after
   `initialize()`.
5. **Wire construction.** Caller constructs `NvmeBackend`, calls
   `initialize(dm, BackendConfig{phys_id, vdevice_count, quota_per_vdevice})`
   **before** building the engine (metadata depends on a populated roster).
6. **Rewrite Layer 4 test doubles.** `MockBackendProvider` derived from the
   deleted interface. For CI, test against `NvmeBackend` + the mock Device
   Manager (real `prepare_descriptors` runs on mock vdevices); gate real HW
   behind `TUTTI_NVME_REAL_HW`.
7. **Clear stale artifacts and rebuild** via `tutti/build.sh` (vcpkg toolchain).

**Exit criterion for Phase 0**: the engine compiles and links against the new
backends, and the existing single-target `submit_batch` still works end-to-end
against one vdevice (no striping yet). Only then do we add the stripe manager.

## 3. The vdevice-selection seam (how a shard reaches one vdevice)

Reading the NVMe backend clarified the exact mechanism, and it fits the
one-backend/many-vdevice model cleanly:

1. `backend->vdevice_handle_at(i)` → `VDeviceHandle{i}` (a dense index; not a
   pointer, because NVMe `submit_one` is a `__device__` function that only needs
   an index into the roster).
2. `backend->acquire_target_handle(StorageTarget, VDeviceHandle)` is **the**
   binding call. It resolves `NvmeVirtualDevice* = nvme_vdev_at(hdl.index)` and
   **bakes that vdevice's queue slice** (`d_qps`, `queue_quota`) inline into a
   GPU-resident `NvmeFileDeviceHandle` (cudaMalloc + H2D copy). After this, the
   opaque `target_handle` **fully encodes both the extent map and the chosen
   vdevice** — `VDeviceHandle` is not needed downstream.
3. `prepare_descriptors` is **vdevice-agnostic** (shared PRP cache). Targeting is
   carried entirely by `target_handle`, not by descriptors.
4. On device, `select_queue(queue_quota) = global_tid % queue_quota` hashes
   threads across *that vdevice's own* queue pairs, so every command lands on a
   queue belonging to the targeted vdevice.

**Design consequence**: the stripe manager only needs to emit, per sub-IO, a
`VDeviceHandle` (which vdevice) + a physical offset + a buffer slice. The engine
turns `(StorageTarget, VDeviceHandle)` into a `target_handle` via
`acquire_target_handle`, then submits. The stripe manager itself **never touches
the backend**; it is pure math.

### 3.1 Decision: the target_handle cache lives inside NvmeBackend

A `target_handle` is a **backend/transport-private object** — concretely an
`NvmeFileDeviceHandle` full of NVMe-only fields (PRP, `nvm_queue_t*`, LBA
extents, GPU device pointers). The IO Engine only ever holds it as an opaque
`void*`; it cannot interpret it, and an RDMA/GDS backend's handle would look
nothing like it. Therefore **handle materialization, caching, and freeing are
owned by the concrete `NvmeBackend`**, not by the engine and not lifted onto the
device-agnostic `IBackend`. File/extent semantics deliberately live in the NVMe
backend for this phase (accepted per §0.5; not abstracted).

Concretely:
- `acquire_target_handle(StorageTarget, VDeviceHandle)` becomes **get-or-create**:
  on a cache hit it returns the existing GPU handle; on a miss it `cudaMalloc`s a
  new one and caches it. Guarded by the existing `target_handles_mutex_`.
- **Cache key = (target_id, shard physical range, vdev_index)** — NOT vdev_index
  alone. One vdevice hosts handles for many shards (§ the cardinality table), so
  keying on the vdevice would collapse distinct shards' extents into one handle.
  The key pairs "which data" (extents) with "which vdevice" (queue slice).
- **Release = simplest option**: cached handles live until `shutdown()`, which
  already `cudaFree`s every tracked handle. The engine **stops calling
  `release_target_handle` per-IO**. A per-file `invalidate(target_id)` is
  deferred (see §9).

**Two distinct "release" operations — do not conflate:**
- `release_descriptors` (returns PRP-list pages to the PRP cache) — **unchanged,
  still per-batch**. This must keep running or GPU memory leaks.
- `release_target_handle` (frees a GPU handle) — **now shutdown-only** under the
  cache model above.

The engine's role shrinks to: run the stripe manager → get `(shard, vdev)` →
call `backend.acquire_target_handle(shard_target, vdev)` (cached) → use the
returned `void*`. It never caches or frees handles per-IO. The upper layer
(Block Storage) remains the sole **source** of each shard's `StorageTarget`
(extents = filesystem semantics); the engine merely **passes it through** to the
backend, which materializes it.

## 4. Data Structures

### 4.1 Inputs the mapper reads (all already exist)

The IO-time mapper is the read-side dual of allocation. Its inputs come from the
open file's shard geometry — **not** from a single `stripe_size` field:

- **Ordered shard list**: `GpuFile.shards` (`std::vector<FileShard>`), index =
  logical shard number. `FileShard{device_id, namespace_id, start_lba,
  length_blocks}`.
- **Per-shard byte length**: derived as `length_blocks * block_size`. Do **not**
  trust `GpuFile.stripe_size` — `allocate_shards` may shrink the effective
  stripe (clamped by `max_shards_per_file` or namespace count) and does **not**
  persist it back. Build a prefix-sum of real shard byte-lengths instead.
- **Block size (bytes per LBA)**: **must be an explicit per-namespace input**,
  sourced from `NamespaceInfo` / `get_namespace_info(ns_id)`. Two conflicting
  values exist in the tree today — the allocator hardcodes **512**
  (`length_blocks = (shard_size+511)/512`), while `StorageTarget` defaults to
  **4096**. The mapper must fix on one namespace-derived value and use the same
  one the allocator used, or LBAs are off by 8×.
- **Backend submission handle per shard**: obtained via `acquire_target_handle`
  as in §3 (not read from Block Storage in this phase).

### 4.2 `SubIo` — the mapper's output (new)

One physical piece of a logical request:

```
SubIo {
  VDeviceHandle vdev;            // which vdevice (from FileShard placement)
  uint64_t      physical_offset; // byte offset within that vdevice's space
                                 // = shard.start_lba * block_size + off_in_shard
  uint64_t      byte_length;     // size of this piece
  uint64_t      region_byte_offset; // offset within the logical MemoryRegion
                                    // (for buffer slicing / ioaddr indexing)
}
```

`region_byte_offset` lets the engine pick the right `ioaddr_index` from
`region->backend_private` for each piece, so a striped read scatters into the
correct sub-ranges of one registered buffer.

### 4.3 What we deliberately drop from rev 1

- `StripingMetadata` embedded in `StorageTarget` — **removed**. Placement is read
  from `shards[]`, not carried on the target.
- `shard_to_backend` — **removed**. Wrong granularity; replaced by per-`SubIo`
  `VDeviceHandle` (§0.2, §3).
- `IStripingCalculator` strategy registry / RAID1 / erasure coding — **deferred**.
  One concrete contiguous-per-shard mapper for now.

## 5. The Mapping Math (what the new mapper implements)

Given a logical byte offset `L` and length `S` on an open target:

1. Find shard `i` such that `prefix[i] <= L < prefix[i+1]`, where `prefix` is the
   cumulative sum of shard byte-lengths (`length_blocks * block_size`).
2. `off_in_shard = L - prefix[i]`.
3. `physical_offset = shards[i].start_lba * block_size + off_in_shard`;
   `vdev = handle_for(shards[i])`.
4. If `L + S` crosses `prefix[i+1]`, **split at the shard boundary** into
   multiple `SubIo`s; `region_byte_offset` of each = its logical start `- L`.
5. Each `SubIo` may still exceed the backend's `max_io_size` (MDTS) → the engine
   fans it into transport-sized chunks (existing STEP 3 logic, scoped per SubIo).

**First-cut simplification**: a trivial 1-shard identity mapper (whole request →
one vdevice) is enough to prove the wiring in the single-request milestone. The
prefix-sum/split logic lands once the identity path is verified.

## 6. Single-Request Flow (the first milestone)

We add a `submit_one` entry point and get it correct before touching batching.
`submit_batch` later becomes a loop over this path.

```
submit_one(IoRequest req, bool is_read, AccelStream stream)
  1. Map:    stripe_manager.map(target, req.byte_offset, req.byte_length)
             → vector<SubIo>   (identity = 1 SubIo in the first cut)
  1. Map:    stripe_manager.map(target, req.byte_offset, req.byte_length)
             → vector<SubIo>   (identity = 1 SubIo in the first cut)
  2. Bind:   for each SubIo, get target_handle from the BACKEND cache via
             NvmeBackend::acquire_target_handle(shard StorageTarget, vdev).
             This is a get-or-create INSIDE NvmeBackend (see §3): the engine
             just receives an opaque void* — it does NOT own/cache/free it.
  3. Fanout: split each SubIo into ≤ max_io_size_ chunks → SubSliceInfo[]
             (offset_bytes = SubIo.physical_offset + chunk_off;
              ioaddr picked via SubIo.region_byte_offset + chunk_off)
  4. Descr:  NvmeBackend::prepare_descriptors(ioaddrs, slices, n, out_descs)
  5. Stage:  accel.memcpy_async(d_descs_, descs, n·sizeof(BufferDescriptor))
  6. Launch: one launch_batch_gpu_stream per distinct target_handle
             (a single logical request → 1..K launches, one per vdevice)
  7. Complete: sync path → synchronize_stream then release_descriptors
               (PRP-list pages ONLY — target_handles are NOT released here,
                they live in the backend cache until shutdown);
               async path → one AsyncBatchContext for the whole request,
               event recorded after the last launch (one logical completion).
```

**Two distinct "releases" — do not conflate:** `release_descriptors` returns
PRP-list pages to the PRP cache and stays per-batch (step 7). `release_target_handle`
frees the GPU handle and now runs **only at backend shutdown** (§3). Stopping the
former by accident leaks PRP pages.

**Who computes streams?** Per §0 (C5): the stripe manager is pure math and
stops at step 1. Steps 2–7 — including how many streams/launches a request
needs and how they join — belong to the IO Engine's execution module, not the
mapper. For the single-request/one-stream milestone this is a loop on one
stream; multi-stream fan-out/join is a later execution-side concern.

## 7. Module Structure

```
tutti/io_engine/
├── include/
│   ├── stripe_manager.h        # io_engine::StripeManager, SubIo
│   └── io_engine.h             # + submit_one / submit_one_async
├── src/
│   ├── stripe_manager.cpp      # contiguous-per-shard mapper (identity first)
│   └── io_engine_impl.cpp      # Phase 0 migration + submit_one path
└── tests/
    ├── stripe_manager_test.cpp # pure-math: prefix-sum, boundary split
    └── submit_one_test.cpp     # against mock Device Manager
```

**Dependencies**: the stripe manager depends only on the shard geometry types it
maps over and `SubIo`; it does **not** include backend or HAL headers. The IO
Engine (not the mapper) depends on `backends` (§0.5).

## 8. Phased Rollout

- **Phase 0 — Compile.** Migrate the engine off `IBackendProvider` onto
  `NvmeBackend` (§2.1). Exit: builds, single-target `submit_batch` works.
- **Phase 1 — `submit_one` + identity mapper.** One request → one vdevice,
  end-to-end, verified on the mock Device Manager. No real striping yet.
- **Phase 2 — Contiguous mapper.** Implement prefix-sum + boundary split;
  unit-test the math (single-shard, cross-shard, last-shard remainder).
- **Phase 3 — Real HW.** Validate `submit_one` across ≥2 vdevices under
  `TUTTI_NVME_REAL_HW`. Resolve the 512-vs-4096 block-size input for real.
- **Phase 4 — Batching.** Generalize `submit_batch` as a loop over the
  single-request path; group launches by `target_handle`.
- **Deferred.** Block Storage re-adaptation, `IBatchSubmitter` (Option B),
  multi-stream fan-out/join, alignment policy, RAID1/erasure.

## 9. Known Gaps / Open Questions (carried forward)

1. **Block-size source of truth (must fix by Phase 3).** Allocator uses 512;
   `StorageTarget` defaults 4096. The mapper must take a namespace-derived block
   size and match whatever the allocator used, or physical offsets are 8× wrong.
2. **`device_id == namespace_id` today** — no independent device abstraction.
   The mapper reads `namespace_id` for routing; revisit if that split appears.
3. **Effective stripe not persisted** — derive boundaries from `shards[]`, never
   from `GpuFile.stripe_size`.
4. **Unaligned / partial-block IO** (C8/C9) — no handling exists; deferred to
   when the sharding logic matures. The mapper must eventually own head/tail
   partial-block reads.
5. **`slice_fanout(region)` becomes incomplete** once striping is in — fanout
   depends on shard geometry too, not just region size. Revisit the capacity API
   in Phase 4.
6. **Cached-handle staleness (accepted for now).** With release-at-`shutdown()`
   (§3), a cached `target_handle` outlives nothing until teardown. If a file's
   placement changes (delete + reallocate reusing the same `target_id`, or a
   rewrite that moves extents) while a handle is cached, the stale handle points
   at the old extents. Harmless in the single-request milestone (no such
   lifecycle yet); the fix is a later `invalidate(target_id)` freeing that
   target's handles across all vdevices on file close.
7. **File semantics live in `NvmeBackend`, not `IBackend` (by decision).** The
   handle cache, extent materialization, and file→handle mapping are concrete
   NVMe-backend responsibilities and are intentionally NOT abstracted onto the
   device-agnostic SPI. A second backend would re-solve this in its own terms.
   Consistent with binding the engine to concrete `NvmeBackend*` (§2.1 Option A).

## 10. Summary of Changes from rev 1 (`block-layer-design.md`)

- **Renamed** block layer → stripe manager; clarified two distinct StripeManagers.
- **Added Phase 0**: the engine does not compile against the new `IBackend`; this
  is the real starting point.
- **Corrected granularity**: `shard → VDeviceHandle`, not `shard → backend`.
  One backend, many vdevices.
- **Corrected layout model**: contiguous-per-shard + greedy least-loaded
  placement (read from `shards[]`), not interleaved round-robin.
- **Dropped**: `StripingMetadata`-on-`StorageTarget`, `shard_to_backend`,
  strategy registry — all premature.
- **Scoped**: single-request first; Block Storage adaptation and multi-request
  batching deferred; IO Engine → Backends dependency accepted explicitly.

---

**Document History**:
- 2026-07-27: rev 1 (`block-layer-design.md`, block-layer framing).
- 2026-07-27: rev 2 — code-grounded rewrite as stripe manager (this document).



