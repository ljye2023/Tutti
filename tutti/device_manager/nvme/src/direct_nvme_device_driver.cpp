// direct_nvme_device_driver.cpp -- DirectNvmeDeviceDriver implementation
//
// Single-process mode: opens NVMe devices directly via libnvm.
// Uses NullLeaseManager (no cross-process arbitration, no heartbeat thread).

#include "direct_nvme_device_driver.h"
#include "nvme_virtual_device.h"

#include <algorithm>
#include <cassert>
#include <string>

namespace tutti {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

DirectNvmeDeviceDriver::DirectNvmeDeviceDriver(IAccelerator* accel,
                                               ILeaseManager* lease_mgr)
    : accel_(accel), lease_mgr_(lease_mgr) {
    assert(accel_    && "DirectNvmeDeviceDriver: accel must not be null");
    assert(lease_mgr_ && "DirectNvmeDeviceDriver: lease_mgr must not be null");
}

DirectNvmeDeviceDriver::~DirectNvmeDeviceDriver() {
    shutdown();
}

// ---------------------------------------------------------------------------
// IDeviceDriver::enumerate
// ---------------------------------------------------------------------------

int DirectNvmeDeviceDriver::enumerate(std::vector<IPhysicalDevice*>& out_devices) {
    // TODO: replace with real libnvm device discovery.
    //   Real implementation would:
    //     nvm_ctrl_t* ctrl;
    //     for each /dev/libnvmX:
    //       nvm_ctrl_init(&ctrl, fd);
    //       read namespace info, MDTS, caps;
    //       create NvmePhysicalDevice with process_grant = all user QPs;
    //       allocate NvmeQueueGroup via accel_->allocate();

    // Mock: one device, 16 queue pairs, GPUDirect-capable.
    constexpr uint32_t MOCK_CAPS = 0x1u;          // bit 0: GPUDIRECT_CAPABLE
    constexpr uint32_t MOCK_GRANT = 16u;           // 16 QPs in direct mode = full budget

    auto dev = std::make_unique<NvmePhysicalDevice>(
        static_cast<int32_t>(phys_devices_.size()),  // id
        "0000:17:00.0",                               // pci_addr
        "Mock NVMe SSD (direct)",                     // display_name
        MOCK_CAPS,
        MOCK_GRANT);

    // Populate namespace metadata (from libnvm in real impl)
    dev->namespace_id  = 1;
    dev->blk_size      = 4096;
    dev->blk_size_log  = 12;
    dev->max_data_size = 1u << 20;  // 1 MiB MDTS

    out_devices.push_back(dev.get());
    phys_devices_.push_back(std::move(dev));
    return 1;
}

// ---------------------------------------------------------------------------
// IDeviceDriver::alloc_vdevice
// ---------------------------------------------------------------------------

IVirtualDevice* DirectNvmeDeviceDriver::alloc_vdevice(IPhysicalDevice* dev,
                                                       uint32_t resource_quota,
                                                       std::string* error) {
    if (!dev) {
        if (error) *error = "alloc_vdevice: dev is null";
        return nullptr;
    }
    if (resource_quota == 0) {
        if (error) *error = "alloc_vdevice: resource_quota must be > 0";
        return nullptr;
    }

    auto* phys = static_cast<NvmePhysicalDevice*>(dev);  // safe: all our devs are NvmePhysicalDevice

    if (phys->available_grant() < resource_quota) {
        if (error) {
            *error = "alloc_vdevice: insufficient queues (available=" +
                     std::to_string(phys->available_grant()) +
                     ", requested=" + std::to_string(resource_quota) + ")";
        }
        return nullptr;
    }

    phys->reserve(resource_quota);

    // Assign a dense vdev_id among all vdevices from this physical device.
    uint32_t vdev_id = static_cast<uint32_t>(vdevices_.size());
    uint32_t caps    = phys->caps();

    auto vdev = std::make_unique<NvmeVirtualDevice>(phys->id(), vdev_id, caps);
    vdev->queue_quota   = resource_quota;
    vdev->namespace_id  = phys->namespace_id;
    vdev->blk_size      = phys->blk_size;
    vdev->blk_size_log  = phys->blk_size_log;
    vdev->max_data_size = phys->max_data_size;
    // vdev->d_qps: set to a slice of phys->queue_group->d_qps() in the real impl.
    // Requires an active NvmeQueueGroup; left null in mock.

    IVirtualDevice* raw = vdev.get();
    vdevices_.push_back(std::move(vdev));
    return raw;
}

// ---------------------------------------------------------------------------
// IDeviceDriver::free_vdevice
// ---------------------------------------------------------------------------

void DirectNvmeDeviceDriver::free_vdevice(IVirtualDevice* vdev) {
    if (!vdev) return;

    // Return resources to the physical device.
    auto* nvme_vdev = static_cast<NvmeVirtualDevice*>(vdev);
    if (nvme_vdev->phys_id() >= 0) {
        for (auto& phys : phys_devices_) {
            if (phys->id() == nvme_vdev->phys_id()) {
                phys->release(nvme_vdev->queue_quota);
                break;
            }
        }
    }

    auto it = std::find_if(vdevices_.begin(), vdevices_.end(),
        [vdev](const std::unique_ptr<IVirtualDevice>& v) { return v.get() == vdev; });
    if (it != vdevices_.end()) {
        vdevices_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// IDeviceDriver::shutdown
// ---------------------------------------------------------------------------

void DirectNvmeDeviceDriver::shutdown() {
    // No heartbeat thread in direct mode.
    if (lease_mgr_) {
        lease_mgr_->release_lease("");
    }
    vdevices_.clear();
    phys_devices_.clear();
}

} // namespace tutti
