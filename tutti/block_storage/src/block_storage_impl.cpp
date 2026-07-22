#include "block_storage_impl.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "accel/include/common/accel_types.h"

#include <filesystem>
#include <algorithm>

namespace tutti {
namespace block_storage {

BlockStorageImpl::BlockStorageImpl()
    : backend_provider_(nullptr), accelerator_(nullptr), raw_device_(nullptr),
      initialized_(false), stripe_manager_initialized_(false) {
}

BlockStorageImpl::~BlockStorageImpl() {
    cleanup();
}

bool BlockStorageImpl::initialize(const BlockStorageConfig& config,
                                 backends::IBackendProvider* backend_provider,
                                 IAccelerator* accelerator) {
    if (initialized_) {
        return false;
    }

    if (!validate_config(config)) {
        return false;
    }

    if (!backend_provider || !accelerator) {
        return false;
    }

    config_ = config;
    backend_provider_ = backend_provider;
    accelerator_ = accelerator;

    // Initialize metadata journal
    if (!metadata_journal_.initialize(config_.root_directory)) {
        return false;
    }

    // Initialize stripe manager with mock device support
    // Note: stripe_manager can be initialized with nullptr raw_device for testing
    raw_device_ = nullptr;  // Will be set later via set_raw_device() if needed

    if (!stripe_manager_.initialize(backend_provider_, nullptr, config_.stripe_config)) {
        return false;
    }
    stripe_manager_initialized_ = true;

    // Recover metadata from journal
    if (!recover_metadata()) {
        return false;
    }

    initialized_ = true;
    return true;
}

void BlockStorageImpl::cleanup() {
    if (!initialized_) {
        return;
    }

    // Close all open files
    std::unique_lock<std::shared_mutex> lock(handle_lock_);
    for (auto& pair : open_files_map_) {
        destroy_file_handle(pair.second);
    }
    open_files_map_.clear();

    // Close journal
    metadata_journal_.close();

    initialized_ = false;
}

GpuFileHandle* BlockStorageImpl::open_gpu_file(const std::string& name,
                                               FileOpenMode mode,
                                               uint64_t stripe_size,
                                               uint64_t initial_size) {
    if (!initialized_ || name.empty()) {
        return nullptr;
    }

    std::unique_lock<std::shared_mutex> lock(handle_lock_);

    // Check if we've reached max open files limit
    if (open_files_map_.size() >= config_.max_open_files) {
        return nullptr;
    }

    GpuFile file;
    bool file_exists = file_directory_.lookup_by_name(name, file);

    if (mode == FileOpenMode::CREATE_NEW) {
        if (file_exists) {
            // File already exists
            return nullptr;
        }

        // Create new file
        FileId file_id = file_directory_.generate_file_id();
        GpuFile new_file(file_id, name, initial_size,
                        stripe_size == 0 ? config_.stripe_config.stripe_size : stripe_size);

        // Allocate shards
        if (initial_size > 0) {
            new_file.shards = stripe_manager_.allocate_shards(initial_size, new_file.stripe_size);
            if (new_file.shards.empty()) {
                return nullptr;
            }
        }

        // Add to directory
        if (!file_directory_.add_file(new_file)) {
            if (!new_file.shards.empty()) {
                stripe_manager_.deallocate_shards(new_file.shards);
            }
            return nullptr;
        }

        // Log creation
        metadata_journal_.log_create(new_file);

        if (!file_directory_.lookup_by_name(name, file)) {
            return nullptr;
        }

    } else if (mode == FileOpenMode::OPEN_OR_CREATE) {
        if (!file_exists) {
            // Create new file
            FileId file_id = file_directory_.generate_file_id();
            GpuFile new_file(file_id, name, initial_size,
                            stripe_size == 0 ? config_.stripe_config.stripe_size : stripe_size);

            if (initial_size > 0) {
                new_file.shards = stripe_manager_.allocate_shards(initial_size, new_file.stripe_size);
                if (new_file.shards.empty()) {
                    return nullptr;
                }
            }

            if (!file_directory_.add_file(new_file)) {
                if (!new_file.shards.empty()) {
                    stripe_manager_.deallocate_shards(new_file.shards);
                }
                return nullptr;
            }

            metadata_journal_.log_create(new_file);
            if (!file_directory_.lookup_by_name(name, file)) {
                return nullptr;
            }
        }

    } else {
        // READ_ONLY or READ_WRITE mode
        if (!file_exists) {
            return nullptr;
        }
    }

    // Check if file is already open
    if (is_file_open(file.file_id)) {
        return nullptr;
    }

    // Create handle
    GpuFileHandle* handle = create_file_handle(&file);
    if (!handle) {
        return nullptr;
    }

    open_files_map_[file.file_id] = handle;
    return handle;
}

bool BlockStorageImpl::close_gpu_file(GpuFileHandle* handle) {
    if (!initialized_ || !handle) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(handle_lock_);

    auto it = open_files_map_.find(handle->file_id);
    if (it == open_files_map_.end()) {
        return false;
    }

    // Sync if dirty
    if (handle->dirty && config_.durability_config.sync_mode != SyncMode::NONE) {
        // Note: sync_file requires stream parameter, but we're closing
        // For now, just mark as needing flush
        flush_metadata();
    }

    // Destroy handle
    destroy_file_handle(handle);
    open_files_map_.erase(it);

    return true;
}

bool BlockStorageImpl::delete_gpu_file(const std::string& name) {
    if (!initialized_ || name.empty()) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(handle_lock_);

    GpuFile file;
    if (!file_directory_.lookup_by_name(name, file)) {
        return false;
    }

    // Check if file is open
    if (is_file_open(file.file_id)) {
        return false;
    }

    FileId file_id = file.file_id;
    std::vector<FileShard> shards = file.shards;

    // Remove from directory
    if (!file_directory_.remove_file(name)) {
        return false;
    }

    // Deallocate shards
    stripe_manager_.deallocate_shards(shards);

    // Log deletion
    metadata_journal_.log_delete(file_id, name);

    return true;
}

std::vector<GpuFileHandle*> BlockStorageImpl::open_gpu_files_batch(
    const std::vector<std::string>& names,
    const std::vector<FileOpenMode>& modes,
    size_t count) {

    std::vector<GpuFileHandle*> handles;

    if (!initialized_ || count == 0 || names.size() < count || modes.size() < count) {
        return handles;
    }

    handles.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        GpuFileHandle* handle = open_gpu_file(names[i], modes[i], 0, 0);
        if (handle) {
            handles.push_back(handle);
        } else {
            // Failed to open file, close all previously opened files
            for (auto h : handles) {
                close_gpu_file(h);
            }
            return std::vector<GpuFileHandle*>();
        }
    }

    return handles;
}

std::vector<FileInfo> BlockStorageImpl::list_gpu_file_names() {
    if (!initialized_) {
        return std::vector<FileInfo>();
    }

    return file_directory_.list_files();
}

backends::StorageTarget BlockStorageImpl::acquire_device_handle(
    GpuFileHandle* handle,
    size_t shard_index) {

    backends::StorageTarget target{};

    if (!initialized_ || !handle) {
        return target;
    }

    std::shared_lock<std::shared_mutex> lock(handle_lock_);

    GpuFile file;
    if (!file_directory_.lookup_by_id(handle->file_id, file) || shard_index >= file.shards.size()) {
        return target;
    }

    const FileShard& shard = file.shards[shard_index];

    // Map shard to StorageTarget
    target.kind = backends::StorageTargetKind::NVME_RAW;
    target.namespace_id = shard.namespace_id;
    target.start_lba = shard.start_lba;
    target.length_blocks = shard.length_blocks;

    // Acquire backend handle using StorageTarget
    void* backend_handle = backend_provider_->acquire_target_handle(target);

    if (backend_handle) {
        // Store handle
        if (shard_index < handle->target_handles.size()) {
            handle->target_handles[shard_index] = backend_handle;
        } else {
            handle->target_handles.resize(shard_index + 1, nullptr);
            handle->target_handles[shard_index] = backend_handle;
        }
    }

    return target;
}

bool BlockStorageImpl::release_device_handle(GpuFileHandle* handle,
                                            size_t shard_index) {
    if (!initialized_ || !handle) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(handle_lock_);

    if (shard_index >= handle->target_handles.size()) {
        return false;
    }

    void* backend_handle = handle->target_handles[shard_index];
    if (backend_handle) {
        backend_provider_->release_target_handle(backend_handle);
        handle->target_handles[shard_index] = nullptr;
    }

    return true;
}

bool BlockStorageImpl::sync_file(GpuFileHandle* handle, void* stream) {
    if (!initialized_ || !handle) {
        return false;
    }

    // NONE mode: no sync required
    if (config_.durability_config.sync_mode == SyncMode::NONE) {
        return true;
    }

    // METADATA_ONLY mode: flush metadata journal
    if (config_.durability_config.sync_mode == SyncMode::METADATA_ONLY) {
        handle->dirty = false;
        return flush_metadata();
    }

    // FULL mode: synchronize stream and flush all devices
    if (config_.durability_config.sync_mode == SyncMode::FULL) {
        // Synchronize accelerator stream to ensure all GPU operations complete
        if (stream && accelerator_) {
            accelerator_->synchronize_stream(AccelStream(stream));
        }

        // Lookup file to get shard information
        GpuFile file;
        if (!file_directory_.lookup_by_id(handle->file_id, file)) {
            return false;
        }

        // Flush each device using CPU sync submission (no data, just flush)
        // We use empty descriptor arrays to trigger device flush
        for (size_t i = 0; i < file.shards.size(); ++i) {
            if (i < handle->target_handles.size() && handle->target_handles[i]) {
                // Empty batch submission triggers device flush in backend
                backends::SubmissionResult result = backend_provider_->submit_batch_cpu_sync(
                    handle->target_handles[i],
                    nullptr,  // No descriptors
                    0,        // Zero count
                    true);    // Direction doesn't matter for flush

                if (!result.success) {
                    return false;
                }
            }
        }

        // Flush metadata after device flush
        if (!flush_metadata()) {
            return false;
        }

        handle->dirty = false;
        return true;
    }

    return false;
}

bool BlockStorageImpl::flush_metadata() {
    if (!initialized_) {
        return false;
    }

    // Collect all files for checkpoint
    std::vector<FileInfo> file_infos = file_directory_.list_files();
    std::vector<GpuFile> all_files;
    all_files.reserve(file_infos.size());

    for (const auto& info : file_infos) {
        GpuFile file;
        if (file_directory_.lookup_by_id(info.file_id, file)) {
            all_files.push_back(file);
        }
    }

    return metadata_journal_.checkpoint(all_files);
}

bool BlockStorageImpl::validate_config(const BlockStorageConfig& config) {
    if (config.root_directory.empty()) {
        return false;
    }

    if (config.stripe_config.stripe_size == 0) {
        return false;
    }

    if (config.max_open_files == 0) {
        return false;
    }

    return true;
}

GpuFileHandle* BlockStorageImpl::create_file_handle(const GpuFile* file) {
    if (!file) {
        return nullptr;
    }

    GpuFileHandle* handle = new GpuFileHandle(file->file_id, backend_provider_);
    handle->target_handles.resize(file->shards.size(), nullptr);
    handle->dirty = false;

    return handle;
}

bool BlockStorageImpl::destroy_file_handle(GpuFileHandle* handle) {
    if (!handle) {
        return false;
    }

    // Release all backend handles
    for (size_t i = 0; i < handle->target_handles.size(); ++i) {
        if (handle->target_handles[i]) {
            backend_provider_->release_target_handle(handle->target_handles[i]);
        }
    }

    delete handle;
    return true;
}

bool BlockStorageImpl::is_file_open(FileId file_id) {
    return open_files_map_.find(file_id) != open_files_map_.end();
}

bool BlockStorageImpl::recover_metadata() {
    // Recover journal entries
    std::vector<JournalEntry> entries = metadata_journal_.recover();

    // Apply entries to directory
    for (const auto& entry : entries) {
        switch (entry.op_type) {
            case JournalOpType::CREATE: {
                GpuFile file(entry.file_id, entry.name, entry.logical_size, entry.stripe_size);
                file.shards = entry.shards;
                file_directory_.add_file(file);
                break;
            }
            case JournalOpType::DELETE: {
                file_directory_.remove_file(entry.name);
                break;
            }
            case JournalOpType::RESIZE: {
                // Handle resize if needed
                break;
            }
        }
    }

    return true;
}

std::unique_ptr<IBlockStorage> create_block_storage() {
    return std::make_unique<BlockStorageImpl>();
}

}  // namespace block_storage
}  // namespace tutti
