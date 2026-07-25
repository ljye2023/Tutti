// device_manager_impl.cpp -- concrete IDeviceManager implementation
#include "common/device_manager_impl.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/idevice_driver.h"
#include "common/idevice_manager.h"
#include "common/iphysical_device.h"
#include "common/ivirtual_device.h"

namespace tutti {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

DeviceManagerImpl::DeviceManagerImpl(
    std::vector<std::unique_ptr<IDeviceDriver>> drivers)
    : drivers_(std::move(drivers)) {}

DeviceManagerImpl::~DeviceManagerImpl() {
    if (opened_) {
        Close();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool DeviceManagerImpl::Open() {
    if (opened_) {
        return true;
    }

    devices_.clear();

    for (auto& driver : drivers_) {
        if (!driver) {
            continue;
        }
        std::vector<IPhysicalDevice*> found;
        const int n = driver->enumerate(found);
        if (n < 0) {
            // Driver reported failure; roll back and return false.
            devices_.clear();
            return false;
        }
        for (IPhysicalDevice* dev : found) {
            if (dev) {
                devices_.push_back(dev);
            }
        }
    }

    opened_ = true;
    return true;
}

void DeviceManagerImpl::Close() {
    if (!opened_) {
        return;
    }

    // Release all live virtual devices in reverse allocation order.
    // Iterate a copy so that close_vdevice's erase does not invalidate
    // the iteration — we do the teardown manually here without the mutex
    // (Close() itself is not called concurrently per API contract).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [vdev, driver] : live_vdevices_) {
            if (vdev && driver) {
                driver->free_vdevice(vdev);
            }
        }
        live_vdevices_.clear();
    }

    // Shut down each driver (stops heartbeat, releases lease, frees phys devs).
    for (auto& driver : drivers_) {
        if (driver) {
            driver->shutdown();
        }
    }

    devices_.clear();
    opened_ = false;
}

// ---------------------------------------------------------------------------
// Physical device registry
// ---------------------------------------------------------------------------

int DeviceManagerImpl::device_count() const {
    return static_cast<int>(devices_.size());
}

IPhysicalDevice* DeviceManagerImpl::device_at(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= devices_.size()) {
        return nullptr;
    }
    return devices_[static_cast<size_t>(index)];
}

IPhysicalDevice* DeviceManagerImpl::find_by_id(int32_t id) const {
    for (IPhysicalDevice* dev : devices_) {
        if (dev && dev->id() == id) {
            return dev;
        }
    }
    return nullptr;
}

IPhysicalDevice* DeviceManagerImpl::find_by_type(DeviceType t, int ordinal) const {
    int seen = 0;
    for (IPhysicalDevice* dev : devices_) {
        if (dev && dev->type() == t) {
            if (seen == ordinal) {
                return dev;
            }
            ++seen;
        }
    }
    return nullptr;
}

std::vector<IPhysicalDevice*> DeviceManagerImpl::list() const {
    return devices_;
}

// ---------------------------------------------------------------------------
// Virtual device allocation
// ---------------------------------------------------------------------------

IVirtualDevice* DeviceManagerImpl::open_vdevice(
    int32_t      phys_id,
    uint32_t     resource_quota,
    std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate resource_quota early.
    if (resource_quota == 0) {
        if (error) {
            *error = "open_vdevice: resource_quota must be > 0";
        }
        return nullptr;
    }

    // Look up physical device.
    IPhysicalDevice* dev = find_by_id(phys_id);
    if (!dev) {
        if (error) {
            *error = "open_vdevice: unknown phys_id " + std::to_string(phys_id);
        }
        return nullptr;
    }

    // Find the driver responsible for this device type.
    IDeviceDriver* matched_driver = nullptr;
    for (auto& driver : drivers_) {
        if (driver && driver->type() == dev->type()) {
            matched_driver = driver.get();
            break;
        }
    }
    if (!matched_driver) {
        if (error) {
            *error = "open_vdevice: no driver registered for device type";
        }
        return nullptr;
    }

    // Delegate allocation to the driver.
    IVirtualDevice* vdev = matched_driver->alloc_vdevice(dev, resource_quota, error);
    if (!vdev) {
        // error message already written by the driver (if error != nullptr).
        return nullptr;
    }

    live_vdevices_.emplace_back(vdev, matched_driver);
    return vdev;
}

void DeviceManagerImpl::close_vdevice(IVirtualDevice* vdev) {
    if (!vdev) {
        return;  // no-op per API contract
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(
        live_vdevices_.begin(), live_vdevices_.end(),
        [vdev](const std::pair<IVirtualDevice*, IDeviceDriver*>& p) {
            return p.first == vdev;
        });

    if (it == live_vdevices_.end()) {
        // Not tracked — either already freed or from a different manager.
        return;
    }

    IDeviceDriver* driver = it->second;
    live_vdevices_.erase(it);

    if (driver) {
        driver->free_vdevice(vdev);
    }
}

// ---------------------------------------------------------------------------
// Resource / capability queries
// ---------------------------------------------------------------------------

uint32_t DeviceManagerImpl::available_resources(int32_t phys_id) const {
    const IPhysicalDevice* dev = find_by_id(phys_id);
    return dev ? dev->available_grant() : 0u;
}

uint32_t DeviceManagerImpl::caps(int32_t phys_id) const {
    const IPhysicalDevice* dev = find_by_id(phys_id);
    return dev ? dev->caps() : 0u;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<IDeviceManager> create_device_manager(
    std::vector<std::unique_ptr<IDeviceDriver>> drivers)
{
    return std::make_unique<DeviceManagerImpl>(std::move(drivers));
}

} // namespace tutti
