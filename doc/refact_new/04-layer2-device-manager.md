# Layer 2: Device Manager

> Local-NVMe virtualization base: controller bring-up, queue-pair budget, vDevice allocation.  
> Sits **below** backends as a peer to the Accelerator HAL.

## Architecture Change: The DM Inversion

**v0.1 (current)**: Device Manager sits **above** backends but includes libnvm (backwards dependency).

**New design**: Device Manager is the **local-NVMe virtualization base** that sits **below** backends:
- Owns physical controller bring-up and queue-pair budget
- Hands each backend a **vDevice** (queue slice + namespace view + caps)
- Backends pull queues *down* from it
- Nothing above DM includes libnvm headers

This fixes the backwards dependency and enforces the "backends don't own their queues" invariant.

---

## Four Sub-Components

Device Manager consists of four pieces:

| Component | Status | Purpose |
|---|---|---|
| **IDeviceRegistry** | EXISTS | Physical controller bring-up and enumeration (direct / service-client drivers) |
| **IVirtualNvme** | **NEW** | Level-2 allocator: split this process's QP grant into per-backend vDevices |
| **ILeaseManager** | EXISTS | Cross-process heartbeat + release (Level-1 arbiter) |
| **Device-side QueuePair contract** | EXISTS but **wrong location** | `__device__` helpers: `acquire_queue`, `issue_nvme_cmd`, `poll` |

---

## 2.1 IDeviceRegistry (Exists, No Changes)

`device_manager/include/device_registry.h` — responsible for physical controller bring-up.

```cpp
class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;
    
    virtual bool Open() = 0;
    virtual void Close() = 0;
    
    virtual int device_count() const = 0;
    virtual const Device* device_at(int index) const = 0;
    virtual const Device* find_by_id(uint32_t device_id) const = 0;
    virtual std::vector<const Device*> list() const = 0;
};
```

**Two concrete drivers** (unchanged):
1. `LocalNvmeDirectRegistry` — in-process controller ownership via `nvm_controller_init_b3`
2. `NvmeServiceBackedRegistry` — service-client mode via `nvm_ctrl_attach_client` + gRPC daemon

**No action required** beyond ensuring it doesn't transitively include `cuda_runtime.h`.

---

## 2.2 IVirtualNvme (NEW — Must Be Created)

This is the **Level-2 allocator** that splits a process's queue-pair grant into per-backend slices.

### Two-Level Allocation Model

```
        Physical NVMe controller (budget = N queue pairs)
                        │
   ┌────────────────────┴────────────────────┐   ← Level ①: cross-process
   │   DM arbiter (daemon = NVMeService)      │      physical budget → per-process grant
   │   ledger + heartbeat + dead-proc reaper  │
   └──────┬───────────────────────────┬───────┘
          │ grant(a pairs)            │ grant(b pairs)   a + b + kernel ≤ N
          ▼                           ▼
   Coordinator P1 · DM(in-proc)   Coordinator P2 · DM(in-proc)   ← Level ②: in-process
     ├ vDevice → file backend       └ vDevice → ...                grant → per-backend vDevice
     └ vDevice → raw backend
```

- **Level ① (cross-process, ILeaseManager)** — today's NVMeService daemon; guarantees two Coordinators never collide
- **Level ② (in-process, IVirtualNvme)** — split this process's grant into vDevices for its backends

### VDevice Struct

```cpp
// device_manager/include/vdevice.h
#pragma once
#include <cstdint>
#include <cstddef>

struct nvm_queue_t;  // forward-decl (defined in libnvm)

namespace tutti {

// A virtual storage device: one backend's slice of a physical NVMe controller.
struct VDevice {
    // Identity
    int32_t  phys_device_id;   // which LocalNvmeDevice this slices
    uint32_t vdev_id;          // dense index within IVirtualNvme (0, 1, 2, ...)
    
    // Level-2 allocation: the slice of d_qps[] this backend owns
    nvm_queue_t* d_qps;        // GPU-resident pointer into NvmeQueueGroup::d_qps_[slice_start]
    uint32_t     queue_quota;  // number of QPs in this slice
    
    // Namespace view (from LocalNvmeDevice)
    uint32_t namespace_id;
    uint32_t blk_size;
    uint32_t blk_size_log;
    size_t   max_data_size;    // MDTS in bytes (controller-reported limit)
    
    // Capabilities (bitmask)
    uint32_t caps;
    // bit 0: GPUDIRECT_CAPABLE (NvmeQueueGroup exists)
    // bit 1-31: reserved
};

} // namespace tutti
```

### IVirtualNvme Interface

```cpp
// device_manager/include/virtual_nvme.h
#pragma once
#include <cstdint>
#include <string>
#include "vdevice.h"

namespace tutti {

// Level-2 allocator: carves vDevices from the in-process QP grant.
class IVirtualNvme {
public:
    virtual ~IVirtualNvme() = default;
    
    // Carve a VDevice from phys_device_id's QP pool.
    // quota: number of QPs to reserve for this backend.
    // Returns nullptr if:
    //   - phys_id unknown
    //   - quota == 0
    //   - insufficient QPs remain in the pool
    // Error message written to *error if provided.
    virtual VDevice* open_vdevice(
        int32_t phys_id,
        uint32_t quota,
        std::string* error = nullptr) = 0;
    
    // Return the QP slice back to the pool. No-op on nullptr.
    // The VDevice* becomes invalid after this call.
    virtual void close_vdevice(VDevice* vdev) = 0;
    
    // Remaining unallocated QPs for a given physical device.
    virtual uint32_t available_queues(int32_t phys_id) const = 0;
    
    // Capability bitmask of the underlying physical device.
    virtual uint32_t caps(int32_t phys_id) const = 0;
};

} // namespace tutti
```

### Concrete Implementation: LocalNvmeVirtualRegistry

Create `device_manager/src/local_nvme_virtual.{h,cpp}`:

```cpp
// device_manager/src/local_nvme_virtual.h
#pragma once
#include "device_manager/include/virtual_nvme.h"
#include <vector>
#include <mutex>

namespace tutti {

class IDeviceRegistry;  // forward-decl

// Concrete Level-2 allocator for local NVMe devices.
class LocalNvmeVirtualRegistry : public IVirtualNvme {
public:
    // Does NOT own registry — caller keeps it alive.
    explicit LocalNvmeVirtualRegistry(IDeviceRegistry* registry);
    ~LocalNvmeVirtualRegistry() override;
    
    VDevice* open_vdevice(int32_t phys_id, uint32_t quota, std::string* error) override;
    void close_vdevice(VDevice* vdev) override;
    uint32_t available_queues(int32_t phys_id) const override;
    uint32_t caps(int32_t phys_id) const override;

private:
    struct PerDeviceState {
        int32_t phys_id;
        nvm_queue_t* d_qps_base;  // from LocalNvmeDevice::queue_group->d_qps()
        uint32_t total_qps;
        std::vector<bool> allocated;  // free-list: allocated[i] = true if QP i is in use
    };
    
    IDeviceRegistry* registry_;  // not owned
    std::vector<PerDeviceState> devices_;
    std::vector<VDevice> vdevices_;  // storage for returned VDevice*
    mutable std::mutex mutex_;
};

} // namespace tutti
```

**Allocation strategy (v0.1)**: Contiguous-first-fit.
- Scan `allocated[]` for the first contiguous run of `quota` false entries
- Mark them true, return a VDevice pointing to `d_qps_base + start_index`
- No queue migration, no NUMA-aware placement (deferred to post-v0.1)

---

## 2.3 ILeaseManager (Exists, No Changes)

`device_manager/include/lease_manager.h` — cross-process heartbeat protocol.

```cpp
class ILeaseManager {
public:
    virtual ~ILeaseManager() = default;
    
    virtual bool heartbeat(const std::string& lease_id) = 0;
    virtual bool release_lease(const std::string& lease_id) = 0;
    virtual bool has_lease(const std::string& lease_id) const = 0;
};
```

**Concrete implementation**: NVMeService daemon (Level-1 arbiter).

**No changes needed** — it's already the right abstraction.

---

## 2.4 Device-Side QueuePair Contract

### Current Location: WRONG

`nvme_storage/include/queue_acquire_helper.cuh` contains:
- `acquire_queue(nvm_queue_t* d_qps, uint32_t n_qps)` — `__device__` function
- `issue_nvme_cmd(nvm_queue_t* qp, ...)` — `__device__` function
- `poll(nvm_queue_t* qp, uint16_t cid)` — `__device__` function

These operate on `nvm_queue_t*` (libnvm struct), which is the device-side QP layout owned by `NvmeQueueGroup` and now exposed via `VDevice::d_qps`.

**Problem**: Keeping them in `nvme_storage/` creates a cross-backend dependency. A future raw backend or RDMA-fallback-to-NVMe backend would also need these helpers.

**Solution**: Move to Device Manager.

### Action Required

**Move**: `nvme_storage/include/queue_acquire_helper.cuh` → `device_manager/include/queue_acquire_helper.cuh`

**Update references**: Only one file includes it:
- `nvme_storage/include/nvme_storage_device.cuh:8` — change `#include "queue_acquire_helper.cuh"` to `#include "device_manager/include/queue_acquire_helper.cuh"`

**Do NOT move** `nvme_storage_device.cuh` itself — it depends on `NvmeFileDeviceHandle` (an nvme_storage-layer type). Only the transport-agnostic queue helpers move.

### Revised API (After Move)

```cpp
// device_manager/include/queue_acquire_helper.cuh
#pragma once
#include "tutti/abstraction/accel.h"  // TUTTI_DEVICE, TUTTI_FORCEINLINE
#include <cstdint>

struct nvm_queue_t;  // forward-decl (defined in libnvm queue.h)

namespace tutti {

// Acquire one queue from the d_qps[] array.
// Returns the index of the acquired queue (0..n_qps-1).
// Uses round-robin + atomic ticket to avoid contention.
TUTTI_DEVICE TUTTI_FORCEINLINE
uint32_t acquire_queue(nvm_queue_t* d_qps, uint32_t n_qps);

// Submit one NVMe command to the SQ ring and ring the doorbell.
// Blocking: waits for a free SQ slot if full.
// out_cid: receives the command ID for polling.
TUTTI_DEVICE TUTTI_FORCEINLINE
void issue_nvme_cmd(
    nvm_queue_t* qp,
    uint64_t prp1,
    uint64_t prp2,
    uint32_t n_blocks,
    uint64_t lba,
    uint8_t opcode,
    uint16_t* out_cid);

// Poll the CQ ring for completion of command `cid`.
// Blocking: spins until the command completes.
TUTTI_DEVICE TUTTI_FORCEINLINE
void poll(nvm_queue_t* qp, uint16_t cid);

} // namespace tutti
```

**Implementation** stays in `device_manager/src/queue_acquire_helper_impl.cuh` (inline device code).

---

## QueuePair Memory Layout (Contract)

The `nvm_queue_t` struct (defined in libnvm `include/queue.h`) is the device-side contract:

```cpp
struct nvm_queue_t {
    // SQ ring
    volatile uint32_t* sq_ring;     // submission queue entries
    volatile uint32_t* sq_doorbell; // doorbell register (mapped BAR0)
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t sq_depth;
    
    // CQ ring
    volatile uint32_t* cq_ring;     // completion queue entries
    volatile uint32_t* cq_doorbell;
    uint32_t cq_head;
    uint32_t cq_phase;
    uint32_t cq_depth;
    
    // Namespace metadata
    uint32_t ns_id;
    uint32_t blk_size;
    
    // Ticket lock for multi-thread acquire
    TUTTI_ATOMIC_U32_SYS sq_ticket;
    TUTTI_ATOMIC_U32_SYS sq_serving;
};
```

**Backends must not manipulate this struct directly** — they call the three `__device__` helpers above.

---

## Integration with Backends

### v0.1 Flow (NVMe backend)

```cpp
// backends/local_nvme/local_nvme_backend.cpp

class LocalNvmeBackend : public IBackendProvider {
public:
    bool initialize(VDevice* vdev) override {
        vdev_ = vdev;  // Hold the vDevice
        // No need to call acquire_queue() — we already have vdev->d_qps
        return true;
    }
    
    void launch_batch_gpu_stream(AccelStream stream, ...) override {
        // Device kernel will call:
        //   uint32_t qid = acquire_queue(vdev_->d_qps, vdev_->queue_quota);
        //   issue_nvme_cmd(vdev_->d_qps + qid, ...);
        //   poll(vdev_->d_qps + qid, cid);
        my_submit_kernel<<<grid, block, 0, (cudaStream_t)stream>>>(vdev_->d_qps, ...);
    }

private:
    VDevice* vdev_ = nullptr;
};
```

### Non-NVMe Backends (GDS, RDMA)

```cpp
bool GdsBackend::initialize(VDevice* vdev) override {
    // vdev is nullptr for non-NVMe backends — ignore it
    if (vdev != nullptr) {
        // Log warning or error: GDS doesn't use NVMe queues
    }
    // Initialize cuFile handles instead
    return true;
}
```

---

## Migration Checklist

### Files to Create

```
device_manager/include/vdevice.h                    (NEW)
device_manager/include/virtual_nvme.h               (NEW)
device_manager/src/local_nvme_virtual.h             (NEW)
device_manager/src/local_nvme_virtual.cpp           (NEW)
```

### Files to Move

```
nvme_storage/include/queue_acquire_helper.cuh
  → device_manager/include/queue_acquire_helper.cuh
```

### Files to Modify

```
nvme_storage/include/nvme_storage_device.cuh:8
  - #include "queue_acquire_helper.cuh"
  + #include "device_manager/include/queue_acquire_helper.cuh"
```

### Files Unchanged

```
device_manager/include/device_registry.h            (no changes)
device_manager/include/lease_manager.h              (no changes)
device_manager/include/local_nvme_device.h          (no changes, but gains VDevice usage)
device_manager/include/nvme_queue_group.h           (no changes)
```

---

## Validation Criteria

✅ Device Manager is correct when:

1. **IVirtualNvme exists** — `open_vdevice` / `close_vdevice` work
2. **VDevice contains d_qps slice** — pointer arithmetic `vdev->d_qps + i` reaches valid QP
3. **No hot-path calls to DM** — backends call DM only at `initialize()`, never during IO
4. **Queue helpers in right place** — `queue_acquire_helper.cuh` is under `device_manager/`
5. **No libnvm leaks above DM** — backends include `vdevice.h` but not `nvm_types.h` directly
6. **Level-1 + Level-2 split works** — NVMeService daemon (Level-1) + LocalNvmeVirtualRegistry (Level-2) both operational

---

## Open Questions

See `10-open-questions.md` for:
- **Q6**: VDevice granularity — static bootstrap allocation vs. dynamic borrow/return?  
  **Current decision**: Static for v0.1 (contiguous-first-fit at startup).
- **Q7**: Should `IVirtualNvme` be transport-neutral or NVMe-specific?  
  **Current decision**: NVMe-specific (name explicitly says "Nvme"). Future RDMA/GDS backends don't use it.
