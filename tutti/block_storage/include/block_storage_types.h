#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace tutti {

namespace backends {
    class IBackendProvider;
}

namespace block_storage {

using FileId = uint64_t;

constexpr FileId INVALID_FILE_ID = 0;

struct FileShard {
    uint32_t device_id;
    uint32_t namespace_id;
    uint64_t start_lba;
    uint64_t length_blocks;

    FileShard()
        : device_id(0), namespace_id(0), start_lba(0), length_blocks(0) {}

    FileShard(uint32_t dev_id, uint32_t ns_id, uint64_t lba, uint64_t len)
        : device_id(dev_id), namespace_id(ns_id), start_lba(lba), length_blocks(len) {}
};

struct GpuFile {
    FileId file_id;
    std::string name;
    uint64_t logical_size;
    uint64_t stripe_size;
    std::vector<FileShard> shards;
    std::chrono::system_clock::time_point creation_time;

    GpuFile()
        : file_id(INVALID_FILE_ID), logical_size(0), stripe_size(0),
          creation_time(std::chrono::system_clock::now()) {}

    GpuFile(FileId id, const std::string& fname, uint64_t size, uint64_t stripe)
        : file_id(id), name(fname), logical_size(size), stripe_size(stripe),
          creation_time(std::chrono::system_clock::now()) {}
};

struct GpuFileHandle {
    FileId file_id;
    std::vector<void*> target_handles;
    backends::IBackendProvider* backend_provider;
    bool dirty;

    GpuFileHandle()
        : file_id(INVALID_FILE_ID), backend_provider(nullptr), dirty(false) {}

    GpuFileHandle(FileId id, backends::IBackendProvider* provider)
        : file_id(id), backend_provider(provider), dirty(false) {}
};

struct FileInfo {
    FileId file_id;
    std::string name;
    uint64_t size;
    size_t shard_count;
    std::chrono::system_clock::time_point creation_time;

    FileInfo()
        : file_id(INVALID_FILE_ID), size(0), shard_count(0),
          creation_time(std::chrono::system_clock::now()) {}

    FileInfo(FileId id, const std::string& fname, uint64_t fsize, size_t shards,
             std::chrono::system_clock::time_point ctime)
        : file_id(id), name(fname), size(fsize), shard_count(shards), creation_time(ctime) {}
};

enum class FileOpenMode {
    READ_ONLY,
    READ_WRITE,
    CREATE_NEW,
    OPEN_OR_CREATE
};

}  // namespace block_storage
}  // namespace tutti
