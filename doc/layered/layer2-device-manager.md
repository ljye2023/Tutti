# Layer 2: Device Manager

**Version:** 2.1  
**Date:** 2026-07-26  
**Status:** Redesigned — vendor-neutral interfaces live; NVMe daemon path real, direct path stubbed; mock backend complete  
**Library:** `libtutti_device_manager`  
**Location:** `tutti/device_manager/`

---

## 1. Motivation

### Physical Device Ownership

A physical NVMe controller (or RDMA NIC, GDS device) can only be initialized once. If two paths in the same process — or two separate processes — both call `nvm_ctrl_init()` on the same PCI address, they corrupt each other's queue state. Somebody must own the device exclusively and hand out slices.

### Cross-Process Conflicts

When multiple coordinators share one server, they share one physical NVMe controller. Without a mediator, process A and process B each try to claim the full queue budget. The result is either a hard failure at the kernel driver level or silent queue aliasing (two processes issuing commands on the same queue pair, mixing completions).

### Resource Sharing

A physical controller has a fixed queue-pair budget `N`. The kernel's block-layer holds some; user processes hold the rest. This budget must be divided correctly:

```
N queue pairs total
├── kernel blk-mq reserve  (fixed at driver load)
└── user pool
    ├── Process A: K pairs
    └── Process B: N − kernel − K pairs
```

Within a process the pool is further divided among backends. A two-level allocator handles both splits.

> **Illustrative naming.** Throughout this doc, "file backend" and "raw device backend" are used as *example* L2 consumers to motivate the per-backend split. **No such backends consume L2 today** — the raw-device / file-backend distinction is a planned Layer 3 shape, not implemented at L2. The only current L2 consumers are the mock test suite and the NVMe real-hardware tests (see §6).

**Device Manager is the answer to all three problems.** It owns the physical device, arbitrates the cross-process grant, and slices the per-process grant into per-backend virtual devices — for every device type, not only NVMe.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Backends (Layer 3)  — PLANNED consumer shape                     │
│  local_nvme / rdma / gds  (none consume L2 yet)                   │
│  call open_vdevice() once at initialize(); cast to subtype        │
│  Today only the mock suite + NVMe real-HW tests call open_vdevice │
└──────────────────────────┬──────────────────────────────────────┘
                           │  IDeviceManager (facade)
┌──────────────────────────▼──────────────────────────────────────┐
│  Device Manager Core (Layer 2)                                    │
│                                                                   │
│  IDeviceManager                                                   │
│  ├── Physical device registry  (IPhysicalDevice per controller)  │
│  ├── Two-level allocator       (Level ① cross-proc, Level ②)    │
│  └── Plugin registry           (one IDeviceDriver per type)      │
│                                                                   │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │
│  │ NvmeDeviceDriver │  │ RdmaDeviceDriver │  │  GdsDriver    │  │
│  │  (IDeviceDriver) │  │  (IDeviceDriver) │  │ (IDeviceDriver│  │
│  └──────────────────┘  └──────────────────┘  └───────────────┘  │
│                                                                   │
│  Cross-process arbitration: ILeaseManager (DeviceService daemon) │
└──────────────────────────┬──────────────────────────────────────┘
                           │  cudaMallocManaged d_qps, map doorbells
┌──────────────────────────▼──────────────────────────────────────┐
│  Layer 1: Accelerator HAL                                         │
└─────────────────────────────────────────────────────────────────┘
```

### Device Manager Core

`IDeviceManager` is the single facade exposed upward. It owns:
- A flat registry of `IPhysicalDevice*` instances (one per enumerated controller)
- A plugin table of `IDeviceDriver*` (one per backend family, registered at startup)
- The two-level allocation state

### Device Drivers

Each backend family provides one `IDeviceDriver` implementation. The driver is responsible for:
1. Enumerating its physical devices (`enumerate()`)
2. Allocating and freeing virtual devices (`alloc_vdevice()` / `free_vdevice()`)

Adding a new transport (e.g., CXL) means implementing `IDeviceDriver` — no changes to `IDeviceManager`.

### Virtual Device API

`IDeviceManager::open_vdevice()` returns an `IVirtualDevice*`. The caller checks `type()` and downcasts to the concrete subtype to access transport-specific resources:

```cpp
IVirtualDevice* vdev = mgr->open_vdevice(phys_id, quota);
if (vdev->type() == DeviceType::LOCAL_NVME) {
    auto* ndev = static_cast<NvmeVirtualDevice*>(vdev);
    // use ndev->d_qps, ndev->queue_quota, ndev->blk_size, ...
}
```

---

## 3. Device-Agnostic Interfaces

This section is the complete vendor-neutral surface: the interfaces, the key structures/ownership, and how they call each other. All files live under `tutti/device_manager/include/common/`. Nothing here depends on NVMe, libnvm, or CUDA.

### `DeviceType` — Transport Family Enum
**File:** `include/common/device_type.h`

```cpp
enum class DeviceType {
    LOCAL_NVME = 0,
    RDMA       = 1,
    GDS        = 2,
};
```

Used as a type discriminator for safe downcasting of `IPhysicalDevice*` and `IVirtualDevice*`.

---

### `IPhysicalDevice` — Physical Controller Descriptor
**File:** `include/common/iphysical_device.h`

Pure interface. One instance per enumerated physical controller, owned by the `IDeviceDriver` that created it. Pointers are valid until `IDeviceManager::Close()`.

| Method | Return | Description |
|--------|--------|-------------|
| `id()` | `int32_t` | Stable identifier assigned during enumeration |
| `type()` | `DeviceType` | Transport family; use for safe downcast |
| `pci_addr()` | `string_view` | PCI address string (e.g., `"0000:17:00.0"`) |
| `display_name()` | `string_view` | Human-readable label |
| `process_grant()` | `uint32_t` | Resource units granted to this process (QPs for NVMe; driver-defined for others). Per-process view, not hardware total. |
| `available_grant()` | `uint32_t` | `process_grant()` minus units already carved into vDevices |
| `caps()` | `uint32_t` | Capability bitmask (bit 0: GPUDIRECT_CAPABLE; bits 1–31 reserved) |

**Resource units** are transport-defined:
- `LOCAL_NVME` → NVMe queue pairs
- `RDMA` → queue-pair / memory-region slots (driver-defined)
- `GDS` → driver-defined

---

### `IVirtualDevice` — Generic Virtual Device Base
**File:** `include/common/ivirtual_device.h`

Generic base for all virtual devices. Backends program against this interface; they downcast to the concrete subtype when transport-specific resources are needed.

| Method | Return | Description |
|--------|--------|-------------|
| `phys_id()` | `int32_t` | Which `IPhysicalDevice` this slices |
| `vdev_id()` | `uint32_t` | Dense index within one `IDeviceManager` instance |
| `type()` | `DeviceType` | Transport family; use for safe downcast |
| `resource_count()` | `uint32_t` | Units granted to this virtual device |
| `caps()` | `uint32_t` | Capability bitmask (same encoding as `IPhysicalDevice::caps`) |

**Downcast pattern:**
```cpp
IVirtualDevice* vdev = mgr->open_vdevice(phys_id, quota);
switch (vdev->type()) {
  case DeviceType::LOCAL_NVME:
    auto* ndev = static_cast<NvmeVirtualDevice*>(vdev); // NVMe fields here
    break;
  case DeviceType::RDMA:
    auto* rdev = static_cast<RdmaVirtualDevice*>(vdev);
    break;
}
```

---

### `IDeviceDriver` — Plugin Factory
**File:** `include/common/idevice_driver.h`

Each backend family registers one `IDeviceDriver` with `IDeviceManager` at startup. The manager calls `enumerate()` once during `Open()` and delegates all virtual-device lifecycle calls to the appropriate driver.

| Method | Return | Description |
|--------|--------|-------------|
| `type()` | `DeviceType` | Which backend family this driver handles |
| `enumerate(out_devices)` | `int` | Discover physical devices; append to `out_devices`; return count appended |
| `alloc_vdevice(dev, quota, error)` | `IVirtualDevice*` | Allocate a virtual device with `quota` resource units from `dev`; return `nullptr` on failure |
| `free_vdevice(vdev)` | `void` | Return the virtual device to the pool; no-op on `nullptr` |
| `shutdown()` | `void` | Stop the heartbeat thread, release the lease, and free all enumerated `IPhysicalDevice` instances. The driver is unusable afterward. |

**Ownership rules:**
- `IPhysicalDevice*` instances returned by `enumerate()` are owned by the driver; lifetime until `shutdown()` / driver destruction
- `IVirtualDevice*` instances returned by `alloc_vdevice()` are owned by the driver; lifetime until `free_vdevice()`

---

### `IDeviceManager` — Layer 2 Facade
**File:** `include/common/idevice_manager.h`

The single interface callers (backends, coordinator) program against. Replaces the old `IDeviceRegistry` + `IVirtualNvme` split.

#### Lifecycle

| Method | Return | Description |
|--------|--------|-------------|
| `Open()` | `bool` | Probe all registered drivers; build physical device registry. Must be called first. **Idempotent:** a second call while already open returns `true` immediately without re-enumerating (guarded by `opened_`). |
| `Close()` | `void` | Release all virtual devices; tear down all physical devices. No-op if not open. |

#### Physical Device Registry

| Method | Return | Description |
|--------|--------|-------------|
| `device_count()` | `int` | Number of enumerated physical devices |
| `device_at(index)` | `IPhysicalDevice*` | Device by dense index `[0, device_count())`; `nullptr` if out of range |
| `find_by_id(id)` | `IPhysicalDevice*` | Device by `id()`; `nullptr` if not found |
| `find_by_type(t, ordinal=0)` | `IPhysicalDevice*` | n-th device of backend type `t`; `nullptr` if not found |
| `list()` | `vector<IPhysicalDevice*>` | All devices |

#### Virtual Device Allocation

| Method | Return | Description |
|--------|--------|-------------|
| `open_vdevice(phys_id, quota, error)` | `IVirtualDevice*` | Carve `quota` resource units from `phys_id`; `nullptr` on unknown id / quota 0 / pool exhausted |
| `close_vdevice(vdev)` | `void` | Return slice to pool; no-op on `nullptr`; invalidates `vdev` |
| `available_resources(phys_id)` | `uint32_t` | Unallocated units for `phys_id`; `0` if unknown |
| `caps(phys_id)` | `uint32_t` | Capability bitmask; `0` if unknown |

**Typical backend usage:**
```cpp
// At initialize():
mgr->Open();
auto* phys = mgr->find_by_type(DeviceType::LOCAL_NVME);
auto* vdev = mgr->open_vdevice(phys->id(), queue_quota);
auto* ndev = static_cast<NvmeVirtualDevice*>(vdev);  // access d_qps, etc.

// At teardown():
mgr->close_vdevice(vdev);
mgr->Close();
```

#### Construction

`DeviceManagerImpl` is the only concrete `IDeviceManager`. It is never constructed directly; callers assemble a driver set and hand it to the factory, which takes ownership of the drivers:

```cpp
// src/common/device_manager_impl.cpp
std::unique_ptr<IDeviceManager> create_device_manager(
    std::vector<std::unique_ptr<IDeviceDriver>> drivers);
```

`Open()` iterates the drivers and calls `enumerate()` on each. A driver returning `n < 0` aborts `Open()` (returns `false`, clears `devices_`); `n == 0` is a success that simply contributes no devices. The rollback only clears the manager's registry — it does **not** call `shutdown()` on drivers that already enumerated. In practice that is usually harmless: the drivers stay owned by `DeviceManagerImpl`, so their destructors self-clean (e.g. `~DaemonNvmeDeviceDriver()` calls `shutdown()`, which joins the heartbeat thread, releases the lease, and tears down libnvm state). The leak only bites drivers whose destructor does **not** shut down — e.g. `MockDeviceDriver`, whose dtor does not release its lease/heartbeat, so a peer driver's failed `Open()` would strand a mock lease until the manager itself is destroyed. So the blanket "caller's responsibility" claim holds only for that class of driver.

---

### Key Structures & Ownership

The generic layer defines no concrete data structures beyond the interfaces above; the only concrete class is `DeviceManagerImpl`, whose internal state captures the whole model:

| Member | Type | Meaning |
|--------|------|---------|
| `drivers_` | `vector<unique_ptr<IDeviceDriver>>` | **Owned.** One driver per backend family. |
| `devices_` | `vector<IPhysicalDevice*>` | Flat registry, raw pointers **owned by the drivers**. Built during `Open()`. |
| `live_vdevices_` | `vector<pair<IVirtualDevice*, IDeviceDriver*>>` | Maps each open vDevice to the driver that made it, so `close_vdevice()` can route teardown correctly. |
| `mutex_` | `std::mutex` | Guards `open_vdevice` / `close_vdevice`. |

### Call Relationships

```
create_device_manager(drivers)          // factory takes ownership of drivers
        │
        ▼
DeviceManagerImpl::Open()
        └── for each driver: driver->enumerate(devices_)   // drivers create & own IPhysicalDevice
                                     │
                                     └─ [daemon] gRPC connect + attach + create_group
                                        [direct] open device via libnvm   (stubbed today)
                                        [mock]   fabricate N devices in memory

DeviceManagerImpl::open_vdevice(phys_id, quota)
        ├── find_by_id(phys_id) → IPhysicalDevice*
        ├── pick driver whose type() == dev->type()
        ├── driver->alloc_vdevice(dev, quota)  → IVirtualDevice*   // driver owns it
        └── record {vdev, driver} in live_vdevices_

caller: static_cast<NvmeVirtualDevice*>(vdev)   // gated by type(); reads d_qps etc.

DeviceManagerImpl::close_vdevice(vdev)
        └── look up driver in live_vdevices_ → driver->free_vdevice(vdev)

DeviceManagerImpl::Close()
        ├── free every live vdevice via its driver
        └── driver->shutdown() for each driver   // frees IPhysicalDevice, stops heartbeat
```

The manager never touches `ILeaseManager`; leases are entirely a driver concern (injected at driver construction). This is why the cross-process/in-process boundary sits inside `enumerate()`, not in the manager (see §5).

---

### `ILeaseManager` — Cross-Process Arbitration
**File:** `include/common/ilease_manager.h`

Manages the per-process resource grant issued by the **DeviceService** daemon. Must be called periodically from the heartbeat thread; the daemon reaps expired grants (dead-process cleanup).

| Method | Return | Description |
|--------|--------|-------------|
| `heartbeat(lease_id)` | `bool` | Keep grant alive; `false` if lease expired or unknown |
| `release_lease(lease_id)` | `bool` | Explicitly release grant before exit; `false` if not held |
| `has_lease(lease_id)` | `bool` | Query whether grant is currently active |

**Implementations shipping today** (see §6 for status):
- `NullLeaseManager` (`include/common/null_lease_manager.h`) — no-op for direct / single-process mode; all three methods return `true`.
- `MockLeaseManager` (`mock/include/mock_lease_manager.h`) — counts heartbeats/releases for tests; all methods succeed.

> **Note:** There is **no** dedicated daemon-mode `ILeaseManager` implementation in the tree yet. In daemon mode the cross-process lease is kept alive by the `NvmeServiceClient`'s own gRPC bidi-stream heartbeat inside `DaemonNvmeDeviceDriver`; the injected `ILeaseManager*` is a higher-level tracker and is currently a `MockLeaseManager` even in the real-hardware tests. A real `DeviceServiceLeaseManager` is a future item.

---

## 4. Resource Management

### Level ① — Cross-Process Arbitration (DeviceService)

**Goal:** Prevent multiple processes from colliding on the same physical controller.

**Mechanism (as built):** The `nvmeservice` daemon holds the chrdev/bind for each NVMe and keeps the owner-side `nvm_ctrl_t` alive. Each process attaches as a client (`nvm_ctrl_attach_client`) and brings up its **own** per-fd queue group. Cross-process safety comes from two facts, **not** from a daemon-side queue accountant:

1. The kernel B3 queue-group model gives every client fd an independent queue group, capped per fd by `NVM_MAX_QUEUES_PER_GROUP`; the kernel's user-QID pool is the single source of truth.
2. The daemon owns the chrdev/bind, so no other process can open the controller as owner.

The daemon keeps only a per-allocation **lease** (`{allocation_id → PID + starttime + last_heartbeat}`, see `Allocation` in `nvmeservice_state.h`) so the reaper can clean up dead clients' bookkeeping. `granted_queues` in the Connect grant is **policy advice only** — the client must not exceed it, but the kernel separately enforces the per-fd cap. There is **no** `{pid → resource_range}` ledger and no quota refund: when a dead client's fd closes, the kernel's `snvm_dev_release` fd-cascade reclaims the actual queues.

```
        physical device (budget = N resource units, kernel owns the ledger)
                        │
   ┌────────────────────▼──────────────────────────┐   ← Level ①
   │   nvmeservice daemon                            │     owns chrdev/bind + owner nvm_ctrl_t
   │   per-allocation lease + heartbeat + PID reaper │     (NO queue ledger; policy-only grant)
   └──────┬───────────────────────────┬─────────────┘
          │ attach_client + grant(a)  │ attach_client + grant(b)
          ▼                           ▼
   Process A: DM (in-proc)        Process B: DM (in-proc)
   own per-fd queue group         own per-fd queue group  (kernel caps each fd)
```

**Single-process mode:** Registry opens the device directly; grant = entire budget; `ILeaseManager` is a no-op.

### Level ② — In-Process Slicing (IDeviceManager)

**Goal:** Carve this process's grant into per-backend virtual devices.

**Mechanism:** `IDeviceManager` tracks the per-physical-device grant via an atomic counter (`NvmePhysicalDevice::reserve`/`release`), not a free-list of specific queue indices. Each backend calls `open_vdevice()` once at initialization; the returned quota is a *count*, and the driver assigns which physical queues back it.

The two diagrams below are **illustrative** — "file backend" / "raw backend" are placeholder names for future L3 consumers (none exist today; see the §1 note and §6):

```
   Process A: DM (in-proc)
   grant = 16 queue pairs
        │
        ├── vDevice 0 → [example] file backend    (8 QPs)
        └── vDevice 1 → [example] raw backend     (8 QPs)
```

**NVMe example with two processes (illustrative):**
```
NVMe Controller (32 QPs total)
    │
    ├── kernel blk-mq: QPs 0–3  (fixed)
    │
    ├── Process A: 16 QPs granted
    │       ├── [example] file backend vDevice:  8 QPs
    │       └── [example] raw backend vDevice:   8 QPs
    │
    └── Process B: 12 QPs granted
            └── [example] file backend vDevice: 12 QPs
```

---

## Process Boundary & Design Decisions

### Process Boundary Map

There are **three** scopes, not two:

```
┌─────────────────────────────────────────────────────────────┐
│  SYSTEM-WIDE  (one per machine — lives in DeviceService)      │
│                                                               │
│  • Owner-side physical handle (nvm_ctrl_t) + chrdev/bind      │
│  • Global resource budget (hardware truth: kernel's QID pool) │
│  • Lease table  { allocation_id → PID + starttime + last_hb } │
│    (bookkeeping only — NOT a { pid → resource_range } ledger) │
│  • Dead-process reaper thread (cleans up leases; the kernel   │
│    fd-cascade reclaims the actual queues)                     │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  PER-PROCESS  (one per coordinator process)                   │
│                                                               │
│  • IDeviceManager          ← the Layer 2 facade               │
│  • IDeviceDriver plugins    ← one per device type             │
│  • IPhysicalDevice*         ← per-process VIEW of hardware     │
│  • ILeaseManager client     ← NullLeaseManager or DaemonClient │
│  • DeviceService gRPC stub  ← connection to daemon            │
│  • Heartbeat thread         ← calls ILeaseManager::heartbeat()│
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  PER-BACKEND  (one per backend instance inside a process)     │
│                                                               │
│  • IVirtualDevice*          ← resource slice for this backend │
└─────────────────────────────────────────────────────────────┘
```

### Object-by-Object Breakdown

| Object | Scope | Owner | Notes |
|--------|-------|-------|-------|
| Physical device handle | System | nvmeservice daemon | Owner-side `nvm_ctrl_t` + chrdev/bind, opened once. Daemon is the exclusive owner in multi-process mode; clients `attach_client`. Direct mode: this process owns it. |
| Global resource budget | System | Kernel (via daemon) | Hardware truth = the kernel's user-QID pool. The daemon does **not** keep a queue ledger; each client fd gets its own kernel-capped queue group. |
| Lease table | System | nvmeservice daemon | Maps `allocation_id → {PID, starttime, last_heartbeat}` (`Allocation` in `nvmeservice_state.h`). Bookkeeping for the reaper only — **not** a `{pid → resource_range}` allocator. No quota is decremented or refunded. |
| Heartbeat / reaper | System | nvmeservice daemon | Reaper thread sweeps leases whose PID died; the kernel `snvm_dev_release` fd-cascade reclaims the queues themselves. |
| `IDeviceManager` | Process | In-proc (Coordinator) | One per process. Manages this process's slice. |
| `IDeviceDriver` instances | Process | In-proc (IDeviceManager) | One per device type per process. In daemon mode, it talks to DeviceService; in direct mode, it opens the device itself. |
| `IPhysicalDevice*` | Process | In-proc (IDeviceDriver) | **Per-process view only.** `process_grant()` = this process's grant, not hardware total. `available_grant()` = grant minus already-allocated vDevices. |
| `ILeaseManager` | Process | In-proc (IDeviceDriver) | `NullLeaseManager` (direct mode) or `MockLeaseManager` (daemon mode today; a real daemon-mode implementation is a future item — see §3). |
| Heartbeat thread | Process | In-proc (IDeviceDriver) | Calls `ILeaseManager::heartbeat()` periodically. The driver owns this thread — not the manager, not the coordinator. |
| `IVirtualDevice*` | Backend | In-proc (IDeviceDriver) | One per backend. Lifetime: `open_vdevice()` → `close_vdevice()`. |

### Where the Boundary Sits

The cross-process / in-process boundary falls **inside `IDeviceDriver::enumerate()`**, not in `IDeviceManager`:

```
IDeviceManager::Open()
    │
    └── driver->enumerate(out_devices)
              │
              │← HERE: cross-process grant acquisition
              │
              │  [daemon mode]              [direct mode]
              │  contact DeviceService      open /dev/device directly
              │  "give me N resources"      own the full budget
              │  receive lease_id           no lease needed
              │  start heartbeat thread     NullLeaseManager
              │
              └── create IPhysicalDevice(grant_size)
                  // process_grant()   = grant_size
                  // available_grant() = grant_size (nothing allocated yet)
```

After `enumerate()` returns, `IDeviceManager` is purely in-process — it never touches the daemon again during steady-state. This is why DM has no hot-path role: the cross-process cost is paid once, at bring-up.

### Ownership Structure

```
system:   DeviceService daemon (process-local to the daemon)
              └── owns: physical handle, grant ledger, heartbeat tracker, reaper

process:  DeviceManagerImpl
              ├── owns: IDeviceDriver[] (one per type)
              └── each driver owns:
                      ├── ILeaseManager (NullLeaseManager or DaemonClient)
                      ├── heartbeat thread
                      └── IPhysicalDevice[] (per-process grant view)

backend:  IVirtualDevice* (carved from one IPhysicalDevice's grant)
```

The four decisions below follow directly from this structure.

### Decision 1: IPhysicalDevice Semantics

`IPhysicalDevice` represents **this process's view** of a physical device, not the hardware total.

- `process_grant()` — the resource units DeviceService granted this process (Level ① allocation). In direct mode, this equals the hardware total. In daemon mode, this is a subset.
- `available_grant()` — `process_grant()` minus the sum of all `IVirtualDevice::resource_count()` values allocated from this device.

**Example (NVMe):** Hardware has 32 queue pairs total. Kernel reserves 4. DeviceService grants process A 16 pairs and process B 12 pairs. Process A's `IPhysicalDevice::process_grant()` returns **16**, not 32.

### Decision 2: Heartbeat Thread Ownership

**`IDeviceDriver` owns the heartbeat thread**, not `IDeviceManager` or the Coordinator.

- In direct mode (`NullLeaseManager`): no heartbeat thread needed.
- In daemon mode: `DaemonNvmeDeviceDriver` starts `heartbeat_loop()` on a background thread and stops it in `shutdown()`.
- The thread calls `lease_mgr_->heartbeat(lease_id)` every **5 seconds** (sliced into 100 ms polls for responsive shutdown).
- The Coordinator never sees the lease protocol.

> The injected `lease_mgr_` is currently a `MockLeaseManager` (no real daemon-mode `ILeaseManager` exists yet — see §3). The authoritative cross-process keep-alive today is the `NvmeServiceClient`'s own gRPC bidi-stream heartbeat, separate from this thread.

### Decision 3: Direct vs. Daemon — Two IDeviceDriver Implementations

NVMe support is provided by **two separate `IDeviceDriver` implementations**:

| Driver | Mode | Physical Device Ownership | ILeaseManager | Status |
|--------|------|---------------------------|---------------|--------|
| `DirectNvmeDeviceDriver` | Single-process | Opens the device directly via libnvm | `NullLeaseManager` (no-op) | `enumerate()` is a mock stub; `d_qps` left null |
| `DaemonNvmeDeviceDriver` | Multi-process | Connects to the daemon via gRPC | injected `ILeaseManager*` (Mock today) | Real gRPC path when `TUTTI_NVMESERVICE_ENABLED` + `mock_mode=false`; fills `d_qps` |

Both return the same `NvmeVirtualDevice*` upward. The mode difference is encapsulated in the driver, not exposed to `IDeviceManager`. `DaemonNvmeDeviceDriver` also accepts a `mock_mode` flag that bypasses gRPC even when compiled in, for hardware-free unit tests.

### Decision 4: ILeaseManager Injection Per-Driver

Each `IDeviceDriver` receives its own `ILeaseManager*` at construction:

```cpp
DirectNvmeDeviceDriver(IAccelerator* accel, ILeaseManager* lease_mgr);
DaemonNvmeDeviceDriver(IAccelerator* accel, ILeaseManager* lease_mgr,
                       std::string daemon_addr, bool mock_mode = false);
```

The `ILeaseManager` lifetime is managed by the caller (typically the Coordinator). The driver does not own it.

> **Note on `IAccelerator* accel_`:** both NVMe drivers take an `IAccelerator*`, but it is **not wired into queue allocation**. `DirectNvmeDeviceDriver` only `assert()`-checks it is non-null in its constructor and otherwise never touches it; `DaemonNvmeDeviceDriver` stores it and never reads it at all. Actual GPU queue allocation (`daemon_nvme_alloc_queues()`) calls CUDA directly (`cudaMallocManaged`, `cudaSetDevice`, `cudaHostGetDevicePointer`), bypassing L1's HAL. The `accel` parameter is a placeholder for a future wiring where queue memory would flow through `IAccelerator::allocate()` (see the `// allocate NvmeQueueGroup via accel_->allocate();` TODO in `direct_nvme_device_driver.cpp`). The "L2 → L1 IAccelerator" edge in the §2 box therefore reflects intended, not current, coupling.

---

## 5. NVMe Concrete Types

This section documents the NVMe-specific types reached by downcasting; §6 records what is actually wired up today.

### `NvmeVirtualDevice` — the downcast target

**Concrete type:** `NvmeVirtualDevice : IVirtualDevice`  
**File:** `nvme/include/nvme_virtual_device.h`

`NvmeVirtualDevice` extends `IVirtualDevice` with the NVMe-specific resources a backend needs. Only NVMe backends should downcast to this type.

```cpp
struct NvmeVirtualDevice : IVirtualDevice {
    // IVirtualDevice implementation (phys_id, vdev_id, type, resource_count, caps)

    // NVMe-specific fields
    nvm_queue_t* d_qps = nullptr;   // GPU-resident queue slice: d_qps[0..queue_quota-1]
    uint32_t     queue_quota = 0;   // number of QPs in this slice

    // libnvm controller handle. Non-null only on the real gRPC path
    // (mock_mode=false); the daemon driver copies ctx.ctrl here so backends
    // can call nvm_dma_map_data_device() to register IO buffers for DMA.
    nvm_ctrl_t*  ctrl = nullptr;

    uint32_t namespace_id;
    uint32_t blk_size;
    uint32_t blk_size_log;
    size_t   max_data_size;   // MDTS in bytes (controller-reported limit)
};
```

**Device-side queue mechanics** (for use inside backend submit kernels):  
`nvme/include/queue_acquire_helper.cuh` provides `__device__` helpers:

| Helper | Description |
|--------|-------------|
| `acquire_queue(d_qps, n_qps)` | Pure hash selection: `(blockDim.x * 32 + threadIdx.x) % n_qps`. No atomic, no ticket — it just spreads warps deterministically across the slice. |
| `issue_nvme_cmd(qp, prp1, prp2, n_blocks, lba, opcode, out_cid)` | Compose SQE + ring doorbell |
| `poll(qp, cid)` | Busy-poll CQ for completion of command `cid` |

> The interface header `nvme/include/queue_acquire_helper.cuh` still carries a stale comment describing `acquire_queue` as "round-robin + atomic ticket"; the actual implementation in `nvme/src/queue_acquire_helper_impl.cuh` is the pure modulo hash above.

**Concrete NVMe files** (under `nvme/`, not part of the vendor-neutral API):

| File | Provides |
|------|----------|
| `nvme/include/nvme_physical_device.h` | `NvmePhysicalDevice : IPhysicalDevice` (ctrl handle, `queue_group` pointer, ns metadata) |
| `nvme/include/nvme_virtual_device.h` | `NvmeVirtualDevice : IVirtualDevice` (the downcast target above) |
| `nvme/include/nvme_queue_group.h` | `NvmeQueueGroup` — **dead/reserved**. A skeletal class with getters only; it is never instantiated or populated, and `NvmePhysicalDevice::queue_group` stays `nullptr` everywhere. The real GPU queue array is built by `daemon_nvme_alloc_queues()` and handed to `NvmeVirtualDevice::d_qps` directly, bypassing this class. |
| `nvme/include/direct_nvme_device_driver.h` | `DirectNvmeDeviceDriver : IDeviceDriver` |
| `nvme/include/daemon_nvme_device_driver.h` | `DaemonNvmeDeviceDriver : IDeviceDriver` |
| `nvme/include/queue_acquire_helper.cuh` | device-side queue mechanics (above) |
| `nvme/libnvm/` | libnvm integration |
| `nvme/nvmeservice/` | daemon + gRPC client library |

> The old `local_nvme_device.h` / `local_nvme_virtual.h` (the `LocalNvmeVirtualRegistry` allocator) were **removed** — the concrete `IPhysicalDevice` / driver split replaced them.

---

## 6. Current Implementation Status

This section records what is actually built today (v2.1), so consumers know which paths are load-bearing and which are stubs.

### Mock backend — complete

A first-class in-memory backend at `device_manager/mock/`, implementing every generic interface with no hardware, libnvm, or CUDA dependency:

| Type | Interface | File |
|------|-----------|------|
| `MockPhysicalDevice` | `IPhysicalDevice` | `mock/include/mock_physical_device.h` |
| `MockVirtualDevice` | `IVirtualDevice` | `mock/include/mock_virtual_device.h` |
| `MockDeviceDriver` | `IDeviceDriver` | `mock/include/mock_device_driver.h` |
| `MockLeaseManager` | `ILeaseManager` | `mock/include/mock_lease_manager.h` |

- `MockDeviceDriver::enumerate()` fabricates a configurable count of devices (ctor params: `type, base_id, device_count, grant_each, caps, lease_mgr`), each with the given grant and caps, and drives the lease at bring-up if a `lease_mgr` was supplied.
- `alloc_vdevice()` performs **real accounting**: checks `available_grant()`, calls `reserve()`, and constructs a `MockVirtualDevice` recording the quota as `resource_count`. `free_vdevice()` releases the grant back.
- `MockVirtualDevice` is **metadata-only** by design — it has no `d_qps`-style resource pointer, matching the fact that mock has no transport.

Because it depends only on `common/` headers, it is the proof that the vendor-neutral layer has no vendor coupling: a target linking `DeviceManagerImpl` + mock pulls in zero NVMe/CUDA symbols. It backs the Layer 2 vendor-neutral test suite (`tutti/tests/device_manager/`) and is suitable for CI on NVMe-less machines.

### NVMe backend — daemon path real, direct path stubbed

| Path | `enumerate()` | `alloc_vdevice()` → `d_qps` | Notes |
|------|---------------|------------------------------|-------|
| `DaemonNvmeDeviceDriver`, real (`TUTTI_NVMESERVICE_ENABLED` + `mock_mode=false`) | **Real** — gRPC `list_devices` + `connect` + `nvm_ctrl_attach_client` + `nvm_create_group` | **Real** — `daemon_nvme_alloc_queues()` fills `d_qps` when `ctx.ctrl != nullptr` | Verified on hardware; see the facade test below |
| `DaemonNvmeDeviceDriver`, `mock_mode=true` | Fabricates one 16-QP device, `ctrl` null | null | For hardware-free unit tests |
| `DaemonNvmeDeviceDriver`, built w/o gRPC | Fabricates one "MockGrant" device | null | `#else` fallback |
| `DirectNvmeDeviceDriver` | **Stub** — fabricates one 16-QP device (`// TODO: replace with real libnvm device discovery`) | **null** (`// left null in mock`) | Not yet wired to real libnvm |

Regardless of path, `namespace_id` / `blk_size` / `blk_size_log` / `max_data_size` on the returned `NvmeVirtualDevice` are copied from the owning `NvmePhysicalDevice`; only `d_qps` depends on a live `ctrl`.

**Tests:** `tutti/tests/device_manager/nvme/` — `daemon_driver_test.cpp` (driver unit + real-HW tiers) and `device_manager_real_hw_test.cpp` (the `IDeviceManager` facade path: `open_vdevice` → downcast → resource-field access, both mock and real-HW tiers).

### RDMA / GDS — not implemented

`RdmaVirtualDevice` / `GdsVirtualDevice` are reserved `DeviceType` values only. When added, each implements the same `IDeviceDriver` + `I{Physical,Virtual}Device` triad and exposes its own resource handles (RDMA queue-pair / memory-region slots) in place of `d_qps`.

---

## 7. Migration from v1 API

The v1 API (`IDeviceRegistry` + `IVirtualNvme` + `VDevice`) is superseded by this design. Key mapping:

| v1 | v2 |
|----|----|
| `Device` struct | `IPhysicalDevice*` (interface, not struct) |
| `IDeviceRegistry` | Part of `IDeviceManager` (physical registry methods) |
| `IVirtualNvme` | Part of `IDeviceManager` (virtual device methods) |
| `VDevice` struct (NVMe-coupled) | `IVirtualDevice*` base + `NvmeVirtualDevice*` subtype |
| `open_vdevice(phys_id, quota)` | `IDeviceManager::open_vdevice(phys_id, resource_quota)` |
| `available_queues(phys_id)` | `IDeviceManager::available_resources(phys_id)` |

The old headers (`device.h`, `device_registry.h`, `vdevice.h`, `virtual_nvme.h`, `lease_manager.h`, `local_nvme_device.h`, `local_nvme_virtual.h`) have been **deleted** from `tutti/device_manager/`. Any code still including them (e.g. parts of `tutti/backends/nvme/` that took a flat `VDevice*`) must migrate to `IVirtualDevice*` + a `static_cast<NvmeVirtualDevice*>` after checking `type()`.

> The daemon is still named **nvmeservice** in the tree (`nvme/nvmeservice/`). An earlier draft of this doc called it "DeviceService"; that rename has not happened in code.

---

## 8. Vendor-Neutral Boundary

This document covers the `common/` API. The vendor-neutral surface:

| File | Provides |
|------|----------|
| `include/common/device_type.h` | `DeviceType` enum |
| `include/common/iphysical_device.h` | `IPhysicalDevice` interface |
| `include/common/ivirtual_device.h` | `IVirtualDevice` interface |
| `include/common/idevice_driver.h` | `IDeviceDriver` plugin interface |
| `include/common/idevice_manager.h` | `IDeviceManager` facade |
| `include/common/ilease_manager.h` | `ILeaseManager` interface |
| `include/common/null_lease_manager.h` | `NullLeaseManager` (direct-mode no-op) |
| `include/common/device_manager_impl.h` | `DeviceManagerImpl` + `create_device_manager()` factory |

Nothing above Layer 2 includes libnvm. The `nvm_types.h` include in `NvmeVirtualDevice` is the only cross-boundary type, and it is confined to NVMe backends that explicitly downcast.

---

## 9. Implementation Status

The status vocabulary is: **Complete** (built and behaving), **Stub** (present but fabricates/returns placeholder data), **Not implemented** (only a reserved name/enum), **Dead-reserved** (a type exists but is never instantiated), **Intentionally-unimplemented** (deliberately absent by design). §6 gives the narrative; this table is the checklist.

| Component | Status | Tested |
|-----------|--------|--------|
| Vendor-neutral core (`DeviceManagerImpl` + `create_device_manager` factory) | Complete | Yes — `tests/device_manager/device_manager_test.cpp` (lifecycle, registry, alloc, teardown, idempotent Open) |
| Mock backend (`MockDeviceDriver` / `MockPhysicalDevice` / `MockVirtualDevice`, real grant accounting) | Complete | Yes — `device_manager_test.cpp`, `physical_device_test.cpp` |
| `NullLeaseManager` (direct-mode no-op) | Complete | Yes — `lease_manager_test.cpp` |
| `MockLeaseManager` (heartbeat/release counting for tests) | Complete | Yes — `lease_manager_test.cpp` (injection, per-driver ownership) |
| NVMe daemon driver — mock-grant path (`mock_mode=true`) + `#else` fallback | Complete | Yes — `tests/device_manager/nvme/daemon_driver_test.cpp` (unit tier) |
| NVMe daemon driver — real gRPC/libnvm path + `daemon_nvme_alloc_queues` (fills `d_qps` when `ctrl != null`) | Complete | Yes, on hardware — gated `TUTTI_NVME_REAL_HW=1` + `TUTTI_NVMESERVICE_ENABLED`; `daemon_driver_test.cpp` + `device_manager_real_hw_test.cpp` |
| `nvmeservice` daemon (owner B3 bring-up, ACL, symlinks, lease reaper) | Complete | Yes — exercised by the real-HW tier |
| `IDeviceManager` facade over the daemon driver (open_vdevice → downcast → field access) | Complete | Yes — `device_manager_real_hw_test.cpp` (unit + real-HW tiers) |
| NVMe **direct** driver (`DirectNvmeDeviceDriver`) | Stub | Partially — enumerate() fabricates one 16-QP device; `d_qps` stays null. No real libnvm discovery. |
| `NvmeQueueGroup` | Dead-reserved | No — never instantiated; `NvmePhysicalDevice::queue_group` stays null |
| Real daemon-mode `ILeaseManager` (e.g. `DeviceServiceLeaseManager`) | Not implemented | N/A — injected mgr is always a `MockLeaseManager`; the authoritative keep-alive is `NvmeServiceClient`'s own gRPC bidi-stream heartbeat |
| Daemon-side queue-quota ledger (`{pid → resource_range}`) | Intentionally-unimplemented | N/A — by design; the kernel B3 per-fd queue group + fd-cascade own the queue budget |
| `IAccelerator` wiring into queue allocation | Not implemented | N/A — `accel_` is assert-checked (direct) / stored-unused (daemon); queue alloc calls CUDA directly |
| RDMA / GDS drivers (`RdmaVirtualDevice` / `GdsVirtualDevice`) | Not implemented | N/A — reserved `DeviceType` values only |

---

## 10. Known Issues & Gaps

The items below are what a future implementer must fix; each names the symptom and the code location.

- **`DirectNvmeDeviceDriver::enumerate()` is a stub.** It fabricates one device (`"0000:17:00.0"`, 16 QPs) and never opens real hardware; `alloc_vdevice()` leaves `d_qps == nullptr` (`// left null in mock`). Any backend that takes the direct path and dereferences `d_qps` will fault. Location: `nvme/src/direct_nvme_device_driver.cpp` (`enumerate`, `alloc_vdevice`).

- **`NvmeQueueGroup` is dead code.** The class in `nvme/include/nvme_queue_group.h` is never instantiated and `NvmePhysicalDevice::queue_group` is never set; it advertises a GPU-resident queue array that does not exist. The real queue array comes from `daemon_nvme_alloc_queues()` straight into `NvmeVirtualDevice::d_qps`. Either wire the direct path through this class or delete it. Location: `nvme/include/nvme_queue_group.h`, `nvme/include/nvme_physical_device.h:41`.

- **Stale comment on `acquire_queue`.** The public header `nvme/include/queue_acquire_helper.cuh` documents "round-robin + atomic ticket," but `nvme/src/queue_acquire_helper_impl.cuh` is a pure `(blockDim.x*32 + threadIdx.x) % n_qps` hash with no atomic and no fairness guarantee. Two warps hashing to the same queue contend inside libnvm; there is no ticket to serialize them. Fix the comment (or implement the ticket if fairness is actually wanted). Location: `nvme/include/queue_acquire_helper.cuh:11-13,24-28`.

- **`IAccelerator* accel_` is dead weight.** Both NVMe driver constructors require it (`DirectNvmeDeviceDriver` even `assert()`s it non-null), but neither uses it for allocation — `daemon_nvme_alloc_queues()` calls CUDA directly. The L1 HAL is bypassed, so any accelerator-portability guarantees L1 is meant to provide do not hold for L2 queue memory. Location: `nvme/src/direct_nvme_device_driver.cpp:22-23`, `nvme/src/daemon_nvme_device_driver.cpp:68`, `nvme/src/daemon_nvme_queue_alloc.cu`.

- **No real daemon-mode `ILeaseManager`.** In daemon mode the injected `lease_mgr_` is a `MockLeaseManager` even in the real-HW tests, and `DaemonNvmeDeviceDriver::heartbeat_loop()` beats against it purely for higher-level tracking. The load-bearing cross-process keep-alive is `NvmeServiceClient`'s own gRPC bidi-stream heartbeat, which is a separate mechanism. A production `ILeaseManager` that actually talks to the daemon does not exist. Location: `nvme/src/daemon_nvme_device_driver.cpp:233-253`.

- **Partial-`Open()` failure can strand a lease for leak-on-failure drivers.** `DeviceManagerImpl::Open()` rolls back by clearing `devices_` but does **not** call `shutdown()` on drivers that already enumerated. NVMe drivers self-clean via their destructors, but `MockDeviceDriver`'s dtor does not release its lease/heartbeat, so a mixed driver set where a later driver fails to enumerate leaves an earlier mock driver's lease held until the manager is destroyed. Location: `src/common/device_manager_impl.cpp:37-64`, `mock/src/mock_device_driver.cpp` (no dtor override).

- **`DirectNvmeDeviceDriver::shutdown()` releases an empty lease id.** It calls `lease_mgr_->release_lease("")` unconditionally; harmless with `NullLeaseManager` but meaningless with any real lease manager since no lease id was ever acquired. Location: `nvme/src/direct_nvme_device_driver.cpp:142-149`.

- **Documented "file backend" / "raw device backend" consumers do not exist.** No Layer 3 backend consumes L2 today; the split is a planned L3 shape. The only current callers of `open_vdevice()` are the mock suite and the NVMe real-HW tests. Location: `tests/device_manager/**`.

---

## Related Documents

- [Architecture Overview (L0–5)](architecture-overview.md)
- [Layer 1: Accelerator HAL](layer1-accelerator-hal.md) — peer foundation layer
- [Layer 3: Backends](layer3-backends.md) — consumer of this API
