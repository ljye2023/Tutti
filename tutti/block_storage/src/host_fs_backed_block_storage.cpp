// tutti/block_storage/src/host_fs_backed_block_storage.cpp
// Layer 5: Block Storage - Host Filesystem Backed Implementation

#include "tutti/block_storage/include/block_storage.h"
#include "tutti/block_storage/include/gpu_file.h"

namespace tutti {

// GpuFile implementation
GpuFile::GpuFile(const std::string& path, uint64_t size)
    : path_(path), size_(size) {
}

GpuFile::~GpuFile() {
}

const std::string& GpuFile::get_path() const {
    return path_;
}

uint64_t GpuFile::get_size() const {
    return size_;
}

uint32_t GpuFile::get_num_shards() const {
    return static_cast<uint32_t>(shards_.size());
}

const ShardInfo& GpuFile::get_shard(uint32_t index) const {
    return shards_[index];
}

void GpuFile::add_shard(const ShardInfo& shard) {
    shards_.push_back(shard);
}

StorageTarget GpuFile::resolve_offset(uint64_t offset, uint64_t size) const {
    // TODO: Implement offset resolution
    (void)offset;
    (void)size;
    StorageTarget target;
    target.device_id = 0;
    target.lba = 0;
    target.size = 0;
    return target;
}

} // namespace tutti
