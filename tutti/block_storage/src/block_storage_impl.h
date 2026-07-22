#pragma once

#include "block_storage.h"
#include "file_directory.h"
#include "stripe_manager.h"
#include "metadata_journal.h"
#include "accel/include/common/iaccel.h"
#include "accel/include/common/accel_types.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace tutti {

namespace coordinator {
class IRawDevice;
}

namespace block_storage {

class BlockStorageImpl : public IBlockStorage {
public:
    BlockStorageImpl();
    ~BlockStorageImpl() override;

    // IBlockStorage interface implementation
    bool initialize(const BlockStorageConfig& config,
                   backends::IBackendProvider* backend_provider,
                   IAccelerator* accelerator) override;

    void cleanup() override;

    GpuFileHandle* open_gpu_file(const std::string& name,
                                FileOpenMode mode,
                                uint64_t stripe_size = 0,
                                uint64_t initial_size = 0) override;

    bool close_gpu_file(GpuFileHandle* handle) override;

    bool delete_gpu_file(const std::string& name) override;

    std::vector<GpuFileHandle*> open_gpu_files_batch(
        const std::vector<std::string>& names,
        const std::vector<FileOpenMode>& modes,
        size_t count) override;

    std::vector<FileInfo> list_gpu_file_names() override;

    backends::StorageTarget acquire_device_handle(
        GpuFileHandle* handle,
        size_t shard_index) override;

    bool release_device_handle(GpuFileHandle* handle,
                              size_t shard_index) override;

    bool sync_file(GpuFileHandle* handle, void* stream) override;

    bool flush_metadata() override;

private:
    // Helper methods
    bool validate_config(const BlockStorageConfig& config);
    GpuFileHandle* create_file_handle(const GpuFile* file);
    bool destroy_file_handle(GpuFileHandle* handle);
    bool is_file_open(FileId file_id);
    bool recover_metadata();

    // Configuration and dependencies
    BlockStorageConfig config_;
    backends::IBackendProvider* backend_provider_;
    IAccelerator* accelerator_;
    coordinator::IRawDevice* raw_device_;

    // Core components
    FileDirectory file_directory_;
    StripeManager stripe_manager_;
    MetadataJournal metadata_journal_;

    // Handle tracking
    std::unordered_map<FileId, GpuFileHandle*> open_files_map_;
    std::shared_mutex handle_lock_;

    // State
    bool initialized_;
    bool stripe_manager_initialized_;
};

}  // namespace block_storage
}  // namespace tutti
