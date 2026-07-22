#pragma once

#include "coordinator_types.h"
#include "accel/include/common/accel_types.h"
#include <vector>
#include <cstdint>

namespace tutti {

// Forward declaration
struct MemoryRegion;

namespace coordinator {

// Coordinator-level IoRequest (mirrors tutti::IoRequest for the API)
struct IoRequest {
    MemoryRegion* region = nullptr;
    void* target_handle = nullptr;
    uint64_t byte_offset = 0;
    uint64_t byte_length = 0;
};

class IRawDevice {
public:
    virtual ~IRawDevice() = default;

    virtual RawTargetHandle* acquire_raw_target(
        uint32_t namespace_id,
        uint64_t start_lba,
        uint64_t length_blocks) = 0;

    virtual bool release_raw_target(RawTargetHandle* handle) = 0;

    virtual bool submit_read(
        RawTargetHandle* handle,
        MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        AccelStream stream = AccelStream()) = 0;

    virtual bool submit_write(
        RawTargetHandle* handle,
        MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        AccelStream stream = AccelStream()) = 0;

    virtual BatchSubmitResult submit_read_batch(
        RawTargetHandle* handle,
        IoRequest* requests,
        uint32_t count,
        AccelStream stream = AccelStream()) = 0;

    virtual BatchSubmitResult submit_write_batch(
        RawTargetHandle* handle,
        IoRequest* requests,
        uint32_t count,
        AccelStream stream = AccelStream()) = 0;

    virtual NamespaceInfo get_namespace_info(uint32_t namespace_id) = 0;

    virtual std::vector<uint32_t> list_namespaces() = 0;
};

} // namespace coordinator
} // namespace tutti
