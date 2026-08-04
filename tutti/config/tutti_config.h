// tutti/config/tutti_config.h
//
// Round 20 S1 — application config loader.
//
// Translates config/tutti_config.yaml into a RuntimeComponents struct
// passed to StorageRuntime::create().  The loader does NOT bypass the
// public API — it constructs DataPaths and Resolvers via the same
// constructors that programmatic callers use, then injects them.
//
// Priority (highest wins):
//   1. Programmatic injection (caller builds RuntimeComponents directly)
//   2. Config file (this loader)
//   3. Built-in DataPath defaults (0 = OFF / auto)
//
// TUTTI_HANDLE_CACHE_CAP / TUTTI_PRP_CACHE_CAP env vars are test-only
// backdoors: they apply ONLY when the config file omits the key.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <tutti/status.h>

namespace tutti {

// Forward declarations (avoid heavy includes in this header).
class StorageRuntime;
struct RuntimeComponents;

namespace data_paths::local_nvme { class LocalNvmeDataPath; }
namespace resolvers::local_file { class LocalFileResolver; }

namespace config {

// Owned runtime bundle — the caller must keep this alive while using
// the StorageRuntime.  Destroying it shuts down the runtime and frees
// all owned DataPaths/Resolvers.
struct TuttiRuntime {
    std::unique_ptr<StorageRuntime> runtime;
    std::vector<std::unique_ptr<data_paths::local_nvme::LocalNvmeDataPath>> datapaths;
    std::vector<std::unique_ptr<resolvers::local_file::LocalFileResolver>> resolvers;
};

// Load config from a YAML file and wire up a StorageRuntime.
//
//   path     — path to tutti_config.yaml
//   programmatic (optional) — overrides for cache capacities that take
//              precedence over the config file.  Pass 0 to defer to
//              the config file (then env, then default).
struct ProgrammaticOverrides {
    std::uint32_t handle_cache_capacity = 0;  // 0 = defer to config/env
    std::uint32_t prp_cache_capacity = 0;
    std::uint32_t handle_cache_l2_capacity = 0;
};

// Device map entry — one CUDA device ↔ one NVMe controller.
// The map is DERIVED from local_nvme_config.yaml (the deployment fact
// file shared with the daemon; nvmes[] array order = device_id = ssnvme
// minor, allowed_gpus picks the CUDA device).  tutti_config.yaml links
// to that file via its `local_nvme_config` key — device topology has a
// single source of truth.
struct DeviceSpec {
    std::uint32_t cuda_device = 0;
    std::string snvme_dev;
    std::uint32_t bar0_size = 16384;
    std::uint32_t namespace_id = 1;
    std::uint32_t block_size = 4096;
};

Result<std::unique_ptr<TuttiRuntime>> load_tutti_config(
    const std::string& path,
    const ProgrammaticOverrides& overrides = {});

// Parse-only: returns the parsed config values without constructing
// any objects.  Useful for testing and validation.
struct ParsedConfig {
    std::string gpu_vendor = "nvidia";
    std::string storage_backend = "local-nvme";
    std::uint64_t default_stripe_unit = 0;

    // local_nvme tuning
    std::uint32_t handle_cache_capacity = 0;
    std::uint32_t prp_cache_capacity = 0;
    std::uint32_t handle_cache_l2_capacity = 0;
    std::uint64_t max_in_flight_operations = 0;
    std::uint64_t max_batch_entries = 0;
    std::uint32_t num_user_queues = 0;
    std::uint64_t io_granularity = 0;

    // Link to the local-NVMe deployment fact file (may be empty;
    // resolved relative to the tutti_config.yaml directory).
    std::string local_nvme_config;
};

Result<ParsedConfig> parse_tutti_config(const std::string& path);

// Derive the device map from a local_nvme_config.yaml: nvmes[] array
// order gives the ssnvme minor; each allowed_gpus entry yields one
// DeviceSpec (bar0_size/block_size use built-in defaults).
// Entries without an explicit allowed_gpus list are SKIPPED (no unique
// mapping — fail-closed, do not guess).  Fail-closed on missing/bad yaml.
Result<std::vector<DeviceSpec>> derive_local_nvme_devices(
    const std::string& path);

// Resolve the effective cache capacities for a given config + overrides,
// applying the priority chain (programmatic > config file > env > default).
// Exposed for testing.
struct EffectiveCacheConfig {
    std::uint32_t handle_cache_capacity = 0;
    std::uint32_t prp_cache_capacity = 0;
    std::uint32_t handle_cache_l2_capacity = 0;
};
EffectiveCacheConfig resolve_cache_config(
    const ParsedConfig& parsed,
    const ProgrammaticOverrides& overrides);

} // namespace config
} // namespace tutti
