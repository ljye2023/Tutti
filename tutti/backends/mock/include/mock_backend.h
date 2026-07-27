#ifndef TUTTI_BACKENDS_MOCK_BACKEND_H_
#define TUTTI_BACKENDS_MOCK_BACKEND_H_

#include "backends/include/backend.h"
#include "backends/include/backend_types.h"

#include <cstdint>
#include <vector>

namespace tutti {

class IDeviceManager;   // device_manager/include/common/idevice_manager.h
class IVirtualDevice;   // device_manager/include/common/ivirtual_device.h

} // namespace tutti

namespace tutti {
namespace backends {

// In-memory IBackend implementation for device-agnostic testing.
//
// Peer to Layer 2's MockDeviceDriver: it depends only on the vendor-neutral
// interfaces (IBackend, IDeviceManager, IVirtualDevice) and pulls in zero
// NVMe / libnvm / CUDA symbols. A test target that links only MockBackend +
// backend_factory + DeviceManagerImpl + MockDeviceDriver proves the Layer 3
// device-agnostic boundary holds.
//
// Behaviour:
//   - initialize(): opens cfg.vdevice_count vdevices from cfg.phys_id via the
//     Device Manager, each with cfg.quota_per_vdevice units. On any failure it
//     rolls back every vdevice already opened and returns false.
//   - shutdown(): returns every vdevice to the Device Manager. Idempotent.
//
// register / submit_one are intentionally absent: they are transport-specific
// and deferred (see IBackend). MockBackend exercises only the device-agnostic
// contract this round covers.
class MockBackend : public IBackend {
public:
    MockBackend() = default;
    ~MockBackend() override;

    MockBackend(const MockBackend&) = delete;
    MockBackend& operator=(const MockBackend&) = delete;

    // ── IBackend: lifecycle ──────────────────────────────────────────────────
    bool initialize(IDeviceManager* dm, const BackendConfig& cfg) override;
    void shutdown() override;

    // ── IBackend: vdevice roster ─────────────────────────────────────────────
    uint32_t        vdevice_count() const override;
    IVirtualDevice* vdevice_at(uint32_t i) const override;
    VDeviceHandle   vdevice_handle_at(uint32_t i) const override;

    // ── IBackend: metadata ───────────────────────────────────────────────────
    BackendType     backend_type() const override { return BackendType::MOCK; }
    const char*     backend_name() const override { return "mock"; }
    BackendMetadata metadata()     const override;

    // ── Test observability ───────────────────────────────────────────────────
    int  initialize_count() const { return initialize_count_; }
    int  shutdown_count()   const { return shutdown_count_; }
    bool is_initialized()   const { return dm_ != nullptr; }

private:
    IDeviceManager*              dm_ = nullptr;   // not owned
    std::vector<IVirtualDevice*> vdevices_;       // not owned (owned by DM's driver)

    int initialize_count_ = 0;
    int shutdown_count_   = 0;
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_MOCK_BACKEND_H_
