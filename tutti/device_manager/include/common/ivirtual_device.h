#pragma once
#include <cstdint>
#include "common/device_type.h"

namespace tutti {

/**
 * IVirtualDevice -- one backend's slice of a physical device.
 *
 * Obtained from IDeviceManager::open_vdevice() and returned by
 * IDeviceManager::close_vdevice(). The pointer is valid between those calls.
 *
 * To access transport-specific fields (NVMe queue pointers, RDMA memory
 * regions, etc.) cast to the concrete subtype after checking type():
 *
 *   if (vdev->type() == DeviceType::LOCAL_NVME)
 *       auto* ndev = static_cast<NvmeVirtualDevice*>(vdev);
 *
 * The generic part carries only identity, resource count, and capabilities.
 * Transport details remain in the subtype to keep this interface clean.
 */
class IVirtualDevice {
public:
    virtual ~IVirtualDevice() = default;

    // Identity
    virtual int32_t     phys_id()        const = 0;  // which IPhysicalDevice this slices
    virtual uint32_t    vdev_id()        const = 0;  // dense index within one IDeviceManager
    virtual DeviceType type()           const = 0;  // transport family (use for safe downcast)

    // Resources
    virtual uint32_t    resource_count() const = 0;  // units granted to this virtual device
    virtual uint32_t    caps()           const = 0;  // capability bitmask (same encoding as IPhysicalDevice::caps)
};

} // namespace tutti
