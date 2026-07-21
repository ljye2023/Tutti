/**
 * local_nvme_virtual.cpp -- Level-2 allocator for local NVMe queue pairs.
 *
 * Implements LocalNvmeVirtualRegistry, which splits a process's QP grant
 * into per-backend vDevices using a contiguous-first-fit allocation strategy.
 */

#include "local_nvme_virtual.h"
#include "local_nvme_device.h"
#include "nvme_queue_group.h"
#include "common/device_registry.h"
#include "common/device.h"

// Need full definition for pointer arithmetic
#include <nvm_types.h>
#include <nvm_queue.h>

#include <algorithm>
#include <cstdio>

namespace tutti {

namespace {
    constexpr uint32_t CAP_GPUDIRECT = 0x1;
}

LocalNvmeVirtualRegistry::LocalNvmeVirtualRegistry(IDeviceRegistry* registry)
    : registry_(registry)
{
    if (!registry_) {
        std::fprintf(stderr, "[LocalNvmeVirtualRegistry] registry is null\n");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    size_t n = registry_->device_count();
    devices_.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const Device* dev = registry_->device_at(i);
        if (!dev || !dev->backend_private) {
            continue;
        }

        const LocalNvmeDevice* ldev = static_cast<const LocalNvmeDevice*>(dev->backend_private);

        if (!ldev->queue_group) {
            continue;
        }

        PerDeviceState state;
        state.phys_id = ldev->device_id;
        state.d_qps_base = reinterpret_cast<nvm_queue_t*>(ldev->queue_group->d_qps());
        state.total_qps = ldev->queue_group->n_qps();
        state.allocated.resize(state.total_qps, false);

        devices_.push_back(std::move(state));
    }
}

LocalNvmeVirtualRegistry::~LocalNvmeVirtualRegistry() {
    std::lock_guard<std::mutex> lock(mutex_);
    vdevices_.clear();
    devices_.clear();
}

VDevice* LocalNvmeVirtualRegistry::open_vdevice(int32_t phys_id, uint32_t quota, std::string* error) {
    if (quota == 0) {
        if (error) {
            *error = "quota is zero";
        }
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [phys_id](const PerDeviceState& s) { return s.phys_id == phys_id; });

    if (it == devices_.end()) {
        if (error) {
            *error = "phys_device_id not found";
        }
        return nullptr;
    }

    PerDeviceState& dev_state = *it;

    // Contiguous-first-fit allocation
    uint32_t start_idx = UINT32_MAX;
    uint32_t run_len = 0;

    for (uint32_t i = 0; i < dev_state.total_qps; ++i) {
        if (!dev_state.allocated[i]) {
            if (run_len == 0) {
                start_idx = i;
            }
            ++run_len;

            if (run_len == quota) {
                break;
            }
        } else {
            run_len = 0;
            start_idx = UINT32_MAX;
        }
    }

    if (run_len < quota) {
        if (error) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "insufficient contiguous QPs (requested=%u, available_run=%u)",
                          quota, run_len);
            *error = buf;
        }
        return nullptr;
    }

    // Mark allocated
    for (uint32_t i = start_idx; i < start_idx + quota; ++i) {
        dev_state.allocated[i] = true;
    }

    // Lookup physical device for metadata
    const Device* phys_dev = registry_->find_by_id(phys_id);
    if (!phys_dev || !phys_dev->backend_private) {
        if (error) {
            *error = "phys_device lookup failed after allocation";
        }
        // Rollback allocation
        for (uint32_t i = start_idx; i < start_idx + quota; ++i) {
            dev_state.allocated[i] = false;
        }
        return nullptr;
    }

    const LocalNvmeDevice* ldev = static_cast<const LocalNvmeDevice*>(phys_dev->backend_private);

    // Construct VDevice
    VDevice vdev;
    vdev.phys_device_id = phys_id;
    vdev.vdev_id = static_cast<uint32_t>(vdevices_.size());
    vdev.d_qps = dev_state.d_qps_base + start_idx;
    vdev.queue_quota = quota;
    vdev.namespace_id = ldev->namespace_id;
    vdev.blk_size = ldev->blk_size;
    vdev.blk_size_log = ldev->blk_size_log;
    vdev.max_data_size = ldev->max_data_size;
    vdev.caps = (ldev->queue_group ? CAP_GPUDIRECT : 0);

    vdevices_.push_back(vdev);

    return &vdevices_.back();
}

void LocalNvmeVirtualRegistry::close_vdevice(VDevice* vdev) {
    if (!vdev) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [vdev](const PerDeviceState& s) { return s.phys_id == vdev->phys_device_id; });

    if (it == devices_.end()) {
        std::fprintf(stderr, "[LocalNvmeVirtualRegistry] close_vdevice: device not found (phys_id=%d)\n",
                     vdev->phys_device_id);
        return;
    }

    PerDeviceState& dev_state = *it;

    // Validate pointer range
    if (vdev->d_qps < dev_state.d_qps_base ||
        vdev->d_qps >= dev_state.d_qps_base + dev_state.total_qps) {
        std::fprintf(stderr, "[LocalNvmeVirtualRegistry] close_vdevice: d_qps out of range\n");
        return;
    }

    uint32_t start_idx = static_cast<uint32_t>(vdev->d_qps - dev_state.d_qps_base);
    uint32_t end_idx = start_idx + vdev->queue_quota;

    if (end_idx > dev_state.total_qps) {
        std::fprintf(stderr, "[LocalNvmeVirtualRegistry] close_vdevice: end_idx out of bounds\n");
        return;
    }

    // Free the allocation
    for (uint32_t i = start_idx; i < end_idx; ++i) {
        dev_state.allocated[i] = false;
    }
}

uint32_t LocalNvmeVirtualRegistry::available_queues(int32_t phys_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [phys_id](const PerDeviceState& s) { return s.phys_id == phys_id; });

    if (it == devices_.end()) {
        return 0;
    }

    return static_cast<uint32_t>(std::count(it->allocated.begin(), it->allocated.end(), false));
}

uint32_t LocalNvmeVirtualRegistry::caps(int32_t phys_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const Device* dev = registry_->find_by_id(phys_id);
    if (!dev || !dev->backend_private) {
        return 0;
    }

    const LocalNvmeDevice* ldev = static_cast<const LocalNvmeDevice*>(dev->backend_private);

    return (ldev->queue_group ? CAP_GPUDIRECT : 0);
}

} // namespace tutti
