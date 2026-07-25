#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "common/device_type.h"
#include "common/iphysical_device.h"
#include "common/ivirtual_device.h"

namespace tutti {

/**
 * IDeviceDriver -- plugin factory for one storage backend family.
 *
 * Each backend (NVMe, RDMA, GDS, …) provides one IDeviceDriver registered
 * with IDeviceManager at startup. The manager calls enumerate() once during
 * Open() to discover physical devices, then delegates alloc/free_vdevice()
 * for all subsequent virtual-device lifecycle calls.
 *
 * Driver owns heartbeat thread lifecycle (see Decision 2).
 *
 * Ownership rules:
 *   - IPhysicalDevice* returned by enumerate() is owned by the driver;
 *     lifetime is until the driver is destroyed.
 *   - IVirtualDevice* returned by alloc_vdevice() is owned by the driver;
 *     lifetime is until free_vdevice() is called.
 */
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    // Which backend family this driver handles.
    virtual DeviceType type() const = 0;

    // Discover all physical devices of this type.
    // Appends to out_devices; returns the number of devices appended.
    virtual int enumerate(std::vector<IPhysicalDevice*>& out_devices) = 0;

    // Allocate a virtual device with resource_quota units from dev.
    // dev must have been returned by this driver's enumerate().
    // Returns nullptr on failure; error message written to *error if non-null.
    virtual IVirtualDevice* alloc_vdevice(
        IPhysicalDevice* dev,
        uint32_t         resource_quota,
        std::string*     error = nullptr) = 0;

    // Return a virtual device to the pool. No-op on nullptr.
    // vdev must have been returned by this driver's alloc_vdevice().
    virtual void free_vdevice(IVirtualDevice* vdev) = 0;

    // Shutdown: called by IDeviceManager::Close() to let the driver clean up.
    // Driver responsibilities:
    //   - Stop heartbeat thread (if daemon mode)
    //   - Call lease_mgr_->release_lease(lease_id)
    //   - Free all IPhysicalDevice instances created during enumerate()
    // After shutdown(), the driver must not be used again.
    virtual void shutdown() = 0;
};

} // namespace tutti
