// tutti/config/tutti_config_parse.cpp
//
// Round 20 S1 — config parse + resolve (pure host, no CUDA dependency).
// Split from tutti_config.cpp so unit tests can link without CUDA.

#include "tutti/config/tutti_config.h"

#include <cstdlib>
#include <string>

#include <yaml-cpp/yaml.h>

namespace tutti::config {

Result<ParsedConfig> parse_tutti_config(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        return Result<ParsedConfig>::Failure(
            Status(StatusCode::INVALID_ARGUMENT,
                   "yaml parse error: " + std::string(e.what())));
    }

    ParsedConfig cfg;

    if (auto gpu = root["gpu"]) {
        if (auto v = gpu["vendor"]) cfg.gpu_vendor = v.as<std::string>();
    }

    if (auto storage = root["storage"]) {
        if (auto b = storage["backend"])
            cfg.storage_backend = b.as<std::string>();
        if (auto s = storage["default_stripe_unit"])
            cfg.default_stripe_unit = s.as<std::uint64_t>();
    }

    if (auto ln = root["local_nvme"]) {
        if (auto v = ln["handle_cache_capacity"])
            cfg.handle_cache_capacity = v.as<std::uint32_t>();
        if (auto v = ln["prp_cache_capacity"])
            cfg.prp_cache_capacity = v.as<std::uint32_t>();
        if (auto v = ln["handle_cache_l2_capacity"])
            cfg.handle_cache_l2_capacity = v.as<std::uint32_t>();
        if (auto v = ln["max_in_flight_operations"])
            cfg.max_in_flight_operations = v.as<std::uint64_t>();
        if (auto v = ln["max_batch_entries"])
            cfg.max_batch_entries = v.as<std::uint64_t>();
        if (auto v = ln["num_user_queues"])
            cfg.num_user_queues = v.as<std::uint32_t>();
        if (auto v = ln["io_granularity"])
            cfg.io_granularity = v.as<std::uint64_t>();
    }

    if (auto v = root["local_nvme_config"])
        cfg.local_nvme_config = v.as<std::string>();

    if (cfg.storage_backend == "rdma") {
        return Result<ParsedConfig>::Failure(
            Status(StatusCode::UNSUPPORTED,
                   "storage.backend=rdma is not yet implemented"));
    }

    return Result<ParsedConfig>::Success(std::move(cfg));
}

Result<std::vector<DeviceSpec>> derive_local_nvme_devices(
    const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        return Result<std::vector<DeviceSpec>>::Failure(
            Status(StatusCode::INVALID_ARGUMENT,
                   "local_nvme_config yaml parse error: " +
                   std::string(e.what())));
    }

    std::vector<DeviceSpec> out;
    std::uint32_t minor = 0;  // nvmes[] array order = daemon device_id
    if (auto nvmes = root["nvmes"]) {
        for (const auto& nv : nvmes) {
            std::uint32_t ns = 1;
            if (auto v = nv["namespace_id"]) ns = v.as<std::uint32_t>();
            if (auto ag = nv["allowed_gpus"]) {
                for (const auto& g : ag) {
                    DeviceSpec spec;
                    spec.cuda_device = g.as<std::uint32_t>();
                    spec.snvme_dev = "/dev/ssnvme" + std::to_string(minor);
                    spec.namespace_id = ns;
                    // bar0_size / block_size keep built-in defaults.
                    out.push_back(spec);
                }
            }
            // Entries without explicit allowed_gpus are skipped: no
            // unique CUDA-device mapping — fail-closed, do not guess.
            ++minor;
        }
    }
    return Result<std::vector<DeviceSpec>>::Success(std::move(out));
}

static std::uint32_t env_or_zero(const char* name) {
    const char* v = std::getenv(name);
    return v ? static_cast<std::uint32_t>(std::atoi(v)) : 0;
}

EffectiveCacheConfig resolve_cache_config(
    const ParsedConfig& parsed,
    const ProgrammaticOverrides& overrides) {
    EffectiveCacheConfig eff;

    if (overrides.handle_cache_capacity > 0) {
        eff.handle_cache_capacity = overrides.handle_cache_capacity;
    } else if (parsed.handle_cache_capacity > 0) {
        eff.handle_cache_capacity = parsed.handle_cache_capacity;
    } else {
        eff.handle_cache_capacity = env_or_zero("TUTTI_HANDLE_CACHE_CAP");
    }

    if (overrides.prp_cache_capacity > 0) {
        eff.prp_cache_capacity = overrides.prp_cache_capacity;
    } else if (parsed.prp_cache_capacity > 0) {
        eff.prp_cache_capacity = parsed.prp_cache_capacity;
    } else {
        eff.prp_cache_capacity = env_or_zero("TUTTI_PRP_CACHE_CAP");
    }

    if (overrides.handle_cache_l2_capacity > 0) {
        eff.handle_cache_l2_capacity = overrides.handle_cache_l2_capacity;
    } else if (parsed.handle_cache_l2_capacity > 0) {
        eff.handle_cache_l2_capacity = parsed.handle_cache_l2_capacity;
    }

    return eff;
}

} // namespace tutti::config
