# Layer 5: Storage Interfaces (Block Storage + Raw Device)

**Version:** 1.0
**Date:** 2026-07-29
**Status:** Two app-facing peers, both mid-migration. `block_storage` (`IBlockStorage`) is **feature-rich but does-not-compile** against the current lower layers and **never calls** the L4 IO Engine. `raw_device` (`IRawDevice`) — authoritatively implemented under `tutti/coordinator/` — **does** drive the L4 `IIoEngine::submit_batch`, but still depends on a **deleted** `backends::IBackendProvider` SPI, so it too does-not-compile today. Both subdirectories are **disabled** in the top-level build
**Library:** `libtutti_block_storage` (block_storage) · `libtutti_coordinator` (packages the authoritative raw_device). Neither is currently built
**Location:** `tutti/block_storage/` · `tutti/coordinator/` (raw_device) · `tutti/raw_device/` (**legacy stub — to retire**). **Target:** consolidate both peers under `tutti/storage-interfaces/` (see the "Intended directory layout" note below)

**Scope:** This document covers the **two Layer 5 app-facing peers** — `block_storage` and `raw_device`. The Layer 4 IO Engine that both are meant to drive is **below** and documented in [layer4-io-engine.md](layer4-io-engine.md); the Layer 3 backends are further below in [layer3-backends.md](layer3-backends.md). The Layer 6 `coordinator` that orchestrates both peers for applications is **above** and out of scope here.

> **Placement tension (read this first).** The authoritative `IRawDevice` implementation currently lives under `tutti/coordinator/` — a directory the build labels **Layer 6** (`tutti/coordinator/CMakeLists.txt:18`, `set(LAYER_NUMBER 6)`). Conceptually it is a **Layer 5 app-facing peer of `block_storage`**: it exposes namespace+LBA storage access directly to a caller, exactly parallel to `block_storage`'s file interface. It is documented here as an L5 interface despite its physical packaging. Separately, the old `tutti/raw_device/` directory is a **superseded duplicate stub** (different interface, all-`TODO` bodies) that should be retired in favor of the coordinator-packaged one. Both facts are called out again in §4 and Known Issues & Gaps.

> **Intended directory layout (target state).** The current on-disk split — `block_storage/` at the top level, the authoritative `raw_device` buried in `coordinator/`, and a stale `raw_device/` stub — does **not** match this layer's structure and should be reorganized. Both Layer 5 components are to be implemented together under a single **`tutti/storage-interfaces/`** directory (e.g. `tutti/storage-interfaces/block_storage/` and `tutti/storage-interfaces/raw_device/`), so the two app-facing peers live side by side at their own layer rather than being scattered across `block_storage/`, `coordinator/`, and a legacy `raw_device/`. This is a refactoring target, not the present state: the paths cited throughout this document (`tutti/block_storage/…`, `tutti/coordinator/…`) reflect the code as it stands today. Moving to `storage-interfaces/` also resolves the placement tension above — the raw-device implementation leaves `coordinator/` (Layer 6) and joins `block_storage` at Layer 5, and the duplicate `tutti/raw_device/` stub is dropped in the process.

---

## 1. Overview

Layer 5 is the top of the storage stack that applications actually hold. It offers **two peer entry points at the same level**, each a different abstraction over the same NVMe-through-`StorageTarget` machinery below:

- **`block_storage` (`IBlockStorage`) — block/file mode over a striped logical namespace.** A caller opens a *named* `GpuFile` with a logical byte size and a stripe size; block_storage decomposes that logical space into physical `FileShard`s (each a contiguous NVMe LBA extent), journals the metadata for crash recovery, and hands back per-shard `backends::StorageTarget` descriptors. This is the "files over a logical namespace" side.
- **`raw_device` (`IRawDevice`) — direct `namespace_id` + LBA access.** A caller names a raw extent — `namespace_id`, `start_lba`, `length_blocks` — with no file, no directory, no striping, and no metadata journal. It gets back a `RawTargetHandle` and submits reads/writes against it. This is the "namespace + LBA" side.

Both converge on the same noun — `backends::StorageTarget{NVME_RAW, namespace_id, start_lba, length_blocks}` — and both are *meant* to route that target through the Layer 4 `IIoEngine` down to the Layer 3 backends. Where they differ today is whether they actually do (§4).

### Model points (a–c) as reported by the code

Three model claims were checked against the source. Verdicts:

- **(a) `block_storage` is the block-mode L5 peer of `raw_device` that "serves applications directly" — PARTIAL.** The peer-level relationship is confirmed *by design*: the build labels `block_storage` Layer 5 (`tutti/block_storage/CMakeLists.txt`), and both `IBlockStorage` and `IRawDevice` are top interfaces intended for application callers. But "serves applications directly" is **not** demonstrably live: `block_storage` is not compiled (`add_subdirectory(block_storage)` is commented out at `tutti/CMakeLists.txt:133`) and has no in-tree application caller; the coordinator that would drive it is also disabled (`:134`). Peer relationship = confirmed by design; live app path = refuted by build state.
- **(b) The block-half of the interface distinction (block/file mode vs direct LBA) — CONFIRMED.** `block_storage` exposes a named-file/block interface (`open_gpu_file(name, …)` → `GpuFile{logical_size, stripe_size, shards}`, `block_storage.h:33`, `block_storage_types.h:33`), and the physical mapping is exactly `namespace_id + start_lba + length_blocks` (`FileShard`, `block_storage_types.h:20`), surfaced as `StorageTarget{NVME_RAW, …}` in `acquire_device_handle` (`block_storage_impl.cpp:299-302`). `raw_device` exposes that same LBA triple directly (`raw_device.h:27-30`). The two-mode distinction holds.
- **(c) An L5 interface "builds a StorageTarget then calls the L4 engine to launch IO" — SPLIT: REFUTED for `block_storage`, CONFIRMED for `raw_device`.** `block_storage` produces `StorageTarget`s (`acquire_device_handle`) but **never calls** an `IIoEngine`: it holds no engine reference and there is no `submit_batch` / `submit_one` call anywhere in `block_storage/src`. Its only "drive" path is to a legacy `backends::IBackendProvider*` whose API no longer exists (`acquire_target_handle` / `submit_batch_cpu_sync`, `block_storage_impl.cpp:305,375`), and it does not compile. **`raw_device`, by contrast, does call the engine** — `submit_read` / `submit_write` build a `tutti::IoRequest` and invoke `io_engine_->submit_batch(...)` (`raw_device_impl.cpp:104,125,154,185`) — so the "drive L4" model is realized on the raw side, though it also still depends on the deleted `IBackendProvider` and so does not compile either.

---

## 2. Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  Applications  (and Layer 6 coordinator — OUT OF SCOPE here)        │
└───────────┬───────────────────────────────────┬────────────────────┘
            │ named files, logical byte ranges   │ namespace_id + LBA
            ▼                                     ▼
┌───────────────────────────┐         ┌───────────────────────────────┐
│  IBlockStorage (L5)        │  peers  │  IRawDevice (L5)               │
│  block/file mode           │◄──────► │  direct namespace + LBA        │
│  GpuFile{logical,stripe,   │  same   │  RawTargetHandle{ns,lba,len}   │
│          shards[]}         │  level  │  (impl packaged under          │
│  FileDirectory             │         │   tutti/coordinator/)          │
│  MetadataJournal (WAL)     │         │                                │
│  StripeManager (allocator) │         │  no directory / no striping /  │
│  → StorageTarget per shard │         │  no metadata journal           │
└───────────┬────────────────┘         └───────────┬───────────────────┘
            │ StorageTarget{NVME_RAW,ns,lba,len}    │ StorageTarget{NVME_RAW,…}
            │ (produced; NOT routed to engine —     │ submit_read/write →
            │  see §4: block_storage never calls    │ IoRequest → submit_batch
            │  the engine)                          │
            ▼                                       ▼
┌───────────────────────────────────────────────────────────────────┐
│  Layer 4: IO Engine   (IIoEngine::submit_batch / submit_one)        │
│  — driven live only by raw_device today —                           │
└───────────────────────────────┬────────────────────────────────────┘
                                 │ IBatchSubmitter (NVMe-scoped SPI)
┌────────────────────────────────▼────────────────────────────────────┐
│  Layer 3: Backends   (NvmeBackend : IBackend, IBatchSubmitter)       │
└───────────────────────────────────────────────────────────────────┘
```

Both peers are intended to sit *above* the same L4 engine. The asymmetry the code shows today: `raw_device` wires through to `IIoEngine::submit_batch`, while `block_storage` stops at producing `StorageTarget`s and expects an unspecified caller to route them — a gap, not a design choice (§4, Known Issues & Gaps).

---

## 3. Block Storage (IBlockStorage)

**Files:** `include/block_storage.h`, `include/block_storage_types.h`, `include/storage_config.h`, `src/block_storage_impl.{h,cpp}`, `src/stripe_manager.{h,cpp}`, `src/file_directory.{h,cpp}`, `src/metadata_journal.{h,cpp}`
**Library:** `libtutti_block_storage` (currently not built)

### 3.1 Purpose

`block_storage` is the **named / striped / persistent block-mode** L5 interface. It manages a directory of named `GpuFile`s, stripes each file's logical byte space across multiple NVMe namespaces (shards), journals metadata for crash recovery, and produces per-shard `StorageTarget` descriptors a caller is meant to hand to the IO Engine. "Block mode" here means: a *named logical byte space* (`GpuFile.logical_size` + `stripe_size`) decomposed into `FileShard`s, each a contiguous NVMe LBA extent — the counterpart to `raw_device`'s bare namespace+LBA view.

### 3.2 Key types

- **`GpuFile`** (struct, `block_storage_types.h:33`) — the block-mode file object: `file_id`, `name`, `logical_size`, `stripe_size`, `std::vector<FileShard> shards`, `creation_time`. This *is* what "block mode" means here — a named logical byte space mapped onto shards.
- **`FileShard`** (`block_storage_types.h:20`) — one stripe's physical placement: `device_id`, `namespace_id`, `start_lba`, `length_blocks`. This is the unit that becomes a `StorageTarget`.
- **`GpuFileHandle`** (`block_storage_types.h:50`) — runtime open-file handle: `file_id`, `std::vector<void*> target_handles` (opaque backend handles, one per shard), `backends::IBackendProvider* backend_provider`, `dirty` flag.
- **`FileInfo`** (`block_storage_types.h:63`) / **`FileOpenMode`** (`:79`, `READ_ONLY` / `READ_WRITE` / `CREATE_NEW` / `OPEN_OR_CREATE`).
- **`FileDirectory`** (`src/file_directory.h:13`) — thread-safe `name → GpuFile` and `id → GpuFile` registry (`shared_mutex`); `generate_file_id`, add/remove/lookup/list.
- **`MetadataJournal`** (`src/metadata_journal.h:32`) — append-only WAL (`CREATE` / `DELETE` / `RESIZE`) + checkpoint file under `root_directory`; `recover()` replays checkpoint then journal. Binary serialization of shards.
- **`StripeManager` (L5)** (`src/stripe_manager.h:35`) — the **write/allocator-side** stripe manager: `allocate_shards(file_size, stripe_size)` bump-pointer-allocates LBA ranges across namespaces with least-allocated load balancing; `deallocate_shards` only decrements counters (no free-list reuse). Distinct from L4's IO-time `io_engine::StripeManager`.
- **`DeviceLbaAllocator`** (`src/stripe_manager.h:25`) — per-namespace bump allocator: `namespace_id`, `total_blocks`, `next_free_lba`, `allocated_blocks`.
- **`BlockStorageImpl`** (`src/block_storage_impl.h:22`) — concrete `IBlockStorage`; holds `FileDirectory`, L5 `StripeManager`, `MetadataJournal`, `backends::IBackendProvider*`, `IAccelerator*`, `coordinator::IRawDevice* raw_device_` (forced to `nullptr` at `block_storage_impl.cpp:47`), and `open_files_map_`.

> **Three divergent `GpuFile` designs coexist — a real divergence risk.** (1) the *active* `block_storage::GpuFile` **struct** above; (2) an **orphaned** `tutti::GpuFile` **class** (`include/gpu_file.h:23` + `src/host_fs_backed_block_storage.cpp` + `include/gpu_file_resolve.cuh`) that is **not** in the CMake sources, uses the older `types/storage_target.h`, and whose `resolve_offset` is a `TODO` stub; (3) a doc-proposed *revised* `GpuFile`/`GpuFileHandle` with device-resident `d_shards_dev` pointer arrays — intent only, unimplemented. Only (1) is live.

### 3.3 Public API

All methods on `IBlockStorage` (`include/block_storage.h`); impl in `src/block_storage_impl.cpp`.

| Method | Signature | Role |
|--------|-----------|------|
| `initialize` | `bool initialize(const BlockStorageConfig&, backends::IBackendProvider*, IAccelerator*)` | Init journal + L5 `StripeManager` + `recover_metadata`. Forces `raw_device_ = nullptr` (`impl.cpp:47`), so the StripeManager always runs the synthetic **4-namespace mock** (`stripe_manager.cpp:31-48`). Impl `:21`. |
| `open_gpu_file` | `GpuFileHandle* open_gpu_file(const std::string& name, FileOpenMode, uint64_t stripe_size=0, uint64_t initial_size=0)` | `CREATE_NEW` / `OPEN_OR_CREATE` call `StripeManager::allocate_shards` then `FileDirectory::add_file` + `MetadataJournal::log_create`. Impl `:81`. |
| `close_gpu_file` | `bool close_gpu_file(GpuFileHandle*)` | Close + destroy handle. Impl `:182`. |
| `delete_gpu_file` | `bool delete_gpu_file(const std::string& name)` | `StripeManager::deallocate_shards` + journal `log_delete`. Impl `:208`. |
| `open_gpu_files_batch` | `std::vector<GpuFileHandle*> open_gpu_files_batch(names, modes, count)` | Serial loop over `open_gpu_file` with rollback on failure. Impl `:242`. |
| `list_gpu_file_names` | `std::vector<FileInfo> list_gpu_file_names()` | Snapshot of the directory. Impl `:271`. |
| `acquire_device_handle` | `backends::StorageTarget acquire_device_handle(GpuFileHandle*, size_t shard_index)` | Build `StorageTarget{NVME_RAW, namespace_id, start_lba, length_blocks}` from the shard, call `backend_provider_->acquire_target_handle(target)`, cache the opaque handle. Impl `:279`. |
| `release_device_handle` | `bool release_device_handle(GpuFileHandle*, size_t shard_index)` | `backend_provider_->release_target_handle(...)`. Impl `:320`. |
| `sync_file` | `bool sync_file(GpuFileHandle*, void* stream)` | `FULL` mode: `accelerator_->synchronize_stream` then, per shard, `backend_provider_->submit_batch_cpu_sync(handle, nullptr, 0, true)` as a **0-descriptor flush**, then `flush_metadata`. Impl `:341`. |
| `flush_metadata` | `bool flush_metadata()` | `MetadataJournal::checkpoint` of all files. Impl `:399`. |
| `cleanup` | `void cleanup()` | Release resources. Impl `:63`. |
| `create_block_storage` | `std::unique_ptr<IBlockStorage> create_block_storage()` | Factory. Impl `:494`. |

### 3.4 Block-mode semantics: how it allocates and stripes

Striping is **allocation-time** here (contrast L4's IO-time mapper). `StripeManager::allocate_shards` splits `file_size` into `ceil(file_size / stripe_size)` shards (capped by `max_shards_per_file` and available namespaces), then round-robins by **least-allocated-blocks** across namespaces, bump-allocating LBA ranges (`stripe_manager.cpp:75-145`). Block size is **hardcoded 512**: `length_blocks = (shard_size + 511) / 512` (`stripe_manager.cpp:127`). Because `raw_device_` is `nullptr`, real device enumeration never runs; the allocator uses a synthetic roster of namespaces `{1,2,3,4}` at 512 GB each (`stripe_manager.cpp:33-48`).

There is **no LBA reclamation**: `deallocate_shards` decrements `allocated_blocks` only; `next_free_lba` never rewinds (`stripe_manager.cpp:176-182`; `STRIPE_MANAGER_NOTES.md` "No Free List Management").

### 3.5 How it (does not) drive the L4 engine

`acquire_device_handle` builds a `StorageTarget` per shard and expects the **caller** to route it — matching L4's "callers route/stripe" model and the intended `StorageTarget → IIoEngine` convergence. But the file itself contains **no engine invocation**: it holds no `IIoEngine`/`IBatchSubmitter` reference, and there is no `submit_batch` / `submit_one` anywhere in `block_storage/src`. The only downward calls are to a **legacy `backends::IBackendProvider`** (`acquire_target_handle` at `impl.cpp:305`; `submit_batch_cpu_sync` at `:375`), an SPI that no longer exists in the current L3 (which now exposes `tutti::backends::IBackend`, `backends/include/backend.h:58`). So the data-transfer launch path is entirely absent, and the durability "flush" bypasses the engine.

### 3.6 Design decisions

- **`StorageTarget` is the convergence noun.** L5 emits `backends::StorageTarget{NVME_RAW}` per shard; this is the same descriptor `raw_device` produces and the same one the L4 engine / L3 backend consume.
- **Durability via WAL + checkpoint.** `MetadataJournal` logs `CREATE` / `DELETE` / `RESIZE` and checkpoints; `SyncMode` is `NONE` / `METADATA_ONLY` / `FULL`. `FULL` "flush" is modeled as an empty (0-descriptor) `submit_batch_cpu_sync` per shard — a placeholder, not real data movement.
- **Thread safety.** `shared_mutex` in `FileDirectory` and around `open_files_map_`.
- **No FILE/FIEMAP resolution.** Only fixed contiguous LBA extents per shard; there is no filesystem-file → extent-array metadata production (that missing capability is the same L3 gap documented in [layer3-backends.md](layer3-backends.md) §5.5).

---

## 4. Raw Device (IRawDevice)

**Files (authoritative):** `tutti/coordinator/include/raw_device.h`, `tutti/coordinator/include/coordinator_types.h`, `tutti/coordinator/src/raw_device_impl.{h,cpp}`
**Files (legacy stub — to retire):** `tutti/raw_device/include/raw_device.h`, `tutti/raw_device/src/raw_device_impl.{h,cpp}`
**Library:** `libtutti_coordinator` (packages the authoritative impl; currently not built)

### 4.1 Purpose and packaging

`raw_device` is the **direct namespace + LBA** L5 peer. A caller names a raw extent (`namespace_id`, `start_lba`, `length_blocks`), gets a `RawTargetHandle`, and submits reads/writes against it — no files, no directory, no striping, no metadata journal. It is `block_storage`'s peer: same level, same `StorageTarget` convergence, opposite abstraction.

**Packaging tension:** the authoritative implementation lives under `tutti/coordinator/`, which the build labels **Layer 6** (`tutti/coordinator/CMakeLists.txt:18`). It is documented here as an L5 interface because it *is* one conceptually — a top-level app-facing storage entry point parallel to `block_storage`. The coordinator library also bundles the coordinator orchestrator proper (out of scope); only `IRawDevice` / `RawDeviceImpl` are the L5 peer.

**Legacy duplicate:** `tutti/raw_device/` is an older, superseded stub with a *different, LBA-target-oriented* interface. Its `IRawDevice` (`tutti/raw_device/include/raw_device.h`) declares `acquire_raw_target(device_index, namespace_id, start_lba, lba_count, stream) -> StorageTarget`, `release_raw_target(const StorageTarget&, stream)`, and `get_lba_size(device_index, namespace_id)` — note it has **no `submit_*` path**, so it cannot serve IO. Its impl class (`tutti/raw_device/src/raw_device_impl.h:18-33`) declares a *third* set of names — `open_namespace` / `close_namespace` / `read(nsid, buf, lba, num_blocks)` / `write(...)` — as `override`, but the interface declares none of these, so the `override` would not compile even if the includes were fixed. And they are not: `include/raw_device.h:6-7` includes `tutti/types/storage_target.h` and `tutti/accel/include/accel_types.h`, **neither of which exists** (the real paths are `backends/include/storage_target.h` and `accel/include/common/accel_types.h`; there is no `tutti/types/` directory). Every impl body is an all-`TODO` no-op that returns `0` without touching the engine (`tutti/raw_device/src/raw_device_impl.cpp:15-64`). So this legacy peer **does not compile** on two independent counts and should be deleted in favor of the coordinator-packaged one; it is disabled at `tutti/CMakeLists.txt:135`.

### 4.2 Key types

- **`IRawDevice`** (`coordinator/include/raw_device.h:23`) — the authoritative interface (in namespace `tutti::coordinator`).
- **`RawTargetHandle`** (`coordinator/include/coordinator_types.h:44`) — the acquired raw extent: `namespace_id`, `start_lba`, `length_blocks`, an opaque `void* target_handle` (from the backend), and a `region_id`.
- **`coordinator::IoRequest`** (`raw_device.h:16`) — a coordinator-level mirror of `tutti::IoRequest` for the batch API: `MemoryRegion* region`, `void* target_handle`, `byte_offset`, `byte_length`.
- **`BatchSubmitResult`** (`coordinator_types.h:57`) — `success`, `completed_count`, `failed_count`, `error_code`.
- **`NamespaceInfo`** (`coordinator_types.h:82`) — `namespace_id`, `block_size`, `capacity_blocks`, `mdts_bytes`.

### 4.3 Public API

`IRawDevice` (`coordinator/include/raw_device.h`); impl in `coordinator/src/raw_device_impl.cpp`.

| Method | Signature | Role |
|--------|-----------|------|
| `acquire_raw_target` | `RawTargetHandle* acquire_raw_target(uint32_t namespace_id, uint64_t start_lba, uint64_t length_blocks)` | Build `StorageTarget{NVME_RAW, ns, lba, len}`, call `backend_provider_->acquire_target_handle(target)`, wrap the opaque handle in a tracked `RawTargetHandle`. Impl `:30`. |
| `release_raw_target` | `bool release_raw_target(RawTargetHandle*)` | `backend_provider_->release_target_handle(...)`, drop from the tracking map. Impl `:61`. |
| `submit_read` | `bool submit_read(RawTargetHandle*, MemoryRegion*, uint64_t byte_offset, uint64_t byte_length, AccelStream)` | Build one `tutti::IoRequest` from the handle, call **`io_engine_->submit_batch(reqs, /*is_read=*/true, stream)`**. Impl `:86`. |
| `submit_write` | `bool submit_write(RawTargetHandle*, MemoryRegion*, uint64_t byte_offset, uint64_t byte_length, AccelStream)` | Same, `is_read=false`. Impl `:107`. |
| `submit_read_batch` | `BatchSubmitResult submit_read_batch(RawTargetHandle*, IoRequest*, uint32_t count, AccelStream)` | Validate every request targets the same handle (else `error_code -2`), convert to `tutti::IoRequest[]`, `io_engine_->submit_batch(..., true, ...)`. Impl `:128`. |
| `submit_write_batch` | `BatchSubmitResult submit_write_batch(...)` | Same, `is_read=false`. Impl `:159`. |
| `get_namespace_info` | `NamespaceInfo get_namespace_info(uint32_t namespace_id)` | Cached; **placeholder** values on miss — `NamespaceInfo(ns, 4096, 0, 131072)` (`impl.cpp:201`). Impl `:190`. |
| `list_namespaces` | `std::vector<uint32_t> list_namespaces()` | **Placeholder** — returns `{1}` (`impl.cpp:218`). Impl `:211`. |

### 4.4 Direct-LBA semantics and how it drives L4

`acquire_raw_target` synthesizes a `backends::StorageTarget` with `kind = NVME_RAW` and the caller's `namespace_id` / `start_lba` / `length_blocks` (`raw_device_impl.cpp:39-43`), then asks the backend provider for an opaque GPU target handle. On submit, it packs `{region, target_handle, byte_offset, byte_length}` into a `tutti::IoRequest` and calls `io_engine_->submit_batch(...)` (`raw_device_impl.cpp:97-104`) — this is the live realization of the "L5 builds a `StorageTarget`, then the L4 engine launches the IO" model (model point (c), confirmed on the raw side). Note the engine's batch path routes the whole batch to `requests[0].target_handle`; the batch APIs here defend against that by rejecting mixed-handle batches up front (`raw_device_impl.cpp:138-142,169-173`).

**Caveat:** despite driving `IIoEngine` correctly, `raw_device_impl` still includes and depends on the **deleted** `backends::IBackendProvider` (`raw_device_impl.h:5` `#include "backends/include/backend_provider.h"` — a header that does not exist under `tutti/backends/include/`, whose contents are only `backend.h`, `backend_factory.h`, `backend_types.h`, `storage_target.h`). So it does-not-compile against the current L3 for the acquire/release path, even though its submit path already speaks the current L4 `IIoEngine`. It is mid-migration.

---

## 5. Block vs Raw — when to use which

| Dimension | `block_storage` (`IBlockStorage`) | `raw_device` (`IRawDevice`) |
|-----------|-----------------------------------|-----------------------------|
| Abstraction | Named files over a striped logical namespace | Direct namespace + LBA extent |
| Addressing unit | File name + logical byte offset (`open_gpu_file` + shard math) | `namespace_id` + `start_lba` + `length_blocks` |
| Metadata | Directory + WAL + checkpoint (`FileDirectory`, `MetadataJournal`) | None — caller owns placement |
| Striping | Allocation-time, across namespaces, least-loaded (L5 `StripeManager`) | None — one contiguous extent per target |
| Persistence / recovery | Crash-recoverable metadata (`recover()` on init) | Stateless; nothing to recover |
| Drives L4 engine today? | No — produces `StorageTarget`s, never calls the engine | Yes — `submit_read/write` → `io_engine_->submit_batch` |
| Target audience | Apps wanting file-like, striped, durable storage | Apps wanting raw block access / their own layout |

---

## 6. Cross-layer flow

Two walkthroughs. Steps marked **(absent)** are not wired in the current code (§ Known Issues & Gaps).

### (i) Block-mode file read/write (as intended; the launch step is absent today)

```
App / coordinator
  │ open_gpu_file("data.bin", CREATE_NEW, stripe=1MiB, size=8MiB)
  ▼
IBlockStorage (L5)
  ├─ StripeManager::allocate_shards(size, stripe) ── shards[] (ns+lba+len each)
  ├─ FileDirectory::add_file + MetadataJournal::log_create
  │
  │ acquire_device_handle(handle, shard_i)
  ├─ StorageTarget{NVME_RAW, ns, start_lba, length_blocks}
  ├─ backend_provider_->acquire_target_handle(target)  ── (legacy SPI, deleted)
  │
  │ (absent) route each shard's StorageTarget + buffer slice to the engine
  ┊  ───────────────────────────► IIoEngine::submit_batch / submit_one
  ┊                                     (block_storage never makes this call)
  ▼
  sync_file(FULL): synchronize_stream → submit_batch_cpu_sync(0 descs)  ── flush placeholder
                   → flush_metadata (checkpoint)
```

### (ii) Raw-LBA submit (live path down through L4 → L3)

```
App / coordinator
  │ h = acquire_raw_target(ns=1, start_lba=0, length_blocks=8)
  ▼
IRawDevice (L5, coordinator-packaged)
  ├─ StorageTarget{NVME_RAW, ns, lba, len}
  ├─ backend_provider_->acquire_target_handle(target) → h->target_handle  ── (legacy SPI, deleted)
  │
  │ submit_read(h, buffer, byte_offset, byte_length, stream)
  ├─ IoRequest{region=buffer, target_handle=h->target_handle, offset, length}
  ▼
IIoEngine::submit_batch({req}, is_read=true, stream)          ── Layer 4
  ├─ MDTS fan-out → prepare_descriptors → memcpy_async(CPU→GPU)
  ▼
NvmeBackend::launch_batch_gpu_stream(...)                     ── Layer 3
  └─ GPU submit kernel: resolve_lba → build SQE → ring doorbell → poll CQ
  ◄─ synchronize_stream → release_descriptors → return true
```

---

## Implementation Status

| Component | Status | Tested |
|-----------|--------|--------|
| `IBlockStorage` interface + config/type surface (`block_storage.h`, `block_storage_types.h`, `storage_config.h`) | Complete (self-contained; would compile) | `layer5_basic_test` (type/factory compile only) |
| `block_storage` named-file lifecycle (create/open/close/delete/list, batch open + rollback) | Complete | No (module not built) |
| `block_storage` allocation-time striping (`StripeManager::allocate_shards`, least-loaded, 512 B blocks) | Complete | No |
| `block_storage` `StorageTarget` production per shard (`acquire/release_device_handle`) | Complete | No |
| `block_storage` metadata durability (WAL `CREATE`/`DELETE`/`RESIZE` + checkpoint + `recover`) | Complete (`RESIZE` replay empty, `impl.cpp:484`) | No |
| `block_storage` driving the L4 IO Engine (data-transfer launch) | Not implemented (no `IIoEngine` ref, no `submit_*` call) | No |
| `block_storage` compile/link vs current lower layers | Does-not-compile (includes non-existent `backends/include/backend_provider.h`; coded against deleted `backends::IBackendProvider`) | No |
| `block_storage` subdir enablement | Does-not-compile / disabled (`tutti/CMakeLists.txt:133` commented out) | No |
| `block_storage` LBA reclamation (free-list reuse) | Not implemented (`stripe_manager.cpp:176-182`) | No |
| `block_storage` real device enumeration | Not implemented (`raw_device_ = nullptr`, always mock 4-ns; `impl.cpp:47`) | No |
| `block_storage` FILE/FIEMAP extent metadata; `tutti::GpuFile`/`ShardResolveContext` device resolve | Not implemented (orphaned `gpu_file.h` family; `resolve_offset` `TODO`, `host_fs_backed_block_storage.cpp:37`) | No |
| `IRawDevice` interface + `RawTargetHandle`/`NamespaceInfo`/`BatchSubmitResult` (coordinator) | Complete | Via coordinator tests (not built) |
| `raw_device` acquire/release raw target (`StorageTarget{NVME_RAW}` + tracked handle) | Complete (against legacy SPI) | No |
| `raw_device` `submit_read`/`submit_write` → `IIoEngine::submit_batch` | Complete (live L4 drive) | No |
| `raw_device` `submit_read_batch`/`submit_write_batch` (same-handle validation) | Complete | No |
| `raw_device` `get_namespace_info` / `list_namespaces` | Stub (placeholder values, `impl.cpp:201,218`) | No |
| `raw_device` compile/link vs current L3 | Does-not-compile (depends on deleted `backends::IBackendProvider`, `raw_device_impl.h:5`) | No |
| `raw_device` (coordinator) subdir enablement | Disabled (`tutti/CMakeLists.txt:134`) | No |
| Legacy `tutti/raw_device/` stub (interface: `acquire_raw_target`/`release_raw_target`/`get_lba_size`, no `submit_*`) | Legacy-superseded + Does-not-compile (missing includes; impl `override`s methods the interface doesn't declare; all `TODO` no-ops) | No |
| Legacy `tutti/raw_device/` subdir enablement | Disabled (`tutti/CMakeLists.txt:135`) | No |

Neither L5 peer is built today: `block_storage`, `coordinator` (raw_device), and legacy `raw_device` are all commented out at `tutti/CMakeLists.txt:133-135`. Only the layers below — `io_engine` (`:132`) and `backends` (`:131`) — are enabled.

---

## Known Issues & Gaps

Both L5 peers are mid-migration against the new `IBackend` / `IIoEngine` stack; neither compiles against the current lower layers, and neither is built. The items below are concrete limitations.

- **`block_storage` does-not-compile: it targets a deleted SPI and a missing header.** `block_storage_impl.cpp:2` (and `stripe_manager.cpp`, the smoke/integration tests) `#include "backends/include/backend_provider.h"`, which does not exist under `tutti/backends/include/` (only `backend.h`, `backend_factory.h`, `backend_types.h`, `storage_target.h`). The interface it codes against — `backends::IBackendProvider` with `acquire_target_handle` / `release_target_handle` / `submit_batch_cpu_sync` — was retired; L3 now exposes `tutti::backends::IBackend` (`backends/include/backend.h:58`). Fix: migrate to `IBackend` / `IIoEngine`.
- **`block_storage` never calls the L4 IO Engine — the launch path is absent.** No `IIoEngine`/`IBatchSubmitter` member, no `submit_batch`/`submit_one` call in `block_storage/src`. It produces `StorageTarget`s (`acquire_device_handle`, `block_storage_impl.cpp:299-317`) and expects an unspecified caller to route them; the actual data-transfer launch is missing, and `sync_file`'s "flush" is a 0-descriptor `submit_batch_cpu_sync` (`block_storage_impl.cpp:375`) that moves no data.
- **`raw_device` does-not-compile despite driving L4 correctly.** `submit_read/write` already call the current `io_engine_->submit_batch` (`raw_device_impl.cpp:104,125,154,185`), but the acquire/release path still depends on the deleted `backends::IBackendProvider` via `raw_device_impl.h:5` `#include "backends/include/backend_provider.h"` (non-existent header). Migrate the target-handle path to `IBackend` to build.
- **Directory layout does not match the layer; consolidate under `tutti/storage-interfaces/`.** The two L5 peers are scattered across `tutti/block_storage/`, `tutti/coordinator/` (raw_device), and a legacy `tutti/raw_device/`. The target layout is a single `tutti/storage-interfaces/` directory holding both (`storage-interfaces/block_storage/` + `storage-interfaces/raw_device/`), so the app-facing peers sit together at their own layer. This move also resolves the two sub-issues below in one step: the raw-device implementation leaves `coordinator/` (Layer 6), and the stale `tutti/raw_device/` stub is dropped.
- **Two `raw_device` implementations + coordinator/L5 placement tension.** The authoritative `IRawDevice` is packaged under `tutti/coordinator/` (build-labeled Layer 6, `tutti/coordinator/CMakeLists.txt:18`) yet is conceptually an L5 peer of `block_storage`. Meanwhile `tutti/raw_device/` is a separate, superseded stub with a *different* interface and all-`TODO` bodies (`tutti/raw_device/src/raw_device_impl.cpp:15-64`). Retire the legacy `tutti/raw_device/` directory and reconcile the coordinator packaging with the layer label (see the `storage-interfaces/` consolidation above).
- **`raw_device` namespace enumeration is stubbed.** `get_namespace_info` returns fixed placeholder geometry `NamespaceInfo(ns, 4096, 0, 131072)` (`raw_device_impl.cpp:201`) and `list_namespaces` always returns `{1}` (`:218`) — no real query to the backend.
- **`block_storage` runs a synthetic device roster, not real hardware.** `raw_device_` is hardcoded `nullptr` (`block_storage_impl.cpp:47`), so `StripeManager` always uses the mock `{1,2,3,4}` 4-namespace roster (`stripe_manager.cpp:31-48`); the real `coordinator::IRawDevice` enumeration branch never executes.
- **`block_storage` has no LBA reclamation.** `deallocate_shards` decrements `allocated_blocks` only; `next_free_lba` never rewinds (`stripe_manager.cpp:176-182`; `STRIPE_MANAGER_NOTES.md`). Repeated create/delete leaks the logical LBA space.
- **`block_storage` `RESIZE` is a no-op on recovery.** The journal op exists but `recover_metadata`'s `RESIZE` case is empty (`block_storage_impl.cpp:484`); there is no resize / write-extend API.
- **Host-fs-backed vs GPU-direct is unfinished.** The orphaned `tutti::GpuFile` family (`include/gpu_file.h`, `src/host_fs_backed_block_storage.cpp`, `include/gpu_file_resolve.cuh`) is not in the CMake sources, uses the older `types/storage_target.h`, and its `resolve_offset` is a `TODO` stub (`host_fs_backed_block_storage.cpp:37`); `ShardResolveContext::resolve` (`gpu_file_resolve.cuh:20`) has no compiled definition. Three divergent `GpuFile` designs coexist (§3.2).
- **FILE-type IO depends on the missing L3 metadata layer.** `block_storage` only ever produces fixed contiguous LBA extents per shard. Turning a real filesystem file into an `LbaExtent[]` (FIEMAP / on-disk header parse) does not exist anywhere in the stack — the same gap documented in [layer3-backends.md](layer3-backends.md) §5.5. Until that exists, neither L5 peer can serve real FILE-backed IO; both are RAW-extent only.
- **Neither peer is built or tested end-to-end.** All three subdirs are disabled (`tutti/CMakeLists.txt:133-135`). `layer5_basic_test` checks only type/factory compilation; `layer5_smoke_test` / `layer5_integration_test` embed a `MockBackendProvider` implementing the stale `backends::IBackendProvider` and therefore cannot compile against the current stack either.

---

## Related Documents

- [Layer 4: IO Engine + StripeManager](layer4-io-engine.md) — the layer **below**; supplies the `IIoEngine::submit_batch` / `submit_one` path that `raw_device` drives (and that `block_storage` is meant to, but does not).
- [Layer 3: Backends](layer3-backends.md) — further below; the `StorageTarget` consumer and the source of the missing FILE→extent (FIEMAP) metadata layer both L5 peers ultimately need.
- [Architecture Overview (L0–5)](architecture-overview.md)
- Layer 6 (`coordinator`) — the orchestrator **above** these two peers that wires them for applications. Out of scope here. (Note: the authoritative `raw_device` impl is currently packaged inside the coordinator directory; see §4.1.)
