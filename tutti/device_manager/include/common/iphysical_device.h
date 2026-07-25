#pragma once
#include <cstdint>
#include <string_view>
#include "common/device_type.h"

namespace tutti {

/**
 * IPhysicalDevice -- abstract descriptor for one physical storage controller.
 *
 * Layer 2 owns one IPhysicalDevice per enumerated controller. Callers above
 * this layer receive const pointers valid until IDeviceManager::Close().
 * They must never delete or downcast without checking type() first.
 *
 * Per-process view only; does not reflect cross-process hardware total.
 *
 * Resource units are backend-defined:
 *   LOCAL_NVME  → NVMe queue pairs
 *   RDMA        → queue-pair / memory-region slots (driver-defined)
 *   GDS         → (driver-defined)
 */
class IPhysicalDevice {
public:
    virtual ~IPhysicalDevice() = default;

    // Identity
    virtual int32_t          id()           const = 0;
    virtual DeviceType      type()         const = 0;
    virtual std::string_view pci_addr()     const = 0;
    virtual std::string_view display_name() const = 0;

    // Resources (units are backend-defined; see class doc)
    // Process grant: resource units DeviceService granted this process (Level ① allocation).
    // In direct mode = hardware total; in daemon mode = this process's subset.
    // For NVMe: queue pairs. For RDMA/GDS: driver-defined.
    virtual uint32_t process_grant()     const = 0;

    // Available grant: process_grant() minus sum of allocated IVirtualDevice resource_count().
    // This is what remains for new open_vdevice() calls within this process.
    virtual uint32_t available_grant()   const = 0;

    // Capability bitmask (bit 0: GPUDIRECT_CAPABLE; bits 1-31 reserved)
    virtual uint32_t caps() const = 0;
};

} // namespace tutti
