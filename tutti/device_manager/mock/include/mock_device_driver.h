#pragma once
#include <memory>
#include <string>
#include <vector>
#include "common/device_type.h"
#include "common/idevice_driver.h"
#include "common/ilease_manager.h"
#include "mock_physical_device.h"
#include "mock_virtual_device.h"

namespace tutti {

/**
 * MockDeviceDriver -- in-memory IDeviceDriver backend, peer to the NVMe drivers.
 *
 * Fabricates `device_count` physical devices with ids [base_id, base_id+count)
 * and serves vdevice allocations entirely in memory. Optionally drives an
 * injected ILeaseManager (Decision 4) so lease lifecycle can be observed.
 *
 * Exposes lightweight lifecycle counters for tests/bring-up diagnostics.
 */
class MockDeviceDriver : public IDeviceDriver {
public:
    MockDeviceDriver(DeviceType type, int32_t base_id, int device_count,
                     uint32_t grant_each, uint32_t caps,
                     ILeaseManager* lease_mgr = nullptr)
        : type_(type), base_id_(base_id), device_count_(device_count),
          grant_each_(grant_each), caps_(caps), lease_mgr_(lease_mgr) {}

    ~MockDeviceDriver() override = default;  // manager owns lifecycle

    DeviceType type() const override { return type_; }
    int enumerate(std::vector<IPhysicalDevice*>& out_devices) override;
    IVirtualDevice* alloc_vdevice(IPhysicalDevice* dev, uint32_t resource_quota,
                                  std::string* error) override;
    void free_vdevice(IVirtualDevice* vdev) override;
    void shutdown() override;

    // Diagnostics / test observability.
    void   set_fail_enumerate(bool v) { fail_enumerate_ = v; }
    int    enumerate_count()   const  { return enumerate_count_; }
    int    shutdown_count()    const  { return shutdown_count_; }
    size_t live_vdevice_count() const { return vdevices_.size(); }

private:
    DeviceType     type_;
    int32_t        base_id_;
    int            device_count_;
    uint32_t       grant_each_;
    uint32_t       caps_;
    ILeaseManager* lease_mgr_;
    std::string    lease_id_;

    bool           fail_enumerate_ = false;
    int            enumerate_count_ = 0;
    int            shutdown_count_ = 0;

    std::vector<std::unique_ptr<MockPhysicalDevice>> phys_devices_;
    std::vector<std::unique_ptr<MockVirtualDevice>>  vdevices_;
};

} // namespace tutti
