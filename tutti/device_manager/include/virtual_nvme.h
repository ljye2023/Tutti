// tutti/device_manager/include/virtual_nvme.h -- Level-2 NVMe queue allocator
//
// IVirtualNvme is the interface for in-process NVMe queue virtualization.
// It sits between IDeviceRegistry (physical device bring-up) and backends
// (consumers of queue slices).
//
// Responsibility:
//   - Allocate VDevice slices from the process-level queue-pair pool
//   - Enforce per-backend quotas
//   - Provide device-side queue helpers to backends

#pragma once

#include <cstdint>
#include "tutti/device_manager/include/vdevice.h"

namespace tutti {

// ---------------------------------------------------------------------------
// Virtual NVMe allocator interface
// ---------------------------------------------------------------------------

class IVirtualNvme {
public:
    virtual ~IVirtualNvme() = default;

    // -----------------------------------------------------------------------
    // VDevice lifecycle
    // -----------------------------------------------------------------------

    // Open a virtual device with the specified queue quota
    // Returns nullptr if quota cannot be satisfied
    // physical_device_id: index of physical NVMe device
    // queue_quota: number of queue pairs requested
    virtual VDevice* open_vdevice(uint32_t physical_device_id, uint32_t queue_quota) = 0;

    // Close a virtual device and return its queues to the pool
    virtual bool close_vdevice(VDevice* vdev) = 0;

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------

    // Get available queue quota for a device
    virtual uint32_t available_queue_quota(uint32_t physical_device_id) const = 0;

    // Get total queue quota for a device
    virtual uint32_t total_queue_quota(uint32_t physical_device_id) const = 0;
};

}  // namespace tutti
