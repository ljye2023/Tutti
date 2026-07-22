#pragma once

#include "block_storage_types.h"

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

namespace tutti {
namespace block_storage {

class FileDirectory {
public:
    FileDirectory();
    ~FileDirectory();

    // Add a new file to the directory
    bool add_file(const GpuFile& file);

    // Remove a file from the directory
    bool remove_file(const std::string& name);

    // Lookup file by name (returns copy to avoid use-after-free)
    bool lookup_by_name(const std::string& name, GpuFile& out_file);

    // Lookup file by ID (returns copy to avoid use-after-free)
    bool lookup_by_id(FileId file_id, GpuFile& out_file);

    // List all files
    std::vector<FileInfo> list_files() const;

    // Check if file exists
    bool exists(const std::string& name) const;

    // Generate a new unique file ID
    FileId generate_file_id();

    // Get file count
    size_t file_count() const;

private:
    mutable std::shared_mutex directory_lock_;
    std::unordered_map<std::string, GpuFile> name_to_file_map_;
    std::unordered_map<FileId, GpuFile*> id_to_file_map_;
    FileId next_file_id_;
};

}  // namespace block_storage
}  // namespace tutti
