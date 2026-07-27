// mock_backend.cpp -- MockBackend implementation (device-agnostic test backend)

#include "mock_backend.h"

#include "backends/include/backend_factory.h"
#include "common/idevice_manager.h"
#include "common/ivirtual_device.h"

namespace tutti {
namespace backends {

MockBackend::~MockBackend() {
    shutdown();
}

bool MockBackend::initialize(IDeviceManager* dm, const BackendConfig& cfg) {
    ++initialize_count_;

    if (dm == nullptr) {
        return false;
    }
    if (cfg.vdevice_count == 0 || cfg.quota_per_vdevice == 0) {
        return false;
    }

    dm_ = dm;
    vdevices_.reserve(cfg.vdevice_count);

    for (uint32_t i = 0; i < cfg.vdevice_count; ++i) {
        std::string err;
        IVirtualDevice* vdev =
            dm_->open_vdevice(cfg.phys_id, cfg.quota_per_vdevice, &err);
        if (vdev == nullptr) {
            // Roll back everything opened so far, then fail.
            for (IVirtualDevice* opened : vdevices_) {
                dm_->close_vdevice(opened);
            }
            vdevices_.clear();
            dm_ = nullptr;
            return false;
        }
        vdevices_.push_back(vdev);
    }

    return true;
}

void MockBackend::shutdown() {
    if (dm_ == nullptr) {
        return;  // idempotent: never initialized or already shut down
    }
    ++shutdown_count_;

    for (IVirtualDevice* vdev : vdevices_) {
        dm_->close_vdevice(vdev);
    }
    vdevices_.clear();
    dm_ = nullptr;
}

uint32_t MockBackend::vdevice_count() const {
    return static_cast<uint32_t>(vdevices_.size());
}

IVirtualDevice* MockBackend::vdevice_at(uint32_t i) const {
    if (i >= vdevices_.size()) {
        return nullptr;
    }
    return vdevices_[i];
}

VDeviceHandle MockBackend::vdevice_handle_at(uint32_t i) const {
    if (i >= vdevices_.size()) {
        return VDeviceHandle{};  // invalid
    }
    return VDeviceHandle{i};
}

BackendMetadata MockBackend::metadata() const {
    BackendMetadata m;
    m.name = backend_name();
    m.type = BackendType::MOCK;
    m.capabilities = 0;       // mock has no transport capabilities
    m.max_io_size = 0;
    m.max_batch_size = 0;
    m.alignment_bytes = 1;
    return m;
}

// Factory registration (scheme B): MockBackend is reachable via
// BackendFactory::create_backend(BackendType::MOCK).
//
// The REGISTER_BACKEND macro token-pastes its type argument into an identifier,
// which breaks for a scoped enum value (BackendType::MOCK). So register through
// an explicit static registrar instead, matching the NVMe backend's pattern.
namespace {
struct MockBackendRegistrar {
    MockBackendRegistrar() {
        BackendFactory::register_backend(
            BackendType::MOCK,
            []() -> IBackend* { return new MockBackend(); });
    }
};
static MockBackendRegistrar g_mock_backend_registrar;
} // anonymous namespace

} // namespace backends
} // namespace tutti
