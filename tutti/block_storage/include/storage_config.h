#pragma once

#include <cstdint>
#include <string>

namespace tutti {
namespace block_storage {

struct StripeConfig {
    uint64_t stripe_size;
    uint64_t interleave_unit;
    size_t max_shards_per_file;

    StripeConfig()
        : stripe_size(256 * 1024),  // 256 KB default
          interleave_unit(0),        // tensor_size, 0 means auto-detect
          max_shards_per_file(64) {}

    StripeConfig(uint64_t stripe, uint64_t interleave, size_t max_shards)
        : stripe_size(stripe), interleave_unit(interleave), max_shards_per_file(max_shards) {}
};

enum class SyncMode {
    NONE,
    METADATA_ONLY,
    FULL
};

struct DurabilityConfig {
    SyncMode sync_mode;
    uint32_t flush_interval_ms;

    DurabilityConfig()
        : sync_mode(SyncMode::METADATA_ONLY), flush_interval_ms(5000) {}

    DurabilityConfig(SyncMode mode, uint32_t interval)
        : sync_mode(mode), flush_interval_ms(interval) {}
};

struct BlockStorageConfig {
    std::string root_directory;
    StripeConfig stripe_config;
    DurabilityConfig durability_config;
    size_t max_open_files;

    BlockStorageConfig()
        : root_directory("/tmp/tutti_storage"), max_open_files(1024) {}

    BlockStorageConfig(const std::string& root, const StripeConfig& stripe,
                       const DurabilityConfig& durability, size_t max_files)
        : root_directory(root), stripe_config(stripe),
          durability_config(durability), max_open_files(max_files) {}
};

}  // namespace block_storage
}  // namespace tutti
