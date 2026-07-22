#pragma once

#include "block_storage_types.h"
#include "storage_config.h"
#include "backends/include/storage_target.h"

#include <vector>
#include <memory>

namespace tutti {

class IAccelerator;

namespace backends {
    class IBackendProvider;
}

namespace block_storage {

class IBlockStorage {
public:
    virtual ~IBlockStorage() = default;

    // Initialize the block storage system with configuration and dependencies
    virtual bool initialize(const BlockStorageConfig& config,
                          backends::IBackendProvider* backend_provider,
                          IAccelerator* accelerator) = 0;

    // Shutdown and release all resources
    virtual void cleanup() = 0;

    // Open a single GPU file
    virtual GpuFileHandle* open_gpu_file(const std::string& name,
                                        FileOpenMode mode,
                                        uint64_t stripe_size = 0,
                                        uint64_t initial_size = 0) = 0;

    // Close a GPU file handle
    virtual bool close_gpu_file(GpuFileHandle* handle) = 0;

    // Delete a GPU file
    virtual bool delete_gpu_file(const std::string& name) = 0;

    // Open multiple GPU files in batch
    virtual std::vector<GpuFileHandle*> open_gpu_files_batch(
        const std::vector<std::string>& names,
        const std::vector<FileOpenMode>& modes,
        size_t count) = 0;

    // List all GPU files
    virtual std::vector<FileInfo> list_gpu_file_names() = 0;

    // Acquire a device handle for a specific shard
    virtual backends::StorageTarget acquire_device_handle(
        GpuFileHandle* handle,
        size_t shard_index) = 0;

    // Release a device handle for a specific shard
    virtual bool release_device_handle(GpuFileHandle* handle,
                                      size_t shard_index) = 0;

    // Synchronize file data to storage
    virtual bool sync_file(GpuFileHandle* handle, void* stream) = 0;

    // Flush metadata to persistent storage
    virtual bool flush_metadata() = 0;
};

// Factory function to create block storage instance
std::unique_ptr<IBlockStorage> create_block_storage();

}  // namespace block_storage
}  // namespace tutti
