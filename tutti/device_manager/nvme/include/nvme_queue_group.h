#pragma once

/**
 * nvme_queue_group.h -- GPU-resident NVMe queue pool
 *
 * NvmeQueueGroup manages a set of NVMe user queue pairs on GPU.
 * Created by device_manager, consumed by backends via NvmeVirtualDevice.
 */

#include <cstdint>

// Include full definition (libnvm uses typedef, not forward-declarable)
#include <nvm_types.h>

namespace tutti {

// GPU-resident NVMe queue pool
class NvmeQueueGroup {
public:
    NvmeQueueGroup() : ctrl_(nullptr), n_qps_(0), d_qps_(nullptr) {}

    // Returns device pointer to queue array
    void* d_qps() const { return d_qps_; }

    // Number of queue pairs in the group
    uint32_t n_qps() const { return n_qps_; }

protected:
    nvm_ctrl_t* ctrl_;
    uint32_t n_qps_;
    void* d_qps_;  // nvm_queue_t[] on device
};

} // namespace tutti
