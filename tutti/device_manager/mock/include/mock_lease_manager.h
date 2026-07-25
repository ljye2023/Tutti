#pragma once
#include <atomic>
#include <string>
#include "common/ilease_manager.h"

namespace tutti {

/**
 * MockLeaseManager -- in-memory ILeaseManager that records call counts.
 *
 * Peer to a real DeviceServiceLeaseManager. All operations succeed; the
 * counters let callers assert the lease lifecycle (heartbeat on acquire,
 * release on shutdown) without a live DeviceService daemon.
 */
class MockLeaseManager : public ILeaseManager {
public:
    bool heartbeat(const std::string&) override     { ++heartbeats_; return true; }
    bool release_lease(const std::string&) override  { ++releases_;   return true; }
    bool has_lease(const std::string&) const override { return true; }

    int heartbeats() const { return heartbeats_.load(); }
    int releases()   const { return releases_.load(); }

private:
    std::atomic<int> heartbeats_{0};
    std::atomic<int> releases_{0};
};

} // namespace tutti
