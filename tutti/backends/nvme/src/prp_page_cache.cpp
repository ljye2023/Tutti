#include "backends/include/storage_target.h"
#include "prp_page_cache.h"
#include <cuda_runtime.h>
#include <cstring>
#include <algorithm>

namespace tutti {
namespace backends {
namespace nvme {

PrpPageCache::PrpPageCache(size_t page_size, size_t l1_capacity, size_t l2_capacity)
    : page_size_(page_size)
    , l1_capacity_(l1_capacity)
    , l2_capacity_(l2_capacity)
    , stats_{0, 0, 0, 0, 0, l1_capacity, l2_capacity}
{
    // Pre-allocate L1 pool (GPU-resident)
    l1_pool_.reserve(l1_capacity);
    for (size_t i = 0; i < l1_capacity; ++i) {
        void* page = nullptr;
        if (cudaMalloc(&page, page_size) == cudaSuccess && page != nullptr) {
            l1_pool_.push_back(page);
            l1_free_list_.push(page);
        }
    }

    // Pre-allocate L2 pool (host-pinned)
    l2_pool_.reserve(l2_capacity);
    for (size_t i = 0; i < l2_capacity; ++i) {
        void* page = nullptr;
        if (cudaMallocHost(&page, page_size) == cudaSuccess && page != nullptr) {
            l2_pool_.push_back(page);
            l2_free_list_.push(page);
        }
    }

    stats_.l1_size = l1_pool_.size();
    stats_.l2_size = l2_pool_.size();
}

PrpPageCache::~PrpPageCache() {
    // Free L1 pool (GPU-resident)
    for (void* page : l1_pool_) {
        cudaFree(page);
    }

    // Free L2 pool (host-pinned)
    for (void* page : l2_pool_) {
        cudaFreeHost(page);
    }
}

void* PrpPageCache::allocate_gpu_page() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Fast path: L1 cache hit
    if (!l1_free_list_.empty()) {
        void* page = l1_free_list_.top();
        l1_free_list_.pop();
        stats_.l1_hits++;
        return page;
    }

    // Slow path: cudaMalloc
    stats_.l1_misses++;
    return allocate_gpu_page_slow();
}

void* PrpPageCache::allocate_host_page() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Fast path: L2 cache hit
    if (!l2_free_list_.empty()) {
        void* page = l2_free_list_.top();
        l2_free_list_.pop();
        stats_.l2_hits++;
        return page;
    }

    // Slow path: cudaMallocHost
    return allocate_host_page_slow();
}

void PrpPageCache::free_gpu_page(void* page) {
    if (page == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Return to L1 cache if from pool and not full
    if (is_from_l1_pool(page)) {
        if (l1_free_list_.size() < l1_capacity_) {
            l1_free_list_.push(page);
            return;
        }
    }

    // L1 full or not from pool - immediate free
    cudaFree(page);
}

void PrpPageCache::free_host_page(void* page) {
    if (page == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Return to L2 cache if from pool and not full
    if (is_from_l2_pool(page)) {
        if (l2_free_list_.size() < l2_capacity_) {
            l2_free_list_.push(page);
            return;
        }
    }

    // L2 full or not from pool - immediate free
    cudaFreeHost(page);
}

PrpPageCache::Stats PrpPageCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void PrpPageCache::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.l1_hits = 0;
    stats_.l2_hits = 0;
    stats_.l1_misses = 0;
}

void* PrpPageCache::allocate_gpu_page_slow() {
    // Called with mutex held
    void* page = nullptr;
    if (cudaMalloc(&page, page_size_) != cudaSuccess) {
        return nullptr;
    }
    return page;
}

void* PrpPageCache::allocate_host_page_slow() {
    // Called with mutex held
    void* page = nullptr;
    if (cudaMallocHost(&page, page_size_) != cudaSuccess) {
        return nullptr;
    }
    return page;
}

bool PrpPageCache::is_from_l1_pool(void* page) const {
    // Called with mutex held
    return std::find(l1_pool_.begin(), l1_pool_.end(), page) != l1_pool_.end();
}

bool PrpPageCache::is_from_l2_pool(void* page) const {
    // Called with mutex held
    return std::find(l2_pool_.begin(), l2_pool_.end(), page) != l2_pool_.end();
}

} // namespace nvme
} // namespace backends
} // namespace tutti
