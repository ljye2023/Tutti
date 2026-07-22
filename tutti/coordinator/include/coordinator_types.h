#pragma once

#include <sys/types.h>
#include "accel/include/common/accel_types.h"
#include "accel/include/common/memory_region.h"
#include "backends/include/backend_types.h"
#include <cstdint>
#include <string>
#include <chrono>

namespace tutti {

// Forward declarations from other layers
class IAccelerator;
class IIoEngine;

namespace backends {
class IBackendProvider;
}

namespace block_storage {
class IBlockStorage;
}

namespace coordinator {

class IRawDevice;

struct CoordinatorConfig {
    backends::IBackendProvider* backend_provider = nullptr;
    IAccelerator* accelerator = nullptr;
    block_storage::IBlockStorage* block_storage = nullptr;
    IIoEngine* io_engine = nullptr;

    uint32_t max_batch_size = 128;
    AccelStream default_stream;

    bool is_valid() const {
        return backend_provider && accelerator &&
               block_storage && io_engine && max_batch_size > 0;
    }
};

struct RawTargetHandle {
    uint32_t namespace_id = 0;
    uint64_t start_lba = 0;
    uint64_t length_blocks = 0;

    void* target_handle = nullptr;
    uint64_t region_id = 0;

    RawTargetHandle() = default;
    RawTargetHandle(uint32_t ns_id, uint64_t lba, uint64_t len)
        : namespace_id(ns_id), start_lba(lba), length_blocks(len) {}
};

struct BatchSubmitResult {
    bool success = false;
    uint32_t completed_count = 0;
    uint32_t failed_count = 0;
    int32_t error_code = 0;

    BatchSubmitResult() = default;
    BatchSubmitResult(bool ok, uint32_t completed = 0, uint32_t failed = 0, int32_t err = 0)
        : success(ok), completed_count(completed), failed_count(failed), error_code(err) {}
};

struct BufferRegistrationInfo {
    MemoryRegion* region = nullptr;
    std::chrono::steady_clock::time_point registered_time;
    std::chrono::steady_clock::time_point last_access_time;
    uint64_t access_count = 0;

    BufferRegistrationInfo() = default;
    explicit BufferRegistrationInfo(MemoryRegion* r)
        : region(r),
          registered_time(std::chrono::steady_clock::now()),
          last_access_time(registered_time),
          access_count(0) {}
};

struct NamespaceInfo {
    uint32_t namespace_id = 0;
    uint32_t block_size = 0;
    uint64_t capacity_blocks = 0;
    uint32_t mdts_bytes = 0;

    NamespaceInfo() = default;
    NamespaceInfo(uint32_t ns_id, uint32_t bs, uint64_t cap, uint32_t mdts)
        : namespace_id(ns_id), block_size(bs), capacity_blocks(cap), mdts_bytes(mdts) {}
};

using BatchCompletionCallback = void (*)(void* user_data, const BatchSubmitResult& result);

} // namespace coordinator
} // namespace tutti
