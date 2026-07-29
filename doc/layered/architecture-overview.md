# Tutti Layered Architecture — Layers 0-6

**Version:** 2.0
**Date:** 2026-07-29
**Status:** Overview — L0 (Abstraction) / L1 (Accelerator HAL) live; L2 (Device Manager) vendor-neutral core + mock complete, NVMe daemon path real, direct path stubbed; L3 (Backends) device-agnostic core + mock done, NVMe **RAW** GPU IO real-HW verified, **FILE** metadata (FIEMAP) missing; L4 (IO Engine) submit path compiles against `IBatchSubmitter` but `StripeManager` is **not wired** and the test subdir does-not-configure; L5 (Storage Interfaces) both peers — `block_storage` and `raw_device` — mid-migration and **do-not-compile** (only `raw_device` drives the engine); L6 (Coordinator) the app-facing façade that receives the injected L3-L5 objects, owns the `raw_device` sub-service + a default stream, and dispatches per-IO work to the injected engine — **does-not-compile** (still bound to the deleted `backends::IBackendProvider`) and disabled in the build
**Scope:** Layers 0 through 6

> **Scope note.** This document now spans the whole current Tutti stack, L0-6: the
> compile-time GPU abstraction (L0), the runtime accelerator HAL (L1), the device
> manager (L2), the backends (L3), the IO engine + stripe mapper (L4), the two
> app-facing storage interfaces (L5), and the top-level coordinator (L6). Exhaustive
> per-layer detail lives in the sibling docs — `layer0-*.md`, `layer1-*.md`,
> `layer2-device-manager.md`, `layer3-backends.md`, `layer4-io-engine.md`,
> `layer5-storage-interfaces.md`, and `layer6-coordinator.md`. This file is the
> connective tissue between them and the corrected source of truth for cross-layer
> facts. Where a per-layer doc and the source disagree, **prefer code truth**; the
> drift table below is the standing correction backlog for the lower three layers.

---

## 1. Overview

### What Tutti solves

Tutti is a GPU-driven storage stack. The end goal (upper layers) is to let GPU
threads issue NVMe IO more or less directly, without bouncing every request
through the host CPU. To get there, the lower layers have to solve three
orthogonal portability/ownership problems and keep them from leaking into each
other:

- **Vendor portability of device code (L0).** Kernels and dual-compiled helpers
  must build against CUDA today and ROCm/SYCL/CANN later without rewriting call
  sites. This is a *compile-time* concern — pure preprocessor, zero runtime cost.
- **Vendor portability of the runtime (L1).** Allocating device/pinned/managed
  memory, streams, events, async copies, kernel launches, and IPC handles must go
  through one vendor-neutral C++ interface so higher layers never `#include`
  `cuda_runtime.h`.
- **Physical-device ownership and slicing (L2).** A physical NVMe controller can
  be initialized only once; multiple processes and multiple in-process backends
  must share its fixed queue-pair budget without corrupting each other. L2 owns
  the controller and hands out slices.
- **Per-transport data-transfer / GPU submission SPI (L3).** Turn a vdevice slice
  into an actual IO path: a device-agnostic `IBackend` (lifecycle + vdevice roster)
  plus an NVMe-scoped `IBatchSubmitter` that builds the GPU IO context (target
  handles, PRP descriptors) and launches the device-side submit kernel. RAW LBA IO
  works end-to-end on hardware; FILE metadata (FIEMAP → LBA extents) is missing.
- **Turning already-routed requests into GPU-launched NVMe IO (L4).** The IO Engine
  takes shard-scoped requests, fans each out into MDTS-sized sub-IOs, builds PRP
  descriptors, and drives the backend's GPU submit kernel on a stream. Stripe
  splitting is a separate, **not-yet-wired** `StripeManager` library; the engine
  itself does only MDTS fan-out.
- **App-facing storage entry points (L5).** Two peers over the same
  `StorageTarget` machinery: `block_storage` (named files over a striped logical
  namespace) and `raw_device` (direct `namespace_id` + LBA). Both are meant to
  route targets through the L4 engine.
- **Top-level orchestration for applications (L6).** A single `ICoordinator`
  façade that ties the lower layers into one usable stack: it takes fully-built
  L3/L4/L5 objects (dependency-injected via `CoordinatorConfig`), owns and drives a
  `raw_device` sub-service plus a default stream, tracks registered buffers, and
  dispatches per-IO read/write batches down to the injected engine. It is the entry
  point an application holds — **not** a builder that constructs the layers below it.

The layering rule is strict and **downward-only**: each layer depends only on the
layers beneath it. L0 depends on nothing. L1 depends on L0 (for include paths) and
the CUDA runtime. L2 depends on L1 (for GPU memory/queues) and L0 (for device-side
helper qualifiers), plus libnvm/gRPC confined to its NVMe subtree. L3 depends on L2
(vdevices, via `open_vdevice` + downcast to `NvmeVirtualDevice`), on L1's HAL types
(carried but today largely unused), and on libnvm/CUDA inside its NVMe subtree. L4
depends on L3's `IBatchSubmitter` and on L1's HAL (`IAccelerator` for staging /
stream sync / device scratch). L5 depends on L4 (`IIoEngine`) and on L3
(`StorageTarget` + target handles). L6 depends on L5 (`IBlockStorage` exposed via
getter; the `raw_device` peer packaged with it), on L4 (`IIoEngine`, to which it
forwards every submit), on L3 (the injected backend provider handed into the
`raw_device` sub-service), and on L1 (`IAccelerator` for buffer registration and
the default stream) — it *holds* these injected objects rather than constructing
them, and touches L2 not at all.

### Layer diagram

```
┌───────────────────────────────────────────────────────────────────┐
│ L6  Coordinator                  tutti/coordinator/                 │
│     ICoordinator → CoordinatorImpl (thin façade / dispatcher)       │
│     injects L3/L4/L5 via CoordinatorConfig; owns RawDeviceImpl      │
│     + default stream; forwards submit_*_batch to the io engine      │
│     does-not-compile (deleted IBackendProvider); build-disabled     │
└───────────────────────────────────────────────────────────────────┘
                    ▲  holds injected IBlockStorage / raw_device peer;
                    │  exposes both data paths via getters
┌───────────────────────────────────────────────────────────────────┐
│ L5  Storage Interfaces   tutti/{block_storage, coordinator(raw_dev)}│
│     → target: tutti/storage-interfaces/                             │
│     IBlockStorage (block/file) | IRawDevice (ns+LBA)                │
│     neither compiles yet                                            │
└───────────────────────────────────────────────────────────────────┘
                    ▲  IIoEngine::submit_batch / submit_one
                    │  (only raw_device calls it today)
┌───────────────────────────────────────────────────────────────────┐
│ L4  IO Engine + StripeManager    tutti/io_engine/                   │
│     IIoEngine → IoEngineImpl holds nvme::IBatchSubmitter*           │
│     StripeManager (pure math, NOT wired into the engine)           │
└───────────────────────────────────────────────────────────────────┘
                    ▲  IBatchSubmitter: prepare_descriptors /
                    │  acquire_target_handle / launch_batch_gpu_stream
┌───────────────────────────────────────────────────────────────────┐
│ L3  Backends                     tutti/backends/                    │
│     IBackend (device-agnostic) + IBatchSubmitter (NVMe)            │
│     NvmeBackend: RAW real, FILE metadata missing ; MockBackend     │
└───────────────────────────────────────────────────────────────────┘
                    ▲  open_vdevice() once; downcast to NvmeVirtualDevice
┌───────────────────────────────────────────────────────────────────┐
│ L2  Device Manager            tutti/device_manager/                 │
│     IDeviceManager → IDeviceDriver → IPhysicalDevice → IVirtualDevice│
│     + ILeaseManager (cross-process keep-alive)                      │
│     NVMe: direct (stub) | daemon (real gRPC/libnvm) | mock          │
└───────────────────────────────────────────────────────────────────┘
                                  ▲  IAccelerator: device mem, streams, IPC
                                  │  (NVMe queue alloc uses CUDA managed mem)
┌───────────────────────────────────────────────────────────────────┐
│ L1  Accelerator HAL           tutti/accel/                          │
│     IAccelerator  (26 pure-virtual methods, 9 groups)               │
│     CudaAccelerator = sole impl; MemoryRegion registry              │
│     builds libtutti_accel  (→ CUDA::cudart)                         │
└───────────────────────────────────────────────────────────────────┘
                                  ▲  TUTTI_ACCEL_CUDA propagated PUBLIC
┌───────────────────────────────────────────────────────────────────┐
│ L0  Abstraction (macros)      tutti/abstraction/accel.h  (97 lines) │
│     TUTTI_DEVICE/GLOBAL/HOST/HOST_DEVICE/FORCEINLINE,               │
│     TUTTI_ATOMIC_*_{DEV,SYS}, TUTTI_LAUNCH_KERNEL, THREADFENCE      │
│     header-only, no library target                                  │
└───────────────────────────────────────────────────────────────────┘
```

Note the two upward arrows are different in kind: L0→L1 is a **build-time**
coupling (L1's `tutti_accel` target is what actually *defines* `TUTTI_ACCEL_CUDA`
and propagates it PUBLIC to everyone above), whereas L1→L2 and L2→L3+ are ordinary
**runtime** interface dependencies.

---

## 2. Layer 0 — Abstraction (compile-time macro layer)

**Location:** `tutti/abstraction/accel.h` (single 97-line header, no CMake target)

### Purpose

Map portable `TUTTI_*` tokens onto one selected GPU vendor target (CUDA, ROCm,
SYCL, CANN) or a host-only fallback, chosen by exactly one `-DTUTTI_ACCEL_*`
macro. It is pure preprocessor: 11 macros total, zero runtime cost, no vtables.
Only the **CUDA** branch and the **host-only** fallback are functionally complete.

### Key types / macros

| Macro | File | Role |
|---|---|---|
| `TUTTI_DEVICE` / `TUTTI_GLOBAL` / `TUTTI_HOST` | `accel.h:14-16,39-41,80-82` | Function qualifiers. CUDA/ROCm → `__device__`/`__global__`/`__host__`; host-only → empty. |
| `TUTTI_HOST_DEVICE` | `accel.h:17,42,83` | Dual-compilation qualifier; CUDA/ROCm bundle `__forceinline__`, host-only → `inline`. |
| `TUTTI_FORCEINLINE` | `accel.h:18,43,84` | `__forceinline__` (CUDA/ROCm) / `__attribute__((always_inline)) inline` (host). |
| `TUTTI_ATOMIC_{U32,U64}_{DEV,SYS}` | `accel.h:21-24,46-49,87-90` | Atomic type aliases. CUDA → `cuda::atomic<uintN_t, thread_scope_{device,system}>`; ROCm → placeholder plain ints (FIXME); host → aligned plain ints. |
| `TUTTI_LAUNCH_KERNEL(k,grid,block,shmem,stream,...)` | `accel.h:27-28,51-52,92-93` | Kernel launch. CUDA → `<<<>>>`; ROCm → `hipLaunchKernelGGL`; host → `static_assert(false, ...)`. |
| `TUTTI_THREADFENCE_SYSTEM()` | `accel.h:31,54,95` | System-scope fence. CUDA/ROCm → `__threadfence_system()`; host → `((void)0)`. |

### Design decisions

- **Compile-time dispatch** via a plain `#if/#elif/#else` chain in the order
  CUDA → ROCm → SYCL → CANN → host-only (`accel.h:6,33,56,66,72`).
- **Explicit atomic memory scope**: separate `_DEV` (`thread_scope_device`) and
  `_SYS` (`thread_scope_system`) variants so callers opt into the slower
  PCIe-coherent atomics only when they need cross-device visibility.
- **`TUTTI_HOST_DEVICE` deliberately bundles `__forceinline__`** so small
  dual-compiled utilities are always inlined.
- **Host-only atomics degrade to aligned plain integers**, not `std::atomic<>`,
  to avoid pulling the C++ stdlib into device headers ("matches libnvm pattern").
- **SYCL and CANN branches hard-fail with `#error`** rather than silently
  mis-mapping.

### Downward dependencies

None. L0 is a leaf. Its only "dependencies" are the per-branch runtime includes it
pulls in when selected (`<cuda_runtime.h>`+`<cuda/atomic>` for CUDA,
`<hip/hip_runtime.h>` for ROCm, `<cstdint>` for host-only). Critically, **L0 does
not define `TUTTI_ACCEL_CUDA` itself** — the L1 `tutti_accel` target sets it PUBLIC
(`tutti/accel/CMakeLists.txt:65-67`), which is what makes the scheme work for L2+.

---

## 3. Layer 1 — Accelerator HAL

**Location:** `tutti/accel/` — builds `libtutti_accel` → depends on `CUDA::cudart`
and the L0 `tutti_types` include library.

### Purpose

A *runtime* HAL that hides the vendor GPU runtime behind the vendor-neutral C++
interface `tutti::IAccelerator`. It exposes device identity, host/device/pinned/
managed memory allocation, a central `MemoryRegion` registry, host→device pointer
translation, stream/event lifecycle, async memcpy, function-pointer kernel launch,
and IPC export/import. `CudaAccelerator` is the **sole** implementation; it maps
each method onto CUDA runtime calls and adds a thread-safe registry plus a 64 KB
device-alignment scheme.

### Key types / interfaces

| Type | File | Role |
|---|---|---|
| `IAccelerator` | `tutti/accel/include/common/iaccel.h` | Pure-virtual HAL: 26 pure-virtual methods across 9 groups (Identity, Memory Alloc, Registry, Pointer Translation, Stream, Event, Transfer, Kernel Launch, IPC). |
| `CudaAccelerator` | `tutti/accel/include/cuda/cuda_accelerator.h`, `src/cuda/cuda_accelerator.cu` | Sole impl. Holds `regions_by_id_`, `ptr_to_region_id_` (`registry_mutex_`), `next_region_id_`, `aligned_to_raw_` (`alloc_mutex_`). |
| `MemoryRegion` | `include/common/memory_region.h` | Central tracking struct from `register_*`: id/kind/device_id/host_ptr/device_ptr/size/external* + opaque `backend_private` slot for L3+ DMA/RDMA metadata. |
| `ExternalMemorySpec` | `include/common/memory_region.h` | Tagged union: `APP_MANAGED`, `DEVICE_IPC` (`uint8_t handle[64]`), `HOST_SHM` (`shm_fd`), `HOST_FD_MAP` (`fd`+`offset`). Deep-copied by `register_external`. |
| `MemoryKind` | `include/common/memory_kind.h` | `enum class uint8_t`: HOST, PINNED_HOST, DEVICE, MANAGED, EXTERNAL. Selects alloc/free path. |
| `MemoryAccessFlags` | `include/common/memory_kind.h` | Bitflags NONE/READ/WRITE/HOST_MAPPED — **declared but never used** by any signature or impl. |
| `AccelStream` / `AccelEvent` | `include/common/accel_types.h` | Opaque `void*` handle wrappers (cudaStream_t/cudaEvent_t) with `is_valid()`/==/!=. |
| `IpcHandle` | `include/common/accel_types.h` | 64-byte (`MAX_HANDLE_SIZE`) zero-init buffer sized for `cudaIpcMemHandle_t`. |
| `Dim3` | `include/common/accel_types.h` | Vendor-neutral `{u32 x,y,z}` launch dims (default 1,1,1) → CUDA `::dim3`. |

### Public API surface (9 groups)

- **Identity** — `vendor_name()` (const `"CUDA"`), `device_count()`, `set_device()`, `get_device()`.
- **Memory alloc** — `allocate_host(size, kind)` (HOST=`malloc`, PINNED_HOST=`cudaHostAlloc`; **anything else → nullptr**), `allocate_device(size, kind, dev)` (DEVICE over-allocates `size+65536` and hands out a 64 KB-aligned sub-pointer; MANAGED=`cudaMallocManaged`), `free(ptr, kind)`.
- **Registry** — `register_host/register_device/register_external` (**bookkeeping only**, no `cudaHostRegister`/mmap), `unregister` (erases maps + deletes spec only), `lookup`, `lookup_by_id`.
- **Pointer translation** — `device_pointer_for()` → `cudaHostGetDevicePointer`.
- **Stream** — `create_stream`/`destroy_stream`/`synchronize_stream` (null-safe).
- **Event** — `create/destroy/record/wait/query_event` (`query_event(null)==true`).
- **Transfer** — `memcpy_async(dst, src, n, stream)` → `cudaMemcpyAsync` with `cudaMemcpyDefault` (UVA infers direction).
- **Kernel launch** — `launch(fn, grid, block, shmem, stream, args)` → `cudaLaunchKernel` (return ignored).
- **IPC** — `ipc_export()` → `cudaIpcGetMemHandle`; `ipc_import()` → `cudaIpcOpenMemHandle` (see status note — currently returns null).

### Design decisions

- **Opaque `void*` handles** keep `cuda_runtime.h` out of `common/` headers, so
  higher layers compile without CUDA.
- **Central `MemoryRegion` registry**: two maps (`regions_by_id_` by `uint64_t`,
  `ptr_to_region_id_` by `uintptr_t`) under one `registry_mutex_`; `next_region_id_`
  is a monotonic counter from 1, never reused. Returned pointers alias into a
  node-stable `unordered_map`; const lookups `const_cast` a mutable pointer out.
- **64 KB device alignment**: over-allocate `size+65536`, return an aligned
  sub-pointer, and recover the exact `cudaMalloc` pointer for `cudaFree` via the
  `alloc_mutex_`-guarded `aligned_to_raw_` map (guarded by a 200-iteration leak test).
- **`free` takes the `kind` explicitly** so it works for never-registered raw allocs.
- **Return-value error model**: `check_cuda()` logs to stderr and returns bool; no
  exceptions, no error-code enum.
- **`register_*` is pure bookkeeping**; actual acquisition (`cudaIpcOpenMemHandle`)
  happens in `ipc_import`, and teardown of external resources is **not** performed.

### Downward dependencies

- **L0** (`tutti_types`, linked PUBLIC) — include paths for the abstraction header.
- **CUDA Toolkit** via `find_package(CUDAToolkit REQUIRED)` and `CUDA::cudart` (PUBLIC).
- No libnvm, no kernel modules, no higher layers. DMA mapping is deferred upward.

---

## 4. Layer 2 — Device Manager

**Location:** `tutti/device_manager/` — vendor-neutral `common/`, `mock/`, and an
`nvme/` subtree (with the `nvmeservice` daemon under `TUTTI_NVMESERVICE_ENABLED`).

### Purpose

L2 owns physical storage controllers and hands out resource slices to backends. It
solves single-owner initialization, cross-process conflict, and two-level
(cross-process then in-process) resource splitting. The chain is:

```
IDeviceManager (facade)
  → IDeviceDriver     (per-transport plugin / factory, owns instances + heartbeat)
    → IPhysicalDevice (per-process VIEW of one controller)
      → IVirtualDevice (a backend's slice; downcast to a concrete subtype)
  + ILeaseManager     (cross-process keep-alive)
```

Once `enumerate()` returns, **the manager has no hot-path role** — steady-state IO
never calls L2; backends operate directly on the `IVirtualDevice` resources.

### Key types / interfaces

| Type | File | Role |
|---|---|---|
| `IDeviceManager` | `include/common/idevice_manager.h` | Facade: physical registry (Open/Close, device queries) + vdevice alloc (open/close_vdevice, available_resources, caps). |
| `DeviceManagerImpl` | `include/common/device_manager_impl.h`, `src/common/device_manager_impl.cpp` | Only concrete impl. Owns `drivers_` (unique_ptr); `devices_`/`live_vdevices_` hold non-owning raw pointers; `mutex_`. Dispatches by `driver->type()==dev->type()`. |
| `IDeviceDriver` | `include/common/idevice_driver.h` | Per-transport plugin: `type/enumerate/alloc_vdevice/free_vdevice/shutdown`. Owns the devices it hands out + the heartbeat thread. |
| `IPhysicalDevice` | `include/common/iphysical_device.h` | Abstract per-process controller descriptor (id/type/pci_addr/grants/caps). A **per-process view, not hardware total**. |
| `IVirtualDevice` | `include/common/ivirtual_device.h` | Generic slice base (phys_id/vdev_id/type/resource_count/caps). Downcast after checking `type()`. |
| `ILeaseManager` / `NullLeaseManager` | `include/common/ilease_manager.h`, `null_lease_manager.h` | Cross-process keep-alive (heartbeat/release/has_lease). Null = direct-mode no-op. |
| `NvmeVirtualDevice` | `nvme/include/nvme_virtual_device.h` | NVMe downcast target: `d_qps`, `queue_quota`, **`ctrl`**, `namespace_id`, `blk_size`, `blk_size_log`, `max_data_size`. |
| `NvmePhysicalDevice` | `nvme/include/nvme_physical_device.h`, `src/…cpp` | LOCAL_NVME phys device; `ctrl` (null in daemon mode), `queue_group` (unused), ns metadata; atomic `allocated_` drives `available_grant()`. |
| `DaemonNvmeDeviceDriver` | `nvme/include/daemon_nvme_device_driver.h`, `src/…cpp` | Multi-process driver: real gRPC + libnvm + `daemon_nvme_alloc_queues`; also mock_mode/`#else` fallbacks. |
| `DirectNvmeDeviceDriver` | `nvme/include/direct_nvme_device_driver.h`, `src/…cpp` | Single-process driver — **stub**: fabricates one 16-QP device, leaves `d_qps` null. |
| `Mock*` (Driver/Physical/Virtual/LeaseManager) | `mock/include/*.h`, `src/mock_device_driver.cpp` | Complete in-memory backend, real grant accounting, depends only on `common/`. Backs the vendor-neutral test suite. |
| `ServiceState` / `NvmeServiceImpl` / `NvmeServiceClient` | `nvme/nvmeservice/src/*` | Daemon (owns chrdev/bind + one `nvm_ctrl_t` per NVMe + lease reaper) and its gRPC client (ListDevices/Connect/Disconnect/Heartbeat). |

### Public API surface

- `IDeviceManager::Open()` → probe all drivers, build registry; **idempotent** (returns true if already opened).
- `IDeviceManager::Close()` → free live vdevices via owning driver, then `shutdown()` each driver.
- `open_vdevice(phys_id, resource_quota, error=nullptr)` → `IVirtualDevice*`; nullptr on quota==0 / unknown id / no matching driver / pool exhausted.
- `close_vdevice(vdev)` → no-op on null / untracked pointer; routes teardown back to the originating driver via the `{vdev,driver}` map.
- Registry queries: `device_count/device_at/find_by_id/find_by_type(t,ordinal=0)/list/available_resources/caps`.
- `IDeviceDriver::enumerate(out)` → count appended (n<0 aborts `Open()`); `alloc_vdevice/free_vdevice/shutdown`.
- `create_device_manager(vector<unique_ptr<IDeviceDriver>>)` → `unique_ptr<IDeviceManager>` (factory takes ownership of drivers).
- Device-side (`nvme/include/queue_acquire_helper.cuh`): `acquire_queue(d_qps,n_qps)`, `issue_nvme_cmd(...)`, `poll(qp,cid)`.
- `daemon_nvme_alloc_queues(args, out)` / `daemon_nvme_free_queues(set)` (extern C, capped at `NVM_MAX_QUEUES_PER_GROUP`=16).

### Design decisions

- **Ownership**: `DeviceManagerImpl` owns `drivers_`; drivers own the raw
  `IPhysicalDevice*`/`IVirtualDevice*` they hand out; the manager holds only
  non-owning pointers. `close_vdevice` routes teardown back through `live_vdevices_`.
- **Downcast-by-type contract**: `IVirtualDevice`/`IPhysicalDevice` are
  vendor-neutral; callers check `type()` then `static_cast`. `nvm_types.h` is the
  only cross-boundary include and stays confined to the `nvme/` subtree.
- **Two NVMe drivers, one interface**: direct (`NullLeaseManager`, no heartbeat)
  vs daemon (injected `ILeaseManager` + heartbeat thread + gRPC). Mode is hidden
  behind `IDeviceDriver`.
- **Heartbeat owned by the driver, not the manager**: `heartbeat_loop()` beats
  every 5 s, sliced into 100 ms polls for responsive shutdown; `shutdown()` uses an
  atomic-exchange guard so double-shutdown is safe.
- **GPU queue memory model**: `daemon_nvme_alloc_queues` uses **`cudaMallocManaged`**
  for `QueuePair[]` (CPU-constructed, GPU-read — not plain `cudaMalloc`), batches one
  `NVM_ADD_USER_QUEUE` ioctl, and wires doorbells via `cudaHostGetDevicePointer(BAR0)`.
  `ctrl` is exposed on the vdevice so backends can call `nvm_dma_map_data_device`.
- **Daemon does NOT arbitrate a queue quota**: `ServiceState` tracks only
  per-allocation leases (PID+starttime+last_heartbeat) for the reaper;
  `granted_queues` is policy-only. Cross-process safety comes from the kernel's
  per-fd B3 queue-group model + the daemon owning the chrdev/bind — **not** a
  resource-range ledger.

### Downward dependencies

- **L1 Accelerator HAL**: drivers take an `IAccelerator* accel_`, but today it is
  only assert-checked in `DirectNvmeDeviceDriver` and otherwise **unused** — GPU
  queue allocation calls CUDA directly, not through L1. `queue_acquire_helper.cuh`
  includes **L0** `accel.h` for `TUTTI_DEVICE`/`TUTTI_FORCEINLINE`.
- **libnvm** (`nvme/libnvm/`): types, ctrl (attach/free/create_group/add_user_queue),
  dma, QueuePair(B3), nvm_cmd/io — confined to `nvme/`.
- **CUDA runtime**: `cudaMallocManaged/cudaSetDevice/cudaHostGetDevicePointer` in
  `daemon_nvme_queue_alloc.cu`.
- **gRPC + protobuf** (nvmeservice): only under `TUTTI_NVMESERVICE_ENABLED`, kept
  out of the public include graph via the opaque `NvmeClientState`.
- **snvme kernel driver**: `NVM_ADD_USER_QUEUE` ioctl; the kernel is the source of
  truth for the user QID pool.
- Downward only — nothing in L2 depends on L3+. The mock backend depends solely on
  `common/`, proving zero vendor coupling.

**What's above:** L3 backends call `open_vdevice()` once at `initialize()`, downcast
to `NvmeVirtualDevice*`, and read `d_qps`/`ctrl`/ns fields; from there the L4 IO
engine drives GPU-side IO. See §5 (L3) and §6 (L4).

---

## 5. Layer 3 — Backends

**Location:** `tutti/backends/` — device-agnostic core + mock (`libtutti_backends`);
NVMe transport in `nvme/` (`libtutti_backends_nvme`). Detail: `layer3-backends.md`.

### Purpose

A backend adapts one transport family (NVMe, and reserved RDMA/GDS) into the two
operations upper layers need: **register** an IO context against a vdevice, and
**submit_one** IO. L2 hands out an `IVirtualDevice*` (a resource grant); a backend
turns that grant into a usable IO path — for NVMe, the GPU-resident target handle,
the PRP descriptors, and the device-side submit kernel.

### Key types

| Type | File | Role |
|---|---|---|
| `IBackend` | `include/backend.h` | Device-agnostic SPI: lifecycle (`initialize`/`shutdown`), vdevice roster + `VDeviceHandle`, `metadata`/`backend_type`. Forward-declares L2 facade types (no libnvm/CUDA). |
| `IBatchSubmitter` | `nvme/include/batch_submitter.h` | Narrow **NVMe-scoped** submission SPI: `metadata` / `prepare_descriptors` / `release_descriptors` / `acquire_target_handle` / `launch_batch_gpu_stream`. Traffics in `nvme::SubSliceInfo`/`BufferDescriptor`, so it lives off the neutral contract. This is what L4 holds. |
| `BackendFactory` | `include/backend_factory.h` | Type→constructor registry; backends self-register at static-init via an explicit registrar struct (`REGISTER_BACKEND` can't token-paste a scoped enum). |
| `NvmeBackend` | `nvme/include/nvme_backend.h` | `: public IBackend, public IBatchSubmitter` + a `__device__ submit_one` in the GPU submit kernel. Downcasts each roster entry to `NvmeVirtualDevice*`. |
| `MockBackend` | `mock/include/mock_backend.h` | In-memory `IBackend`; zero NVMe/libnvm/CUDA symbols — proves the vendor-neutral boundary. |
| `StorageTarget` / `LbaExtent` | `include/storage_target.h` | Transport-neutral target: `NVME_RAW` = one `{start_lba, length_blocks}` range; `NVME_FILE` = an extent array. |
| `NvmeFileDeviceHandle` | `nvme/include/nvme_target_handle.h` | GPU-resident handle the kernel consumes: ns params, inline (≤8) + overflow extents, and an inline copy of the vdevice queue slice (`d_qps`, `queue_quota`). No host-heap pointer. |

### Public API surface

- **`IBackend`** — `initialize(dm, cfg)` (open `cfg.vdevice_count` vdevices from
  `cfg.phys_id`), `shutdown()` (idempotent, returns all vdevices), roster accessors
  `vdevice_count` / `vdevice_at` / `vdevice_handle_at`, `metadata`/`backend_type`.
- **`IBatchSubmitter` (NVMe)** — `acquire_target_handle(target, handle)` → opaque
  GPU handle; `prepare_descriptors` → PRP `BufferDescriptor[]`; `launch_batch_gpu_stream`
  → the GPU submit kernel (primary fast path); plus `release_*`. `submit_batch_cpu_sync`
  is a host-side **stub**.

### Design decisions

- **`register` / `submit_one` are deliberately off `IBackend`.** Their signatures
  and host-vs-device residency are transport-specific — NVMe's `submit_one` is a
  `__device__` function, so it cannot be a host virtual method. The base fixes only
  lifecycle / roster / handle / metadata.
- **`VDeviceHandle` is a dense index, never a pointer.** The host SPI names *which*
  vdevice without dereferencing host memory (the kernel resolves it device-side).
- **L4 binds to `IBatchSubmitter*`, not `NvmeBackend*`**, so the engine stays
  mockable without CUDA/libnvm — at the cost of speaking NVMe descriptor types.
- **RAW works, FILE metadata is missing.** `NVME_RAW` GPU read+write are hardware-
  verified. `NVME_FILE` builds a handle only from **pre-supplied** extents; nothing
  in `tutti/backends` turns a real file into `LbaExtent[]` (no FIEMAP / on-disk
  header parse — reference model lives in root `nvme_storage/`), and a single IO
  spanning multiple extents is unimplemented (`submit_one_impl` returns `-2`).

### Downward dependencies

L2 (`open_vdevice` + downcast to `NvmeVirtualDevice*` — the sole cross-boundary
cast, confined to the NVMe backend); L1/L0 (HAL types + `TUTTI_DEVICE` qualifiers,
carried but largely unused); libnvm + CUDA inside `nvme/` only. The device-agnostic
core and mock pull **zero** NVMe/CUDA symbols.

## 6. Layer 4 — IO Engine + StripeManager

**Location:** `tutti/io_engine/` — builds `libtutti_io_engine` (`io_engine_impl.cpp`
+ `local_nvme_io_engine.cpp`; `stripe_manager.cpp` is **not** in the library).
Detail: `layer4-io-engine.md`.

### Purpose

The IO Engine turns **already-routed** (shard-scoped) IO requests into GPU-launched
NVMe transfers. For each request it acquires a GPU target handle, fans the request
out into MDTS-sized sub-IOs, asks the backend to build PRP descriptors from the
buffer's DMA addresses, stages them CPU→GPU, launches the backend's submit kernel
on a stream, then synchronizes and returns the PRP pages. Alongside it ships
`io_engine::StripeManager`, a stateless logical→physical stripe mapper that is
**not yet wired** into the engine.

### Key types

| Type | File | Role |
|---|---|---|
| `IIoEngine` | `include/io_engine.h` | Backend-neutral engine contract: `submit_batch` / `submit_batch_async` / `submit_one` + capacity helpers. |
| `IoEngineImpl` | `src/io_engine_impl.{h,cpp}` | Concrete engine. Holds `backends::nvme::IBatchSubmitter* backend_` (not `IBackend`, not concrete), `IAccelerator* accel_`, and a GPU descriptor scratch `d_descs_`. |
| `LocalNvmeIoEngine` | `include/local_nvme/local_nvme_io_engine.h` | Thin PIMPL facade delegating to `IoEngineImpl`; its config is currently ignored. |
| `IoRequest` | `include/io_types.h` | Input to `submit_batch`: region + **pre-built** `target_handle` + logical byte offset/length. |
| `SingleShardIoRequest` | `include/io_types.h` | Input to `submit_one`: already shard-scoped, carries `StorageTarget shard_target` + `VDeviceHandle vdev` (routing resolved at IO time). |
| `StripeManager` / `StripeLayout` / `SubIo` | `include/stripe_manager.h` | IO-time pure-math logical→physical mapper: `map(layout, off, len)` → one `SubIo` per shard crossed. Complete + unit-tested; **not** called by the engine, **not** in the library. |

### Public API surface

- `submit_batch(reqs, is_read, stream)` — blocking; validate → MDTS fan-out per
  request → `prepare_descriptors` → stage CPU→GPU → `launch_batch_gpu_stream` →
  `synchronize_stream` → release. Each request must already carry a resolved handle.
- `submit_batch_async(...)` — returns after the kernel is queued; records an
  `AccelEvent`, defers descriptor release to lazy `cleanup_completed_async_ops`.
- `submit_one(SingleShardIoRequest, is_read, stream)` — blocking; the one path that
  resolves routing at IO time via `acquire_target_handle(shard_target, vdev)`.
- `max_entries_per_batch()` / `slice_fanout(region)` — capacity helpers.

### Design decisions

- **Holds the narrow `IBatchSubmitter` SPI**, so it is unit-testable against a
  lightweight mock (no CUDA/libnvm). Backend-neutrality is aspirational — it speaks
  NVMe descriptor types today.
- **Does MDTS fan-out, not stripe fan-out.** Stripe splitting is `StripeManager`'s
  job and is expected to run in the L5 caller before it builds per-shard requests.
  This is the **IO-time** StripeManager (read-side: "where the data already is"),
  distinct from L5's allocate-time StripeManager.
- **No memory pinning/registration.** The engine reads DMA addresses from
  `region->backend_private` and fails if unmapped; pinning is the caller's job via
  the HAL. It issues **no raw CUDA calls** — `d_descs_` is allocated through
  `IAccelerator`.
- **Batch routes the whole batch to `requests[0].target_handle`** (unvalidated) —
  callers must pre-group by target.

### Downward dependencies

L3 (`IBatchSubmitter`: `metadata` / `prepare_descriptors` / `release_descriptors`
/ `acquire_target_handle` / `launch_batch_gpu_stream`); L1 (`IAccelerator` for
`memcpy_async` staging, `synchronize_stream`, events, and device scratch alloc).
Not L2, not raw CUDA.

## 7. Layer 5 — Storage Interfaces

**Location:** `tutti/block_storage/` (`libtutti_block_storage`) and
`tutti/coordinator/` (packages the authoritative `raw_device`,
`libtutti_coordinator`); a legacy `tutti/raw_device/` stub is **to retire**.
**Target:** consolidate both peers under `tutti/storage-interfaces/`. Detail:
`layer5-storage-interfaces.md`.

### Purpose

The top of the storage stack applications hold. Two **peer** entry points at the
same level, each a different abstraction over the same `StorageTarget` machinery:
`block_storage` (named files over a striped logical namespace) and `raw_device`
(direct `namespace_id` + LBA). Both converge on
`backends::StorageTarget{NVME_RAW, ns, lba, len}` and are meant to route it through
the L4 engine — they differ today in whether they actually do.

### Key types

| Type | File | Role |
|---|---|---|
| `IBlockStorage` | `block_storage/include/block_storage.h` | Block/file-mode interface: named `GpuFile` lifecycle + per-shard `StorageTarget` production. |
| `GpuFile` / `FileShard` | `block_storage/include/block_storage_types.h` | A named logical byte space (`logical_size`, `stripe_size`) decomposed into `FileShard`s, each a contiguous `{device, ns, start_lba, length_blocks}` extent. |
| `StripeManager` (L5) | `block_storage/src/stripe_manager.h` | **Allocate-time** stripe manager: bump-allocates LBA ranges across namespaces, least-loaded. Distinct from L4's IO-time mapper. |
| `MetadataJournal` / `FileDirectory` | `block_storage/src/*.h` | Append-only WAL + checkpoint (crash recovery); thread-safe name/id → `GpuFile` registry. |
| `IRawDevice` | `coordinator/include/raw_device.h` | Direct namespace+LBA peer (namespace `tutti::coordinator`); the authoritative impl is `RawDeviceImpl`. |
| `RawTargetHandle` | `coordinator/include/coordinator_types.h` | An acquired raw extent: `{ns, start_lba, length_blocks}` + opaque backend `target_handle` + `region_id`. |

### Public API surface

- **`IBlockStorage`** — `open_gpu_file` / `close` / `delete` / `open_gpu_files_batch`
  / `list_gpu_file_names`; `acquire_device_handle(handle, shard)` → per-shard
  `StorageTarget`; `sync_file` / `flush_metadata`.
- **`IRawDevice`** — `acquire_raw_target(ns, lba, len)` → `RawTargetHandle`;
  `submit_read` / `submit_write` and their `_batch` forms →
  `io_engine_->submit_batch(...)`; `get_namespace_info` / `list_namespaces` (stubs).

### Design decisions

- **`StorageTarget` is the convergence noun** — both peers emit the same descriptor
  the L4 engine / L3 backend consume.
- **Block mode = allocate-time striping + durability** (WAL + checkpoint); raw mode
  = bare namespace+LBA, no directory, no journal, no striping.
- **`raw_device` drives the engine; `block_storage` does not.** `submit_read/write`
  call `IIoEngine::submit_batch`; `block_storage` only produces `StorageTarget`s and
  holds no engine reference (its "flush" is a 0-descriptor no-op).
- **Neither peer compiles today** — both still `#include` the deleted
  `backends::IBackendProvider` (`backends/include/backend_provider.h`, non-existent);
  all three L5 subdirs are disabled in the top-level build. FILE IO on either peer is
  additionally blocked on the missing L3 FIEMAP metadata layer.

### Downward dependencies

L4 (`IIoEngine::submit_batch` / `submit_one` — realized by `raw_device`); L3
(`StorageTarget` + `acquire_target_handle` target handles); L1 (`IAccelerator` for
stream sync). `block_storage` additionally forces `raw_device_ = nullptr`, so it
runs a synthetic 4-namespace roster instead of real enumeration.

## 8. Layer 6 — Coordinator

**Location:** `tutti/coordinator/` — builds `libtutti_coordinator` (same directory
and target that packages the L5 `raw_device`). Detail: `layer6-coordinator.md`.

### Purpose

The application-facing entry point that ties the lower layers into one usable
stack. It exposes `ICoordinator` — buffer registration, read/write batch
submission, and two data-path getters — and owns/exposes a `RawDeviceImpl`
(`IRawDevice`, the L5 peer packaged here for fileless namespace/LBA access). It is
a thin **façade/dispatcher**, not a builder of the stack.

### Key types

| Type | File | Role |
|---|---|---|
| `ICoordinator` / `CoordinatorImpl` | `include/coordinator.h`, `src/coordinator_impl.{h,cpp}` | Public interface + impl. Holds injected `backend_provider_`/`accelerator_`/`block_storage_`/`io_engine_`, a `BufferRegistry`, a `BatchBuilder`, and a `unique_ptr<RawDeviceImpl>`. Dispatches `submit_*_batch` to `io_engine_->submit_batch(_async)`. |
| `CoordinatorConfig` | `include/coordinator_types.h` | Dependency-injection struct. `is_valid()` requires `backend_provider && accelerator && block_storage && io_engine && max_batch_size>0`. This is **how** the lower layers reach the coordinator — built elsewhere, handed in. |
| `BufferRegistry` | `src/buffer_registry.{h,cpp}` | Pure in-memory bookkeeping: `ptr→region` + `id→region` maps + byte/count stats under a `shared_mutex`. Does **no** pinning itself — pinning is delegated to `IAccelerator`. |
| `BatchBuilder` | `src/batch_builder.h` | Declares `pack_requests()` to split under `max_batch_size`. **Declared but never used** — submit paths forward the whole vector straight to the engine. |
| `RawDeviceImpl` | `src/raw_device_impl.h` | The L5 peer sub-service the coordinator constructs and owns; ctor takes `(IBackendProvider*, IIoEngine*)`. |

### Public API surface

- `initialize(CoordinatorConfig)` / `cleanup()` — validate + cache layer pointers +
  create/adopt a default stream + build `RawDeviceImpl`; cleanup refuses if buffers
  or raw targets are still open, then resets the sub-service and destroys an owned stream.
- `register_buffer(ptr, size, MemoryKind)` / `unregister_buffer(region)` — dispatch
  to `accelerator_->register_host/register_device/register_external` (the HAL does
  the real pin), then record/remove the `MemoryRegion` in `buffer_registry_`.
- `submit_read_batch` / `submit_write_batch(IoRequest*, count, stream)` — validate
  each request's region against the registry, copy field-by-field into a
  `vector<tutti::IoRequest>`, single `io_engine_->submit_batch(is_read, stream)` call.
- `submit_*_batch_async(..., callback, user_data)` — launch via the engine; the
  **completion callback is never invoked** (event mechanism is a TODO stub).
- `get_block_storage()` / `get_raw_device()` — expose the two data paths;
  `max_batch_size()` / `slice_fanout(region)` are pure pass-throughs to the engine.
- `create_coordinator()` / `destroy_coordinator()` free functions.

### Orchestration / bring-up ordering

`initialize()` caches the four injected pointers (it constructs **none** of them),
wires a default stream (create-and-own if the caller's is invalid, else adopt),
then builds the **only** layer object it constructs itself:
`raw_device_impl_ = make_unique<RawDeviceImpl>(backend_provider_, io_engine_)`. The
per-IO submit path chooses a stream, validates every request region against the
registry, copies into a `tutti::IoRequest` vector, and makes one engine call.
`cleanup()` guards emptiness, resets the sub-service, destroys the owned stream —
it never tears down the injected backend/engine/storage (not owned).

### Design decisions

- **Dependency injection over construction.** All four lower layers arrive via
  `CoordinatorConfig`; the coordinator owns none of them (no `delete` in cleanup) —
  only the `RawDeviceImpl` and, conditionally, the default stream.
- **"Orchestrates all the layers" is only partially true.** It does not bring up
  L2/L3/L4/L5 — it receives fully-built objects and merely stores pointers, wires a
  stream, builds the raw-device sub-service, and forwards per-IO work. It touches
  the L2 device manager **not at all** (no `IDeviceManager`/vdevice references).
- **Buffer registration is split**: `BufferRegistry` is bookkeeping only; the real
  GPU pin/registration is the `IAccelerator` HAL's job.
- **`BatchBuilder` is dead code** and **async is a stub** (callback accepted then
  ignored); batches are forwarded whole regardless of `max_batch_size`.
- **Does not compile against the current stack.** `coordinator_impl.h` and
  `raw_device_impl.h` still `#include "backends/include/backend_provider.h"` —
  deleted in commit `cc430c4` (2026-07-27) when the NVMe backend migrated to
  `IBackend`. The config still requires a `backends::IBackendProvider*`, so the whole
  `tutti_coordinator` library is uncompilable; `add_subdirectory(coordinator)` is
  commented out in `tutti/CMakeLists.txt`, and the on-disk `.a`/`.so` artifacts are
  stale (pre-deletion). Migrating it onto the current `IBackend`/`IBatchSubmitter`
  seam is the same Phase-0 work already flagged for L4/L5.

### Downward dependencies

L5 (`IBlockStorage`, exposed via `get_block_storage()`; the `raw_device` peer is
packaged in the same library and owned as a sub-service); L4 (`IIoEngine` — every
`submit_*` forwards to it, and `max_batch_size`/`slice_fanout` pass through); L3
(the injected backend provider, forwarded into `RawDeviceImpl`); L1 (`IAccelerator`
for buffer pin/registration and the default stream). **Not L2.**

---

## 9. Cross-layer data / control flow

### Bring-up: allocating a GPU-resident NVMe slice (daemon mode)

A backend asks L2 for a slice; L2 routes to the NVMe daemon driver, which uses the
kernel + libnvm to create queues and CUDA (directly) to place them in GPU-visible
managed memory. The `IAccelerator` from L1 is present but, today, only carried —
the queue allocation itself calls CUDA directly (see status/drift notes).

```
Backend (L3+)         DeviceManagerImpl        DaemonNvmeDeviceDriver      kernel/libnvm + CUDA
    │                        │                          │                          │
    │ open_vdevice(id,quota) │                          │                          │
    │───────────────────────▶│                          │                          │
    │                        │ find dev by id;          │                          │
    │                        │ match driver->type()     │                          │
    │                        │─────────────────────────▶│ alloc_vdevice(dev,quota) │
    │                        │                          │ reserve on phys (atomic) │
    │                        │                          │ daemon_nvme_alloc_queues │
    │                        │                          │─────────────────────────▶│ NVM_ADD_USER_QUEUE (batch)
    │                        │                          │                          │ cudaMallocManaged QueuePair[]
    │                        │                          │                          │ map doorbells (BAR0)
    │                        │                          │◀─────────────────────────│ d_qps, ctrl
    │                        │◀─────────────────────────│ NvmeVirtualDevice*       │
    │◀───────────────────────│ IVirtualDevice*          │ (undo reserve on fail)   │
    │ check type()==LOCAL_NVME; static_cast<NvmeVirtualDevice*>                     │
    │ read d_qps / ctrl / blk_size ...                                             │
    │                                                                              │
    ▼  steady-state IO: GPU threads call acquire_queue()/issue_nvme_cmd()/poll()
       on d_qps directly — L2 is no longer on the path.
```

Key points:

- **Dispatch is by type**: `DeviceManagerImpl` finds the physical device by id, then
  picks the driver whose `type()` matches `dev->type()`.
- **Accounting lives on the physical device**: the driver reserves against
  `NvmePhysicalDevice::allocated_` (atomic) before allocating queues and undoes the
  reserve if `daemon_nvme_alloc_queues` fails.
- **L2 sits on L1 conceptually, on CUDA in practice**: the design intent is that L2
  uses L1's `IAccelerator` for GPU memory/queues, but the current NVMe path calls
  `cudaMallocManaged`/`cudaHostGetDevicePointer` directly and only *carries* the
  `IAccelerator*`. This is flagged in the drift table.
- **L0 shows up on the device side**: the `__device__` helpers in
  `queue_acquire_helper.cuh` are qualified with `TUTTI_DEVICE`/`TUTTI_FORCEINLINE`
  from L0, and compile against the CUDA target that L1's `tutti_accel` selected.

### Teardown

`close_vdevice(vdev)` looks up the `{vdev, driver}` pair in `live_vdevices_` and
calls `driver->free_vdevice(vdev)` (which frees queues and releases the reserve).
`Close()` frees all live vdevices first, then calls `shutdown()` on each driver
(stopping the heartbeat thread and releasing the lease). Even without an explicit
`Close()`, driver destructors that call `shutdown()` self-clean on manager
destruction.

### Steady-state IO: an app request down the full stack (L5 → L4 → L3 → GPU)

Once slices are allocated, an app request threads L5 → L4 → L3 and out to the GPU
submit kernel over `d_qps`. Below is the **`raw_device`** path — the only L5 peer
that reaches the engine today. Steps marked **(planned/absent)** are not wired.
(In the intended full-stack shape this path is *entered* via the L6 Coordinator,
which owns the `raw_device` peer and forwards `submit_*_batch` to the same engine —
see §8; both share the compile break on the deleted `IBackendProvider`.)

```
App                 IRawDevice (L5)        IIoEngine (L4)         NvmeBackend (L3)      GPU / d_qps
 │                       │                      │                       │                    │
 │ acquire_raw_target(ns,lba,len)               │                       │                    │
 │──────────────────────▶│ StorageTarget{NVME_RAW,…}                    │                    │
 │                       │ acquire_target_handle(target) ──────────────▶│ build GPU          │
 │                       │◀───────────── opaque target_handle ──────────│ NvmeFileDeviceHandle│
 │ submit_read(h, region, off, len, stream)     │                       │                    │
 │──────────────────────▶│ IoRequest{region, target_handle, off, len}   │                    │
 │                       │ submit_batch({req}, is_read, stream)          │                    │
 │                       │─────────────────────▶│ read ioaddrs =        │                    │
 │                       │                       │  region->backend_private                   │
 │                       │                       │ MDTS fan-out → SubSliceInfo[]              │
 │                       │                       │ prepare_descriptors ─▶│ PRP BufferDescriptor[]
 │                       │                       │ memcpy_async(descs CPU→GPU)                │
 │                       │                       │ launch_batch_gpu_stream ─────────────────▶│ submit_one:
 │                       │                       │                       │                    │ resolve_lba→SQE→
 │                       │                       │                       │                    │ ring doorbell→
 │                       │                       │ synchronize_stream ◀──────────────────────│ poll CQ (inline)
 │                       │                       │ release_descriptors ─▶│ PRP pages → cache  │
 │◀──────────────────────│◀───────── true ───────│                       │                    │
```

Honest notes on what is/isn't wired:

- **`raw_device` reaches the engine; `block_storage` does not.** `block_storage`
  produces `StorageTarget`s in `acquire_device_handle` but holds no `IIoEngine`
  reference and never calls `submit_batch` / `submit_one` — the launch step above is
  **absent** on the block path, and its `sync_file` "flush" is a 0-descriptor no-op.
- **(planned/absent) stripe splitting.** No `StripeManager::map` runs on this path;
  the engine does only MDTS fan-out. Logical→shard splitting is expected in the L5
  caller and is not yet built anywhere.
- **Neither L5 peer compiles today** — both still depend on the deleted
  `backends::IBackendProvider` for the acquire/release path, even though
  `raw_device`'s submit path already speaks the current `IIoEngine`.
- The RAW GPU submit + inline CQ poll (L3) is hardware-verified; a FILE target would
  need the missing L3 FIEMAP metadata layer before it could reach the kernel.

---

## 10. Implementation status

| Component | Status | Tested |
|---|---|---|
| **L0** CUDA branch | Complete (all 11 macros → real intrinsics) | Implicitly via L1+ usage; no dedicated L0 tests |
| **L0** host-only fallback | Complete (qualifiers empty, atomics aligned ints, launch `static_assert`) | Implicitly |
| **L0** ROCm branch | Qualifiers/launch/fence ready; **atomics are placeholder plain ints (FIXME)** | Never built (no target defines `TUTTI_ACCEL_ROCM`) |
| **L0** SYCL / CANN | Intentionally unimplemented (`#error`) | — |
| **L1** Identity / alloc (HOST/PINNED/DEVICE/MANAGED) / registry / ptr-translation | Complete | Yes — `tutti/tests/accel/*.cu` (GoogleTest, 35 cases) |
| **L1** stream / event / transfer / kernel launch | Complete | Yes (incl. cross-stream `wait_event`, 200-iter leak regression) |
| **L1** `ipc_export` arg validation | Complete | Yes (hard-asserted) |
| **L1** `ipc_import` | **Non-functional** — calls `register_external(...,size=0)` which returns nullptr; opened pointer leaked | Test `GTEST_SKIP`s the null-import path |
| **L1** `unregister` external teardown | **Not implemented** (no `cudaIpcCloseMemHandle`/`munmap`/`cudaHostUnregister`) | — |
| **L1** `register_*` pinning/mapping | Bookkeeping only (no `cudaHostRegister`/shm/mmap) | Error paths tested |
| **L1** `MemoryAccessFlags`, ROCm/SYCL backends, `memset`, stream query | Not implemented / dead | — |
| **L2** vendor-neutral core (`DeviceManagerImpl` + factory) | Complete | Yes — `device_manager_test.cpp` (lifecycle, dispatch, accounting, teardown) |
| **L2** mock backend (Driver/Physical/Virtual/Lease) | Complete, real accounting, no NVMe/CUDA/gRPC | Yes — physical/lease/device_manager tests |
| **L2** `NullLeaseManager` | Complete | Yes — `lease_manager_test.cpp` |
| **L2** NVMe **daemon** driver: mock-grant + `#else` fallback | Complete | Yes — `DaemonDriverUnit.*` |
| **L2** NVMe **daemon** driver: real gRPC/libnvm + `daemon_nvme_alloc_queues` | Complete | Yes on hardware — `DaemonDriverRealHw.*`, `DeviceManagerFacadeRealHw.*` (gated `TUTTI_NVME_REAL_HW=1` + `TUTTI_NVMESERVICE_ENABLED`) |
| **L2** `nvmeservice` daemon (ServiceState/Impl/Client) | Complete | Exercised by real-HW tests |
| **L2** IDeviceManager facade over daemon driver | Complete | Yes (mock + real-HW tiers) |
| **L2** NVMe **direct** driver | **Stub** — fabricates one 16-QP device, `d_qps` left null; no real libnvm discovery | — |
| **L2** `NvmeQueueGroup` | **Dead/reserved** — never instantiated; `NvmePhysicalDevice::queue_group` stays null | — |
| **L2** daemon-mode `ILeaseManager` (real) | Not implemented — injected lease mgr is always `MockLeaseManager`; keep-alive is `NvmeServiceClient`'s gRPC heartbeat | — |
| **L2** daemon-side queue-quota ledger | Not implemented (by design) — kernel B3 queue-group is source of truth | — |
| **L2** RDMA / GDS drivers | Not implemented — reserved `DeviceType` values only | — |
| **L2** `IAccelerator` integration into queue alloc | Not wired — `accel_` unused beyond a direct-driver assert | — |
| **L3** device-agnostic core (`IBackend` lifecycle/roster/`VDeviceHandle`/metadata, `BackendFactory`) + `MockBackend` | Complete | Yes — `backend_test` (11 cases, zero NVMe/CUDA symbols) |
| **L3** NVMe roster lifecycle + PRP descriptor build (SINGLE/DUAL/LIST) + two-tier page cache | Complete | Unit + real-HW (LIST > 2 pages not driven by real IO) |
| **L3** NVMe **RAW** GPU IO (`launch_batch_gpu_stream` → kernel → `submit_one_impl` + inline CQ poll) | Complete | Yes — real-HW read + write→read→verify |
| **L3** NVMe **FILE** handle build from **pre-supplied** extents (inline ≤8 + overflow) | Complete | Acquire/release only (extents hand-fabricated) |
| **L3** NVMe **FILE** file→extent metadata production (FIEMAP / on-disk header) | Not implemented (absent from `tutti/backends`; reference in root `nvme_storage/`) | No |
| **L3** NVMe single IO spanning multiple extents; kernel per-descriptor error readback; `submit_batch_cpu_sync`; SGL | Not implemented / stub (`submit_one_impl` returns `-2`; failures dropped; CPU-sync fakes success; PRP-only) | No |
| **L3** RDMA / GDS backends | Not implemented (enum values reserved) | — |
| **L4** `IIoEngine` / `IoEngineImpl` submit path (`submit_batch` / `_async` / `submit_one`) | Complete; compiles + links against `IBatchSubmitter` (`tutti/CMakeLists.txt:132`) | `submit_one_test` (mock submitter); `submit_batch` compiled, not run |
| **L4** `LocalNvmeIoEngine` PIMPL facade | Complete (config ignored) | Compiles |
| **L4** `io_engine::StripeManager` (contiguous-per-shard pure-math mapping) | Complete pure math — but **NOT wired** into the engine and **not** in `libtutti_io_engine` | Yes — 10/10 standalone (`stripe_manager_test`) |
| **L4** `tests/io_engine` subdir configuration | Does-not-configure — references deleted `layer4_smoke_test.cpp`; blocks whole-tree `cmake` at generate step | — |
| **L5** `block_storage` (named-file lifecycle, allocate-time striping, `StorageTarget` production, WAL + checkpoint) | Feature-rich / complete — but **does-not-compile** (deleted `IBackendProvider`) and **never calls** the engine | No (module not built) |
| **L5** `raw_device` (coordinator) `submit_read/write` → `IIoEngine::submit_batch` | Complete — live L4 drive — but **does-not-compile** (acquire/release still on deleted `IBackendProvider`) | No |
| **L5** `raw_device` `get_namespace_info` / `list_namespaces` | Stub (placeholder geometry) | No |
| **L5** legacy `tutti/raw_device/` stub | Does-not-compile (missing includes; `override`s methods the interface omits; all-`TODO` bodies) — **to retire** | No |
| **L5** subdir enablement (`block_storage` / `coordinator` / legacy `raw_device`) | All disabled (`tutti/CMakeLists.txt:133-135`); neither peer built | — |
| **L6** `ICoordinator`/`CoordinatorImpl` core (init/cleanup lifecycle, sync `submit_read/write_batch` dispatch, register/unregister buffer, default-stream lifecycle, `RawDeviceImpl` ownership + `IBlockStorage`/capacity pass-throughs, factory fns) | Feature-complete against the **old** API — but **does-not-compile** (bound to the deleted `backends::IBackendProvider`) | Tests exist but **only against mocks**, no real IO; smoke + integration tests fail to compile (define `MockBackendProvider : backends::IBackendProvider`), the rest cannot link (uncompilable lib) |
| **L6** `BufferRegistry` (thread-safe `ptr`+`id` maps + byte/count stats; pin delegated to `IAccelerator`) | Complete | Via the coordinator tests (not independently) |
| **L6** `BatchBuilder` request packing | **Dead code** — `pack_requests` declared but never called; batches forwarded whole regardless of `max_batch_size` | — |
| **L6** async completion callbacks (`submit_*_batch_async`) | **Stub** — callback/user_data accepted then ignored (explicit TODO; no event record/observe) | — |
| **L6** compile against the current stack / migration off `IBackendProvider` onto `IBackend`/`IBatchSubmitter` | Not done — config member + `RawDeviceImpl` ctor/`handle_map_` + the three tests' mock still on the deleted type | — |
| **L6** any construction/bring-up of L2-L5 | Not implemented (by design) — all lower layers are injected via `CoordinatorConfig`; no device-manager/vdevice references at all | — |
| **L6** subdir enablement | Disabled (`tutti/CMakeLists.txt:134`, `# add_subdirectory(coordinator)`); on-disk `.a`/`.so` are stale (pre-`cc430c4`) | — |

**Where "raw_device" fits.** The L2 docs use "file backend" and "raw device
backend" as illustrative in-process consumers of a virtual device — and at L2 that
is still true (its only consumers are the mock and real-HW tests). The real
raw-device / block-storage split now lives at **Layer 5** and is documented in §7:
`raw_device` (`IRawDevice`, coordinator-packaged) drives the L4 engine while
`block_storage` (`IBlockStorage`) produces `StorageTarget`s but does not — and
neither compiles yet.

---

## 11. Documentation drift found

The per-layer docs contain claims that the source contradicts. Prefer code truth;
the table below is the correction backlog for `layer0-*.md`, `layer1-*.md`, and
`layer2-device-manager.md`.

| Layer | Doc claim | Code reality |
|---|---|---|
| L0 | "Multiple flags will cause compile errors" (`layer0-abstraction.md:202`) | Plain `#if/#elif/#else`; no arity check. Defining two flags does **not** error — first branch (CUDA) silently wins. |
| L0 | "one file, ~150 lines" (`:247`) | `accel.h` is **97 lines**. |
| L0 | Selection via CMake options `-DTUTTI_ACCEL_CUDA=ON/OFF` (`:18,190-200`) | No `option()`/cache var exists; `TUTTI_ACCEL_CUDA` is hardcoded PUBLIC on `tutti_accel` (`accel/CMakeLists.txt:65-67`). `=ON/OFF` do nothing. The two L0 docs contradict each other here. |
| L0 | Example include site `nvme/queue_group.cuh` (`macro-mapping.md:114`) | File does not exist. Real consumers: `nvme/include/queue_acquire_helper.cuh:18`, `block_storage/include/gpu_file_resolve.cuh:8`. |
| L0 | Decision 2 cites `gpu_file_resolve.h` (`:263`) | The accel.h consumer is `gpu_file_resolve.cuh` (`.h` exists but is not the consumer). |
| L1 | `memcpy_async` auto-detects direction via `cudaPointerGetAttributes()` (both docs) | Code calls `cudaMemcpyAsync` with `cudaMemcpyDefault`; never calls `cudaPointerGetAttributes` — UVA/runtime infers direction. |
| L1 | `register_host` → `cudaHostRegister`, `unregister` → `cudaHostUnregister` (cuda-mapping.md) | No such calls. `register_host` is pure bookkeeping (sets kind=HOST, does not pin); `unregister` only deletes the spec + erases maps. |
| L1 | `unregister` does `cudaIpcCloseMemHandle`/`munmap` (Decision 5, IPC flow step 4) | None performed — imported IPC pointers and mappings are leaked. |
| L1 | `allocate_host(MANAGED)` → `cudaMallocManaged` | `allocate_host` only handles HOST/PINNED_HOST; MANAGED → "invalid kind" → nullptr. MANAGED is available only via `allocate_device`. |
| L1 | External flows: `HOST_SHM`→shm_open+mmap, `HOST_FD_MAP`→mmap, EXTERNAL→`cudaHostRegister`/IPC | `register_external` only stores pointers + a deep copy of the spec. |
| L1 | `ipc_import` returns an EXTERNAL/DEVICE_IPC region | It calls `register_external(...,size=0)` which returns nullptr on size==0, so import **always returns null** and leaks the opened pointer. |
| L1 | Testing = `layer1_smoke_test.cu`, 10-11 tests, memset (both docs + README) | Current suite is `tutti/tests/accel/*.cu` via GoogleTest, 35 cases. Old smoke test retired. "Coverage gaps" list is stale. |
| L1 | README: "async memset", "stream query" | No `cudaMemset` and no stream query method exist. |
| L1 | `MemoryAccessFlags` documented as a core type | Defined but never referenced by any signature or the impl (dead). |
| L2 | `acquire_queue` = "round-robin via atomic ticket" (§5 + `.cuh` header comment) | `queue_acquire_helper_impl.cuh` is a pure hash: `(blockDim.x*32u + threadIdx.x) % n_qps` — no atomic, no ticket. |
| L2 | §5 `NvmeVirtualDevice` struct omits `ctrl` | Real struct has `nvm_ctrl_t* ctrl = nullptr;`, load-bearing — daemon sets it so backends can `nvm_dma_map_data_device()`. |
| L2 | Daemon holds a "Grant ledger { pid → resource_range }" arbitrating the queue budget (§1/§4 Level ①) | `nvmeservice_state.h`: no queue ledger; `granted_queues` is policy-only; `Allocation` stores no resource_range. Safety = kernel B3 queue-group + daemon owning chrdev/bind. |
| L2 | §2 box: L2→L1 edge "cudaMalloc d_qps" | Uses `cudaMallocManaged` (QueuePair has C++ members needing CPU construction). |
| L2 | Partial-Open cleanup "is the caller's responsibility" (blanket) | Manager rollback clears `devices_`; drivers stay owned so their dtors (`~DaemonNvmeDeviceDriver`→`shutdown()`) self-clean. Only holds for drivers whose dtor doesn't shutdown (e.g. `MockDeviceDriver` leaks its lease/heartbeat on a peer's failed Open). |
| L2 (gap) | Lifecycle table says only "Open must be called first" | `Open()` is idempotent — a second call returns true without re-enumerating (`opened_` guard). |
| L2 (gap) | §5 presents `NvmeQueueGroup` as an active "GPU-resident queue array" | Never instantiated or populated; `queue_group` stays null everywhere — dead/reserved code. |
| L2 (gap) | Architecture box implies L2 uses L1 to allocate queues | `IAccelerator* accel_` is only assert-checked (direct) and otherwise unused; queue alloc calls CUDA directly. |
| L2 (gap) | §2/§4 use "file backend" / "raw device backend" as consumers | No such backends consume L2 today; only the mock and real-HW tests do. Illustrative, not implemented. |

**On the L3-L5 docs.** `layer3-backends.md`, `layer4-io-engine.md`, and
`layer5-storage-interfaces.md` were written and verified against code in this pass,
so they carry no residual drift rows here — their own **"Known Issues & Gaps"**
sections are the authoritative gap lists for those layers (missing L3 FIEMAP
metadata layer; L4 `StripeManager` not wired + test-subdir configure break; both L5
peers coded against the deleted `IBackendProvider`). The drift table above remains
the L0-L2 correction backlog. The one cross-doc fact worth pinning: the **two
StripeManagers** are different components (L4 IO-time mapper vs L5 allocate-time
allocator) and must not be conflated; both docs already say so, so no inconsistency
is outstanding.

---

## 12. Terminology / glossary

- **Accelerator** — a GPU (or GPU-like) compute device. At L0 it is a compile-time
  vendor target; at L1 it is the runtime `IAccelerator` object (`CudaAccelerator`).
- **`MemoryKind`** — L1 enum selecting an allocation/free path: HOST, PINNED_HOST,
  DEVICE, MANAGED, EXTERNAL.
- **`MemoryRegion`** — L1's central tracking record for a registered buffer
  (id/kind/device+host pointers/size + opaque `backend_private` slot for upper layers).
- **Pinned / managed memory** — *pinned* = page-locked host memory (`cudaHostAlloc`),
  DMA-capable; *managed* = unified memory (`cudaMallocManaged`) addressable from both
  host and device, migrated on demand. L2 uses managed memory for GPU-resident queues.
- **Physical device** — one storage controller as seen **by this process**
  (`IPhysicalDevice`). Its grant is a per-process view, not the hardware total.
- **Virtual device** — a backend's slice of a physical device (`IVirtualDevice`);
  downcast to a concrete subtype (e.g. `NvmeVirtualDevice`) after checking `type()`.
- **Lease** — cross-process keep-alive record (`ILeaseManager`): a heartbeat that
  proves an allocation's owner is still alive so a reaper can reclaim dead ones.
- **Queue pair (QP)** — an NVMe submission/completion ring pair. `d_qps` is the
  GPU-visible array of `QueuePair` (libnvm B3) a backend issues commands on.
- **Queue group** — the kernel's (snvme B3) per-fd grouping of user queues; the
  **source of truth** for the user QID pool and the actual cross-process safety
  mechanism (not a daemon ledger).
- **Doorbell** — the BAR0 MMIO register a producer writes to notify the NVMe
  controller of new submissions; mapped into GPU-visible space via
  `cudaHostGetDevicePointer`.
- **Direct mode** — single-process NVMe driver (`DirectNvmeDeviceDriver`,
  `NullLeaseManager`, no heartbeat). Currently a stub.
- **Daemon mode** — multi-process NVMe driver (`DaemonNvmeDeviceDriver`) talking to
  the `nvmeservice` daemon over gRPC; real path, verified on hardware.
- **IPC handle** — an opaque 64-byte token (`cudaIpcMemHandle_t`) exported by one
  process and imported by another to share a device allocation.
- **`TUTTI_ACCEL_*`** — the compile-time vendor-selection macros; exactly one is
  expected, set PUBLIC by the L1 `tutti_accel` target (not by user CMake options).
- **Backend (L3)** — the adapter for one transport family (NVMe, reserved RDMA/GDS)
  that turns an L2 vdevice grant into a usable IO path (register + submit).
- **`IBackend` vs `IBatchSubmitter`** — the two L3 SPIs. `IBackend` is
  **device-agnostic** (lifecycle, vdevice roster, `VDeviceHandle`, metadata).
  `IBatchSubmitter` is **NVMe-scoped** (`prepare_descriptors` /
  `acquire_target_handle` / `launch_batch_gpu_stream` …) and traffics in NVMe
  descriptor types — it holds the operations `IBackend` deliberately omits. `NvmeBackend`
  implements both; L4 holds only the `IBatchSubmitter*`.
- **`StorageTarget`** — the transport-neutral target descriptor upper layers emit:
  `NVME_RAW` = one `{start_lba, length_blocks}` range; `NVME_FILE` = an `LbaExtent[]`.
  The convergence noun shared by L5, L4, and L3.
- **Target handle / `NvmeFileDeviceHandle`** — the opaque, GPU-resident handle
  `acquire_target_handle` builds from a `StorageTarget`: ns params, inline (≤8) +
  overflow extents, and an inline copy of the vdevice queue slice. The submit kernel
  consumes it and never dereferences host memory.
- **PRP descriptor** — a per-command `BufferDescriptor` (SINGLE / DUAL / LIST) built
  by `NvmeCommandBuilder` from a buffer's DMA `ioaddrs`; lives in GPU-accessible
  memory. SGL is not implemented (PRP only).
- **MDTS** — Maximum Data Transfer Size, the NVMe per-command cap. The L4 engine's
  **MDTS fan-out** splits one request into `max_io_size`-sized sub-IOs (distinct from
  stripe fan-out, which it does not do).
- **IO Engine (L4)** — `IIoEngine` / `IoEngineImpl`: turns shard-scoped requests
  into GPU-launched NVMe IO by driving the backend's `IBatchSubmitter`.
- **StripeManager (two of them)** — logical↔physical stripe logic at **two different
  layers**. The **L4 IO-time** `io_engine::StripeManager` maps *where data already
  is* (read-side, pure byte math, not wired). The **L5 allocate-time**
  `block_storage::StripeManager` decides *where data goes* (bump-allocates LBA ranges
  at file creation). Different components — do not conflate.
- **Block mode vs raw device** — the two L5 peers. **Block mode** (`IBlockStorage`)
  = named files over a striped logical namespace with a metadata journal. **Raw
  device** (`IRawDevice`) = direct `namespace_id` + LBA, no file/directory/striping.
- **`GpuFile` / `FileShard`** — L5 block-mode types: a named logical byte space
  (`logical_size`, `stripe_size`) decomposed into `FileShard`s, each a contiguous
  `{device, ns, start_lba, length_blocks}` extent that becomes a `StorageTarget`.
- **`RawTargetHandle`** — L5 raw-device acquired extent: `{ns, start_lba,
  length_blocks}` + the opaque backend `target_handle` + a `region_id`.
- **Coordinator (L6)** — the application-facing `ICoordinator` / `CoordinatorImpl`
  façade: it receives fully-built L3/L4/L5 objects via `CoordinatorConfig`
  (dependency injection, not construction), owns a `raw_device` sub-service and a
  default stream, tracks registered buffers, and dispatches per-IO read/write
  batches to the injected `IIoEngine`. It orchestrates *use* of the lower layers,
  not their *bring-up*, and does not touch L2.
- **`CoordinatorConfig`** — the L6 dependency-injection struct
  (`backend_provider`/`accelerator`/`block_storage`/`io_engine` + `max_batch_size`
  + default stream); `is_valid()` enforces all four pointers non-null. This is the
  seam by which the pre-built lower layers reach the coordinator.
- **`buffer_registry` (L6)** — the coordinator's in-memory bookkeeping of
  registered buffers (`ptr→region` and `id→region` maps + byte/count stats under a
  `shared_mutex`). Bookkeeping only — the actual GPU pin/registration is delegated
  down to the `IAccelerator` HAL.
- **`batch_builder` (L6)** — a declared-but-unused coordinator helper
  (`pack_requests()`) meant to split submissions under `max_batch_size`; **dead
  code** today — submit paths forward the whole request vector to the engine.


