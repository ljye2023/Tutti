# Layer 4: IO Engine + StripeManager

**Version:** 1.0
**Date:** 2026-07-29
**Status:** `IIoEngine` + `IoEngineImpl` submit path complete and compiling against the current L3 `IBatchSubmitter` seam (blocking / async / single-shard); `io_engine::StripeManager` complete and unit-tested as a standalone pure-math library but **not yet wired into the engine**. Test subdir currently **does not configure** (references a deleted source), which blocks a whole-tree `cmake --build` until fixed
**Library:** `libtutti_io_engine` (`io_engine_impl.cpp` + `local_nvme_io_engine.cpp`; `stripe_manager.cpp` is **not** in the library — tests compile it directly)
**Location:** `tutti/io_engine/`

**Scope:** This document covers **Layer 4 only** — the IO Engine and the IO-time StripeManager. Layer 5 (`block_storage`), which is the intended caller that populates a `StripeLayout` and hands the engine already-routed requests, is **above** this layer and out of scope here. Layer 3 (`backends`), whose `IBatchSubmitter` SPI the engine drives, is **below** and already documented in [layer3-backends.md](layer3-backends.md).

---

## 1. Overview

The IO Engine is the Layer 4 orchestrator that turns shard-scoped IO requests into GPU-launched NVMe transfers. It holds a narrow NVMe submission SPI (`backends::nvme::IBatchSubmitter*`) and the HAL (`IAccelerator*`), and for each request it acquires a GPU-resident target handle, fans the request out into transport-sized (MDTS) sub-IOs, asks the backend to build PRP descriptors from the buffer's DMA addresses, stages those descriptors CPU→GPU, launches the backend's GPU submission kernel on a stream, then synchronizes and returns the PRP pages. Alongside it ships `io_engine::StripeManager`, a stateless logical→physical stripe mapper (the read-side dual of the allocator's StripeManager) that splits a logical byte range over a contiguous-per-shard layout into one `SubIo` per shard touched.

### Intended data-flow

```
Layer 5 logical IoRequest  (offset, length on a striped target)
        │
        ▼
io_engine::StripeManager::map(layout, offset, length) → SubIo[]   (one per shard crossed)
        │   each SubIo names a shard + vdev_index
        ▼
per-shard request (SingleShardIoRequest / IoRequest)  → targets a backend vdevice
        │
        ▼
IO Engine  pre-transfer prep:
   acquire_target_handle(shard_target, vdev)  → GPU-resident target handle
   MDTS fan-out (max_io_size)                 → SubSliceInfo[]
   prepare_descriptors(ioaddrs, slice)        → PRP BufferDescriptor[]
   memcpy_async(descs CPU→GPU)                → GPU descriptor scratch
        │
        ▼
   launch_batch_gpu_stream(stream, handle, descs, n, is_read)   → GPU submit kernel
        │
        ▼
   synchronize_stream → release_descriptors                     → completion
```

### Model points (a–e) as reported by the code

The two validation reports checked five model claims against the source. Verdicts:

- **(a) The engine receives a logical request and splits it across shards — PARTIAL.** `submit_batch` takes `std::vector<IoRequest>`, whose `byte_offset` is documented as a *logical* file/LBA offset (`io_types.h:14`), but each `IoRequest` already carries a pre-built `void* target_handle` produced by `NvmeBackend::acquire_target_handle` (`io_types.h:13`) — so routing happened *above* the engine. `submit_one` takes a `SingleShardIoRequest` that is explicitly *already shard-scoped* (`io_types.h:30-36`). The engine never takes one whole logical request and splits it across shards internally.
- **(b) A StripeManager library inside the engine parses each logical request into physical ones — PARTIAL / not realized.** The `io_engine::StripeManager` library exists and does exactly that math — `map()` prefix-sums shard bytes and emits one `SubIo` per shard crossed (`stripe_manager.cpp:19-78`), 10/10 unit tests pass — **but it is not called anywhere in `IoEngineImpl`** and is not compiled into `libtutti_io_engine` (`CMakeLists.txt:23-26`). Logical→physical stripe splitting is not performed by the running engine today.
- **(c) A physical request names a backend vdevice — CONFIRMED.** `SingleShardIoRequest` carries `backends::VDeviceHandle vdev` (`io_types.h:35`); `submit_one` routes via `acquire_target_handle(req.shard_target, req.vdev)` (`io_engine_impl.cpp:344`). `StripeManager`'s `SubIo` likewise carries `vdev_index` (`stripe_manager.h:65`). `VDeviceHandle` is a dense index into the backend roster.
- **(d) The engine holds the backend SPI and launches the IO kernel — CONFIRMED.** The engine holds `backends::nvme::IBatchSubmitter* backend_` (`io_engine_impl.h:51`), *not* `IBackend` and *not* concrete `NvmeBackend`. It drives `metadata` / `prepare_descriptors` / `release_descriptors` / `acquire_target_handle` / `launch_batch_gpu_stream`, with kernel launch + stream sync via `IAccelerator`.
- **(e) Pre-transfer prep builds the GPU IO context and pins/registers GPU memory — PARTIAL.** Building the GPU IO context is confirmed (`acquire_target_handle` + `prepare_descriptors` staged to GPU via `memcpy_async`). Pinning/registration is **not** the engine's job: it consumes an already-DMA-mapped `MemoryRegion`, reads `region->backend_private` as the ioaddrs array, and returns `false` if unmapped (`io_engine_impl.cpp:337-339`). Registration is done upstream through `IAccelerator`.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 5: Block Storage (caller — OUT OF SCOPE here)             │
│  owns shard records; builds a StripeLayout + per-shard targets;  │
│  hands the engine already-routed requests. NOT YET WIRED.        │
└───────────┬──────────────────────────────────┬───────────────────┘
            │ StripeLayout (bytes-only input)   │ SingleShardIoRequest / IoRequest
            ▼                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│  IO Engine (Layer 4)                                             │
│                                                                  │
│  io_engine::StripeManager      (pure math; NOT in engine lib,    │
│   map(layout,off,len)→SubIo[])  NOT called by IoEngineImpl)      │
│                                                                  │
│  IIoEngine  ── LocalNvmeIoEngine (PIMPL) ── IoEngineImpl         │
│    submit_batch / submit_batch_async / submit_one               │
│    max_entries_per_batch / slice_fanout                         │
│    holds:  backends::nvme::IBatchSubmitter* backend_ (not owned) │
│            IAccelerator*                    accel_   (not owned) │
│            BufferDescriptor*                d_descs_ (GPU scratch)│
└───────────┬───────────────────────────────────┬──────────────────┘
            │ IBatchSubmitter (NVMe-scoped SPI)   │ IAccelerator (HAL)
            │ metadata / prepare_descriptors /    │ memcpy_async /
            │ release_descriptors /               │ synchronize_stream /
            │ acquire_target_handle /             │ record_event / query_event
            │ launch_batch_gpu_stream             │
┌───────────▼───────────────────────────────────────────────────────┐
│  Layer 3: Backends                                                 │
│  NvmeBackend : public IBackend, public IBatchSubmitter             │
│     (device-agnostic IBackend + narrow nvme:: submission SPI)      │
│     + __device__ submit_one in the GPU submit kernel               │
└────────────────────────────────────────────────────────────────────┘
```

The engine binds to the **narrow `IBatchSubmitter`** SPI (scoped under `nvme::`), not the device-agnostic `IBackend` and not concrete `NvmeBackend`. This is deliberate (`batch_submitter.h:14-33`): the four operations the engine needs traffic in `nvme::SubSliceInfo` / `BufferDescriptor`, so they live off the device-agnostic contract and let the engine be unit-tested with a lightweight mock (no CUDA / libnvm). `NvmeBackend` implements both bases (`nvme_backend.h:65`). The engine's backend-neutrality is therefore aspirational: today it speaks NVMe descriptor types.

---

## 3. StripeManager

**Files:** `include/stripe_manager.h`, `src/stripe_manager.cpp`

`io_engine::StripeManager` is the IO-time, backend-agnostic logical→physical stripe mapper — the **read-side dual** of `block_storage::StripeManager`: allocation decides *where data goes*; this decides *where data already is* (`stripe_manager.h:4-9`). It is pure math — stateless, no backend, no HAL, no CUDA, no IO.

### Design decisions

- **Pure byte math, no upward dependency.** The mapper works purely in *bytes*; it never touches block size or LBAs. The 512-vs-4096 block-size reconciliation is pushed to the caller when it computes each shard's byte length (`stripe_manager.h:17-20`). It defines its own input type `StripeLayout` so it never depends on `block_storage` — a Layer 4 → Layer 5 upward dependency would be illegal (`stripe_manager.h:12-16`).
- **Contiguous-per-shard layout (linear concatenation), not round-robin.** Shard 0 holds logical bytes `[0, s0)`, shard 1 holds `[s0, s0+s1)`, etc. (`stripe_manager.h:22-26`).
- **Data-dependent placement.** `vdev_index` is *read from each shard record*, not recomputed by index arithmetic — placement is greedy least-loaded at allocation time, so the mapper must read the shard list (`stripe_manager.h:24-26`; unit test proves `vdev {5,2,9} != index {0,1,2}`).
- **Overflow-safe bounds check.** `map()` rejects when `logical_offset > total` or `length > total - logical_offset`, without ever computing `offset + length` (which could wrap `uint64`) (`stripe_manager.cpp:38`).
- **Single request, one SubIo per shard crossed.** Transport-size (MDTS) fan-out is the engine's job, not the mapper's — each `SubIo` lies wholly within one shard (`stripe_manager.h:60-71`).

### Key types

- **`ShardGeometry`** (`stripe_manager.h:39`) — one shard's geometry: `uint32_t vdev_index`, `uint64_t shard_bytes`. Backend-agnostic; physical base LBA / block size live in the per-shard backend target the engine builds separately.
- **`StripeLayout`** (`stripe_manager.h:53`) — the full stripe layout of one open target: an ordered `std::vector<ShardGeometry> shards` where index == logical shard number. `total_bytes()` sums every `shard_bytes`.
- **`SubIo`** (`stripe_manager.h:63`) — one physical piece after mapping: `shard_index`, `vdev_index` (copied from the shard record for convenience), `shard_byte_offset`, `byte_length`, `region_byte_offset` (0-based from the request's start, for buffer slicing / ioaddr indexing).
- **`StripeManager`** (`stripe_manager.h:74`) — stateless mapper.

### API

| Method | Signature | Role |
|--------|-----------|------|
| `StripeManager::map` | `bool map(const StripeLayout& layout, uint64_t logical_offset, uint64_t length, std::vector<SubIo>& out_subios) const` | Map `[logical_offset, logical_offset+length)` into physical sub-IOs, splitting at each shard boundary crossed. Clears `out_subios` first, then fills `>= 1` entries in ascending logical order. Returns `false` (leaving `out_subios` cleared) if `length == 0`, `layout.shards` is empty, or the range is out of bounds / would overflow. |
| `StripeLayout::total_bytes` | `uint64_t total_bytes() const` | Sum of every shard's `shard_bytes` — the target's total logical size. |

### Mapping math

`map()` prefix-sums `shard_bytes` while walking shards, emitting one `SubIo` per shard the range intersects (`stripe_manager.cpp:42-75`). A `cursor` tracks the logical byte position still to place; for each shard it computes `shard_start` / `shard_end` from the running prefix, skips shards entirely before the cursor (which also skips zero-byte shards), then emits a piece from `cursor` to `min(shard_end, region_end)`. `shard_byte_offset = cursor - shard_start`; `region_byte_offset = cursor - logical_offset`. No backend, HAL, or CUDA is touched.

> **Not wired into the engine.** `StripeManager` is a standalone library component. `IoEngineImpl` never constructs or invokes it, and `stripe_manager.cpp` is excluded from `libtutti_io_engine`'s `LAYER_SOURCES` — it is compiled only by the standalone tests (`stripe_manager_test`, `submit_one_test`). See §7 and §8.

---

## 4. IO Engine

**Files:** `include/io_engine.h`, `include/io_types.h`, `include/local_nvme/local_nvme_io_engine.h`, `src/io_engine_impl.{h,cpp}`, `src/local_nvme_io_engine.cpp`

### 4.1 Request types

**File:** `include/io_types.h`

- **`IoRequest`** (`io_types.h:11`) — input to `submit_batch`: `MemoryRegion* region`, `void* target_handle` (opaque, **pre-built** by `NvmeBackend::acquire_target_handle` via `IBatchSubmitter`), `uint64_t byte_offset` ("file offset or LBA offset" into the logical target), `uint64_t byte_length`. Because it carries a resolved handle, the batch path does **not** call `acquire_target_handle`.
- **`SingleShardIoRequest`** (`io_types.h:30`) — input to `submit_one`: `MemoryRegion* region`, `uint64_t logical_offset` (byte offset *within this shard*), `uint64_t length`, `backends::StorageTarget shard_target`, `backends::VDeviceHandle vdev`. This is the type that names a vdevice for routing, proving the logical→shard split happens *above* the engine.
- **`SubSliceInfo`** (`io_types.h:20`) — an L4-side transport sub-IO descriptor. **Dead:** the impl builds `backends::nvme::SubSliceInfo` (`nvme_io_types.h:16`) instead, never this one.

### 4.2 Engine interface

**File:** `include/io_engine.h`

`IIoEngine` (`io_engine.h:18`) is the backend-neutral engine contract. Its `submit_batch` docstring lists the intended internal pipeline (fan-out → `prepare_descriptors` → `memcpy_async` → `launch_batch_gpu_stream` → `synchronize_stream`) — this matches the implemented MDTS path and says nothing about stripe splitting, consistent with `StripeManager` being a separate, not-yet-integrated stage.

| Method | Signature | Role |
|--------|-----------|------|
| `submit_batch` | `bool submit_batch(const std::vector<IoRequest>&, bool is_read, AccelStream)` | Blocking. Validate → MDTS fan-out per request → per-slice `prepare_descriptors` → stage CPU→GPU → launch → `synchronize_stream` → release. Each `IoRequest` must already carry a resolved `target_handle`. |
| `submit_batch_async` | `bool submit_batch_async(const std::vector<IoRequest>&, bool is_read, AccelStream)` | Returns after the kernel is queued. Records an `AccelEvent`; descriptor release is deferred and reclaimed lazily in `cleanup_completed_async_ops` when the event signals. |
| `submit_one` | `bool submit_one(const SingleShardIoRequest&, bool is_read, AccelStream)` | Blocking, resolves routing at IO time: `acquire_target_handle(shard_target, vdev)` → MDTS fan-out → `prepare_descriptors` → launch → sync → release. |
| `max_entries_per_batch` | `uint32_t max_entries_per_batch() const` | Max entries one batch may flatten to after fan-out — from `BackendMetadata.max_batch_size`, cached at construction. |
| `slice_fanout` | `uint32_t slice_fanout(const MemoryRegion*) const` | How many sub-IOs a region flattens to: `ceil(region->size / max_io_size)` (`io_engine_impl.cpp:322`). |

### 4.3 Concrete engine: `IoEngineImpl` and `LocalNvmeIoEngine`

**`IoEngineImpl`** (`src/io_engine_impl.h:25`) is the concrete `IIoEngine`. It holds `backends::nvme::IBatchSubmitter* backend_` (not owned), `IAccelerator* accel_` (not owned), a GPU descriptor scratch buffer `BufferDescriptor* d_descs_`, and caches `max_batch_entries_` / `max_io_size_` from `backend_->metadata()` at construction (`io_engine_impl.cpp:37-39`) — the comment notes MDTS is fixed after roster init, so caching avoids per-submit metadata calls. The constructor allocates `d_descs_` **through the HAL** — `accel_->allocate_device(max_batch_entries_ * sizeof(BufferDescriptor), MemoryKind::DEVICE, accel_->get_device())` (`io_engine_impl.cpp:46`), freed via `accel_->free` (`:71`) — so the engine issues **no raw CUDA calls**; it throws on a null backend/accel or a failed allocation. The destructor force-releases any still-pending async descriptors and frees `d_descs_`.

**`LocalNvmeIoEngine`** (`include/local_nvme/local_nvme_io_engine.h:23`) is a thin PIMPL facade whose every method delegates to an inner `IoEngineImpl`. Its ctor takes `(IBatchSubmitter* backend, IAccelerator* accel, LocalNvmeIoEngineConfig)`; the `LocalNvmeIoEngineConfig` (`max_batch_size` / `max_inflight_batches` / `enable_polling`) is currently **ignored** — the ctor comment states it delegates directly and the config is reserved for future extensions (`local_nvme_io_engine.cpp:22-25`).

### 4.4 Pre-transfer preparation

For `submit_one` (the routing-resolving path, `io_engine_impl.cpp:325-388`):

1. **Validate** state and inputs; reject a null/zero-length region, an invalid `vdev`, or `max_io_size_ == 0`.
2. **Read DMA addresses** from `req.region->backend_private` (cast to `const uint64_t*`); return `false` if null ("region not DMA-mapped"). The engine does **not** register or pin memory — that is the caller's job via `IAccelerator::register_host` / `register_device` upstream.
3. **Acquire the GPU IO context**: `backend_->acquire_target_handle(req.shard_target, req.vdev)` (`io_engine_impl.cpp:344`), called on every submit — the backend caches the result keyed by `(target, vdev)`.
4. **MDTS fan-out**: split `req.length` into `max_io_size_`-sized `backends::nvme::SubSliceInfo` slices; reject if the slice count exceeds `max_batch_entries_`.
5. **Build PRP descriptors**: one `prepare_descriptors(ioaddrs, &slice, 1, &desc)` call per slice (`io_engine_impl.cpp:364`).
6. **Stage CPU→GPU**: `accel_->memcpy_async(d_descs_, descs, …, stream)`; on failure, release descriptors and return.

The `submit_batch` path is identical minus step 3 — it reuses the pre-built `IoRequest.target_handle` and never calls `acquire_target_handle` (`io_engine_impl.cpp:104-129`). **Precondition (undocumented in code beyond an inline comment):** the batch path uses only `requests[0].target_handle` for the entire fanned-out batch (`io_engine_impl.cpp:160-169`, `263-272`) — every request is assumed to target the same device. A batch mixing handles would misroute all requests but the first; the engine does not validate this. See §8.

### 4.5 Kernel-launch path

After staging, the engine launches the backend's GPU submit kernel: `backend_->launch_batch_gpu_stream(stream.handle, handle, d_descs_, n_slices, is_read)` (`io_engine_impl.cpp:379`). The blocking paths then call `accel_->synchronize_stream(stream)` and `release_descriptors`; the async path records an `AccelEvent` and defers release to `cleanup_completed_async_ops`, which polls `query_event` and reclaims completed contexts (`io_engine_impl.cpp:290`). Kernel launch goes through the backend SPI — **not** the local `launch_batch.h::launch_io_batch` helper, which is declared with no definition and no callers anywhere in the tree (dead/reserved).

---

## 5. Cross-layer flow

One logical request going down to a launched GPU IO and back. Steps marked **(planned)** are not yet wired in L4 code (§8).

```
Layer 5 caller
  │ has: logical target opened as StripeLayout{shards[]}, a registered MemoryRegion,
  │      a logical [offset, length)
  │
  │ (planned) StripeManager::map(layout, offset, length, subios)
  ├─────────────────────────────────────────────► subios[]  (one SubIo per shard)
  │                                                each carries vdev_index + shard_byte_offset
  │
  │ (planned) for each SubIo: build SingleShardIoRequest{region, shard_byte_offset,
  │           byte_length, shard_target, VDeviceHandle{vdev_index}}
  ▼
IoEngineImpl::submit_one(req, is_read, stream)
  │
  ├─ read ioaddrs = region->backend_private            (must be pre-registered by HAL)
  │
  ├─ handle = backend_->acquire_target_handle(         ── Layer 3 ──► NvmeBackend
  │              req.shard_target, req.vdev)                          builds GPU NvmeFileDeviceHandle
  │
  ├─ MDTS fan-out: req.length → SubSliceInfo[]  (chunks of max_io_size_)
  │
  ├─ for each slice: backend_->prepare_descriptors(    ── Layer 3 ──► PRP SINGLE/DUAL/LIST
  │              ioaddrs, &slice, 1, &desc)
  │
  ├─ accel_->memcpy_async(d_descs_ ← descs, stream)    ── HAL ──────► CPU→GPU staging
  │
  ├─ backend_->launch_batch_gpu_stream(                ── Layer 3 ──► GPU submit kernel:
  │              stream, handle, d_descs_, n, is_read)                 resolve_lba → build SQE →
  │                                                                    ring doorbell → poll CQ
  │
  ├─ accel_->synchronize_stream(stream)                ── HAL ──────► DMA complete
  │
  └─ backend_->release_descriptors(descs, n)           ── Layer 3 ──► PRP pages back to cache
     return true                                                     (target handle stays cached)
```

---

## Implementation Status

| Component | Status | Tested |
|-----------|--------|--------|
| `io_engine::StripeManager::map` + `StripeLayout::total_bytes` (contiguous-per-shard byte/prefix-sum mapping, overflow-safe bounds, one SubIo per shard crossed) | Complete | Yes — 10/10 standalone (`stripe_manager_test`) |
| `IoRequest` / `SingleShardIoRequest` request types (`io_types.h`) | Complete | Yes (via `submit_one_test`) |
| `io_types.h::SubSliceInfo` (L4 transport descriptor) | Dead-reserved (impl uses `backends::nvme::SubSliceInfo` instead) | No |
| `IIoEngine` interface (`io_engine.h`) | Complete | Yes (via impls) |
| `IoEngineImpl::submit_batch` (blocking: validate → MDTS fan-out → prepare → stage → launch → sync → release) | Complete | Compiles; not run in the smoke test (source missing) |
| `IoEngineImpl::submit_batch_async` (deferred release via `AsyncBatchContext` + `AccelEvent`) | Complete | Compiles; not directly unit-run |
| `IoEngineImpl::submit_one` (acquire handle → MDTS fan-out → prepare → launch → sync → release) | Complete | Yes — `submit_one_test` (mock `IBatchSubmitter`, MDTS 12288/4096 → 3 descriptors, acquire once) |
| `max_entries_per_batch` / `slice_fanout` capacity helpers | Complete | Yes (`submit_one_test`) |
| `LocalNvmeIoEngine` PIMPL facade (delegates to `IoEngineImpl`) | Complete (config ignored) | Compiles |
| Engine → backend seam: bound to `backends::nvme::IBatchSubmitter*` (mockable, no CUDA/libnvm) | Complete | Yes (mock submitter in `submit_one_test`) |
| `libtutti_io_engine` compile/link against current L2/L3 (`IBatchSubmitter`, no retired `IBackendProvider`) | Complete — `.a` + `.o` build; enabled in `tutti/CMakeLists.txt:132` | Library builds |
| `StripeManager` integration into the engine submit path (logical→physical splitting at IO time) | Not implemented (never called by `IoEngineImpl`; excluded from library `LAYER_SOURCES`) | No |
| `LocalNvmeIoEngineConfig` (max_batch_size / max_inflight / polling) | Dead-reserved (accepted but ignored) | No |
| Memory registration / pinning inside the engine | Not implemented by design (engine consumes a pre-registered `MemoryRegion`; caller pins via HAL) | N/A |
| `local_nvme/launch_batch.h` `launch_io_batch` / `BatchLaunchConfig` | Dead-reserved (declared, no definition, no callers) | No |
| FILE-type (FIEMAP) extent metadata production | Not implemented (upstream / L3 concern; engine takes a caller-supplied `StorageTarget`) | No |
| `tests/io_engine` subdir configuration | Does-not-configure — references deleted `io_engine/tests/layer4_smoke_test.cpp` (`tests/io_engine/CMakeLists.txt:51-53`) | Blocks whole-tree `cmake --build` at the generate step |

The `io_engine` layer is enabled in `tutti/CMakeLists.txt:132` and its library artifact (`build/io_engine/libtutti_io_engine.a` + object files) is present and current against the L3 `IBatchSubmitter` SPI. The **test subdir**, however, does not configure (below), so a fresh whole-tree configure/build fails at the generate step until fixed.

---

## Known Issues & Gaps

The engine's submit path (blocking / async / single-shard) is implemented and compiles against the current L3 `IBatchSubmitter`; `StripeManager` is complete pure math with passing unit tests. The gaps below are real limitations, chiefly around integration and build hygiene.

- **The test subdir does not configure — this blocks rebuilding the whole tree.** `tutti/tests/io_engine/CMakeLists.txt:51-53` adds `layer4_smoke_test`, whose source `${IO_ENGINE_LAYER_DIR}/tests/layer4_smoke_test.cpp` no longer exists — the entire `io_engine/tests/` directory is gone (only `tests/io_engine/{stripe_manager_test.cpp, submit_one_test.cpp, CMakeLists.txt}` remain). Because `tutti/CMakeLists.txt:187` only guards on the *existence* of the test `CMakeLists.txt` (which is present), the subdir is added and `cmake` fails at the generate step ("Cannot find source file … layer4_smoke_test.cpp"). Fix: drop the `layer4_smoke_test` target or restore the source. `stripe_manager_test` and `submit_one_test` sources are present and their targets are otherwise correct.
- **`submit_batch` routes the whole batch to `requests[0].target_handle`.** The batch path uses only the first request's `target_handle` for every fanned-out sub-IO (`io_engine_impl.cpp:160-169`, `263-272`); an inline comment asserts "all requests in batch should target the same device" but the engine does not validate it. A batch mixing handles silently misroutes all requests but the first. Callers must pre-group requests by target, or the batch API should take a single handle explicitly.
- **`StripeManager` is not wired into the engine.** `IoEngineImpl` never constructs or calls `StripeManager` — `grep` of `io_engine/src/` shows `StripeManager`/`StripeLayout`/`SubIo` referenced only inside `stripe_manager.cpp` itself. No code converts a `SubIo` into a `SingleShardIoRequest` / `IoRequest`. Logical→shard splitting is expected to occur in the Layer 5 caller before it builds each per-shard request (`stripe_manager.h:12-16`). The running engine performs only **MDTS** fan-out, not **stripe** fan-out.
- **`stripe_manager.cpp` is not in the shipped library.** `io_engine/CMakeLists.txt:23-26` lists only `io_engine_impl.cpp` + `local_nvme_io_engine.cpp` in `LAYER_SOURCES`, so `StripeManager` symbols are absent from `libtutti_io_engine.a` — only the standalone tests compile `stripe_manager.cpp` directly. When it is integrated it must be added to `LAYER_SOURCES`.
- **Memory registration / pinning is not done by the engine.** The engine reads DMA addresses from `region->backend_private` and returns `false` if null (`io_engine_impl.cpp:337-339`); there is no `cudaHostRegister` / `register_host` / `register_device` anywhere in `io_engine/src`. Pinning/registration is delegated to the HAL (`IAccelerator`) and driven by the caller upstream. This is by design, but callers must not assume the engine pins for them.
- **FILE-type IO depends on the missing L3 metadata layer.** `submit_one` takes a caller-supplied `backends::StorageTarget`; producing an `LbaExtent[]` for a real file (FIEMAP / on-disk header parse) is out of this area and does not exist in the backend (see layer3-backends.md §5.5). RAW targets submit end-to-end today; FILE IO is blocked upstream of the engine.
- **`launch_batch.h` is a dead declaration.** `local_nvme::launch_io_batch(...)` and `BatchLaunchConfig` (`include/local_nvme/launch_batch.h:23`) have no definition and no callers — the real kernel launch goes through `backend_->launch_batch_gpu_stream`. The header remains only for the file listing in `CMakeLists.txt:31`.
- **`io_types.h::SubSliceInfo` is dead.** The impl builds `backends::nvme::SubSliceInfo` (`nvme_io_types.h:16`); the L4-side type at `io_types.h:20` is unused.
- **`LocalNvmeIoEngineConfig` is ignored.** The PIMPL facade accepts a config but delegates directly and never reads it (`local_nvme_io_engine.cpp:22-25`); batch/inflight/polling tuning is reserved for future work.
- **No linked/running smoke test of `submit_batch` through a mock.** The intended `layer4_smoke_test` (mock `IBatchSubmitter`, no backend/CUDA link) is unrealized because its source is missing; `submit_batch` / `submit_batch_async` are compiled but not exercised by a running test. `submit_one` is covered by `submit_one_test`.

### What is NOT a gap (clarifications)

- **The engine binding to `nvme::IBatchSubmitter` rather than a device-agnostic descriptor type** is a deliberate single-backend-phase decision, not a defect (`batch_submitter.h:14-33`). A genuinely neutral descriptor type would be required before a second transport could reuse the engine.
- **Any doc referring to `IBackendProvider`** (e.g. the stale `tutti/io_engine/layer4-fixes-summary.md`) describes a retired design. The current engine compiles against `IBatchSubmitter` and references no retired type.

---

## Related Documents

- [Layer 3: Backends](layer3-backends.md) — the layer **below**; supplies the `IBatchSubmitter` SPI (`metadata` / `prepare_descriptors` / `release_descriptors` / `acquire_target_handle` / `launch_batch_gpu_stream`) and the GPU submit kernel this engine drives.
- [Architecture Overview (L0–5)](architecture-overview.md)
- Layer 5 (`block_storage`) — the intended caller **above** this layer (populates `StripeLayout`, builds per-shard requests). Out of scope here and not yet wired.
