#pragma once
#include <vector>
#include <memory>
#include "common/idevice_driver.h"
#include "common/ilease_manager.h"
#include "nvme_physical_device.h"

namespace tutti {

class IAccelerator;

/**
 * DirectNvmeDeviceDriver -- IDeviceDriver for direct NVMe (single-process mode).
 *
 * Opens /dev/libnvm* directly, owns the physical controller exclusively.
 * No cross-process arbitration; uses NullLeaseManager (no heartbeat thread).
 */
class DirectNvmeDeviceDriver : public IDeviceDriver {
public:
    DirectNvmeDeviceDriver(IAccelerator* accel, ILeaseManager* lease_mgr);
    ~DirectNvmeDeviceDriver() override;

    DeviceType type() const override { return DeviceType::LOCAL_NVME; }
    int enumerate(std::vector<IPhysicalDevice*>& out_devices) override;
    IVirtualDevice* alloc_vdevice(IPhysicalDevice* dev, uint32_t resource_quota,
                                    std::string* error) override;
    void free_vdevice(IVirtualDevice* vdev) override;
    void shutdown() override;

private:
    IAccelerator* accel_;
    ILeaseManager* lease_mgr_;
    std::vector<std::unique_ptr<NvmePhysicalDevice>> phys_devices_;
    std::vector<std::unique_ptr<IVirtualDevice>> vdevices_;
};

} // namespace tutti
