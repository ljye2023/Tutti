#pragma once
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include "common/idevice_driver.h"
#include "common/ilease_manager.h"
#include "nvme_physical_device.h"

namespace tutti {

class IAccelerator;

/**
 * DaemonNvmeDeviceDriver -- IDeviceDriver for daemon NVMe (multi-process mode).
 *
 * Connects to DeviceService daemon via gRPC, requests cross-process grant.
 * Owns a heartbeat thread that calls lease_mgr_->heartbeat() every 5 seconds.
 */
class DaemonNvmeDeviceDriver : public IDeviceDriver {
public:
    DaemonNvmeDeviceDriver(IAccelerator* accel, ILeaseManager* lease_mgr,
                            std::string daemon_addr);
    ~DaemonNvmeDeviceDriver() override;

    DeviceType type() const override { return DeviceType::LOCAL_NVME; }
    int enumerate(std::vector<IPhysicalDevice*>& out_devices) override;
    IVirtualDevice* alloc_vdevice(IPhysicalDevice* dev, uint32_t resource_quota,
                                    std::string* error) override;
    void free_vdevice(IVirtualDevice* vdev) override;
    void shutdown() override;

private:
    void heartbeat_loop();

    IAccelerator* accel_;
    ILeaseManager* lease_mgr_;
    std::string daemon_addr_;
    std::string lease_id_;
    std::vector<std::unique_ptr<NvmePhysicalDevice>> phys_devices_;
    std::vector<std::unique_ptr<IVirtualDevice>> vdevices_;

    std::thread heartbeat_thread_;
    std::atomic<bool> shutdown_requested_{false};
};

} // namespace tutti
