#pragma once

#include <sys/types.h>
#include "accel/include/common/memory_region.h"
#include <unordered_map>
#include <shared_mutex>
#include <cstdint>

namespace tutti {
namespace coordinator {

struct RegistryStats {
    uint64_t total_registered_bytes = 0;
    uint32_t active_region_count = 0;
};

class BufferRegistry {
public:
    BufferRegistry() = default;
    ~BufferRegistry() = default;

    BufferRegistry(const BufferRegistry&) = delete;
    BufferRegistry& operator=(const BufferRegistry&) = delete;

    bool add_region(MemoryRegion* region);

    bool remove_region(MemoryRegion* region);

    MemoryRegion* lookup_by_ptr(void* ptr) const;

    MemoryRegion* lookup_by_id(uint64_t region_id) const;

    RegistryStats get_stats() const;

    bool is_empty() const;

private:
    mutable std::shared_mutex registry_lock_;
    std::unordered_map<void*, MemoryRegion*> ptr_to_region_map_;
    std::unordered_map<uint64_t, MemoryRegion*> id_to_region_map_;

    uint64_t total_registered_bytes_ = 0;
    uint32_t active_region_count_ = 0;
};

} // namespace coordinator
} // namespace tutti
