#pragma once

// tutti/data_paths/striped_local_nvme/striped_arena.h
//
// Per-DataPath, bounded StripedArena for StripedDataPath.
//
// Same design as LocalNvmeDataPath's MetadataArena (see
// tutti/data_paths/local_nvme/metadata/metadata_arena.h): pre-allocates all
// per-op GPU workspace at initialize() time so submit() performs zero
// cudaMalloc/cudaEventCreate calls (Round 15 Session 5 boundary: "禁止 per-op
// cudaMalloc 简化交付").
//
// One structural difference from MetadataArena: the PRP-list page pool must
// be DMA-mapped once PER DEVICE CONTROLLER (N mappings of the same GPU
// buffer), because a PRP-list page's IOVA is only valid within the
// controller/IOMMU domain that mapped it. This mirrors how register_memory
// DMA-maps the caller's data buffer once per device (StripedMemory::dmas).
//
// submit() calls acquire() to lease a slot; release() returns it.  Arena
// exhaustion -> submit() returns RESOURCE_EXHAUSTED (no cudaMalloc fallback).
// Timeout ops use release_with_timeout_leak(): the slot is permanently
// consumed (bounded leak, same semantics as MetadataArena).

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <nvm_types.h>

namespace tutti::data_paths::local_nvme {
struct DeviceTargetHandle;
struct EntryCompletionStatus;  // defined in nvme_submit_primitives.cuh
} // namespace tutti::data_paths::local_nvme

namespace tutti::data_paths::striped_local_nvme {

// Forward declaration (defined in fused_submit_kernel.cuh).
struct StripedDeviceSubmitEntry;

// Reused verbatim from local_nvme (same struct, same layout) -- NOT a
// redeclaration: this is the SAME type as
// tutti::data_paths::local_nvme::EntryCompletionStatus, just visible under
// this namespace too so callers don't need to spell the full local_nvme::
// qualifier everywhere in this file.
using EntryCompletionStatus = tutti::data_paths::local_nvme::EntryCompletionStatus;

class StripedArena {
public:
    struct Config {
        std::uint32_t num_slots = 0;             // = 2 * max_in_flight_operations
        std::uint32_t max_entries_per_slot = 0;  // = max_batch_entries
        std::uint32_t page_size = 4096;          // NVMe page size (assumed uniform across devices)
        std::uint32_t cuda_device = 0;
        // Device table capacity per slot: max distinct (target,shard) handles
        // referenced by one submit() call.  Bounded at num_devices (N) --
        // one submit call fans a single striped target out across at most
        // its N shards.  Batches spanning multiple striped targets that
        // together reference more than N distinct handles are rejected by
        // submit() (RESOURCE_EXHAUSTED), not supported by this arena.
        std::uint32_t dev_table_capacity_per_slot = 0;
    };

    // A lease grants exclusive use of one arena slot's workspace.
    // All pointers are pre-computed at init; acquire() is O(1) with
    // zero CUDA API calls.
    struct Lease {
        std::uint32_t slot_index = UINT32_MAX;
        void* event = nullptr;                          // cudaEvent_t
        StripedDeviceSubmitEntry* d_entries = nullptr;   // GPU: entry array base
        EntryCompletionStatus* d_status = nullptr;       // GPU: status array base
        // PRP-list workspace (pre-allocated, DMA-mapped once per device).
        void* prp_pages_devptr = nullptr;    // GPU: this slot's PRP page base
        std::uint32_t prp_ioaddrs_base = 0;  // index into each device's DMA ioaddrs[]
        std::uint32_t prp_page_capacity = 0; // max PRP pages for this slot
        // Device table workspace (pre-allocated).  Host fills up to
        // dev_table_capacity entries with DeviceTargetHandle* pointers,
        // H2D-copies to d_dev_table, then the kernel indexes it by dev_idx.
        const void** d_dev_table = nullptr;  // GPU: this slot's device table base
        std::uint32_t dev_table_capacity = 0;
    };

    struct AllocCounts {
        std::uint64_t cuda_malloc = 0;
        std::uint64_t cuda_event_create = 0;
        std::uint64_t cuda_free = 0;
        std::uint64_t cuda_event_destroy = 0;
        std::uint64_t nvm_dma_map = 0;
        std::uint64_t nvm_dma_unmap = 0;
    };

    StripedArena() = default;
    ~StripedArena();

    StripedArena(const StripedArena&) = delete;
    StripedArena& operator=(const StripedArena&) = delete;

    // Pre-allocate all GPU memory, events, and PRP-list DMA mappings (one
    // per device controller in `ctrls`).  Must be called after all N
    // controllers are attached.  Returns false on any CUDA/DMA failure
    // (rolls back partial allocations).
    bool init(const Config& cfg, const std::vector<nvm_ctrl_t*>& ctrls);

    // Free all resources. Idempotent. Caller must ensure no in-flight GPU
    // work touches arena memory (sync all streams first).
    // If skip_prp is true, the PRP-list pool (all N DMA mappings + the CUDA
    // allocation) is NOT freed -- used when a timeout op's command may still
    // be in a controller queue.  Events and entry/status pools are always
    // freed.
    void shutdown(bool skip_prp = false);

    bool initialized() const { return initialized_; }
    std::uint32_t capacity() const { return cfg_.num_slots; }
    std::uint32_t available() const;

    bool acquire(Lease& out);
    void release(std::uint32_t slot_index);
    void release_with_timeout_leak(std::uint32_t slot_index);

    const AllocCounts& alloc_counts() const { return alloc_counts_; }
    void reset_alloc_counts() { alloc_counts_ = {}; }

    // Access device dev_idx's DMA mapping of the shared PRP-list pool (for
    // filling a LIST entry's prp2 with THAT device's IOVA of the page).
    const nvm_dma_t* prp_dma(std::uint32_t dev_idx) const {
        return dev_idx < prp_dmas_.size() ? prp_dmas_[dev_idx] : nullptr;
    }

private:
    Config cfg_{};
    std::vector<nvm_ctrl_t*> ctrls_;  // borrowed, one per device
    bool initialized_ = false;

    std::vector<void*> events_;  // cudaEvent_t stored as void*

    StripedDeviceSubmitEntry* d_entries_pool_ = nullptr;
    EntryCompletionStatus* d_status_pool_ = nullptr;
    void* d_dev_table_pool_ = nullptr;  // GPU: const void*[num_slots * dev_table_capacity_per_slot]

    // PRP-list pool: one contiguous GPU buffer, 64 KiB-aligned, DMA-mapped
    // ONCE PER DEVICE (prp_dmas_.size() == ctrls_.size()).
    void* prp_raw_ = nullptr;
    void* prp_aligned_ = nullptr;
    std::vector<nvm_dma_t*> prp_dmas_;
    std::size_t prp_aligned_bytes_ = 0;

    std::uint32_t prp_pages_per_slot_ = 0;

    std::deque<std::uint32_t> free_list_;
    mutable std::mutex mtx_;

    AllocCounts alloc_counts_;
};

} // namespace tutti::data_paths::striped_local_nvme
