// tutti/block_storage/include/gpu_file_resolve.cuh
// Layer 5: Block Storage - Device-side Shard Resolution
//
// Device-side helpers for resolving file offsets to storage targets

#pragma once

#include "tutti/abstraction/accel.h"
#include "tutti/types/storage_target.h"
#include "tutti/block_storage/include/gpu_file.h"

namespace tutti {

// Device-side shard resolution context
struct ShardResolveContext {
    const ShardInfo* shards;
    uint32_t num_shards;
    uint64_t stripe_size;

    TUTTI_DEVICE StorageTarget resolve(uint64_t offset, uint64_t size) const;
};

// Device-side shard resolution helper
TUTTI_DEVICE inline uint32_t find_shard_index(
    const ShardInfo* shards,
    uint32_t num_shards,
    uint64_t offset)
{
    for (uint32_t i = 0; i < num_shards; ++i) {
        if (offset >= shards[i].offset &&
            offset < shards[i].offset + shards[i].size) {
            return i;
        }
    }
    return 0xFFFFFFFF; // Invalid index
}

} // namespace tutti
