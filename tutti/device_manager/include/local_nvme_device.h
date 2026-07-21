// tutti/device_manager/include/local_nvme_device.h
// Layer 2: Device Manager - Local NVMe Device
//
// Concrete device implementation for local NVMe SSDs

#pragma once

#include "tutti/device_manager/include/device_registry.h"
#include <memory>

namespace tutti {

// NVMe queue group configuration
struct NvmeQueueGroupConfig {
    uint32_t num_queues;
    uint32_t queue_depth;
    bool enable_p2p;
};

// Local NVMe device handle
class LocalNvmeDevice {
public:
    LocalNvmeDevice(const DeviceInfo& info);
    ~LocalNvmeDevice();

    // Queue management
    int allocate_queue_group(const NvmeQueueGroupConfig& config);
    void free_queue_group(int group_id);

    // Device properties
    const DeviceInfo& get_info() const;
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tutti
