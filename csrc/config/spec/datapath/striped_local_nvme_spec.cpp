#include "csrc/config/spec/spec_internal.h"

namespace tutti::config::detail {
Status validate_striped_local_nvme_datapath(const DataPathSpec& spec,
                                            const std::string& path) {
    const auto* config =
        std::get_if<StripedLocalNvmeDataPathConfig>(&spec.config);
    if (config == nullptr) {
        return invalid_spec(
            path + ".config does not match type striped-local-nvme");
    }
    return Status::Ok();
}
} // namespace tutti::config::detail
