#pragma once
#include <string>

namespace tutti {

/**
 * ILeaseManager -- Level-① cross-process arbiter.
 *
 * Manages the per-process resource grant issued by the DeviceService daemon.
 * A process holds a lease as long as it calls heartbeat() regularly; the
 * daemon reaps the grant if the heartbeat lapses (dead-process cleanup).
 *
 * Single-process / direct mode: ILeaseManager is a no-op (heartbeat always
 * returns true, has_lease always returns true).
 *
 * DeviceService (formerly NVMeService) is the canonical daemon implementation.
 * It manages all device types, not NVMe alone.
 */
class ILeaseManager {
public:
    virtual ~ILeaseManager() = default;

    // Keep a cross-process resource grant alive.
    // Returns true if the daemon acknowledged the heartbeat.
    // Returns false if the lease has expired or is unknown.
    virtual bool heartbeat(const std::string& lease_id) = 0;

    // Explicitly release a grant before process exit.
    // Returns true if released, false if the lease was not held.
    virtual bool release_lease(const std::string& lease_id) = 0;

    // True if the lease is currently active.
    virtual bool has_lease(const std::string& lease_id) const = 0;
};

} // namespace tutti
