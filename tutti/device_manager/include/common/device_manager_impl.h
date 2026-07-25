#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include "common/idevice_manager.h"
#include "common/idevice_driver.h"

namespace tutti {

/**
 * DeviceManagerImpl -- concrete IDeviceManager.
 *
 * Owns a set of IDeviceDriver plugins. On Open(), enumerates all drivers,
 * assigns stable device IDs, builds a flat IPhysicalDevice registry.
 * On open_vdevice(), dispatches to the appropriate driver by DeviceType.
 *
 * Thread-safe: mutex protects open_vdevice / close_vdevice.
 */
class DeviceManagerImpl : public IDeviceManager {
public:
    explicit DeviceManagerImpl(std::vector<std::unique_ptr<IDeviceDriver>> drivers);
    ~DeviceManagerImpl() override;

    // IDeviceManager implementation
    bool Open() override;
    void Close() override;

    int device_count() const override;
    IPhysicalDevice* device_at(int index) const override;
    IPhysicalDevice* find_by_id(int32_t id) const override;
    IPhysicalDevice* find_by_type(DeviceType t, int ordinal) const override;
    std::vector<IPhysicalDevice*> list() const override;

    IVirtualDevice* open_vdevice(int32_t phys_id, uint32_t resource_quota,
                                   std::string* error) override;
    void close_vdevice(IVirtualDevice* vdev) override;
    uint32_t available_resources(int32_t phys_id) const override;
    uint32_t caps(int32_t phys_id) const override;

private:
    std::vector<std::unique_ptr<IDeviceDriver>> drivers_;
    std::vector<IPhysicalDevice*> devices_;  // flat registry (pointers owned by drivers_)
    std::vector<std::pair<IVirtualDevice*, IDeviceDriver*>> live_vdevices_;
    mutable std::mutex mutex_;
    bool opened_ = false;
};

// Factory
std::unique_ptr<IDeviceManager> create_device_manager(
    std::vector<std::unique_ptr<IDeviceDriver>> drivers);

} // namespace tutti
