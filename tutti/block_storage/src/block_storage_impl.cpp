// tutti/block_storage/src/block_storage_impl.cpp
// Layer 5: Block Storage - Implementation

#include "block_storage_impl.h"
#include "tutti/block_storage/include/gpu_file.h"

namespace tutti {

BlockStorageImpl::BlockStorageImpl(IIoEngine* io_engine)
    : io_engine_(io_engine) {
}

BlockStorageImpl::~BlockStorageImpl() {
    open_files_.clear();
}

GpuFile* BlockStorageImpl::open_file(const std::string& path) {
    auto it = open_files_.find(path);
    if (it != open_files_.end()) {
        return it->second.get();
    }

    // TODO: Get actual file size
    auto file = std::make_unique<GpuFile>(path, 0);
    GpuFile* file_ptr = file.get();
    open_files_[path] = std::move(file);

    return file_ptr;
}

void BlockStorageImpl::close_file(GpuFile* file) {
    if (!file) {
        return;
    }

    open_files_.erase(file->get_path());
}

int BlockStorageImpl::read(
    GpuFile* file,
    void* buffer,
    size_t offset,
    size_t size,
    AccelStream stream)
{
    if (!file || !buffer) {
        return -1;
    }

    // TODO: Convert file offset to IoRequest and submit
    (void)offset;
    (void)size;
    (void)stream;

    return 0;
}

int BlockStorageImpl::write(
    GpuFile* file,
    const void* buffer,
    size_t offset,
    size_t size,
    AccelStream stream)
{
    if (!file || !buffer) {
        return -1;
    }

    // TODO: Convert file offset to IoRequest and submit
    (void)offset;
    (void)size;
    (void)stream;

    return 0;
}

} // namespace tutti
