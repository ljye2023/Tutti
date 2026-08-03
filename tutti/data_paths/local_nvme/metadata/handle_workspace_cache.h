#pragma once

// tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h
//
// Single-tier GPU LRU cache for device target handles (DeviceTargetHandle +
// overflow extents).  Replaces the legacy two-tier TieredHandleCache for
// the LocalNvmeDataPath's working set.
//
// Design rationale (full evaluation in chat/round11/result2.md):
//   The legacy TieredHandleCache (memory/include/tiered_handle_cache.h, 644
//   lines) provides two tiers (L1 GPU / L2 host-pinned), inclusive LRU, batch
//   promote, and stream-fenced slot reuse via GpuSlotPool.  For the current
//   DataPath this is over-engineered:
//     - L2 host template is already LocalNvmeTargetState in the targets_ map.
//     - Batch promote targets "thousands of new files per IO batch" — the
//       current DataPath's batch is per-op (all entries target the same file).
//     - Stream-fenced slot reuse assumes multi-stream concurrent access to
//       the same cache slot — the DataPath is single-threaded for open/close.
//   A simple single-tier (GPU only) LRU with explicit pin/unpin is sufficient.
//
// Pin semantics:
//   submit() pins the target's cache entry; release() unpins.
//   Pin count > 0 ⇒ entry is protected from eviction.
//   Pin count == 0 ⇒ entry is evictable (LRU candidate).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tutti::data_paths::local_nvme {

// Forward declarations (defined in io/device_target.h).
struct DeviceTargetHandle;
struct DeviceLbaExtent;

class HandleWorkspaceCache {
public:
    struct Config {
        std::uint32_t capacity = 0;     // 0 = disabled
        std::uint32_t cuda_device = 0;
    };

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint32_t pinned = 0;       // current total pin count
        std::uint32_t entries = 0;      // current entry count
    };

    // Entry: owns DeviceTargetHandle* + overflow GPU buffer.
    // Returned as a borrowed pointer; caller must NOT free handle/overflow.
    struct Entry {
        std::uint64_t key = 0;
        DeviceTargetHandle* handle = nullptr;
        void* overflow = nullptr;
        std::uint32_t pin_count = 0;
        bool in_use = false;  // true while a target is open (protected from eviction)
    };

    HandleWorkspaceCache() = default;
    ~HandleWorkspaceCache() { shutdown(); }

    HandleWorkspaceCache(const HandleWorkspaceCache&) = delete;
    HandleWorkspaceCache& operator=(const HandleWorkspaceCache&) = delete;

    // Pre-allocate the entry pool.  Does NOT allocate GPU memory per entry —
    // GPU allocations happen lazily on get_or_build() misses.
    bool init(const Config& cfg) {
        if (cfg.capacity == 0) { cfg_ = cfg; return true; }
        entries_.resize(cfg.capacity);
        free_list_.clear();
        for (std::uint32_t i = 0; i < cfg.capacity; ++i)
            free_list_.push_back(i);
        index_.clear();
        lru_.clear();
        lru_pos_.clear();
        stats_ = {};
        cfg_ = cfg;
        initialized_ = true;
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!initialized_) return;
        // Free all GPU allocations owned by entries.
        for (auto& e : entries_) {
            if (e.handle != nullptr || e.overflow != nullptr) {
                free_entry_gpu_(e);
            }
            e = {};
        }
        entries_.clear();
        free_list_.clear();
        index_.clear();
        lru_.clear();
        lru_pos_.clear();
        stats_ = {};
        initialized_ = false;
    }

    bool enabled() const { return cfg_.capacity > 0 && initialized_; }
    std::uint32_t capacity() const { return cfg_.capacity; }

    // Get or build a handle entry.
    // On hit: returns cached Entry* (no GPU alloc / H2D).
    // On miss: calls build_fn(handle_out, overflow_out) to create the GPU
    //   handle.  If the pool is full, evicts the LRU unpinned entry first.
    // Returns nullptr on failure (build_fn failed or pool exhausted with
    //   nothing evictable).
    template <typename BuildFn>
    Entry* get_or_build(std::uint64_t key, BuildFn&& build_fn) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!enabled()) return nullptr;

        auto it = index_.find(key);
        if (it != index_.end()) {
            std::uint32_t slot = it->second;
            touch_lru_(slot);
            ++stats_.hits;
            return &entries_[slot];
        }

        ++stats_.misses;

        std::uint32_t slot = acquire_slot_();
        if (slot == UINT32_MAX) return nullptr;

        Entry& e = entries_[slot];
        e.key = key;
        e.pin_count = 0;
        e.in_use = true;  // protected while target is open
        if (!build_fn(&e.handle, &e.overflow)) {
            // Build failed: return slot to free list.
            free_list_.push_back(slot);
            e = {};
            return nullptr;
        }
        index_[key] = slot;
        // Do NOT add to LRU yet — entry is in_use (target open).
        // release_entry() adds it to LRU when the target is closed.
        ++stats_.entries;
        return &e;
    }

    // Release an entry: mark as no longer in-use by an open target.
    // The entry becomes evictable (added to LRU) if not pinned.
    // Called by close().
    void release_entry(Entry* e) {
        if (!e) return;
        std::lock_guard<std::mutex> lock(mtx_);
        e->in_use = false;
        if (e->pin_count == 0) {
            std::uint32_t slot = static_cast<std::uint32_t>(e - entries_.data());
            if (index_.count(e->key) && !lru_pos_.count(slot)) {
                lru_.push_front(slot);
                lru_pos_[slot] = lru_.begin();
            }
        }
    }

    // Pin: protects the entry from eviction.  Called by submit().
    void pin(Entry* e) {
        if (!e) return;
        std::lock_guard<std::mutex> lock(mtx_);
        ++e->pin_count;
        ++stats_.pinned;
        // Remove from LRU (pinned entries are not evictable).
        auto it = lru_pos_.find(static_cast<std::uint32_t>(e - entries_.data()));
        if (it != lru_pos_.end()) {
            lru_.erase(it->second);
            lru_pos_.erase(it);
        }
    }

    // Unpin: makes the entry evictable again (if not in_use).  Called by release().
    void unpin(Entry* e) {
        if (!e) return;
        std::lock_guard<std::mutex> lock(mtx_);
        if (e->pin_count > 0) {
            --e->pin_count;
            --stats_.pinned;
        }
        if (e->pin_count == 0 && !e->in_use) {
            std::uint32_t slot = static_cast<std::uint32_t>(e - entries_.data());
            if (index_.count(e->key) && !lru_pos_.count(slot)) {
                lru_.push_front(slot);
                lru_pos_[slot] = lru_.begin();
            }
        }
    }

    // Erase: explicitly remove an entry (frees GPU memory).
    // Used when the caller knows the handle is no longer needed
    // (e.g., DataPath shutdown or explicit invalidation).
    void erase(std::uint64_t key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it == index_.end()) return;
        std::uint32_t slot = it->second;
        Entry& e = entries_[slot];
        if (e.pin_count > 0) return;  // pinned: cannot erase
        free_entry_gpu_(e);
        remove_from_lru_(slot);
        index_.erase(it);
        e = {};
        free_list_.push_back(slot);
        if (stats_.entries > 0) --stats_.entries;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return stats_;
    }

private:
    Config cfg_{};
    bool initialized_ = false;
    std::vector<Entry> entries_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
    std::list<std::uint32_t> lru_;  // unpinned, in-index entries; MRU front
    std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> lru_pos_;
    std::list<std::uint32_t> free_list_;
    mutable std::mutex mtx_;
    Stats stats_{};

    // Free GPU allocations for an entry (called under lock).
    void free_entry_gpu_(Entry& e) {
        // Calls free_device_target() — declared in io/device_target.h.
        // We forward through a function pointer set by the DataPath.
        if (free_fn_ && (e.handle != nullptr || e.overflow != nullptr)) {
            free_fn_(e.handle, e.overflow, cfg_.cuda_device);
        }
        e.handle = nullptr;
        e.overflow = nullptr;
    }

    std::uint32_t acquire_slot_() {
        if (!free_list_.empty()) {
            std::uint32_t s = free_list_.front();
            free_list_.pop_front();
            return s;
        }
        // Evict LRU unpinned entry.
        if (lru_.empty()) return UINT32_MAX;  // all pinned
        std::uint32_t victim = lru_.back();
        lru_.pop_back();
        lru_pos_.erase(victim);
        Entry& ve = entries_[victim];
        free_entry_gpu_(ve);
        index_.erase(ve.key);
        ve = {};
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

public:
    // Function pointer for freeing GPU allocations.
    // Set by LocalNvmeDataPath::initialize() to &free_device_target.
    // This avoids a hard dependency on io/device_target.h in this header.
    using FreeFn = void(*)(DeviceTargetHandle*, void*, std::uint32_t);
    void set_free_fn(FreeFn fn) { free_fn_ = fn; }

private:
    FreeFn free_fn_ = nullptr;
};

} // namespace tutti::data_paths::local_nvme
