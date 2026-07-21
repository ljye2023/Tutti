#pragma once
#include <cstdint>
#include <cstddef>

// VDevice uses nvm_queue_t* for GPU-resident queue pointers.
// This is part of the common Layer 2 API but the type comes from libnvm.
// Include the full definition (libnvm uses typedef, not forward-declarable).
#include <nvm_types.h>

namespace tutti {

// A virtual storage device: one backend's slice of a physical NVMe controller.
struct VDevice {
    // Identity
    int32_t  phys_device_id;   // which LocalNvmeDevice this slices
    uint32_t vdev_id;          // dense index within IVirtualNvme (0, 1, 2, ...)

    // Level-2 allocation: the slice of d_qps[] this backend owns
    nvm_queue_t* d_qps;        // GPU-resident pointer into NvmeQueueGroup::d_qps_[slice_start]
    uint32_t     queue_quota;  // number of QPs in this slice

    // Namespace view (from LocalNvmeDevice)
    uint32_t namespace_id;
    uint32_t blk_size;
    uint32_t blk_size_log;
    size_t   max_data_size;    // MDTS in bytes (controller-reported limit)

    // Capabilities (bitmask)
    uint32_t caps;
    // bit 0: GPUDIRECT_CAPABLE (NvmeQueueGroup exists)
    // bit 1-31: reserved
};

} // namespace tutti
