# Layered Architecture Redesign

> **Status**: Design proposal. Supersedes the layer cake in
> [system-architecture.md](system-architecture.md) §2 and folds in the
> decisions from [restructuring-plan.md](../refactor/restructuring-plan.md).
> This document fixes the layer boundaries against the new architecture
> diagram (`doc/architecture/image.png`) and specifies the **functional
> API each module exposes to its neighbours** — what a call buys you, not
> its signature.

## 1. What Changed vs. the v0.1 Baseline

The v0.1 stack was an eight-layer cake where `memory/` was a mid-level
peer and every layer sat on `nvme_storage/` → `backends/local/`. The
redesign reorganises around three ideas:

1. **The Accelerator HAL becomes the foundation.** Raw memory
   allocation, DMA mapping, stream/event lifecycle, kernel launch, and a
   set of reusable device-side helpers all move *below* everything else.
   The old `memory/` layer is dismantled: its generic parts (alloc,
   DMA-map, region tracking) sink into the HAL; its transport-specific
   parts (PRP/SGL descriptor building, PRP-page cache) move *up* into the
   backend.

2. **Two top-level data interfaces, not one.** `Block Storage (GPUFile)`
   is no longer the only way in. A `raw device` interface exposes a
   namespace + LBA range directly, with no file. Both present
   `index / offset / size (persistent)` semantics to the application.

3. **The IO Engine owns command construction + launch.** It builds the
   IO command batch and drives the CPU↔device transfer, calling into the
   backend when transport-specific command bytes are needed (e.g. NVMe
   PRP). `nvme_storage/` is fully absorbed into `backends/`. The IO Engine
   stays unified across transfer schemes (batch GPU-stream, CPU sync,
   async, COOP) so each new transfer mode is written once, not per
   backend.

4. **Device Manager inverts: it sinks to a base layer, peer to the
   Accelerator HAL, consumed by backends.** In v0.1, `device_manager/`
   sat above backends yet `#include`d and linked libnvm (a backwards
   dependency). In the redesign it becomes the **local-NVMe
   virtualization base**: it owns physical controller bring-up and the
   queue-pair budget, and hands each backend a *virtual storage device*
   (a slice of that controller's queues + namespace view). Backends pull
   queues *down* from it; nothing above includes libnvm. It is a
   virtualization layer (device configuration + resource allocation),
   not a scheduler — the old "worker/scheduler" label is dropped. See §7.

| Concern | v0.1 location | Redesign location |
|---|---|---|
| Raw alloc / free | `memory/` | **Accelerator HAL** |
| DMA-map → ioaddrs | `memory/` (libnvm) | **Accelerator HAL** (`dma_map`) |
| `MemoryRegion` registry | `memory/` | **Accelerator HAL** |
| IO-slice fan-out (tensor→sub-IOs) | `memory/` | **IO Engine** (orchestration) |
| PRP/SGL descriptor build | `memory/` | **Backends / NVMe** |
| PRP-page two-tier cache | `memory/` | **Backends / NVMe** |
| NVMe controller bring-up + queue pool | `device_manager/` + `backends/local` | **Device Manager** (NVMe virtualization base, below backends) |
| NVMe queue budget / cross-process arbitration | `device_manager/` + NVMeService | **Device Manager** (two-level allocator) |
| Device-side queue mechanics (`acquire_queue`/`issue_nvme_cmd`/`poll`) | `nvme_storage/` | **Device Manager** (published `QueuePair` contract) |
| Device-side addressing (`resolve_lba`, extents) | `nvme_storage/` | **Backends / NVMe** |
| FIEMAP / namespace resolution | `nvme_storage/` | producer behind **Block Storage / raw device** (StorageTarget) |
| GPUFile striping | `block_storage/` | **Block Storage** (one StorageTarget shape) |
| Device-code macros | (absent) | **Abstraction** |

## 2. Redesigned Layer Cake

```
┌───────────────────────────────────────────────────────────────┐
│  Application                                                    │
│    thinks in: index / offset / size (persistent)                │
└───────────────────────────────────────────────────────────────┘
        │ open a target, register a buffer, submit a batch
        ▼
╔═══════════════════════════════ coordinator ═══════════════════════════════╗
║                                                                            ║
║  ┌──────────────────────────┐        ┌──────────────────────────┐         ║
║  │  Block Storage (GPUFile) │        │  raw device              │         ║
║  │  named, striped, persist │        │  namespace + LBA range   │         ║
║  └────────────┬─────────────┘        └────────────┬─────────────┘         ║
║               │        produce StorageTarget       │                       ║
║               └──────────────────┬──────────────────┘                      ║
║                                  ▼                                          ║
║                       ┌─────────────────────────────┐                      ║
║                       │         IO Engine           │                      ║
║                       │  unified transfer schemes:  │                      ║
║                       │  batch GPU-stream / CPU sync│                      ║
║                       │  / async / COOP             │                      ║
║                       └──────────────┬──────────────┘                      ║
║                     submit work +    │  (build PRP/SGL via SPI)             ║
║                     descriptor build ▼                                      ║
║               ┌──────────────────────────────────────────────┐            ║
║               │                 Backends                      │            ║
║               │  thin transport adapters (SPI: IBackendProvider)│          ║
║               │  ┌────────────────┐  ┌───────┐  ┌───────┐     │            ║
║               │  │ local_nvme     │  │  gds  │  │ rdma  │     │            ║
║               │  │ file / raw     │  │cuFile │  │  QP   │     │            ║
║               │  │ PRP build+cache│  └───────┘  └───────┘     │            ║
║               │  │ resolve_lba,   │                            │            ║
║               │  │ target handle, │   pull vDevice (queues)    │            ║
║               │  │ launch kernel  │───────────────┐            │            ║
║               │  └────────────────┘               │            │            ║
║               └───────────────┬───────────────────┼───────────┘            ║
║          NVMe backends only ↓ │                   │ all backends           ║
║        ┌──────────────────────▼──────────┐        │                        ║
║        │     Device Manager              │        │                        ║
║        │  local-NVMe virtualization base │        │                        ║
║        │  • controller bring-up (drivers:│        │                        ║
║        │    direct / service-client)     │        │                        ║
║        │  • queue-pair budget + 2-level  │        │                        ║
║        │    allocation (cross-process)   │        │                        ║
║        │  • hands out vDevice = queue    │        │                        ║
║        │    slice + ns view + caps       │        │                        ║
║        │  • device-side queue mechanics  │        │                        ║
║        │    (QueuePair contract, poll,   │        │                        ║
║        │     acquire_queue, issue_cmd)   │        │                        ║
║        └───────────────┬─────────────────┘        │                        ║
║                        │ cudaMalloc d_qps,         │                        ║
║                        │ map doorbell (via HAL)    │                        ║
║                        ▼                           ▼                        ║
║               ┌──────────────────────────────────────────────┐            ║
║               │            Accelerator HAL                    │            ║
║               │  ┌──────────┐   ┌──────────┐                  │            ║
║               │  │  launch  │   │  memory  │  dma_map, stream, │            ║
║               │  └──────────┘   └──────────┘  event, memcpy,   │            ║
║               │  MemoryRegion registry; generic atomics        │            ║
║               └───────────────────────┬──────────────────────┘            ║
║                                        ▼                                    ║
║               ┌──────────────────────────────────────────────┐            ║
║               │              Abstraction                       │            ║
║               │  TUTTI_DEVICE / GLOBAL / FORCEINLINE /         │            ║
║               │  ATOMIC / LAUNCH_KERNEL  → per-vendor mapping  │            ║
║               │  (CUDA / ROCm / SYCL / CANN)                   │            ║
║               └──────────────────────────────────────────────┘            ║
╚════════════════════════════════════════════════════════════════════════════╝
```

**Dependency rule**: each layer depends only on the layers below it plus
the shared noun headers (`Device`, `MemoryRegion`, `StorageTarget`,
`BufferDescriptor`, `IORequest`). Two edges deserve note:
- **IO Engine → Backends** for command-byte construction (PRP build),
  expressed through the backend SPI, not a direct include of NVMe
  internals.
- **Backends → Device Manager** (only NVMe-family backends). A backend
  pulls a `vDevice` (queue slice + namespace view) down from DM at
  `initialize()`. Non-NVMe backends (gds via cuFile, rdma via QP) do not
  use DM — the base band is asymmetric by design (see §7.4).
- **Device Manager → Accelerator HAL**: DM builds GPU-resident queues, so
  it uses the HAL for `cudaMalloc`/doorbell mapping. Precise layer order
  is `Abstraction < Accelerator HAL < Device Manager < Backends`.

## 3. Module API Contracts

Each subsection states the module's **responsibility**, the **API it
offers upward** (what a caller gets), and the **API it consumes
downward**. APIs are described by capability, not signature.

### 3.1 Abstraction (bottom)

**Responsibility**: Compile-time vendor dispatch for device code. It is
headers only — no runtime object, no vtable. It is what lets one `.cu`
source compile for CUDA today and ROCm/SYCL/CANN later.

**Offers upward** (to HAL device-side helpers and every backend `.cu`):
- **Function qualifiers**: `TUTTI_DEVICE`, `TUTTI_GLOBAL`,
  `TUTTI_FORCEINLINE`, `TUTTI_HOST_DEVICE` — map to `__device__` /
  `__global__` / … per active vendor macro.
- **Atomics**: `TUTTI_ATOMIC_U32` and the system-scope atomic ops used by
  queue-acquire and CQ-poll loops.
- **Kernel launch**: `TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem,
  stream, …)` — expands to `<<<>>>` on CUDA, `hipLaunchKernelGGL` on
  ROCm, etc.
- **Vendor selection macro**: exactly one of `TUTTI_ACCEL_CUDA` /
  `_ROCM` / `_SYCL` / `_CANN` is defined at build time.

**Consumes downward**: nothing (it is the floor). SYCL's lambda model is
the known hard case and may need a thin wrapper rather than pure macros
(see [gpu-abstraction.md](../design/gpu-abstraction.md) §2.4).

### 3.2 Accelerator HAL

**Responsibility**: The single place accelerator-runtime APIs are called.
Owns raw memory, DMA mapping, streams/events, host↔device copy, and a set
of **reusable device-side helper functions**. This absorbs the generic
half of the old `memory/` layer (points (a)(b)(c) of the memory split).

**Offers upward — host side (`IAccelerator`)**:

| Capability group | What a caller gets |
|---|---|
| **Identity** | vendor name, device count, set-current-device |
| **Memory allocation** | allocate/free of host, pinned-host, device, managed memory (the `launch`+`memory` boxes in the diagram) |
| **DMA mapping** | map a device/host buffer for controller DMA → per-page bus addresses (`ioaddrs`); the vendor-neutral replacement for `nvm_dma_map_data_device` |
| **MemoryRegion registry** | register a caller-owned buffer (host/device/external) and hand back a stable `MemoryRegion*`; look one up by host ptr / device ptr / id; unregister. This is the runtime's "we know about this memory" source of truth |
| **Host↔device pointer** | translate a pinned-host pointer to its device-visible address |
| **Stream / event** | create/destroy/synchronize streams; create/record/query/wait events (opaque `AccelStream` / `AccelEvent` = `void*`) |
| **Transfer** | `memcpy_async` in any direction on a stream |
| **Kernel launch** | launch a caller-provided kernel entry on a stream with given grid/block (the `launch` box); backends call this rather than writing `<<<>>>` directly |
| **IPC** | export/import a device pointer across processes |

**Offers upward — device side (generic only)**: `__device__` primitives
that are genuinely transport-agnostic — system-scope atomics (via the
Abstraction macros), memory fences. It does **not** own the NVMe
queue-acquire / CQ-poll helpers: those operate on `QueuePair` (an
NVMe-specific, DM-owned object) and therefore live in the Device Manager
(§3.4), not here. This corrects an earlier draft that placed
`queue_acquire_helper.cuh` in the HAL.

**Consumes downward**: the Abstraction macros for its own device-side
helpers.

**Key invariant**: no public header above the HAL includes
`cuda_runtime.h`. `AccelStream` (an opaque `void*`) replaces
`cudaStream_t` everywhere upward.

### 3.3 Backends (with NVMe Storage inside)

**Responsibility**: A **thin transport adapter**. A backend knows how to
turn `ioaddrs` + a `StorageTarget` into device-issuable IO commands, and
ships its own device-side submit kernel. It does **not** own its queue
resources any more — for the NVMe family it *pulls* a `vDevice` (queue
slice + namespace view) down from the Device Manager (§3.4). This absorbs
the addressing + command-build half of the old `nvme_storage/` layer and
the transport-specific half of the old `memory/` layer (points (d)(e):
PRP/SGL build + PRP-page cache).

The backend stays deliberately thin because the IO Engine above owns the
transfer *schemes* (batch / async / COOP); the backend owns only the
transport *specifics*. Each backend implements the backend SPI
(`IBackendProvider`). The runtime never includes a backend's private
headers; it talks to the SPI.

**Offers upward (SPI, consumed by IO Engine + Block/raw storage)**:

| Capability group | What a caller gets |
|---|---|
| **Descriptor build** | given `ioaddrs` + a sub-slice layout, produce transport `BufferDescriptor`s. For NVMe this is **PRP/SGL construction**; the IO Engine calls this when assembling a batch (the sideways edge in the diagram). Backend owns the **PRP-list page cache** (two-tier GPU-resident L1 + host-pinned L2) internally |
| **Target handle** | given a `StorageTarget` (file extents, raw LBA range, or remote addr), build a device-resident handle the submit kernel can dereference; release it. For NVMe this is the `NvmeFileDeviceHandle` (extents + a reference to the DM-provided `vDevice`'s queue slice) |
| **Submission — GPU stream** | launch the backend's own IO kernel on a stream (backend picks grid/block; the kernel uses DM's device-side queue mechanics to ring doorbells) |
| **Submission — CPU sync** | CPU prepares + submits + blocks; used for bootstrap / metadata / tests |
| **Submission — CPU async / COOP** | optional modes (future); may report unsupported |
| **Lifecycle** | `initialize(vDevice)` — receives its DM-provided queue slice here; `cleanup()` at shutdown returns it |
| **Metadata** | backend type, name, max IO size (from the vDevice caps) |

**NVMe backend private (lives in `backends/local_nvme/`)**:
- **Device-side addressing** (`.cuh`): `resolve_lba()` (walk file extents
  → LBA), `submit_read_one` / `submit_write_one`. These call *down* into
  DM's device-side queue mechanics (`acquire_queue`, `issue_nvme_cmd`,
  `poll`) rather than owning them.
- **PRP builder + cache**, **target-handle builder**.
- Namespace producers (FIEMAP etc.) are paired here at config time.

> The **queue group construction, libnvm, snvme kernel module, and the
> NVMe service daemon are NOT here** — they moved *down* into the Device
> Manager (§3.4). This is the key inversion: the backend consumes queues,
> it no longer builds them.

**Consumes downward**: **Device Manager** for its `vDevice` (queues +
namespace) and device-side queue mechanics; HAL for alloc/dma_map/launch/
memcpy; Abstraction macros in its kernels.

> Namespace resolution (FIEMAP for ext4, on-device layout, DFS client)
> is a *producer of `StorageTarget`* — see §3.7. It is paired with a
> backend at config time, not compiled into it. This preserves the
> two-axis (transport × namespace) design from
> [storage-extensibility.md](../design/storage-extensibility.md).

### 3.4 Device Manager (local-NVMe virtualization base)

**Responsibility**: A **virtualization layer over physical NVMe
controllers**, sitting below backends (peer to the Accelerator HAL). It
does device *configuration* (controller bring-up, namespaces, queue
budget) and *resource allocation* (slice one physical controller into
per-consumer **virtual storage devices**). It is NVMe-aware by design —
it is the local-NVMe HAL, so it uses `nvm_ctrl_t` / `QueuePair` directly,
exactly as the Accelerator HAL is CUDA-aware. It internally hosts
multiple **drivers** (direct-owned, service-client) — the two v0.1
registries are exactly these drivers.

It is **not** a scheduler and has **no hot-path role**. Backends pull a
`vDevice` from it once, at `initialize()`; steady-state IO never calls DM.

**Resources it MANAGES (owned internally, never exposed raw)**:

| Resource | Backing (from current code) | Why DM must own it |
|---|---|---|
| Physical controller + bring-up | `nvm_ctrl_t`, `init_b3` (chrdev/bind/probe) | A controller can be opened only once — a singleton |
| Queue-pair budget + split | `max_user_qid`/`start_cq_idx` user pool, `kernel_ioq_cap`, `NVM_MAX_QUEUES_PER_GROUP` | Kernel blk-mq and user consumers share one integer pool; needs one arbiter |
| Namespace inventory | `ns_id`, `blk_size` | Physical device shape |
| Cross-process allocation ledger + reaper | `ILeaseManager`, NVMeService daemon (PID-starttime reap) | Dead-process queues must be reclaimed or the pool leaks |

**Resources it PROVIDES (upward, to NVMe-family backends)**:

| Provided | Content | Consumer |
|---|---|---|
| **vDevice handle** | one consumer's slice: `{ d_qps subset, namespace view, queue quota, caps }` | a backend (sees only "my virtual NVMe") |
| **Device-side queue mechanics** | the `QueuePair` memory-layout contract + `acquire_queue` / `issue_nvme_cmd` / `poll` `__device__` helpers | the backend's submit kernel |
| **Capability metadata** | MDTS (`max_data_size`), page_size, block_size | IO Engine (fan-out), Block/raw storage (alignment) |
| **Lease token** | cross-process grant to hold + heartbeat | Coordinator |

**Two-level allocation** (because the device is physically single and
Coordinators are many):

```
        physical NVMe controller (budget = N queue pairs)
                        │
   ┌────────────────────┴────────────────────┐   ← Level ①: cross-process
   │   DM arbiter (daemon = NVMeService)      │      physical budget → per-process grant
   │   ledger + heartbeat + dead-proc reaper  │      (single-process/direct mode: grant = all)
   └──────┬───────────────────────────┬───────┘
          │ grant(a pairs)            │ grant(b pairs)   a + b + kernel ≤ N
          ▼                           ▼
   Coordinator P1 · DM(in-proc)   Coordinator P2 · DM(in-proc)   ← Level ②: in-process
     ├ vDevice → file backend       └ vDevice → ...                grant → per-backend vDevice
     └ vDevice → raw backend
```

- **Level ① (cross-process, daemon)** = today's `NvmeServiceBackedRegistry`
  client + NVMeService daemon. The daemon is the sole holder of the
  physical controller; other processes attach for a grant. This is what
  guarantees two Coordinators never collide on the one physical device.
- **Level ② (in-process)** = split this process's grant into vDevices for
  its own backends (file / raw / …). This is today's queue-group split,
  generalized to per-backend slices.

**Consumes downward**: Accelerator HAL (`cudaMalloc` for `d_qps`, doorbell
host→device pointer mapping) to build the GPU-resident queue rings.

**Boundary note**: `NvmeQueueGroup`, `LocalNvmeDevice`, the two registries
(now "drivers"), libnvm, and NVMeService all live in / under DM. Nothing
above DM includes libnvm — the v0.1 backwards dependency
(`device_manager → backends/libnvm`) is reversed.

### 3.5 IO Engine

**Responsibility**: The batch data-plane brain. It (1) accepts a batch of
work described in application terms, (2) fans each tensor out into
transport-sized sub-IOs (the IO-slice orchestration that used to live in
`memory/`), (3) asks the backend to build the command bytes for each
sub-IO (PRP/SGL), (4) stages the descriptor + request arrays across the
CPU↔device boundary, and (5) launches the submit kernel (or drives the
CPU submit path). It is **backend-neutral** — it holds an
`IBackendProvider*`, never NVMe types.

**Offers upward (to Block Storage / raw device / coordinator)**:

| Capability group | What a caller gets |
|---|---|
| **Batch submit (blocking)** | submit one uniform-direction batch of `(registered tensor region, target handle, offset, size)` items and block until complete |
| **Batch submit (async)** | same, returning after launch is queued on the stream; caller observes completion via stream/event |
| **Batch capacity** | the max entries one batch may flatten to (callers pack under this) |
| **Slice fan-out query** | how many sub-IOs a given registered region flattens to (adapters use this to pack batches) |

**Consumes downward**:
- **Backends SPI** — `prepare_descriptors` (build PRP/SGL from `ioaddrs`)
  and `launch_batch_gpu_stream` / `submit_batch_cpu_sync`. The backend
  already holds its DM-provided queue slice, so the IO Engine does **not**
  talk to the Device Manager — DM has no hot-path API. This is the direct
  answer to "what does the IO Engine need from DM": nothing at runtime.
- **HAL** — `memcpy_async` for the CPU→device staging of descriptor and
  request arrays; `MemoryRegion` lookups; stream/event.

> **Why keep the IO Engine and Backends as separate layers?** So new
> transfer schemes (GPU-stream, CPU sync, async, COOP, CUDA-graph) are
> implemented once in the IO Engine, not re-implemented in every backend.
> The backend contributes only transport specifics (how to build a
> command, how to launch its kernel); the IO Engine contributes the
> orchestration common to all of them.

**What moved in**: the tensor→sub-IO slicing that `memory/` did at
`register_tensor()` time is now IO-Engine orchestration. The IO Engine
asks the HAL for a region's `ioaddrs` and the backend for the per-slice
descriptors, assembling the `BufferDescriptorBatch` + `IORequestBatch`
the SPI consumes.

### 3.6 Block Storage (GPUFile) — top data interface #1

**Responsibility**: The named, striped, persistent container. A `GpuFile`
spans up to N device shards, interleaving data in `tensor_size` units.
Unchanged in spirit from v0.1, but it is now **one producer of a
`StorageTarget`**, not the mandatory bottom of the stack.

**Offers upward (to application / adapters via coordinator)**:

| Capability group | What a caller gets |
|---|---|
| **Directory** | create / open / close / delete GpuFiles (single + batched, with bulk-init deferral); list names; persistent across restart |
| **Acquire handle** | bring a file's shards to the GPU and hand back an acquired handle (→ produces a file-shaped `StorageTarget` / target handle for the IO Engine) |
| **Durability** | flush deferred metadata; sync a file (stream-sync + NVMe flush) |

**Consumes downward**: a namespace resolver (§3.7) to turn a file name
into shard LBA extents; the backend's target-handle builder; the IO
Engine for the actual data movement.

### 3.7 raw device — top data interface #2

**Responsibility**: Direct namespace + LBA-range access with **no file**.
The application supplies `(namespace, start_lba, length, block_size)` and
gets back a target handle it can submit against. This is the
`StorageTarget { NVME_RAW }` path — for databases, raw KV stores, and
block-oriented workloads where a file is pure overhead.

**Offers upward**:

| Capability group | What a caller gets |
|---|---|
| **Acquire raw target** | wrap a caller-provided `(namespace, LBA range)` into a `StorageTarget` and build its device handle — no FIEMAP, no persistent log, no directory |
| **Submit** | the same IO-Engine batch API, keyed by the raw target handle |

**Consumes downward**: the backend's target-handle builder directly (the
`raw_passthrough` namespace producer is a no-op); the IO Engine for
movement. It **bypasses Block Storage entirely**.

### 3.8 The shared noun: `StorageTarget`

Both top interfaces converge on one value type so the IO Engine and
backends stay file-agnostic. A `StorageTarget` carries a kind
(`NVME_FILE` / `NVME_RAW` / `RDMA_REMOTE` / …) plus the addressing payload
that kind needs. **Namespace producers** (FIEMAP, on-device layout, DFS
client, raw passthrough) emit `StorageTarget`s; **backends** consume them
via the target-handle builder. Neither side includes the other's headers.
See [storage-extensibility.md](../design/storage-extensibility.md) §3.2.

## 4. End-to-End Call Flow (GPU-submit read)

```
BOOTSTRAP (once):
  DM: bring up controller (driver: direct / service-client),
      compute queue budget, cudaMalloc d_qps via HAL, map doorbells
  DM: hand each backend a vDevice (queue slice + ns view + caps)
  Backend/NVMe: initialize(vDevice) — now holds its queues

RUNTIME (per batch, DM not involved):
  app: register_tensor(buf, granularity)
          └─ HAL: register MemoryRegion, dma_map → ioaddrs
  app: open Block Storage GPUFile  (or  acquire raw device target)
          └─ namespace producer → StorageTarget
          └─ Backend/NVMe: build target handle (extents/LBA + vDevice ref)
  app: submit_batch(region, target_handle, offset, size, READ, stream)
          └─ IO Engine:
               ├─ fan tensor into transport-sized sub-IOs
               ├─ Backend/NVMe: prepare_descriptors(ioaddrs, slices) → PRP  ← sideways edge
               ├─ HAL: memcpy_async descriptor+request arrays  CPU→device
               └─ Backend/NVMe: launch_batch_gpu_stream(stream, …)
                       └─ device kernel: acquire_queue   (DM mechanics, on vDevice queues)
                                         resolve_lba     (NVMe backend)
                                         issue_nvme_cmd  (DM mechanics)
                                         poll            (DM mechanics)
  app: sync_file → stream sync + NVMe flush
```

## 5. Mapping to the Restructuring Plan

This redesign extends [restructuring-plan.md](../refactor/restructuring-plan.md);
it sharpens three boundaries the plan left implicit:

1. **`memory/` does not survive as a peer layer.** The plan kept a
   `memory/` directory (cleaned of CUDA/libnvm). This redesign splits it:
   generic parts → HAL, transport parts → backend, orchestration →
   IO Engine. The `memory/` directory, if retained, is a thin façade over
   HAL registration; the PRP/slice machinery relocates.

2. **`raw device` is a first-class top interface**, peer to Block
   Storage, both emitting `StorageTarget`. The plan treated raw access as
   a `block_storage` bypass; here it is drawn as its own entry point.

3. **Device Manager inverts to a base layer.** The plan's Phase 3 moved
   `NvmeQueueGroup` / `LocalNvmeDevice` / the registries *into*
   `backends/local_nvme/` — treating DM as a thin registry+lease shell.
   This redesign instead sinks DM *below* backends as the local-NVMe
   virtualization base: those types stay in DM (which owns the physical
   controller + queue budget), and backends pull a `vDevice` down. This
   is the cleaner fix for the v0.1 backwards dependency and for the
   cross-process single-device constraint — but it revises Phase 3's
   destination and must be reconciled with it before execution.

Phases 1–7 of the plan still apply; the accel abstraction (Phase 1),
`StorageTarget` (Phase 5), and generic SPI wiring (Phase 4) are the
load-bearing steps for this shape.

## 6. Open Questions

1. **Where does the IO-slice table live at rest?** The build moves to the
   IO Engine, but a registered tensor's slice table could be cached on the
   `MemoryRegion` (HAL) or in the backend alongside the PRP cache. Leaning
   backend, so PRP pages and their owning descriptors share one owner.
2. **vDevice granularity.** A vDevice = a queue-pair slice of one
   controller (the chosen model). The exact allocation API (static split
   at bootstrap vs. dynamic borrow/return of queue quota) can stay static
   for v0.1 and gain dynamic reallocation later; the `ILeaseManager`
   contract already covers the cross-process accounting either way.
3. **`memory/` directory fate.** Keep as a slim registration façade, or
   delete and let callers use the HAL registry directly? Depends on how
   much the coordinator wants to hide `AccelStream` plumbing.
4. **Base-band asymmetry.** DM serves only NVMe-family backends; GDS
   (cuFile) and RDMA (QP) do not consume it. Acceptable now, but if a
   third NVMe-adjacent transport appears, revisit whether DM should expose
   a transport-neutral "queue provider" seam or stay NVMe-specific.

## 7. Why the Device Manager Exists (and what it must provide)

This section answers the question that drove the redesign: *does DM earn
its place, and what API does it owe each neighbour?*

### 7.1 The test for DM's necessity

DM is justified iff there is a resource that (a) must be owned above any
single backend, and (b) needs arbitration. Both hold here:

- **A physical NVMe controller can be opened only once.** If a file
  backend and a raw backend both drive it, neither can own it — a layer
  below both must.
- **Its queue-pair pool is a finite integer budget** shared by the kernel
  blk-mq path and every user-space consumer. Splitting it needs one
  arbiter.
- **The device is physically single but Coordinators are many.** Two
  processes must not collide — cross-process arbitration is unavoidable.

Take these away (single backend, single process, no sharing) and DM
collapses into the Coordinator. They are all present, so DM stands — but
as a **base layer**, not a mid-stack manager.

### 7.2 What DM provides, per neighbour

| To | API (by capability) | Hot path? |
|---|---|---|
| **NVMe backends** | a `vDevice` (queue slice + namespace view + caps) at `initialize()`; device-side queue mechanics (`QueuePair` contract, `acquire_queue`/`issue_nvme_cmd`/`poll`) | No — pulled once |
| **Coordinator (bootstrap)** | controller configuration; enumerate physical devices + caps; cross-process lease token | No |
| **IO Engine** | *nothing* — the backend already holds its queues | — |
| **Accelerator HAL** | (consumes, does not provide) uses HAL to build GPU-resident rings | — |

The key result: **DM has no hot-path API at all.** All sharing/allocation
happens at bootstrap; steady-state IO flows backend→HAL without touching
DM. "What does the IO Engine need from DM" = nothing directly.

### 7.3 What DM is NOT

- **Not a scheduler / worker pool.** It allocates queues once; it does not
  pick a queue per submission. (The old "worker/scheduler" label is
  retired.)
- **Not transport-neutral.** It is the *local-NVMe* HAL and uses NVMe
  types directly, just as the Accelerator HAL uses CUDA directly. An
  opaque "queue resource pool" was considered and rejected: the pool
  cannot express NVMe's controller/QID-budget/doorbell semantics without
  degenerating into a `void*` box.

### 7.4 The asymmetry, stated plainly

DM is the base for the **NVMe family only**. GDS and RDMA backends bring
their own transport foundations (cuFile is kernel-managed; RDMA uses QPs)
and do not consume DM. The base band is therefore two unequal blocks:
Accelerator HAL (used by everything) and Device Manager (used by NVMe
backends). This is intentional, matching the decision to scope DM as
"local NVMe."

## 8. API Sketch (illustrative signatures)

> Signatures are **shape-only** — names + rough return type, parameters
> elided. The point is *which categories of API each module exposes*, not
> the exact prototypes. Grouped to mirror §3.

### 8.1 Abstraction — macros, not functions

```cpp
// vendor-dispatch macros (compile-time; no runtime object)
TUTTI_DEVICE  TUTTI_GLOBAL  TUTTI_FORCEINLINE  TUTTI_HOST_DEVICE
TUTTI_ATOMIC_U32
TUTTI_LAUNCH_KERNEL(kernel, grid, block, shmem, stream, ...)
```

### 8.2 Accelerator HAL — `IAccelerator`

```cpp
class IAccelerator {
  // --- identity ---
  const char* name();  int device_count();  bool set_device();

  // --- memory allocation ---
  void* allocate(spec);            void  free(ptr, loc);

  // --- MemoryRegion registry ---
  MemoryRegion* register_host(...);   MemoryRegion* register_device(...);
  MemoryRegion* register_external(...);
  MemoryRegion* lookup(key);          void unregister(region);

  // --- DMA mapping (→ ioaddrs) ---
  bool dma_map(...);               void dma_unmap(...);
  void* get_device_ptr(host_ptr);

  // --- stream / event ---
  AccelStream create_stream();     void synchronize_stream(...);
  AccelEvent  create_event();      void record/query/wait_event(...);

  // --- transfer ---
  bool memcpy_async(...);

  // --- kernel launch / IPC ---
  void launch(kernel_entry, grid, block, stream, ...);
  bool ipc_export(...);            void* ipc_import(...);
};
```

### 8.3 Device Manager — virtualization base

```cpp
// device configuration + physical enumeration (bootstrap)
class IDeviceRegistry {                 // one "driver" per bring-up mode
  bool Open();  void Close();           //   direct / service-client
  const Device* device_at(i);  const Device* find_by_id(id);
};

// resource allocation: physical controller → per-consumer vDevice
class IVirtualNvme {
  VDevice* open_vdevice(phys_id, quota);   // level ②: a queue slice + ns view
  void     close_vdevice(vdev);
  Caps     caps(phys_id);                  // MDTS, page_size, block_size
};

// cross-process arbitration (level ①; daemon-backed)
class ILeaseManager {
  bool heartbeat(lease_id);   bool release_lease(...);   bool has_lease(...);
};
```

```cpp
// device-side queue mechanics (published __device__ contract)
__device__ uint32_t acquire_queue(vdev_qps);
__device__ void     issue_nvme_cmd(qp, prp1, prp2, nblocks, lba, op, &cid);
__device__ void     poll(qp, cid);
struct QueuePair { /* sq/cq rings, doorbells — layout is the contract */ };
```

### 8.4 Backends — `IBackendProvider` (SPI) + private device-side

```cpp
class IBackendProvider {
  // --- lifecycle (receives its vDevice here) ---
  bool initialize(vdevice);        void cleanup();

  // --- descriptor build (command bytes; PRP/SGL for NVMe) ---
  bool prepare_descriptors(ioaddrs, slices, out_descs);

  // --- target handle (StorageTarget → device-resident handle) ---
  void* acquire_target_handle(storage_target);
  void  release_target_handle(handle);

  // --- submission modes ---
  void      launch_batch_gpu_stream(...);   // REQUIRED
  bool      submit_batch_cpu_sync(...);      // REQUIRED
  IOFuture* submit_batch_cpu_async(...);     // OPTIONAL (may be null)
  bool      setup_coop_channel(...);         // OPTIONAL

  // --- metadata ---
  BackendType backend_type();  const char* backend_name();
  size_t      max_io_size();
};
```

```cpp
// NVMe backend private (not in SPI): device-side addressing
__device__ bool resolve_lba(handle, logical_off, nbytes, &lba, &nblk);
__device__ void submit_read_one(handle, prp1, prp2, off, nbytes);
__device__ void submit_write_one(handle, prp1, prp2, off, nbytes);
// + PRP builder + PRP-page cache (host-side, internal)
```

### 8.5 IO Engine — `IIoEngine`

```cpp
class IIoEngine {
  // --- batch submit (uniform direction) ---
  bool submit_batch(inputs, is_read, stream);          // blocking
  bool submit_batch_async(inputs, is_read, stream);    // returns after launch

  // --- capacity / planning ---
  uint32_t max_entries_per_batch();
  uint32_t slice_fanout(region);   // # sub-IOs a region flattens to
};
```

### 8.6 Block Storage — `IBlockStorage` (top interface #1)

```cpp
class IBlockStorage {
  // --- directory (single + batch, persistent) ---
  GpuFile* open_gpu_file(spec, flags);   // open/create
  bool     close_gpu_file(f);   bool delete_gpu_file(f);
  vector<GpuFile*> open_gpu_files_batch(...);
  bool     flush_metadata();
  vector<string> list_gpu_file_names();

  // --- acquire handle → produces a file-shaped StorageTarget ---
  GpuFileHandle* acquire_device_handle(f, stream);
  void           release_device_handle(h, stream);

  // --- durability ---
  bool sync_file(id, stream);
};
```

### 8.7 raw device — top interface #2

```cpp
class IRawDevice {
  // --- acquire a fileless (namespace, LBA range) target ---
  RawTargetHandle* acquire_raw_target(storage_target, stream);  // NVME_RAW
  void             release_raw_target(h, stream);
  // submission reuses IIoEngine::submit_batch, keyed by the raw handle
};
```

### 8.8 One-line map: category → owner

| API category | Owner |
|---|---|
| vendor macros | Abstraction |
| alloc / dma_map / stream / memcpy / launch / MemoryRegion | Accelerator HAL |
| controller config / vDevice alloc / lease / device-side queue mechanics | Device Manager |
| descriptor build / target handle / submission modes | Backends |
| batch submit / fan-out planning | IO Engine |
| file directory / acquire handle / durability | Block Storage |
| fileless raw target acquire | raw device |



