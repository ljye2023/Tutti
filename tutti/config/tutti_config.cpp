// tutti/config/tutti_config.cpp
//
// Round 20 S1 — load_tutti_config (constructs DataPaths + StorageRuntime).
// Parse/resolve logic lives in tutti_config_parse.cpp (pure host).

#include "tutti/config/tutti_config.h"

#include <string>
#include <vector>

#include <tutti/storage_runtime.h>
#include <tutti/io_types.h>
#include "tutti/data_paths/local_nvme/local_nvme_data_path.h"
#include <tutti/resolvers/local_file/resolver.h>

namespace tutti::config {

Result<std::unique_ptr<TuttiRuntime>> load_tutti_config(
    const std::string& path,
    const ProgrammaticOverrides& overrides) {
    auto parsed_result = parse_tutti_config(path);
    if (!parsed_result.ok()) {
        return Result<std::unique_ptr<TuttiRuntime>>::Failure(
            parsed_result.status());
    }
    const auto& parsed = parsed_result.value();

    auto eff = resolve_cache_config(parsed, overrides);

    auto tr = std::make_unique<TuttiRuntime>();

    // Device map: follow the local_nvme_config link (relative to the
    // tutti_config.yaml directory) and derive from the deployment fact
    // file; fall back to the built-in single-device default when no link
    // is set or the derived map is empty.
    std::vector<DeviceSpec> specs;
    if (!parsed.local_nvme_config.empty()) {
        std::string p = parsed.local_nvme_config;
        if (!p.empty() && p.front() != '/') {
            const auto slash = path.find_last_of('/');
            const std::string dir =
                (slash == std::string::npos) ? "." : path.substr(0, slash);
            p = dir + "/" + p;
        }
        auto devs = derive_local_nvme_devices(p);
        if (!devs.ok()) {
            return Result<std::unique_ptr<TuttiRuntime>>::Failure(
                devs.status());
        }
        specs = std::move(devs).value();
    }
    if (specs.empty()) {
        DeviceSpec def;
        def.snvme_dev = "/dev/ssnvme0";
        specs.push_back(def);
    }

    RuntimeComponents components;
    for (const auto& spec : specs) {
        auto dp = std::make_unique<
            data_paths::local_nvme::LocalNvmeDataPath>(
                spec.snvme_dev,
                spec.bar0_size,
                spec.cuda_device,
                parsed.num_user_queues,
                spec.namespace_id,
                spec.block_size,
                0,  // mdts_bytes (0 = auto from GET_DEV_INFO)
                static_cast<std::uint32_t>(parsed.max_batch_entries),
                0,  // cq_poll_budget (0 = default)
                eff.handle_cache_capacity,
                eff.prp_cache_capacity,
                parsed.max_in_flight_operations,
                parsed.max_batch_entries,
                0,  // max_request_bytes_override
                eff.handle_cache_l2_capacity);
        components.data_paths.push_back({"local-nvme-ext4", dp.get(),
                                          DataPathConfig{"local_nvme"}});
        tr->datapaths.push_back(std::move(dp));

        auto resolver = std::make_unique<
            resolvers::local_file::LocalFileResolver>(
                "0000:08:00.0",  // controller PCI addr (TODO: from sys_config)
                spec.namespace_id,
                spec.block_size,
                resolvers::local_file::BackingDeviceConfig{
                    "/dev/snvme" + std::to_string(spec.cuda_device) + "n1",
                    0});
        components.resolvers.push_back({"file", resolver.get()});
        tr->resolvers.push_back(std::move(resolver));
    }

    auto created = StorageRuntime::create({}, std::move(components));
    if (!created.ok()) {
        return Result<std::unique_ptr<TuttiRuntime>>::Failure(
            created.status());
    }
    tr->runtime = std::move(created).value();
    return Result<std::unique_ptr<TuttiRuntime>>::Success(std::move(tr));
}

} // namespace tutti::config
