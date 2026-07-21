#pragma once

/**
 * local_nvme_device.h -- NVMe-specific device state
 *
 * LocalNvmeDevice is the backend_private payload for NVMe devices.
 * Contains controller handle, queue group, and namespace metadata.
 */

#include <cstdint>
#include <memory>

// Include full definition from libnvm (NVMe-specific implementation header)
#include <nvm_types.h>

namespace tutti {

class NvmeQueueGroup;  // forward-decl

// NVMe device state (stored in Device::backend_private)
struct LocalNvmeDevice {
    int32_t device_id;
    nvm_ctrl_t* ctrl;              // libnvm controller handle
    NvmeQueueGroup* queue_group;   // GPU-resident queue pool

    uint32_t namespace_id;
    uint32_t blk_size;
    uint32_t blk_size_log;
    size_t max_data_size;

    LocalNvmeDevice()
        : device_id(-1), ctrl(nullptr), queue_group(nullptr),
          namespace_id(0), blk_size(0), blk_size_log(0), max_data_size(0) {}
};

} // namespace tutti
