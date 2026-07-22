#include "file_directory.h"

#include <algorithm>
#include <mutex>

namespace tutti {
namespace block_storage {

FileDirectory::FileDirectory()
    : next_file_id_(1) {
}

FileDirectory::~FileDirectory() {
}

bool FileDirectory::add_file(const GpuFile& file) {
    std::unique_lock<std::shared_mutex> lock(directory_lock_);

    // Check if file already exists
    if (name_to_file_map_.find(file.name) != name_to_file_map_.end()) {
        return false;
    }

    // Insert into maps
    auto result = name_to_file_map_.insert({file.name, file});
    if (!result.second) {
        return false;
    }

    id_to_file_map_[file.file_id] = &result.first->second;
    return true;
}

bool FileDirectory::remove_file(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(directory_lock_);

    auto it = name_to_file_map_.find(name);
    if (it == name_to_file_map_.end()) {
        return false;
    }

    FileId file_id = it->second.file_id;
    id_to_file_map_.erase(file_id);
    name_to_file_map_.erase(it);

    return true;
}

bool FileDirectory::lookup_by_name(const std::string& name, GpuFile& out_file) {
    std::shared_lock<std::shared_mutex> lock(directory_lock_);

    auto it = name_to_file_map_.find(name);
    if (it == name_to_file_map_.end()) {
        return false;
    }

    out_file = it->second;
    return true;
}

bool FileDirectory::lookup_by_id(FileId file_id, GpuFile& out_file) {
    std::shared_lock<std::shared_mutex> lock(directory_lock_);

    auto it = id_to_file_map_.find(file_id);
    if (it == id_to_file_map_.end()) {
        return false;
    }

    out_file = *it->second;
    return true;
}

std::vector<FileInfo> FileDirectory::list_files() const {
    std::shared_lock<std::shared_mutex> lock(directory_lock_);

    std::vector<FileInfo> result;
    result.reserve(name_to_file_map_.size());

    for (const auto& pair : name_to_file_map_) {
        const GpuFile& file = pair.second;
        result.emplace_back(
            file.file_id,
            file.name,
            file.logical_size,
            file.shards.size(),
            file.creation_time
        );
    }

    return result;
}

bool FileDirectory::exists(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(directory_lock_);
    return name_to_file_map_.find(name) != name_to_file_map_.end();
}

FileId FileDirectory::generate_file_id() {
    std::unique_lock<std::shared_mutex> lock(directory_lock_);
    return next_file_id_++;
}

size_t FileDirectory::file_count() const {
    std::shared_lock<std::shared_mutex> lock(directory_lock_);
    return name_to_file_map_.size();
}

}  // namespace block_storage
}  // namespace tutti
