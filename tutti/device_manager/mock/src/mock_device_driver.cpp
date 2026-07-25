// mock_device_driver.cpp -- MockDeviceDriver implementation (in-memory backend)

#include "mock_device_driver.h"

#include <algorithm>

namespace tutti {

int MockDeviceDriver::enumerate(std::vector<IPhysicalDevice*>& out_devices) {
    ++enumerate_count_;
    if (fail_enumerate_) {
        return -1;
    }
    // Decision 4: driver drives its own lease at bring-up.
    if (lease_mgr_) {
        lease_id_ = "mock-lease-" + std::to_string(base_id_);
        lease_mgr_->heartbeat(lease_id_);
    }
    for (int i = 0; i < device_count_; ++i) {
        auto dev = std::make_unique<MockPhysicalDevice>(
            base_id_ + i, type_, "mock:00:0" + std::to_string(i),
            "Mock Device " + std::to_string(base_id_ + i), caps_, grant_each_);
        out_devices.push_back(dev.get());
        phys_devices_.push_back(std::move(dev));
    }
    return device_count_;
}

IVirtualDevice* MockDeviceDriver::alloc_vdevice(IPhysicalDevice* dev,
                                                uint32_t resource_quota,
                                                std::string* error) {
    if (!dev) { if (error) *error = "dev is null"; return nullptr; }
    if (resource_quota == 0) { if (error) *error = "quota is zero"; return nullptr; }

    auto* phys = static_cast<MockPhysicalDevice*>(dev);
    if (phys->available_grant() < resource_quota) {
        if (error) *error = "insufficient grant";
        return nullptr;
    }
    phys->reserve(resource_quota);
    auto vdev = std::make_unique<MockVirtualDevice>(
        phys->id(), static_cast<uint32_t>(vdevices_.size()), type_,
        resource_quota, phys->caps());
    IVirtualDevice* raw = vdev.get();
    vdevices_.push_back(std::move(vdev));
    return raw;
}

void MockDeviceDriver::free_vdevice(IVirtualDevice* vdev) {
    if (!vdev) return;
    for (auto& phys : phys_devices_) {
        if (phys->id() == vdev->phys_id()) {
            phys->release(vdev->resource_count());
            break;
        }
    }
    auto it = std::find_if(vdevices_.begin(), vdevices_.end(),
        [vdev](const std::unique_ptr<MockVirtualDevice>& v) { return v.get() == vdev; });
    if (it != vdevices_.end()) vdevices_.erase(it);
}

void MockDeviceDriver::shutdown() {
    ++shutdown_count_;
    if (lease_mgr_ && !lease_id_.empty()) {
        lease_mgr_->release_lease(lease_id_);
        lease_id_.clear();
    }
    vdevices_.clear();
    phys_devices_.clear();
}

} // namespace tutti
