#pragma once

#include "block_storage_types.h"
#include "storage_config.h"

#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

namespace tutti {

namespace backends {
    class IBackendProvider;
}

namespace coordinator {
    class IRawDevice;
    struct NamespaceInfo;
}

namespace block_storage {

// Per-device LBA allocator for tracking space usage
struct DeviceLbaAllocator {
    uint32_t namespace_id;
    uint64_t total_blocks;
    uint64_t next_free_lba;
    uint64_t allocated_blocks;

    DeviceLbaAllocator()
        : namespace_id(0), total_blocks(0), next_free_lba(0), allocated_blocks(0) {}
};

class StripeManager {
public:
    StripeManager();
    ~StripeManager();

    // Initialize with backend provider, raw device interface, and stripe configuration
    // raw_device provides device enumeration and namespace info
    bool initialize(backends::IBackendProvider* backend_provider,
                   coordinator::IRawDevice* raw_device,
                   const StripeConfig& config);

    // Allocate shards for a file
    std::vector<FileShard> allocate_shards(uint64_t file_size,
                                           uint64_t stripe_size);

    // Deallocate shards
    bool deallocate_shards(const std::vector<FileShard>& shards);

    // Get number of available devices
    size_t get_device_count() const;

private:
    backends::IBackendProvider* backend_provider_;
    coordinator::IRawDevice* raw_device_;
    StripeConfig config_;
    std::vector<uint32_t> available_namespaces_;
    std::map<uint32_t, DeviceLbaAllocator> lba_allocators_;  // Per namespace
    size_t next_device_index_;  // For round-robin allocation
    std::mutex allocation_mutex_;  // Protects lba_allocators_ and next_device_index_
};

}  // namespace block_storage
}  // namespace tutti
