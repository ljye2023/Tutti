#pragma once
#include <string>

namespace tutti {

// Lease manager interface
// Responsibility: Cross-process heartbeat + release (Level-1 arbiter)
class ILeaseManager {
public:
    virtual ~ILeaseManager() = default;

    virtual bool heartbeat(const std::string& lease_id) = 0;
    virtual bool release_lease(const std::string& lease_id) = 0;
    virtual bool has_lease(const std::string& lease_id) const = 0;
};

} // namespace tutti
