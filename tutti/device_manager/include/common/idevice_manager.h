#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "common/device_type.h"
#include "common/iphysical_device.h"
#include "common/ivirtual_device.h"

namespace tutti {

class IDeviceDriver;  // forward-decl

/**
 * IDeviceManager -- the Layer 2 facade.
 *
 * Provides two services to its callers (NVMe-family backends + Coordinator):
 *
 *  1. Physical device registry (Open/Close + enumerate)
 *  2. Virtual device allocation (open_vdevice / close_vdevice)
 *
 * Usage pattern (backend):
 *   mgr->Open();
 *   IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME);
 *   IVirtualDevice*  vdev = mgr->open_vdevice(phys->id(), quota);
 *   // cast vdev to NvmeVirtualDevice* for NVMe-specific fields
 *   ...at teardown:
 *   mgr->close_vdevice(vdev);
 *   mgr->Close();
 *
 * Hot-path note: steady-state IO never calls IDeviceManager. Backends
 * interact directly with the resources inside their IVirtualDevice.
 */
class IDeviceManager {
public:
    virtual ~IDeviceManager() = default;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    // Open: probe all registered drivers, build the physical device registry.
    // Must be called before any other method. Returns false on failure.
    virtual bool Open()  = 0;

    // Close: release all virtual devices, tear down all physical devices.
    virtual void Close() = 0;

    // ── Physical device registry ─────────────────────────────────────────────

    virtual int                          device_count()                            const = 0;
    virtual IPhysicalDevice*             device_at(int index)                      const = 0;
    virtual IPhysicalDevice*             find_by_id(int32_t id)                    const = 0;
    virtual IPhysicalDevice*             find_by_type(DeviceType t, int ordinal=0) const = 0;
    virtual std::vector<IPhysicalDevice*> list()                                   const = 0;

    // ── Virtual device allocation ─────────────────────────────────────────────

    // Carve resource_quota units from phys_id and return a virtual device.
    // Returns nullptr on failure; error message written to *error if non-null.
    // Failure conditions: unknown phys_id, quota == 0, pool exhausted.
    virtual IVirtualDevice* open_vdevice(
        int32_t      phys_id,
        uint32_t     resource_quota,
        std::string* error = nullptr) = 0;

    // Return a virtual device to its physical device's pool. No-op on nullptr.
    virtual void close_vdevice(IVirtualDevice* vdev) = 0;

    // Query unallocated resource units for a physical device (0 if unknown id).
    virtual uint32_t available_resources(int32_t phys_id) const = 0;

    // Capability bitmask for a physical device (0 if unknown id).
    virtual uint32_t caps(int32_t phys_id)               const = 0;
};

} // namespace tutti
