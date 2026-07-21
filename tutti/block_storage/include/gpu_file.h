// tutti/block_storage/include/gpu_file.h
// Layer 5: Block Storage - GpuFile Abstraction
//
// Represents a file accessible from GPU

#pragma once

#include "tutti/types/storage_target.h"
#include <string>
#include <vector>
#include <cstdint>

namespace tutti {

// Shard information for striped files
struct ShardInfo {
    StorageTarget target;
    uint64_t offset;
    uint64_t size;
};

// GPU-accessible file handle
class GpuFile {
public:
    GpuFile(const std::string& path, uint64_t size);
    ~GpuFile();

    // File properties
    const std::string& get_path() const;
    uint64_t get_size() const;
    uint32_t get_num_shards() const;

    // Shard access
    const ShardInfo& get_shard(uint32_t index) const;
    void add_shard(const ShardInfo& shard);

    // Convert file offset to storage target
    StorageTarget resolve_offset(uint64_t offset, uint64_t size) const;

private:
    std::string path_;
    uint64_t size_;
    std::vector<ShardInfo> shards_;
};

} // namespace tutti
