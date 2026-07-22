#pragma once

#include "../include/raw_device.h"
#include "../include/coordinator_types.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/accel_types.h"
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace tutti {

// Forward declarations
class IIoEngine;

namespace coordinator {

class RawDeviceImpl : public IRawDevice {
public:
    RawDeviceImpl(backends::IBackendProvider* backend_provider, tutti::IIoEngine* io_engine);
    ~RawDeviceImpl() override;

    RawTargetHandle* acquire_raw_target(
        uint32_t namespace_id,
        uint64_t start_lba,
        uint64_t length_blocks) override;

    bool release_raw_target(RawTargetHandle* handle) override;

    bool submit_read(
        RawTargetHandle* handle,
        MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        AccelStream stream) override;

    bool submit_write(
        RawTargetHandle* handle,
        MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        AccelStream stream) override;

    BatchSubmitResult submit_read_batch(
        RawTargetHandle* handle,
        IoRequest* requests,
        uint32_t count,
        AccelStream stream) override;

    BatchSubmitResult submit_write_batch(
        RawTargetHandle* handle,
        IoRequest* requests,
        uint32_t count,
        AccelStream stream) override;

    NamespaceInfo get_namespace_info(uint32_t namespace_id) override;

    std::vector<uint32_t> list_namespaces() override;

private:
    backends::IBackendProvider* backend_provider_;
    tutti::IIoEngine* io_engine_;

    std::mutex handle_lock_;
    std::unordered_map<RawTargetHandle*, backends::StorageTarget*> handle_map_;
    std::atomic<uint64_t> next_region_id_{1};

    mutable std::mutex namespace_cache_lock_;
    std::unordered_map<uint32_t, NamespaceInfo> namespace_info_cache_;
};

} // namespace coordinator
} // namespace tutti
