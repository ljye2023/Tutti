#include "buffer_registry.h"
#include "accel/include/common/accel_types.h"
#include <stdexcept>
#include <mutex>

namespace tutti {
namespace coordinator {

bool BufferRegistry::add_region(MemoryRegion* region) {
    if (!region || !region->host_ptr) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(registry_lock_);

    if (ptr_to_region_map_.find(region->host_ptr) != ptr_to_region_map_.end()) {
        return false;
    }

    if (id_to_region_map_.find(region->region_id) != id_to_region_map_.end()) {
        return false;
    }

    ptr_to_region_map_[region->host_ptr] = region;
    id_to_region_map_[region->region_id] = region;

    total_registered_bytes_ += region->size;
    active_region_count_++;

    return true;
}

bool BufferRegistry::remove_region(MemoryRegion* region) {
    if (!region || !region->host_ptr) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(registry_lock_);

    auto ptr_it = ptr_to_region_map_.find(region->host_ptr);
    if (ptr_it == ptr_to_region_map_.end()) {
        return false;
    }

    auto id_it = id_to_region_map_.find(region->region_id);
    if (id_it == id_to_region_map_.end()) {
        return false;
    }

    total_registered_bytes_ -= region->size;
    active_region_count_--;

    ptr_to_region_map_.erase(ptr_it);
    id_to_region_map_.erase(id_it);

    return true;
}

MemoryRegion* BufferRegistry::lookup_by_ptr(void* ptr) const {
    if (!ptr) {
        return nullptr;
    }

    std::shared_lock<std::shared_mutex> lock(registry_lock_);

    auto it = ptr_to_region_map_.find(ptr);
    if (it == ptr_to_region_map_.end()) {
        return nullptr;
    }

    return it->second;
}

MemoryRegion* BufferRegistry::lookup_by_id(uint64_t region_id) const {
    std::shared_lock<std::shared_mutex> lock(registry_lock_);

    auto it = id_to_region_map_.find(region_id);
    if (it == id_to_region_map_.end()) {
        return nullptr;
    }

    return it->second;
}

RegistryStats BufferRegistry::get_stats() const {
    std::shared_lock<std::shared_mutex> lock(registry_lock_);

    RegistryStats stats;
    stats.total_registered_bytes = total_registered_bytes_;
    stats.active_region_count = active_region_count_;

    return stats;
}

bool BufferRegistry::is_empty() const {
    std::shared_lock<std::shared_mutex> lock(registry_lock_);
    return ptr_to_region_map_.empty();
}

} // namespace coordinator
} // namespace tutti
