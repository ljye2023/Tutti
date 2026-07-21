// tutti/device_manager/include/vdevice.h -- Virtual device handle
//
// VDevice represents a queue slice handed from Device Manager to a backend.
// This is the Level-2 allocation result (process-level QP grant → per-backend slice).

#pragma once

#include <cstdint>
#include <cstddef>

namespace tutti {

// ---------------------------------------------------------------------------
// Virtual device capabilities
// ---------------------------------------------------------------------------

struct VDeviceCaps {
    uint32_t max_io_size_bytes;   // Maximum IO size per operation
    uint32_t max_queue_depth;     // Maximum outstanding commands per queue
    uint32_t queue_stride;        // Stride between queue entries (bytes)
    bool supports_sgl;            // Supports Scatter-Gather Lists
    bool supports_prp;            // Supports Physical Region Pages
};

// ---------------------------------------------------------------------------
// Virtual device
// ---------------------------------------------------------------------------

// A VDevice is a backend's view of the physical NVMe device.
// It contains:
//   - A slice of the process-level queue-pair pool (d_qps)
//   - Namespace visibility
//   - Capability information
//
// Lifetime:
//   - Created by IVirtualNvme::open_vdevice()
//   - Passed to IBackendProvider::initialize(VDevice*)
//   - Released by IVirtualNvme::close_vdevice()
struct VDevice {
    // Device identity
    uint32_t physical_device_id;  // Physical NVMe device index
    uint32_t namespace_id;        // Namespace ID (typically 1)

    // Queue pool slice (device-side)
    void* d_qps;                  // Pointer into NvmeQueueGroup::d_qps_[base_qp_index]
    uint32_t queue_quota;         // Number of queue pairs in this slice
    uint32_t base_qp_index;       // Starting index in physical QP pool

    // Capabilities
    VDeviceCaps caps;

    // Host-side queue metadata (for libnvm operations)
    void* host_queue_group;       // Opaque pointer to NvmeQueueGroup (DM-private)

    VDevice()
        : physical_device_id(0), namespace_id(0), d_qps(nullptr),
          queue_quota(0), base_qp_index(0), host_queue_group(nullptr) {}
};

}  // namespace tutti
