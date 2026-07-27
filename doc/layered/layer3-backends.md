# Layer 3: Backends

**Version:** 1.1
**Date:** 2026-07-27
**Status:** Device-agnostic `IBackend` live; mock backend complete; NVMe backend migrated to `IBackend` + `NvmeVirtualDevice` and verified on real hardware (GPU read/write round-trip)
**Library:** `libtutti_backends` (device-agnostic core + mock) · `libtutti_backends_nvme` (NVMe transport)
**Location:** `tutti/backends/`

---

## 1. Motivation

Layer 2 hands out **virtual devices** — resource slices of a physical controller, each an `IVirtualDevice*` that a caller downcasts to a transport subtype. But a virtual device is just a resource grant; it does not know how to register IO contexts or submit IO. That is the job of a **backend**.

A backend adapts one transport family (NVMe, RDMA, GDS) into the operations the upper layers actually need:

- **register** — bind an IO context to a vdevice: PRP/SGL address tables, pinned host memory, and other transport-specific setup done once before the fast path.
- **submit_one** — issue a single IO against a chosen vdevice.

### What changed from v0.1

The old design bound one backend to a single `VDevice` and was batch-only. The redesign makes two structural changes:

1. **A backend owns a roster of multiple vdevices**, not one. It acquires them itself from the Device Manager during `initialize()` — they are not passed in.
2. **The device-agnostic interface fixes only what is genuinely transport-neutral**: lifecycle, the vdevice roster, a selection handle, and metadata. `register` / `submit_one` are *not* on the base interface, because their signatures — and even their host-vs-device residency — are transport-specific.

The last point is the crux. For NVMe, `submit_one` is a **device-side (`__device__`) function** invoked from a submit kernel. It physically cannot be a host virtual method. So the base SPI exposes only a transport-neutral **`VDeviceHandle`** (a dense index) to name *which* vdevice an operation targets; concrete backends define the operations themselves.

This document describes the **device-agnostic** surface (`IBackend`, the shared types, the factory), the **mock backend** that proves the boundary holds, and the concrete **NVMe backend** — the first real transport, which realizes `register` / `submit_one` as NVMe-specific host methods plus a device-side submit kernel.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  IO Engine (Layer 4)                                              │
│  selects a backend by type; drives register / submit_one         │
└──────────────────────────┬──────────────────────────────────────┘
                           │  IBackend (device-agnostic SPI)
                           │  + BackendFactory::create_backend(type)
┌──────────────────────────▼──────────────────────────────────────┐
│  Backends (Layer 3)                                               │
│                                                                   │
│  IBackend                                                         │
│  ├── initialize(dm, cfg) → opens a vdevice roster from Layer 2   │
│  ├── vdevice roster       (dense array, VDeviceHandle = index)    │
│  └── metadata / type                                              │
│                                                                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────┐  ┌─────────┐  │
│  │ NvmeBackend  │  │ RdmaBackend  │  │ GdsBackend│  │  Mock   │  │
│  │ (+ device-   │  │  (IBackend)  │  │ (IBackend)│  │ Backend │  │
│  │  side submit)│  │              │  │           │  │(IBackend│  │
│  └──────────────┘  └──────────────┘  └──────────┘  └─────────┘  │
│                                                                   │
│  BackendFactory: type → constructor registry (self-registering)  │
└──────────────────────────┬──────────────────────────────────────┘
                           │  open_vdevice() / close_vdevice()
┌──────────────────────────▼──────────────────────────────────────┐
│  Layer 2: Device Manager                                          │
│  IDeviceManager → IVirtualDevice (downcast to NvmeVirtualDevice)  │
└─────────────────────────────────────────────────────────────────┘
```

### Backend

Each transport family provides one `IBackend` implementation. A backend:

1. Acquires its vdevice roster from `IDeviceManager` at `initialize()`.
2. Keeps the vdevices in a dense array; `VDeviceHandle{index}` selects one.
3. Defines its own `register` / `submit_one` on its concrete type (host or device side, as the transport requires).
4. Returns every vdevice to the Device Manager at `shutdown()`.

Adding a new transport means implementing `IBackend` and registering it with the factory — no changes to the interface or to Layer 2.

### Factory

`BackendFactory` is a type→constructor registry. Backends self-register at static-initialization time, and the IO Engine (or a test) instantiates one by `BackendType`. This keeps the selection of a concrete backend out of the upper layers' compile-time dependencies.

---

## 3. Device-Agnostic Interfaces

This is the complete vendor-neutral surface of Layer 3: the shared types, the backend interface, and the factory. All files live under `tutti/backends/include/`. Nothing here depends on NVMe, libnvm, or CUDA — the Layer 2 facade types (`IDeviceManager`, `IVirtualDevice`) are forward-declared, not included.

### `BackendType` — Transport Family Enum
**File:** `include/backend_types.h`

```cpp
enum class BackendType {
    LOCAL_NVME = 0,  // Local NVMe via Device Manager vdevices
    RDMA       = 1,  // RDMA-capable remote storage
    GDS        = 2,  // NVIDIA GPUDirect Storage
    MOCK       = 3,  // In-memory test backend (device-agnostic; no transport)
    UNKNOWN    = 255
};
```

Mirrors Layer 2's `DeviceType` but stays independent, so Layer 3's public identity does not depend on the device_manager enum. Used as the factory key and returned by `IBackend::backend_type()`.

---

### `VDeviceHandle` — Transport-Neutral Selection Token
**File:** `include/backend_types.h`

```cpp
struct VDeviceHandle {
    static constexpr uint32_t INVALID = 0xFFFFFFFFu;

    uint32_t index;  // Dense index into the backend's vdevice roster

    VDeviceHandle() : index(INVALID) {}
    explicit VDeviceHandle(uint32_t i) : index(i) {}
    bool is_valid() const { return index != INVALID; }
};
```

The caller-facing token that names *which* vdevice an operation targets. It carries **only** a dense index into the backend's roster — never a pointer to device resources. This is deliberate: NVMe's `submit_one` runs device-side, so the host SPI must be able to name a vdevice without dereferencing anything host-resident. The backend maps the index back to its concrete `IVirtualDevice*` (and any device-side resources) internally.

---

### `BackendConfig` — Bring-Up Parameters
**File:** `include/backend_types.h`

```cpp
struct BackendConfig {
    int32_t  phys_id;             // Physical device id (from IDeviceManager registry)
    uint32_t vdevice_count;       // How many vdevices to open on this backend
    uint32_t quota_per_vdevice;   // Resource units per vdevice (e.g. NVMe queue pairs)

    BackendConfig() : phys_id(-1), vdevice_count(1), quota_per_vdevice(1) {}
};
```

Passed to `initialize()`. Tells the backend which physical device to slice and how to carve its roster: it calls `open_vdevice(phys_id, quota_per_vdevice)` exactly `vdevice_count` times. Transport-specific tuning that is *not* device-agnostic belongs on the concrete backend, not here.

---

### `BackendMetadata` / `BackendCapability` — Capability Record
**File:** `include/backend_types.h`

```cpp
enum BackendCapability : uint32_t {
    SUPPORTS_GPUDIRECT = 1 << 0,  // Can bypass CPU for data movement
    SUPPORTS_COOP      = 1 << 1,  // Supports cooperative kernel mode
    SUPPORTS_ASYNC     = 1 << 2,  // Supports CPU async submission
    SUPPORTS_SGL       = 1 << 3,  // Supports scatter-gather lists
    SUPPORTS_METADATA  = 1 << 4   // Supports metadata pass-through
};

struct BackendMetadata {
    const char* name;          // Human-readable backend name
    BackendType type;          // Backend type enum
    uint32_t    capabilities;  // Bitfield of BackendCapability flags
    size_t      max_io_size;   // Maximum single IO size in bytes
    size_t      max_batch_size;// Maximum batch descriptor count
    size_t      alignment_bytes;// Required buffer alignment
};
```

Returned by `IBackend::metadata()`. Lets Layer 4 discover a backend's transport capabilities and IO constraints without knowing the concrete type.

---

### `SubmissionResult` — Host-Side Completion Summary
**File:** `include/backend_types.h`

```cpp
struct SubmissionResult {
    bool     success;          // Overall success flag
    uint32_t completed_count;  // Number of IOs completed
    uint32_t failed_count;     // Number of IOs failed
    int      error_code;       // First error code encountered (0 = success)

    SubmissionResult() : success(false), completed_count(0), failed_count(0), error_code(0) {}
};
```

For **host-side** submission paths only (bootstrap, tests). Device-side `submit_one` paths report completion through their own transport mechanics and do not use this struct.

---

### `IBackend` — Layer 3 Facade
**File:** `include/backend.h`

Pure interface. One implementation per transport family. Forward-declares `tutti::IDeviceManager` and `tutti::IVirtualDevice` so the header stays free of libnvm / CUDA includes.

| Method | Return | Description |
|--------|--------|-------------|
| `initialize(dm, cfg)` | `bool` | Acquire the vdevice roster: open `cfg.vdevice_count` vdevices from `cfg.phys_id`, each `cfg.quota_per_vdevice` units. `dm` must be non-null and already `Open()`'d. Returns `false` on failure (unknown `phys_id`, pool exhausted); a partial roster is rolled back before returning. |
| `shutdown()` | `void` | Return every vdevice to the Device Manager and release backend resources. Idempotent — a second call is a no-op. |
| `vdevice_count()` | `uint32_t` | Number of vdevices acquired at `initialize()`. |
| `vdevice_at(i)` | `IVirtualDevice*` | The vdevice at dense index `i`, or `nullptr` if `i >= vdevice_count()`. Callers downcast to the transport subtype after checking `type()`. |
| `vdevice_handle_at(i)` | `VDeviceHandle` | Selection handle for the vdevice at index `i`; invalid handle if out of range. Pass to the concrete backend's `register` / `submit_one`. |
| `backend_type()` | `BackendType` | Transport family identity. |
| `backend_name()` | `const char*` | Human-readable name (`"local_nvme"`, `"mock"`, …). |
| `metadata()` | `BackendMetadata` | Full capability record. |

**Ownership:** the Device Manager retains ownership of the `IVirtualDevice*` pointers; the backend holds references and returns them in `shutdown()`.

**Threading:** `initialize()` / `shutdown()` are bring-up/teardown only and need not be thread-safe. Roster accessors are read-only after `initialize()` and may be called concurrently.

#### Why `register` / `submit_one` are not on `IBackend`

They are intentionally absent. Their signatures and residency are transport-specific:

- **`register`** — binds an IO context to a vdevice (PRP/SGL address tables, pinned host memory). The exact input and granularity depend on the transport and are designed alongside each concrete backend.
- **`submit_one`** — submits a single IO to a chosen vdevice. For NVMe this is a **device-side (`__device__`) function** invoked from a submit kernel, so it cannot be a host virtual method at all.

The base interface fixes only what is genuinely device-agnostic — lifecycle, roster, selection handle, metadata. Concrete backends expose `register` / `submit_one` on their own types.

---

### `BackendFactory` — Type → Constructor Registry
**File:** `include/backend_factory.h`, `src/backend_factory.cpp`

Singleton-style registry with static methods. Backends self-register at static-init time; consumers instantiate by type.

| Method | Return | Description |
|--------|--------|-------------|
| `register_backend(type, ctor)` | `void` | Register a constructor for a `BackendType`. Called at static init. Duplicate registrations replace the previous one (last wins). |
| `create_backend(type)` | `unique_ptr<IBackend>` | Instantiate a backend by type, or `nullptr` if the type is not registered. Caller must call `initialize()` before use. |
| `available_backends()` | `vector<BackendType>` | All registered types (capability discovery / testing). |
| `is_registered(type)` | `bool` | Whether a constructor exists for `type`. |

#### Registration pattern (scoped-enum caveat)

The header provides a `REGISTER_BACKEND(TYPE, CTOR)` macro, but it **token-pastes** its type argument into an identifier (`registrar_##TYPE`). That breaks for a scoped enum value such as `BackendType::MOCK` (which is not a valid identifier fragment). Backends therefore register through an **explicit static registrar struct** instead:

```cpp
namespace {
struct MockBackendRegistrar {
    MockBackendRegistrar() {
        BackendFactory::register_backend(
            BackendType::MOCK,
            []() -> IBackend* { return new MockBackend(); });
    }
};
static MockBackendRegistrar g_mock_backend_registrar;
} // anonymous namespace
```

The NVMe backend follows the same explicit-registrar pattern.

---

### Call Relationships

```
Layer 4 (IO Engine / test)
   │
   │  BackendFactory::create_backend(BackendType::X)
   ▼
IBackend*  (concrete backend, not yet usable)
   │
   │  initialize(dm, {phys_id, vdevice_count, quota_per_vdevice})
   ▼
   ├─► dm->open_vdevice(phys_id, quota)   ×vdevice_count
   │      └─ on any failure: close_vdevice() the partial roster, return false
   │
   │  (fast path — concrete backend only)
   ├─► register(VDeviceHandle, io-context...)
   ├─► submit_one(VDeviceHandle, io...)     // host- or device-side per transport
   │
   │  shutdown()
   └─► dm->close_vdevice(vdev)  ×roster     // idempotent
```

---

## 4. Mock Backend

**Files:** `mock/include/mock_backend.h`, `mock/src/mock_backend.cpp`

`MockBackend` is the Layer 3 peer of Layer 2's `MockDeviceDriver`. It is an in-memory `IBackend` that depends only on the vendor-neutral interfaces (`IBackend`, `IDeviceManager`, `IVirtualDevice`) and pulls in **zero** NVMe / libnvm / CUDA symbols. Its job is to prove that the device-agnostic boundary is real: a target that links `MockBackend` + `BackendFactory` + `DeviceManagerImpl` + `MockDeviceDriver` and nothing else confirms the boundary holds.

### Behaviour

- **`initialize(dm, cfg)`** — rejects a null manager, zero `vdevice_count`, or zero `quota_per_vdevice`. Otherwise opens `cfg.vdevice_count` vdevices from `cfg.phys_id`. On *any* failure it rolls back every vdevice already opened (via `close_vdevice`), clears the roster, and returns `false`.
- **`shutdown()`** — returns every vdevice to the Device Manager and clears state. Idempotent (returns immediately if never initialized or already shut down). The destructor calls it.
- **Roster accessors** — bounds-checked: `vdevice_at` returns `nullptr` and `vdevice_handle_at` returns an invalid handle for out-of-range indices.
- **`register` / `submit_one`** — intentionally absent (this round covers only the device-agnostic contract).

### Test observability

`MockBackend` exposes diagnostic counters so tests can assert lifecycle behaviour without transport side effects: `initialize_count()`, `shutdown_count()`, `is_initialized()`.

---

## 5. NVMe Backend

**Library:** `libtutti_backends_nvme`
**Files:** `nvme/include/`, `nvme/src/`, `nvme/device/`

`NvmeBackend` is the first concrete transport. It implements the device-agnostic `IBackend` contract (lifecycle, roster, metadata) and, on top of that, realizes the two operations the base interface deliberately leaves out — `register` and `submit_one` — in the form NVMe actually requires:

- **`register`** becomes a set of **non-virtual host methods** that build the GPU-resident state a submit needs: `acquire_target_handle` (per storage target) and `prepare_descriptors` (per IO buffer).
- **`submit_one`** is a **`__device__` function** (`submit_read_one` / `submit_write_one`) invoked from a GPU submit kernel — it cannot be a host virtual method, which is exactly why it is absent from `IBackend`. The host launch entry point is `launch_batch_gpu_stream`.

`NvmeBackend` downcasts each `IVirtualDevice*` in its roster to `NvmeVirtualDevice*` — the sole cross-boundary cast, confined to this backend.

### 5.1 Component map

```
NvmeBackend  (nvme_backend.cpp)          — IBackend impl + orchestration
 ├── roster: vector<NvmeVirtualDevice*>   (downcast from IVirtualDevice*)
 ├── NvmeCommandBuilder (nvme_command_builder.cpp)  — ioaddrs → PRP descriptors
 ├── PrpPageCache       (prp_page_cache.cpp)        — two-tier PRP-list page pool
 ├── target handles     (nvme_target_handle.cpp)    — GPU-resident NvmeFileDeviceHandle
 └── submission         (nvme_submission.cpp)        — host launch wrappers
        └── device/submit_batch_kernel.cu            — GPU kernel
                └── device/nvme_device_helpers.cuh   — __device__ submit_one + CQ poll
```

`nvme_backend_registration.cpp` self-registers `NvmeBackend` with `BackendFactory` under `BackendType::LOCAL_NVME`, using the same explicit-registrar pattern as the mock backend (the `REGISTER_BACKEND` macro cannot token-paste a scoped enum value).

### 5.2 Public interface

`NvmeBackend` (`nvme/include/nvme_backend.h`) — the IBackend methods plus these NVMe-specific host methods, all called **after** `initialize()`:

| Method | Return | Description |
|--------|--------|-------------|
| `acquire_target_handle(target, hdl)` | `void*` | Build a GPU-resident `NvmeFileDeviceHandle` for `target`, bound to the vdevice named by `hdl` (`VDeviceHandle`). Copies the vdevice's queue slice (`d_qps`, `queue_quota`) and namespace params inline, and `cudaMalloc`s the extent arrays. Returns an opaque device pointer, or `nullptr` on failure. Tracked internally for cleanup. |
| `release_target_handle(handle)` | `void` | Free the GPU handle and its inline/overflow extent arrays. Warns on an untracked pointer. |
| `prepare_descriptors(ioaddrs, slices, n, out_descs)` | `bool` | Build one `BufferDescriptor` per sub-slice from DMA `ioaddrs` (PRP SINGLE/DUAL/LIST). `false` if MDTS is exceeded or the PRP cache is exhausted. Shared across all vdevices. |
| `release_descriptors(descs, n)` | `void` | Return any PRP-list pages held by the descriptors to the cache. |
| `launch_batch_gpu_stream(stream, handle, descs, n, is_read)` | `void` | Launch the GPU submit kernel on `stream` (one thread per descriptor). Primary fast path. |
| `submit_batch_cpu_sync(handle, descs, n, is_read)` | `SubmissionResult` | CPU synchronous submission. **Currently a stub** (reports success without issuing IO) — bootstrap/testing placeholder, see §5.6. |

Metadata: `backend_type()` → `LOCAL_NVME`, `backend_name()` → `"local_nvme"`, and `metadata()` reports `SUPPORTS_GPUDIRECT`, with `max_io_size` / `alignment_bytes` taken from the first vdevice's `max_data_size` / `blk_size`.

### 5.3 Data types

Owned by the NVMe backend (`nvme/include/`), plus `StorageTarget` from the shared `include/`:

- **`StorageTarget` / `LbaExtent` / `StorageTargetKind`** (`include/storage_target.h`) — transport-neutral target description emitted by upper layers. `NVME_FILE` carries an extent array; `NVME_RAW` carries a single `start_lba` + `length_blocks` range. The backend converts this into a GPU handle.
- **`SubSliceInfo`** (`nvme_io_types.h`) — one contiguous region within a larger IO (`offset_bytes`, `length_bytes`, `slice_index` into `ioaddrs`). One sub-slice → one NVMe command.
- **`BufferDescriptor`** (`nvme_io_types.h`) — one per NVMe command: `storage_offset`, `data_length`, PRP entries `prp1`/`prp2`, a `descriptor_flags` PRP-kind tag (SINGLE/DUAL/LIST), and `backend_private` for cleanup (e.g. the PRP-list page). Must live in GPU-accessible memory for the kernel to read it.
- **`NvmeFileDeviceHandle`** (`nvme_target_handle.h`) — GPU-resident file handle the kernel consumes: file identity, namespace params, inline extents (up to `MAX_INLINE_EXTENTS = 8`) + overflow pointer, and an **inline copy** of the vdevice's queue slice (`d_qps`, `queue_quota`). No host-heap pointer is ever stored here, so the kernel never dereferences host memory.

### 5.4 PRP construction and the page cache

`NvmeCommandBuilder` (`nvme_command_builder.h`) turns DMA `ioaddrs` into PRP entries by transfer size:

- **SINGLE** (`≤ page_size`): `prp1 = ioaddrs[0]`, `prp2 = 0`.
- **DUAL** (`≤ 2·page_size`): `prp1 = ioaddrs[0]`, `prp2 = ioaddrs[1]`.
- **LIST** (`> 2·page_size`): `prp1 = ioaddrs[0]`, `prp2` = a PRP-list page populated with `ioaddrs[1..n]`.

SGL construction (`build_sgl_descriptors`) is declared but not yet implemented.

`PrpPageCache` (`prp_page_cache.h`) is a two-tier pool that keeps LIST transfers off the allocation hot path: **L1** is GPU-resident 4 KB pages, **L2** is host-pinned pages, and `cudaMalloc` is the amortized last resort. Freed pages return to the pool rather than being released. `initialize()` sizes it from the aggregate queue quota (L1 = `total_quota·2`, L2 = `L1·4`). `get_stats()` exposes hit/miss counters.

### 5.5 GPU submit path

`launch_batch_gpu_stream` launches `submit_batch_kernel` (`device/submit_batch_kernel.cu`) with one thread per descriptor. Each thread calls `submit_read_one` / `submit_write_one` (`device/nvme_device_helpers.cuh`), which:

1. **Resolve LBA** — walk the handle's inline/overflow extents to map the logical offset to a physical LBA + remaining block count (`resolve_lba`).
2. **Select a queue** — hash the global thread id across `queue_quota` to pick a `QueuePair`.
3. **Build the SQE** — acquire a CID, then `nvm_cmd_header` / `nvm_cmd_data_ptr(prp1, prp2)` / `nvm_cmd_rw_blks(lba, blocks)`.
4. **Enqueue** — `sq_enqueue` rings the SQ doorbell via libnvm's ticket protocol.
5. **Complete** — `cq_poll` spins for the CQE, then `cq_dequeue` + `put_cid` retire it.

> **Completion is handled inside `submit_one_impl`.** The step-4/5 TODO comments in `submit_batch_kernel.cu` ("assume synchronous completion (placeholder)") are stale: the CQ poll happens one level down in the device helper, so when `cudaStreamSynchronize` returns the CQE has already arrived and the DMA is complete. This is what makes the read/write-verify test sound.

### 5.6 Status and limitations

- **GPU read/write path:** complete and hardware-verified.
- **`submit_batch_cpu_sync`:** stub — returns success without issuing IO (`nvme_submission.cpp`). Real CPU submission (build SQE → `nvm_cmd_read`/`write` → poll CQ) is future work.
- **SGL descriptors:** not implemented (PRP only).
- **Multi-page LIST correctness** relies on the caller supplying page-aligned `ioaddrs`; MDTS is enforced by the command builder.

---

## 6. Testing

Two independent targets: a device-agnostic one that proves the vendor-neutral boundary, and an NVMe one that exercises the concrete backend against real hardware.

### 6.1 Device-agnostic tests — `backend_test`

**Location:** `tutti/tests/backends/` (`backend_test.cpp`, `CMakeLists.txt`)

Like the Layer 2 tests, the target **compiles the implementation sources directly** rather than linking `libtutti_backends` / `libtutti_device_manager`:

```
backend_test.cpp
  + backends/mock/src/mock_backend.cpp
  + backends/src/backend_factory.cpp
  + device_manager/src/common/device_manager_impl.cpp
  + device_manager/mock/src/mock_device_driver.cpp
  → links only GTest
```

If this binary links with no NVMe / libnvm / CUDA symbols, the Layer 3 device-agnostic boundary is proven.

**Coverage (11 tests, all passing):**

| Group | Cases |
|-------|-------|
| Lifecycle | initialize opens the roster and reports initialized; metadata identity (`MOCK`, `"mock"`, zero caps) |
| Roster + handles | in-range vdevice/handle validity; out-of-range → `nullptr` / invalid handle |
| `initialize()` guards | rejects null manager, zero vdevice count, zero quota |
| `initialize()` rollback | unknown `phys_id` (first open fails); quota exhaustion (partial roster rolled back, resources fully restored to the DM) |
| `shutdown()` | returns all vdevices to the DM and is idempotent (counter not bumped twice); reinitialize after shutdown |
| Factory | `MOCK` is registered / listed; `create_backend(MOCK)` yields a working `IBackend`; unknown type → `nullptr` |

Verified with `nm` / `ldd`: `backend_test` has no NVMe/CUDA/libnvm symbols (undefined, defined, or linked).

### 6.2 NVMe backend tests — `nvme_backend_test`

**Location:** `tutti/tests/backends/nvme/` (`nvme_backend_test.cpp`, `CMakeLists.txt`)

Links `libtutti_backends_nvme` + `libtutti_device_manager` + CUDA, and compiles the registration TU directly so the factory registrar runs. Two tiers:

**Unit tier — `NvmeBackendUnit.*` (8 tests, no hardware).** Drives `NvmeBackend` through a `DaemonNvmeDeviceDriver` in **mock mode**: initialize/shutdown lifecycle, roster accessors, config guards (null DM, zero count), idempotent shutdown, metadata identity, and factory registration. Mock mode returns `d_qps = nullptr` and zero `blk_size`, so `initialize()` substitutes safe defaults.

**Real-HW tier — `NvmeBackendRealHw.*` (skipped unless `TUTTI_NVME_REAL_HW=1` and a live daemon).** Runs against a real NVMe device via the `nvmeservice` daemon:

| Case | Asserts |
|------|---------|
| `InitializeAcquiresLiveQueues` | roster holds a live `NvmeVirtualDevice` with `d_qps != nullptr` |
| `RosterHoldsNvmeVirtualDevice` | every roster entry is a live NVMe vdevice with the right quota |
| `MetadataPopulatedFromDevice` | namespace id / block size / MDTS come from the daemon session |
| `MultipleVdevicesDistinctQueueSlices` | two vdevices get distinct queue slices and vdev ids |
| `AcquireReleaseTargetHandle` | `acquire_target_handle` builds a GPU handle; release frees it |
| `PrepareAndReleaseDescriptors` | PRP DUAL descriptor built correctly from ioaddrs |
| `GpuSubmitSingleBlock` | full GPU **READ** round-trip: `cudaMalloc` + `nvm_dma_map_data_device` → prepare → `launch_batch_gpu_stream(READ)` → sync clean |
| `GpuWriteReadVerify` | full GPU **WRITE→READ→verify** (gated behind `TUTTI_NVME_DESTRUCTIVE=1`, overwrites LBA 0): writes a pattern, reads it back, matches byte-for-byte |

Env knobs: `TUTTI_NVME_REAL_HW=1` (required), `TUTTI_NVME_ENDPOINT` (default `127.0.0.1:50051`), `TUTTI_NVME_DESTRUCTIVE=1` (write test only). The destructive test uses host-pinned write-combining buffers to dodge the GPU-L2 stale-cache hazard on DMA readback; point it only at a scratch device (`sys_config.b1.yaml`).

### 6.3 Building and running

Build with `tutti/build.sh` (`BUILD_TESTING` on by default; `--reconfigure` after toolchain/dep changes). Binaries land in `tutti/build/bin/`.

```bash
# Device-agnostic (no hardware)
./build/bin/backend_test

# NVMe unit tier (no hardware)
./build/bin/nvme_backend_test --gtest_filter='NvmeBackendUnit.*'

# NVMe real-HW tier (needs live daemon)
TUTTI_NVME_REAL_HW=1 ./build/bin/nvme_backend_test --gtest_filter='NvmeBackendRealHw.*'

# Destructive write/read/verify on a scratch device only
TUTTI_NVME_REAL_HW=1 TUTTI_NVME_DESTRUCTIVE=1 \
  ./build/bin/nvme_backend_test --gtest_filter='NvmeBackendRealHw.GpuWriteReadVerify'
```

Last verified 2026-07-27: unit 8/8, real-HW 7/7 + destructive write/read/verify pass on `/dev/nvme2n1` (`0000:b1:00.0`).

---

## 7. Current Implementation Status

### Device-agnostic core — complete
`IBackend`, `backend_types.h` (`BackendType`, `VDeviceHandle`, `BackendConfig`, `BackendMetadata`, `SubmissionResult`), and `BackendFactory` are implemented and wired into the build (`add_subdirectory(backends)` enabled in `tutti/CMakeLists.txt`).

### Mock backend — complete
`MockBackend` + 11 device-agnostic tests pass; vendor-neutrality verified.

### NVMe backend — complete, hardware-verified
Migrated to `IBackend` + `NvmeVirtualDevice`. `add_subdirectory(nvme)` is enabled in `backends/CMakeLists.txt`, and `libtutti_backends_nvme` builds as part of the default Layer 3 compile. `register` is realized as host methods (`acquire_target_handle` / `prepare_descriptors`), and the device-side `submit_one` runs from the GPU submit kernel with in-kernel CQ polling. GPU read and write paths are verified on real hardware (§6.2). Remaining gap: `submit_batch_cpu_sync` is still a stub and SGL descriptors are not implemented (§5.6).

### RDMA / GDS — not implemented
Enum values reserved; no backend yet.

---

## 8. Vendor-Neutral Boundary

The device-agnostic surface:

| File | Provides |
|------|----------|
| `include/backend_types.h` | `BackendType`, `VDeviceHandle`, `BackendConfig`, `BackendCapability`, `BackendMetadata`, `SubmissionResult` |
| `include/backend.h` | `IBackend` interface |
| `include/backend_factory.h` | `BackendFactory` + `BackendRegistrar` + `REGISTER_BACKEND` macro |
| `mock/include/mock_backend.h` | `MockBackend` (test backend) |

Nothing in this surface includes libnvm or CUDA. Layer 2's facade types are forward-declared. The only cross-boundary type appears when a concrete NVMe backend downcasts an `IVirtualDevice*` to `NvmeVirtualDevice*` — confined to the NVMe backend, exactly as in Layer 2.

---

## Related Documents

- [Layered Architecture Redesign](../architecture/layered-architecture-redesign.md)
- [Layer 2: Device Manager](layer2-device-manager.md) — supplies the vdevices this layer consumes
- [Backend SPI design](../design/backend-spi.md) — original SPI design notes

