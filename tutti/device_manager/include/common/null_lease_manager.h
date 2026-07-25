#pragma once
#include "common/ilease_manager.h"

namespace tutti {

/**
 * NullLeaseManager -- no-op ILeaseManager for direct (single-process) mode.
 *
 * Used when the process owns the physical device exclusively. No cross-process
 * arbitration needed, so heartbeat/release/has_lease are all no-ops returning true.
 */
class NullLeaseManager : public ILeaseManager {
public:
    bool heartbeat(const std::string&) override { return true; }
    bool release_lease(const std::string&) override { return true; }
    bool has_lease(const std::string&) const override { return true; }
};

} // namespace tutti
