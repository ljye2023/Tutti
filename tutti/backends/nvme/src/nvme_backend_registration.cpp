// nvme_backend_registration.cpp -- Register NvmeBackend with BackendFactory

#include "nvme_backend.h"
#include "backends/include/backend_factory.h"

namespace tutti {
namespace backends {
namespace nvme {

namespace {

struct NvmeBackendRegistrar {
    NvmeBackendRegistrar() {
        BackendFactory::register_backend(
            BackendType::LOCAL_NVME,
            []() -> IBackend* {
                return new NvmeBackend();
            });
    }
};

// Static instance triggers registration before main()
static NvmeBackendRegistrar g_nvme_backend_registrar;

} // anonymous namespace

} // namespace nvme
} // namespace backends
} // namespace tutti
