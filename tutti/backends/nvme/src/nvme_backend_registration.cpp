#include "backends/include/storage_target.h"
#include "nvme_backend.h"
#include "backends/include/backend_factory.h"

namespace tutti {
namespace backends {
namespace nvme {

// Backend registration - called at static initialization time
// Registers NVMe backend constructor with BackendFactory
namespace {

struct NvmeBackendRegistrar {
    NvmeBackendRegistrar() {
        BackendFactory::register_backend(
            BackendType::LOCAL_NVME,
            []() -> IBackendProvider* {
                return new NvmeBackend();
            }
        );
    }
};

// Static instance triggers registration before main()
static NvmeBackendRegistrar g_nvme_backend_registrar;

} // anonymous namespace

} // namespace nvme
} // namespace backends
} // namespace tutti
