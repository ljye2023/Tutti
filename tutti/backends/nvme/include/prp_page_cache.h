#ifndef TUTTI_BACKENDS_NVME_PRP_PAGE_CACHE_H_
#define TUTTI_BACKENDS_NVME_PRP_PAGE_CACHE_H_

#include <cstdint>
#include <cstddef>
#include <vector>
#include <stack>
#include <mutex>

namespace tutti {
namespace backends {
namespace nvme {

// Two-tier PRP-list page cache to avoid hot-path allocation overhead.
//
// Design:
//   L1 (GPU-resident): small pool of pre-allocated 4KB pages on GPU for list pages
//   L2 (host-pinned): larger pool of pinned host pages for staging/overflow
//
// Allocation strategy:
//   1. Try L1 free list (fast path, GPU-resident)
//   2. Fall back to L2 free list (host-pinned, needs cudaMemcpy for use)
//   3. Last resort: cudaMalloc (slow, amortized by cache)
//
// Free strategy:
//   1. Return to L1 if not full (preferred)
//   2. Return to L2 if L1 full
//   3. Never immediate cudaFree (amortize alloc cost across operations)
//
// Sizing guidance:
//   L1 pool: ~2 pages per queue typical (queue_quota * 2)
//   L2 pool: 4x larger for burst absorption
//
// Thread safety: mutex protects free lists for concurrent descriptor preparation.
class PrpPageCache {
public:
    // page_size: typically 4096 bytes (NVMe standard)
    // l1_capacity: number of GPU-resident pages to pre-allocate
    // l2_capacity: number of host-pinned pages to pre-allocate
    PrpPageCache(size_t page_size, size_t l1_capacity, size_t l2_capacity);
    ~PrpPageCache();

    // Disable copy/move - cache manages GPU memory pointers
    PrpPageCache(const PrpPageCache&) = delete;
    PrpPageCache& operator=(const PrpPageCache&) = delete;

    // Allocate a GPU-resident PRP-list page.
    // Returns device pointer to 4KB page, or nullptr on allocation failure.
    void* allocate_gpu_page();

    // Allocate a host-pinned page (for staging or CPU-side descriptor construction).
    // Returns host pointer to 4KB page, or nullptr on allocation failure.
    void* allocate_host_page();

    // Free GPU-resident page - returns to L1 cache if space available.
    void free_gpu_page(void* page);

    // Free host-pinned page - returns to L2 cache if space available.
    void free_host_page(void* page);

    // Statistics for monitoring cache effectiveness
    struct Stats {
        size_t l1_hits;           // Allocations served from L1
        size_t l2_hits;           // Allocations served from L2
        size_t l1_misses;         // Allocations requiring cudaMalloc
        size_t l1_size;           // Current L1 pool size
        size_t l2_size;           // Current L2 pool size
        size_t l1_capacity;       // Max L1 capacity
        size_t l2_capacity;       // Max L2 capacity
    };

    Stats get_stats() const;
    void reset_stats();

private:
    const size_t page_size_;
    const size_t l1_capacity_;
    const size_t l2_capacity_;

    // L1 pool: GPU-resident pages
    std::vector<void*> l1_pool_;
    std::stack<void*> l1_free_list_;

    // L2 pool: host-pinned pages
    std::vector<void*> l2_pool_;
    std::stack<void*> l2_free_list_;

    // Thread safety
    mutable std::mutex mutex_;

    // Statistics
    mutable Stats stats_;

    // Internal helpers
    void* allocate_gpu_page_slow();  // cudaMalloc fallback
    void* allocate_host_page_slow(); // cudaMallocHost fallback
    bool is_from_l1_pool(void* page) const;
    bool is_from_l2_pool(void* page) const;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_PRP_PAGE_CACHE_H_
