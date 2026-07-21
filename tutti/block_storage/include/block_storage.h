// tutti/block_storage/include/block_storage.h
// Layer 5: Block Storage Interface
//
// High-level block storage abstraction (GpuFile-based)

#pragma once

#include "tutti/accel/include/accel_types.h"
#include "tutti/types/storage_target.h"
#include <string>
#include <memory>
#include <cstddef>

namespace tutti {

// Forward declarations
class GpuFile;
class IIoEngine;

// Block storage interface
class IBlockStorage {
public:
    virtual ~IBlockStorage() = default;

    // File operations
    virtual GpuFile* open_file(const std::string& path) = 0;
    virtual void close_file(GpuFile* file) = 0;

    // I/O operations
    virtual int read(
        GpuFile* file,
        void* buffer,
        size_t offset,
        size_t size,
        AccelStream stream) = 0;

    virtual int write(
        GpuFile* file,
        const void* buffer,
        size_t offset,
        size_t size,
        AccelStream stream) = 0;
};

} // namespace tutti
