// tutti/device_manager/src/local_nvme_virtual.cpp
// Layer 2: Device Manager - Local NVMe Virtual Registry Implementation

#include "local_nvme_virtual.h"

namespace tutti {

LocalNvmeVirtualRegistry::LocalNvmeVirtualRegistry()
    : initialized_(false) {
}

LocalNvmeVirtualRegistry::~LocalNvmeVirtualRegistry() {
    if (initialized_) {
        shutdown();
    }
}

int LocalNvmeVirtualRegistry::initialize() {
    if (initialized_) {
        return 0;
    }

    // TODO: Initialize device registry
    initialized_ = true;
    return 0;
}

void LocalNvmeVirtualRegistry::shutdown() {
    if (!initialized_) {
        return;
    }

    // Free all allocated devices
    for (auto& vdev : allocated_devices_) {
        free_vdevice(vdev);
    }
    allocated_devices_.clear();

    initialized_ = false;
}

VDevice LocalNvmeVirtualRegistry::allocate_vdevice(uint32_t num_queues) {
    VDevice vdev;
    vdev.device_id = 0;
    vdev.queue_slice_start = 0;
    vdev.queue_slice_len = num_queues;
    vdev.max_transfer_size = 1024 * 1024; // 1 MB
    vdev.supports_p2p = true;

    allocated_devices_.push_back(vdev);
    return vdev;
}

void LocalNvmeVirtualRegistry::free_vdevice(const VDevice& vdev) {
    // TODO: Implement device deallocation
    (void)vdev;
}

} // namespace tutti
