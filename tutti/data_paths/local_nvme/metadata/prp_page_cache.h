#pragma once

// tutti/data_paths/local_nvme/metadata/prp_page_cache.h
//
// Single-tier content-addressed LRU cache for PRP-list pages.
// Replaces the legacy two-tier PrpPageCache (memory/include/prp_page_cache.h,
// 426 lines) for the LocalNvmeDataPath's LIST sub-IO path.
//
// Design rationale (full evaluation in chat/round11/result2.md):
//   The legacy PrpPageCache provides two tiers (L2 host-pinned content / L1
//   GPU-DMA-mapped), a scatter-kernel prp2 patch mechanism, and event-fenced
//   slot reuse.  For the current DataPath this is over-engineered:
//     - The prp2 patch problem (IOVA changes on tier move) doesn't exist
//       when each cache slot has a fixed IOVA from a pre-allocated pool.
//     - Event-fenced slot reuse assumes multi-stream concurrent access — the
//       DataPath is single-threaded for submit/progress/release.
//     - The L2 host tier only saves the content rebuild (cheap CPU work);
//       the real cost is the H2D copy, which the L1 tier already avoids.
//   A simple single-tier (GPU, own DMA-mapped pool) content-addressed LRU
//   with pin/unpin is sufficient.
//
// DMA lifecycle:
//   The cache owns one contiguous GPU allocation (cudaMalloc) and one shared
//   DMA mapping (nvm_dma_map_data_device) covering all capacity pages.
//   - Born together: init() allocates both.
//   - Die together: shutdown() unmaps DMA first, then frees GPU memory.
//   - Per-page eviction: marks slot reusable (no unmap/free).  The pin
//     mechanism ensures no in-flight op references an evicted slot.
//   - This satisfies "DMA mapping lives/dies with backing page": the shared
//     mapping and backing allocation have identical lifetimes.
//
// Content key:
//   {memory_token, start_page, pages_in_io} uniquely determines the PRP-list
//   page content because content = ioaddrs[start_page+1..start_page+pages_in_io-1].
//   If memory is re-registered (new token), old entries are never hit again.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <nvm_types.h>

namespace tutti::data_paths::local_nvme {

class PrpPageCache {
public:
    struct Config {
        std::uint32_t capacity = 0;     // 0 = disabled; number of PRP-list pages
        std::uint32_t page_size = 4096; // NVMe page size
        std::uint32_t cuda_device = 0;
    };

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint32_t pinned = 0;
        std::uint32_t entries = 0;
    };

    // Key: identifies a unique PRP-list page content.
    struct Key {
        std::uint64_t memory_token = 0;
        std::uint32_t start_page = 0;
        std::uint32_t pages_in_io = 0;
        bool operator==(const Key& o) const {
            return memory_token == o.memory_token &&
                   start_page == o.start_page &&
                   pages_in_io == o.pages_in_io;
        }
    };

    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return std::hash<std::uint64_t>()(k.memory_token) ^
                   (std::hash<std::uint32_t>()(k.start_page) << 1) ^
                   (std::hash<std::uint32_t>()(k.pages_in_io) << 2);
        }
    };

    // Entry: a cached PRP-list page.
    struct Entry {
        Key key{};
        void* devptr = nullptr;       // GPU pointer to the page (within pool)
        std::uint64_t ioaddr = 0;     // DMA IOVA of the page
        std::uint32_t pin_count = 0;
        std::uint32_t checkout_refcount = 0;  // P0-1: >0 while checked out by submit
        bool in_use = false;  // deprecated: superseded by checkout_refcount
    };

    PrpPageCache() = default;
    ~PrpPageCache() { shutdown(); }

    PrpPageCache(const PrpPageCache&) = delete;
    PrpPageCache& operator=(const PrpPageCache&) = delete;

    // Pre-allocate the pool: one cudaMalloc + one nvm_dma_map_data_device.
    bool init(const Config& cfg, nvm_ctrl_t* ctrl);
    void shutdown();

    bool enabled() const { return cfg_.capacity > 0 && initialized_; }
    std::uint32_t capacity() const { return cfg_.capacity; }

    // Get or build a PRP-list page.
    // On hit: returns cached Entry* (no H2D fill).
    // On miss: calls fill_fn(devptr) to H2D the content into the cache page.
    // Returns nullptr on failure.
    template <typename FillFn>
    Entry* get_or_build(const Key& key, FillFn&& fill_fn) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!enabled()) return nullptr;

        auto it = index_.find(key);
        if (it != index_.end()) {
            std::uint32_t slot = it->second;
            Entry& hit = entries_[slot];
            ++hit.checkout_refcount;  // P0-1: re-checkout increments refcount
            hit.in_use = true;        // backward compat
            remove_from_lru_(slot);   // checked-out entries are not evictable
            ++stats_.hits;
            return &hit;
        }

        ++stats_.misses;

        std::uint32_t slot = acquire_slot_();
        if (slot == UINT32_MAX) return nullptr;

        Entry& e = entries_[slot];
        e.key = key;
        e.pin_count = 0;
        e.checkout_refcount = 1;  // P0-1: new entry starts with one checkout
        e.in_use = true;  // backward compat
        e.devptr = static_cast<char*>(pool_aligned_) +
                   static_cast<std::size_t>(slot) * cfg_.page_size;
        e.ioaddr = pool_dma_->ioaddrs[slot];

        if (!fill_fn(e.devptr)) {
            // Fill failed: return slot to free list.
            free_list_.push_back(slot);
            e = {};
            return nullptr;
        }

        index_[key] = slot;
        // Do NOT add to LRU — entry is in_use (checked out by submit).
        // unpin() adds it to LRU when the op releases it.
        ++stats_.entries;
        return &e;
    }

    void pin(Entry* e) {
        if (!e) return;
        std::lock_guard<std::mutex> lock(mtx_);
        // P0-1: decrement checkout_refcount (the submit path checked it out,
        // now it's being pinned for the op's lifetime).
        if (e->checkout_refcount > 0) --e->checkout_refcount;
        e->in_use = false;  // no longer just "checked out" — now pinned
        ++e->pin_count;
        ++stats_.pinned;
        auto it = lru_pos_.find(static_cast<std::uint32_t>(e - entries_.data()));
        if (it != lru_pos_.end()) {
            lru_.erase(it->second);
            lru_pos_.erase(it);
        }
    }

    void unpin(Entry* e) {
        if (!e) return;
        std::lock_guard<std::mutex> lock(mtx_);
        if (e->pin_count > 0) {
            --e->pin_count;
            --stats_.pinned;
        }
        if (e->pin_count == 0 && e->checkout_refcount == 0) {
            std::uint32_t slot = static_cast<std::uint32_t>(e - entries_.data());
            if (index_.count(e->key) && !lru_pos_.count(slot)) {
                lru_.push_front(slot);
                lru_pos_[slot] = lru_.begin();
            }
        }
    }

    // Invalidate all entries for a given memory token (called on unregister).
    // Pinned entries are skipped (in-flight op still references them).
    void invalidate_memory(std::uint64_t memory_token) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Key> to_erase;
        for (auto& [k, slot] : index_) {
            if (k.memory_token == memory_token && entries_[slot].pin_count == 0 &&
                entries_[slot].checkout_refcount == 0) {
                to_erase.push_back(k);
            }
        }
        for (const auto& k : to_erase) {
            auto it = index_.find(k);
            if (it == index_.end()) continue;
            std::uint32_t slot = it->second;
            remove_from_lru_(slot);
            entries_[slot] = {};
            free_list_.push_back(slot);
            index_.erase(it);
            if (stats_.entries > 0) --stats_.entries;
        }
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return stats_;
    }

    // Test accessor: check if a page's DMA mapping is active.
    // Returns true if the pool DMA mapping exists (cache initialized).
    bool test_dma_active() const { return pool_dma_ != nullptr; }

private:
    Config cfg_{};
    nvm_ctrl_t* ctrl_ = nullptr;
    bool initialized_ = false;

    // Pool: one contiguous GPU allocation, DMA-mapped as a whole.
    void* pool_raw_ = nullptr;
    void* pool_aligned_ = nullptr;
    nvm_dma_t* pool_dma_ = nullptr;

    std::vector<Entry> entries_;
    std::unordered_map<Key, std::uint32_t, KeyHash> index_;
    std::list<std::uint32_t> lru_;
    std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> lru_pos_;
    std::list<std::uint32_t> free_list_;
    mutable std::mutex mtx_;
    Stats stats_{};

    std::uint32_t acquire_slot_() {
        if (!free_list_.empty()) {
            std::uint32_t s = free_list_.front();
            free_list_.pop_front();
            return s;
        }
        if (lru_.empty()) return UINT32_MAX;
        std::uint32_t victim = lru_.back();
        lru_.pop_back();
        lru_pos_.erase(victim);
        index_.erase(entries_[victim].key);
        entries_[victim] = {};
        if (stats_.entries > 0) --stats_.entries;
        ++stats_.evictions;
        return victim;
    }

    void touch_lru_(std::uint32_t slot) {
        auto it = lru_pos_.find(slot);
        if (it != lru_pos_.end()) lru_.erase(it->second);
        lru_.push_front(slot);
        lru_pos_[slot] = lru_.begin();
    }

    void remove_from_lru_(std::uint32_t slot) {
        auto it = lru_pos_.find(slot);
        if (it != lru_pos_.end()) {
            lru_.erase(it->second);
            lru_pos_.erase(it);
        }
    }
};

} // namespace tutti::data_paths::local_nvme
