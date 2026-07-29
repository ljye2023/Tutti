# Layer 6: Coordinator (Top-Level Orchestrator)

**Version:** 1.0
**Date:** 2026-07-29
**Status:** `ICoordinator` + `CoordinatorImpl` implement the full façade lifecycle (init / cleanup, buffer register/unregister, blocking submit, capacity pass-throughs) against **mocks**, but the library **does-not-compile** against the current stack: it still declares and requires a `backends::IBackendProvider*` whose header was deleted in commit `cc430c4`. Async completion callbacks are stubs, `BatchBuilder` is dead code, and the subdir is **disabled** in the top-level build
**Library:** `libtutti_coordinator` (`coordinator_impl.cpp` + `raw_device_impl.cpp` + `buffer_registry.cpp`; currently not built — the `.a`/`.so` on disk are stale pre-`cc430c4` artifacts)
**Location:** `tutti/coordinator/`

**Scope:** This document covers **Layer 6 — the `coordinator`**, the single top-level object an application constructs to get a working Tutti stack. The layers it drives are **below** and out of scope here: the Layer 5 storage interfaces are in [layer5-storage-interfaces.md](layer5-storage-interfaces.md), the Layer 4 IO Engine in [layer4-io-engine.md](layer4-io-engine.md). The `raw_device` (`IRawDevice`) is **physically packaged in this same directory** (`tutti/coordinator/`) but is documented as a **Layer 5** app-facing peer in [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §4; here it is referenced only as a sub-service the coordinator constructs, owns, and exposes — not re-documented.

> **Read this first — what the coordinator actually orchestrates.** The intuitive model is that the coordinator "orchestrates all the layers" by *constructing and bringing up* the device manager, backend, IO engine, and storage interfaces. The code refutes the construction half. The four lower-layer objects arrive **already built**, injected through `CoordinatorConfig` (`coordinator_types.h:29`); the coordinator merely caches their pointers, wires a default stream, builds one sub-service (`RawDeviceImpl`), and then **dispatches** per-IO work downward. It is a thin façade / dispatcher, not a builder of the stack, and it touches Layer 2 (device manager) **not at all**. See §1 for the model verdict and §4.3 for the actual bring-up ordering.

---

## 1. Overview

The coordinator is the **application-facing entry point** that ties the lower layers into one usable object. An application holds an `ICoordinator`, registers its GPU/host buffers once, and then submits read/write batches — the coordinator forwards each batch to the injected IO Engine and exposes the two storage data paths (`block_storage` and `raw_device`) for callers that want the storage-interface abstractions directly.

Concretely it does four things:

- **Owns the buffer registry.** `register_buffer` / `unregister_buffer` delegate the real GPU pin/registration to the `IAccelerator` HAL, then record the returned `MemoryRegion` in an in-memory `BufferRegistry` for validation and stats (§5).
- **Dispatches IO batches.** `submit_read_batch` / `submit_write_batch` validate each request against the registry, copy the requests into a `std::vector<tutti::IoRequest>`, and make a single `io_engine_->submit_batch(...)` call (§6). The async variants launch through `submit_batch_async` but never fire the completion callback (§4.2, §7).
- **Owns and exposes the two data paths.** It constructs and owns a `RawDeviceImpl` (`IRawDevice`, the L5 namespace/LBA peer packaged here) and exposes both it (`get_raw_device`) and the injected `IBlockStorage` (`get_block_storage`).
- **Passes capacity queries downward.** `max_batch_size()` and `slice_fanout()` are pure pass-throughs to the IO Engine.

### Model verdict: does it "orchestrate all the layers"?

The claim was checked against the source. Verdict: **PARTIAL — coordination yes, construction no.**

- **It does not construct the lower layers.** `initialize()` copies the config and caches `backend_provider_` / `accelerator_` / `block_storage_` / `io_engine_` straight from the injected `CoordinatorConfig` (`coordinator_impl.cpp:26-30`); `CoordinatorConfig::is_valid()` requires all four non-null (`coordinator_types.h:38-41`). There is no `create_*` / `new` of a backend, engine, or block storage anywhere in the coordinator. They are dependency-injected, fully built, by whoever assembles the stack above.
- **It constructs exactly one layer object.** The only lower-layer object the coordinator itself builds is the `RawDeviceImpl` sub-service — `make_unique<RawDeviceImpl>(backend_provider_, io_engine_)` (`coordinator_impl.cpp:40`) — into which it forwards the *injected* backend and engine.
- **It touches Layer 2 not at all.** There is no `IDeviceManager` / `VDevice` / `device_manager` reference anywhere under `coordinator/src` or `coordinator/include`; vdevice provisioning happens below the coordinator. The "wires L2" part of the model is not reflected in code.
- **It owns none of the injected layers.** `cleanup()` resets the owned `RawDeviceImpl` and destroys the owned default stream, but never deletes the backend / engine / storage (they are not owned; `coordinator_impl.cpp:52-80`).

So the coordinator *coordinates* an end-to-end request by dispatching to already-built layers; it is not the stack builder the "orchestrates all layers" phrasing implies.

---

## 2. Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  Application  (holds one ICoordinator; registers buffers; submits) │
└───────────────────────────────┬────────────────────────────────────┘
                                 │ register_buffer / submit_*_batch
                                 │ get_block_storage() / get_raw_device()
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Coordinator (Layer 6)   ICoordinator ── CoordinatorImpl              │
│                                                                       │
│   OWNS:   BufferRegistry        (ptr/id maps + byte/count stats)      │
│           RawDeviceImpl         (IRawDevice — L5 peer, packaged here) │
│           default AccelStream   (create-if-absent, destroy-on-clean)  │
│           BatchBuilder          (declared; DEAD — never invoked)      │
│                                                                       │
│   HOLDS (injected via CoordinatorConfig, NOT owned, NOT constructed): │
│           backends::IBackendProvider* backend_provider_  ── RETIRED   │
│           IAccelerator*               accelerator_                    │
│           block_storage::IBlockStorage* block_storage_               │
│           IIoEngine*                  io_engine_                      │
└──────┬───────────────┬────────────────────┬───────────────┬──────────┘
       │ register_host/ │ submit_batch(_async)│ get_block_    │ owns +
       │ device/external│ (forwards batch)    │ storage()     │ drives
       │ (pin via HAL)  │                     │ pass-through  │
       ▼                ▼                     ▼               ▼
┌────────────┐  ┌───────────────────┐  ┌───────────────┐  ┌──────────────┐
│ IAccelerator│ │ IIoEngine (L4)    │  │ IBlockStorage │  │ RawDeviceImpl│
│ (HAL, L1)  │  │ submit_batch /    │  │ (L5, block/   │  │ (L5, ns+LBA) │
│            │  │ submit_batch_async│  │  file mode)   │  │ submit_read/ │
└────────────┘  └─────────┬─────────┘  └───────┬───────┘  │ write →      │
                          │ IBatchSubmitter    │ (never    │ io_engine_   │
                          ▼   (NVMe SPI)        │  calls    └──────┬───────┘
                ┌───────────────────────┐      │  engine)         │
                │ Layer 3: Backends      │◄─────┘◄─────────────────┘
                │ NvmeBackend            │  StorageTarget{NVME_RAW,…}
                │  — but coordinator +   │
                │    RawDeviceImpl bind  │  Layer 2 (device manager):
                │    the DELETED         │  NOT referenced by the
                │    IBackendProvider —  │  coordinator at all.
                └────────────────────────┘
```

The coordinator sits *above* every storage layer and drives them, but the only edges it actually constructs are the `RawDeviceImpl` sub-service and the default stream. Every other edge is a call on an injected pointer. Both the coordinator itself and the `RawDeviceImpl` it owns still bind to the **deleted** `backends::IBackendProvider` SPI, so the whole `libtutti_coordinator` library does-not-compile against the current tree (§7, Known Issues & Gaps).

---

## 3. Coordinator interface & impl

**Files:** `include/coordinator.h`, `include/coordinator_types.h`, `include/raw_device.h`, `src/coordinator_impl.{h,cpp}`, `src/buffer_registry.{h,cpp}`, `src/batch_builder.h`, `src/raw_device_impl.{h,cpp}`
**Library:** `libtutti_coordinator` (currently not built)

### 3.1 Purpose

`ICoordinator` (`coordinator.h:17`) is the single object an application constructs to obtain a working stack. Its concrete `CoordinatorImpl` (`coordinator_impl.h:18`) holds the four injected layer pointers, an in-memory `BufferRegistry`, a (dead) `BatchBuilder`, and a `unique_ptr<RawDeviceImpl>`, and forwards per-IO work to `io_engine_->submit_batch(_async)` and buffer registration to `accelerator_`.

### 3.2 Key types

| Type | File | Role |
|------|------|------|
| `ICoordinator` | `coordinator/include/coordinator.h:17` | Public app-facing interface: buffer register/unregister, sync + async read/write batch submit, two data-path getters, capacity queries. Namespace `tutti::coordinator`. |
| `CoordinatorImpl` | `coordinator/src/coordinator_impl.h:18` | Concrete `ICoordinator`. Caches injected `backend_provider_`/`accelerator_`/`block_storage_`/`io_engine_`; owns `BufferRegistry`, `BatchBuilder`, `unique_ptr<RawDeviceImpl>`, default stream, and an `open_raw_targets_` set guarded by `init_lock_`/`targets_lock_`. |
| `CoordinatorConfig` | `coordinator/include/coordinator_types.h:29` | **Dependency-injection struct.** Carries the four lower-layer pointers + `max_batch_size` (default 128) + optional `default_stream`. `is_valid()` (`:38`) requires all four pointers non-null and `max_batch_size>0`. This is *how* the layers reach the coordinator — built elsewhere, handed in. |
| `coordinator_types.h` types | `coordinator/include/coordinator_types.h` | `RawTargetHandle` (`:44`), `BatchSubmitResult{success,completed,failed,error_code}` (`:57`), `BufferRegistrationInfo` (`:68`), `NamespaceInfo` (`:82`), `BatchCompletionCallback` typedef (`:93`). |
| `coordinator::IoRequest` | `coordinator/include/raw_device.h:16` | Coordinator-level per-IO record `{MemoryRegion* region, void* target_handle, byte_offset, byte_length}` — a mirror of `tutti::IoRequest` (`io_engine/include/io_types.h:11`) for the public batch API. `submit_*` copies it **field-by-field** into a `std::vector<tutti::IoRequest>` before calling the engine. |
| `gpu_file.h` | `coordinator/include/gpu_file.h` | Listed as a `LAYER_HEADERS` entry (`CMakeLists.txt:35`); it is part of the orphaned `tutti::GpuFile` family documented at [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §3.2, not driven by the coordinator. Not in `LAYER_SOURCES`. |
| `BatchBuilder` | `coordinator/src/batch_builder.h:10` | Declares `pack_requests()` to split requests under `max_batch_size`. **Declared but never used** — held as a member (`coordinator_impl.h:81`) yet `grep` of `coordinator_impl.cpp` shows zero references; submit paths forward the whole vector straight to the engine with no packing (§5). |
| `BufferRegistry` | `coordinator/src/buffer_registry.h:17` | Pure in-memory bookkeeping: `ptr→region` + `id→region` maps and byte/count stats under a `shared_mutex`. Does **no** pinning itself — pinning is the HAL's job (§5). |
| `RawDeviceImpl` | `coordinator/src/raw_device_impl.h:18` | L5 peer sub-service the coordinator constructs and owns; ctor `(backends::IBackendProvider*, IIoEngine*)`, keeps `handle_map_<RawTargetHandle*, backends::StorageTarget*>`. Documented as **Layer 5** in [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §4; referenced here only as an owned component. |
| `backends::IBackendProvider*` | `coordinator_impl.h:71`, `coordinator_types.h:30` | **RETIRED TYPE.** The coordinator still declares and *requires* a pointer to the deleted `IBackendProvider`; the header `backends/include/backend_provider.h` no longer exists. This is the root of the compile break (§7). |

### 3.3 Public API

All methods on `ICoordinator` (`include/coordinator.h`); impl in `src/coordinator_impl.cpp`.

| Method | Signature | Role |
|--------|-----------|------|
| `initialize` | `bool initialize(const CoordinatorConfig&)` | Validate config → cache the four injected layer pointers → create/adopt the default stream → build the owned `RawDeviceImpl`. Guards against double-init under `init_lock_`. Impl `:15`. |
| `cleanup` | `bool cleanup()` | Refuse if buffers are still registered or raw targets still open; then `raw_device_impl_.reset()` and destroy the owned stream. Does **not** tear down injected layers (not owned). Impl `:52`. |
| `register_buffer` | `MemoryRegion* register_buffer(void* ptr, size_t size, MemoryKind kind)` | Switch on `MemoryKind` → `accelerator_->register_host` / `register_device` / `register_external` (HAL does the actual pin), then `buffer_registry_.add_region`. Impl `:82`. |
| `unregister_buffer` | `bool unregister_buffer(MemoryRegion*)` | `buffer_registry_.remove_region` then `accelerator_->unregister`. Impl `:119`. |
| `submit_read_batch` | `BatchSubmitResult submit_read_batch(IoRequest*, uint32_t count, AccelStream)` | Validate → choose stream → copy field-by-field into `std::vector<tutti::IoRequest>` → single `io_engine_->submit_batch(req_vec, /*is_read=*/true, stream)`. Impl `:156`. |
| `submit_write_batch` | `BatchSubmitResult submit_write_batch(IoRequest*, uint32_t count, AccelStream)` | Same, `is_read=false`. Impl `:186`. |
| `submit_read_batch_async` | `bool submit_read_batch_async(IoRequest*, count, AccelStream, BatchCompletionCallback, void* user_data)` | `io_engine_->submit_batch_async(..., true, stream)`; returns launch success. **Callback/`user_data` accepted then ignored** — event mechanism is a TODO stub (`:247`). Impl `:216`. |
| `submit_write_batch_async` | `bool submit_write_batch_async(...)` | Same, `is_read=false`; callback never invoked (TODO at `:285`). Impl `:254`. |
| `get_block_storage` | `block_storage::IBlockStorage* get_block_storage()` | Return the injected `block_storage_` pointer (pass-through). Impl `:290`. |
| `get_raw_device` | `IRawDevice* get_raw_device()` | Return the owned `raw_device_impl_.get()`. Impl `:294`. |
| `max_batch_size` | `uint32_t max_batch_size() const` | Pass-through to `io_engine_->max_entries_per_batch()`. Impl `:298`. |
| `slice_fanout` | `uint32_t slice_fanout(MemoryRegion*) const` | Pass-through to `io_engine_->slice_fanout(region)`. Impl `:305`. |
| `create_coordinator` / `destroy_coordinator` | free functions | Factory `new CoordinatorImpl()` / `delete`. Impl `:312` / `:316`. |

### 3.4 Bring-up ordering (what wires the layers)

`initialize()` is the wiring step. It **caches** injected layers and **constructs** only the one sub-service (`coordinator_impl.cpp:15-50`):

1. **Guard.** Lock `init_lock_`; refuse if already initialized (`:18`). Refuse if `!config.is_valid()` — all four injected pointers must be non-null and `max_batch_size>0` (`:22`, `coordinator_types.h:38`).
2. **Cache injected layers (no construction).** `config_ = config`, then cache `backend_provider_` / `accelerator_` / `block_storage_` / `io_engine_` from the config (`:26-30`). These are the fully-built L3/L4/L5 objects; the coordinator creates none of them.
3. **Default-stream wiring.** If `config.default_stream` is invalid, `default_stream_ = accelerator_->create_stream()` and `owns_default_stream_ = true`; otherwise adopt the caller's stream with `owns_default_stream_ = false` (`:32-38`).
4. **Build the one owned sub-service.** `raw_device_impl_ = make_unique<RawDeviceImpl>(backend_provider_, io_engine_)` — forwarding the *injected* backend and engine into the L5 raw-device peer (`:40`). On failure, destroy the owned stream and bail (`:41-46`).
5. **Mark initialized** (`:48`).

`cleanup()` reverses it defensively (`:52-80`): refuse while `buffer_registry_` is non-empty (`:59`) or `open_raw_targets_` is non-empty (`:64-70`), then `raw_device_impl_.reset()` (`:72`) and destroy the owned stream (`:74-76`). Injected layers are never destroyed — the coordinator owns only the raw-device sub-service and the conditionally-owned stream.

---

## 4. Buffer registry & batch builder

Two pre-IO responsibilities live in the coordinator: buffer registration (implemented, HAL-delegated) and request batching (declared but dead).

### 4.1 BufferRegistry — bookkeeping over HAL-owned pinning

**Files:** `src/buffer_registry.{h,cpp}`

`BufferRegistry` (`buffer_registry.h:17`) is **pure in-memory bookkeeping**: two maps (`ptr_to_region_map_`, `id_to_region_map_`) plus `total_registered_bytes_` / `active_region_count_` stats, all under a `shared_mutex` (`buffer_registry.h:38-43`). It does **no** `cudaHostRegister` and no pinning of its own.

The actual pin/registration is delegated to the `IAccelerator` HAL. `register_buffer` switches on `MemoryKind` and calls `accelerator_->register_host` (HOST), `register_device` (DEVICE), or `register_external` (EXTERNAL) (`coordinator_impl.cpp:93-105`); only the returned `MemoryRegion*` is then recorded via `buffer_registry_.add_region` (`:111`). If bookkeeping fails, the HAL registration is rolled back with `accelerator_->unregister` (`:112`). `unregister_buffer` is the inverse: remove from the registry, then `accelerator_->unregister` (`:124-128`).

The registry is also the submit-time **validation gate**: `validate_requests` rejects any request whose `region` is null or whose `region->host_ptr` is not present via `lookup_by_ptr` (`coordinator_impl.cpp:140-151`) — an IO cannot target an unregistered buffer.

### 4.2 BatchBuilder — declared, never invoked (dead code)

**File:** `src/batch_builder.h`

`BatchBuilder::pack_requests(requests, count, max_batch_size)` (`batch_builder.h:15`) is declared to split a request array into `max_batch_size`-bounded batches. It is held as a member (`coordinator_impl.h:81`) but is **never called**: `grep` of `coordinator_impl.cpp` finds no reference, and every submit path forwards the *whole* request vector to `io_engine_->submit_batch` in a single call regardless of `config_.max_batch_size` (§6). The `max_batch_size` config field is validated (`is_valid()`) but not honored on the submit path; the only capacity signal the coordinator surfaces is the pass-through `max_batch_size()` → `io_engine_->max_entries_per_batch()`. Request packing is unimplemented (§7).

---

## 5. End-to-end IO flow

A single application read going Coordinator → IO Engine → Backend → GPU and back. Steps marked **(stub)** or **(does-not-compile)** are called out honestly; the async-callback path is a stub, and the whole library does-not-compile against the current stack (§7).

```
Application
  │ (once) region = coordinator->register_buffer(ptr, size, MemoryKind::HOST)
  ├─────────────► accelerator_->register_host(ptr, size)      ── HAL pins/maps
  │               buffer_registry_.add_region(region)         ── bookkeeping
  │
  │ (per handle) target_handle acquired via a data path the coordinator EXPOSES
  │              but does not itself drive:
  │                get_raw_device()->acquire_raw_target(ns, lba, len)   [L5 raw]
  │                     — or —  get_block_storage()->acquire_device_handle(...) [L5 block]
  │              → yields the opaque target_handle the caller puts in each IoRequest
  │
  │ submit_read_batch(reqs[], count, stream)
  ▼
CoordinatorImpl::submit_read_batch                                   ── Layer 6
  ├─ validate_requests(): every req.region must be in buffer_registry_
  ├─ target_stream = stream.is_valid() ? stream : default_stream_
  ├─ copy field-by-field: coordinator::IoRequest[] → std::vector<tutti::IoRequest>
  │     (no BatchBuilder packing — whole vector forwarded as one batch)
  ▼
io_engine_->submit_batch(req_vec, is_read=true, target_stream)       ── Layer 4
  ├─ MDTS fan-out → prepare_descriptors → memcpy_async(CPU→GPU)
  │     (batch path routes the whole batch to req_vec[0].target_handle — see L4 §8)
  ▼
NvmeBackend / IBatchSubmitter::launch_batch_gpu_stream(...)          ── Layer 3
  └─ GPU submit kernel: resolve_lba → build SQE → ring doorbell → poll CQ
  ◄─ synchronize_stream → release_descriptors → returns bool
  ▲
  └─ CoordinatorImpl wraps the bool into BatchSubmitResult(success, count|0, …)

Async variant (submit_read_batch_async):
  io_engine_->submit_batch_async(...) is launched, BUT the BatchCompletionCallback
  is (stub) NEVER invoked — no event is recorded/observed (coordinator_impl.cpp:247,285).
```

What is / isn't wired, honestly:

- **Wired:** buffer registration through the HAL; synchronous batch submit down to the engine; capacity pass-throughs; ownership + exposure of `RawDeviceImpl`; exposure of the injected `IBlockStorage`.
- **Not driven by the coordinator:** target-handle acquisition. The coordinator's own submit path takes a **pre-built** `target_handle` in each `IoRequest`; acquiring handles is the job of the `raw_device` / `block_storage` sub-services (which the coordinator exposes). The coordinator does not itself call `acquire_target_handle`.
- **Stub:** async completion callbacks — launched but never fired.
- **Compile reality:** the `block_storage` path never actually calls the engine (see [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §3.5), and the entire `libtutti_coordinator` does-not-compile against the current stack (§7). The end-to-end path above is the *intended* shape; it cannot be built or run as-is today.

---

## Implementation Status

| Component | Status | Tested |
|-----------|--------|--------|
| `ICoordinator` interface + type surface (`coordinator.h`, `coordinator_types.h`) | Complete (self-contained interface) | Via coordinator tests (not built — see below) |
| `CoordinatorImpl` init/cleanup lifecycle (double-init guard, stale-state guards, default-stream ownership) | Complete | Against mocks only |
| `register_buffer` / `unregister_buffer` (HAL-delegated pin + registry bookkeeping + rollback) | Complete | Against mocks only |
| `BufferRegistry` (thread-safe ptr/id maps + byte/count stats, `shared_mutex`) | Complete | Against mocks only |
| `submit_read_batch` / `submit_write_batch` (validate → copy → single `submit_batch`) | Complete | Against mocks only |
| `submit_read_batch_async` / `submit_write_batch_async` completion callbacks | Stub (launched via engine; callback/`user_data` never invoked — TODO `coordinator_impl.cpp:247,285`) | No |
| `BatchBuilder` / request packing (honor `max_batch_size` when forwarding) | Not implemented (declared member `pack_requests` never called; whole vector forwarded) | No |
| `get_block_storage` / `get_raw_device` getters + `max_batch_size` / `slice_fanout` pass-throughs | Complete | Against mocks only |
| Ownership + construction of `RawDeviceImpl` sub-service | Complete (built at init, reset at cleanup) | Against mocks only |
| `create_coordinator` / `destroy_coordinator` factory | Complete | Against mocks only |
| Construction / bring-up of L2–L5 (device manager, backend, engine, block storage) | Not implemented by design (all injected via `CoordinatorConfig`) | N/A |
| Any Layer 2 (device manager) involvement | Not implemented (no `IDeviceManager`/`VDevice`/`device_manager` reference anywhere) | N/A |
| `libtutti_coordinator` compile/link vs the current stack | Does-not-compile (declares/requires deleted `backends::IBackendProvider`; includes non-existent `backends/include/backend_provider.h`) | No |
| Coordinator subdir enablement | Does-not-compile / disabled (`tutti/CMakeLists.txt:134` commented out) | No |
| `layer6_smoke_test` / `layer6_integration_test` | Does-not-compile (each `#include`s `backend_provider.h` and defines a `MockBackendProvider : backends::IBackendProvider`) | No |
| `layer6_hw_basic_test` (includes `backend_provider.h`, no mock) / `layer6_basic_test` / `layer6_smoke_test_simple` (neither) | Compile as TUs but cannot link (`tutti_coordinator` uncompilable; subdir never configured) | No |

The on-disk artifacts (`tutti/build/coordinator/libtutti_coordinator.a` dated 2026-07-25 and `build/lib/libtutti_coordinator.so`) are **stale** — built *before* the 2026-07-27 deletion of `IBackendProvider` in commit `cc430c4`; the current source cannot be recompiled as-is. Context docs under `tutti/coordinator/` (`README.md`, `FINAL_VERIFICATION.md`, `VERIFICATION_REPORT.md`, `SMOKE_TEST_RESULTS.md`) describe *passing* smoke/integration tests, but those runs predate `cc430c4` and reflect the retired `IBackendProvider` API — they are **intent, not the current state**.

---

## Known Issues & Gaps

The coordinator façade is functionally complete against mocks, but it is bound to a retired SPI, its async and batching stages are unfinished, and it is disabled in the build. The items below are concrete limitations, each with file:line evidence.

- **Does-not-compile: still bound to the deleted `backends::IBackendProvider`.** `coordinator_impl.h:8` and `raw_device_impl.h:5` `#include "backends/include/backend_provider.h"`, a header deleted in commit `cc430c4` (2026-07-27, "migrate NVMe backend to IBackend"). The current L3 exposes `tutti::backends::IBackend` (`backends/include/backend.h:58`), not `IBackendProvider`. `coordinator_impl.h:71` and `coordinator_types.h:30` still declare a `backends::IBackendProvider*` member, and `CoordinatorConfig::is_valid()` (`coordinator_types.h:38`) requires it non-null. `RawDeviceImpl`'s ctor and `handle_map_<…, backends::StorageTarget*>` (`raw_device_impl.h:20,65`) also depend on it. Note `coordinator_types.h:6` still includes `backends/include/backend_types.h`, which *does* still exist but does **not** define `IBackendProvider`. Fix: migrate the coordinator + `RawDeviceImpl` off `IBackendProvider` onto the current `IBackend` / `IBatchSubmitter` seam and retype the config member — the same Phase-0 migration already flagged for `io_engine` and `block_storage`.
- **Subdir disabled in the top-level build.** `tutti/CMakeLists.txt:134` has `# add_subdirectory(coordinator)` commented out (only L2/L3/L4 are enabled at `:130-132`). Re-enabling requires fixing the compile break first. The stale `.a` (2026-07-25) predates the SPI deletion.
- **No test can build today, though the mechanism varies.** Two tests define a mock against the deleted SPI: `layer6_smoke_test.cpp` (`:6` include, `:18` def) and `layer6_integration_test.cpp` (`:4` include, `:16` def) each `#include` `backend_provider.h` and define a `MockBackendProvider : public backends::IBackendProvider` — these fail to compile outright. `layer6_hw_basic_test.cpp` (`:29`) includes `backend_provider.h` but defines no mock. `layer6_basic_test.cpp` and `layer6_smoke_test_simple.cpp` include neither the deleted header nor any mock (they set `config.backend_provider = nullptr`) and would compile as translation units — but still cannot **link**, because `tutti_coordinator` itself is uncompilable and the subdir is disabled, so `coordinator/tests/` is never even configured. The two `MockBackendProvider` definitions must move to the new SPI, and the tests re-enabled. Collectively they exercised construction, buffer (multi-)registration, raw-target acquire/release, block-storage getter, batch-submit validation, slice-fanout, and namespace query — all **against mocks, no real IO**.
- **Async completion callbacks are a stub.** `submit_read_batch_async` / `submit_write_batch_async` accept `BatchCompletionCallback callback` + `void* user_data`, launch via `io_engine_->submit_batch_async`, and then **never** invoke the callback — explicit TODO comments note the event record/observe mechanism is unimplemented (`coordinator_impl.cpp:247,285`). Callers relying on completion notification will never be signaled.
- **`BatchBuilder` is dead code; `max_batch_size` is not honored on submit.** `pack_requests` (`batch_builder.h:15`) is declared and the member is held (`coordinator_impl.h:81`) but never called; submit paths forward the whole request vector to `io_engine_->submit_batch` in one call regardless of `config_.max_batch_size` (`coordinator_impl.cpp:171-181,201-211`). Request splitting/packing is unimplemented.
- **The coordinator constructs no lower layer except `RawDeviceImpl`.** No device manager, backend, IO engine, or block storage is created here; all four arrive via `CoordinatorConfig` (`coordinator_types.h:29-33`) and are cached in `initialize()` (`coordinator_impl.cpp:26-30`). The "orchestrates all layers" model holds only for *coordination/dispatch*, not construction. This is by design, but it means bring-up of L2–L5 is the caller's responsibility.
- **No Layer 2 (device manager) wiring at all.** There is no `IDeviceManager` / `VDevice` / `vdevice` / `device_manager` reference under `coordinator/src` or `coordinator/include`; vdevice provisioning lives below the coordinator. The model's "wires L2" claim is not reflected in code.
- **Coordinator does not acquire target handles for its own submit path.** `IoRequest.target_handle` must be supplied **pre-built** by the caller (`raw_device.h:16`); the coordinator never calls `acquire_target_handle` on its `submit_*` path. Handle acquisition is owned by the `raw_device` / `block_storage` sub-services it exposes.
- **Inherits every lower-layer gap it depends on.** Because it dispatches downward, the coordinator cannot deliver capabilities the layers below lack: `block_storage` **never calls the IO Engine** and does-not-compile (see [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §3.5, Known Issues); the L4 `StripeManager` is **not wired into the engine** (see [layer4-io-engine.md](layer4-io-engine.md) §8); and **FILE/FIEMAP extent metadata** does not exist anywhere in the stack, so both exposed data paths are RAW-extent only. The coordinator's end-to-end read/write is therefore blocked below it even after its own compile break is fixed.
- **Layer-numbering / packaging note: this directory also holds the L5 `raw_device`.** `tutti/coordinator/` (build-labeled Layer 6, `CMakeLists.txt:18`) physically packages both the coordinator orchestrator **and** the authoritative `IRawDevice` / `RawDeviceImpl`, which is conceptually a **Layer 5** app-facing peer of `block_storage` (documented in [layer5-storage-interfaces.md](layer5-storage-interfaces.md) §4). Both share the same compile fate via the deleted `IBackendProvider`. The target reorganization moves `raw_device` out to `tutti/storage-interfaces/` (see the L5 doc), which would leave the coordinator alone at Layer 6.

---

## Related Documents

- [Layer 5: Storage Interfaces (Block Storage + Raw Device)](layer5-storage-interfaces.md) — the two app-facing data paths the coordinator exposes; also the authoritative documentation for `raw_device`, which is physically packaged in this Layer 6 directory but is conceptually an L5 peer.
- [Layer 4: IO Engine + StripeManager](layer4-io-engine.md) — the layer the coordinator's `submit_*_batch` forwards to via `IIoEngine::submit_batch` / `submit_batch_async`, and the source of the `max_entries_per_batch` / `slice_fanout` capacity pass-throughs.
- [Architecture Overview (L0–5)](architecture-overview.md)
