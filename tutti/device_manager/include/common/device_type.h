#pragma once

namespace tutti {

/**
 * DeviceType -- storage device family
 *
 * Identifies the device kind for a physical device or virtual device.
 * Defined here (not in device.h) so IPhysicalDevice and IVirtualDevice can
 * include it without pulling in the full device descriptor.
 */
enum class DeviceType {
    LOCAL_NVME = 0,
    RDMA       = 1,
    GDS        = 2,
};

} // namespace tutti
