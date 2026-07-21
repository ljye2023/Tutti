// tutti/block_storage/src/block_storage_impl.h
// Layer 5: Block Storage - Implementation (private header)

#pragma once

#include "tutti/block_storage/include/block_storage.h"
#include "tutti/io_engine/include/io_engine.h"
#include <memory>
#include <unordered_map>

namespace tutti {

class BlockStorageImpl : public IBlockStorage {
public:
    BlockStorageImpl(IIoEngine* io_engine);
    virtual ~BlockStorageImpl();

    // IBlockStorage interface
    GpuFile* open_file(const std::string& path) override;
    void close_file(GpuFile* file) override;

    int read(
        GpuFile* file,
        void* buffer,
        size_t offset,
        size_t size,
        AccelStream stream) override;

    int write(
        GpuFile* file,
        const void* buffer,
        size_t offset,
        size_t size,
        AccelStream stream) override;

private:
    IIoEngine* io_engine_;
    std::unordered_map<std::string, std::unique_ptr<GpuFile>> open_files_;
};

} // namespace tutti
