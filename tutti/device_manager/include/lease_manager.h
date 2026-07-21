// tutti/device_manager/include/lease_manager.h
// Layer 2: Device Manager - Lease Manager
//
// Manages queue leases and resource allocation

#pragma once

#include <cstdint>
#include <memory>

namespace tutti {

// Lease handle for queue resources
struct LeaseHandle {
    uint64_t lease_id;
    uint32_t queue_start;
    uint32_t queue_count;
    bool is_valid;
};

// Lease manager interface
class ILeaseManager {
public:
    virtual ~ILeaseManager() = default;

    // Lease operations
    virtual LeaseHandle acquire_lease(uint32_t num_queues) = 0;
    virtual void release_lease(const LeaseHandle& lease) = 0;
    virtual bool is_lease_valid(const LeaseHandle& lease) const = 0;

    // Resource queries
    virtual uint32_t get_available_queues() const = 0;
    virtual uint32_t get_total_queues() const = 0;
};

} // namespace tutti
