// tutti/data_paths/local_nvme/metadata/metadata_arena.cpp

#include "tutti/data_paths/local_nvme/metadata/metadata_arena.h"

#include <cuda_runtime.h>
#include <nvm_types.h>
#include <nvm_dma.h>

#include "tutti/data_paths/local_nvme/io/submit_one.cuh"

#include <cstdio>
#include <cstring>

namespace tutti::data_paths::local_nvme {

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

MetadataArena::~MetadataArena() {
    shutdown();
}

// -----------------------------------------------------------------------
// init: pre-allocate all workspace
// -----------------------------------------------------------------------

bool MetadataArena::init(const Config& cfg, nvm_ctrl_t* ctrl) {
    if (initialized_) return false;
    if (cfg.num_slots == 0 || cfg.max_entries_per_slot == 0 || ctrl == nullptr) {
        return false;
    }

    cfg_ = cfg;
    ctrl_ = ctrl;

    int prev_dev = -1;
    cudaError_t ce = cudaGetDevice(&prev_dev);
    if (ce != cudaSuccess) return false;
    ce = cudaSetDevice(cfg_.cuda_device);
    if (ce != cudaSuccess) return false;

    // 1. Pre-create events (one per slot).
    events_.resize(cfg_.num_slots, nullptr);
    for (std::uint32_t i = 0; i < cfg_.num_slots; ++i) {
        cudaEvent_t ev;
        ce = cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
        if (ce != cudaSuccess) {
            // Clean up events created so far.
            for (std::uint32_t j = 0; j < i; ++j) {
                cudaEventDestroy(static_cast<cudaEvent_t>(events_[j]));
                --alloc_counts_.cuda_event_destroy;
            }
            events_.clear();
            cudaSetDevice(prev_dev);
            return false;
        }
        events_[i] = ev;
        ++alloc_counts_.cuda_event_create;
    }

    // 2. Allocate entry pool (contiguous GPU buffer for all slots).
    std::size_t entry_pool_bytes = static_cast<std::size_t>(cfg_.num_slots) *
                                   cfg_.max_entries_per_slot *
                                   sizeof(DeviceSubmitEntry);
    ce = cudaMalloc(reinterpret_cast<void**>(&d_entries_pool_), entry_pool_bytes);
    if (ce != cudaSuccess) {
        for (auto& ev : events_) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
        events_.clear();
        cudaSetDevice(prev_dev);
        return false;
    }
    ++alloc_counts_.cuda_malloc;

    // 3. Allocate status pool (contiguous GPU buffer for all slots).
    std::size_t status_pool_bytes = static_cast<std::size_t>(cfg_.num_slots) *
                                    cfg_.max_entries_per_slot *
                                    sizeof(EntryCompletionStatus);
    ce = cudaMalloc(reinterpret_cast<void**>(&d_status_pool_), status_pool_bytes);
    if (ce != cudaSuccess) {
        cudaFree(d_entries_pool_);
        d_entries_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        for (auto& ev : events_) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
        events_.clear();
        cudaSetDevice(prev_dev);
        return false;
    }
    ++alloc_counts_.cuda_malloc;

    // 4. Allocate PRP-list pool: 64 KiB-aligned, DMA-mapped.
    //    Each slot gets max_entries_per_slot PRP pages (worst case:
    //    every entry is a LIST sub-IO needing one PRP-list page).
    prp_pages_per_slot_ = cfg_.max_entries_per_slot;
    std::size_t total_prp_pages = static_cast<std::size_t>(cfg_.num_slots) *
                                  prp_pages_per_slot_;
    std::size_t prp_user_bytes = total_prp_pages * cfg_.page_size;

    // 64 KiB alignment for the DMA mapping base.
    prp_aligned_bytes_ = (prp_user_bytes + 65535) & ~static_cast<std::size_t>(65535);
    if (prp_aligned_bytes_ == 0) prp_aligned_bytes_ = 65536;

    ce = cudaMalloc(&prp_raw_, prp_aligned_bytes_ + 65536);
    if (ce != cudaSuccess) {
        cudaFree(d_status_pool_);
        d_status_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        cudaFree(d_entries_pool_);
        d_entries_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        for (auto& ev : events_) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
        events_.clear();
        cudaSetDevice(prev_dev);
        return false;
    }
    ++alloc_counts_.cuda_malloc;

    // Align to 64 KiB (snvme pins GPU pages at 64 KiB granularity).
    std::uintptr_t raw_addr = reinterpret_cast<std::uintptr_t>(prp_raw_);
    std::uintptr_t aligned_addr = (raw_addr + 65535) & ~static_cast<std::uintptr_t>(65535);
    prp_aligned_ = reinterpret_cast<void*>(aligned_addr);

    // DMA-map the entire PRP-list pool as one contiguous region.
    int rc = nvm_dma_map_data_device(&prp_dma_, ctrl_, prp_aligned_,
                                     prp_aligned_bytes_);
    if (rc != 0 || prp_dma_ == nullptr) {
        cudaFree(prp_raw_);
        prp_raw_ = nullptr;
        ++alloc_counts_.cuda_free;
        cudaFree(d_status_pool_);
        d_status_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        cudaFree(d_entries_pool_);
        d_entries_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        for (auto& ev : events_) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
        events_.clear();
        cudaSetDevice(prev_dev);
        return false;
    }
    ++alloc_counts_.nvm_dma_map;

    // 5. Round 16 S6 (REQUIRED 0): allocate descriptor pool for dynamic-path
    //    entries.  The kernel ALWAYS reads prp1/prp2/data_length from
    //    e.prp_entry; for entries without a pre-built descriptor, the host
    //    writes the computed descriptor into this pool + H2D before launch.
    std::size_t desc_pool_bytes = static_cast<std::size_t>(cfg_.num_slots) *
                                   cfg_.max_entries_per_slot *
                                   sizeof(AddressDescriptor);
    ce = cudaMalloc(reinterpret_cast<void**>(&d_desc_pool_), desc_pool_bytes);
    if (ce != cudaSuccess) {
        nvm_dma_unmap(prp_dma_);
        prp_dma_ = nullptr;
        ++alloc_counts_.nvm_dma_unmap;
        cudaFree(prp_raw_);
        prp_raw_ = nullptr;
        ++alloc_counts_.cuda_free;
        cudaFree(d_status_pool_);
        d_status_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        cudaFree(d_entries_pool_);
        d_entries_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
        for (auto& ev : events_) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
        events_.clear();
        cudaSetDevice(prev_dev);
        return false;
    }
    ++alloc_counts_.cuda_malloc;

    cudaSetDevice(prev_dev);

    // 5. Populate free-list: all slots available.
    for (std::uint32_t i = 0; i < cfg_.num_slots; ++i) {
        free_list_.push_back(i);
    }

    initialized_ = true;
    return true;
}

// -----------------------------------------------------------------------
// shutdown: free all resources
// -----------------------------------------------------------------------

void MetadataArena::shutdown(bool skip_prp) {
    if (!initialized_) return;

    int prev_dev = -1;
    cudaGetDevice(&prev_dev);
    cudaSetDevice(cfg_.cuda_device);

    // PRP-list pool: skip cleanup if any op timed out (conservative retention).
    if (!skip_prp) {
        if (prp_dma_) {
            nvm_dma_unmap(prp_dma_);
            prp_dma_ = nullptr;
            ++alloc_counts_.nvm_dma_unmap;
        }
        if (prp_raw_) {
            cudaFree(prp_raw_);
            prp_raw_ = nullptr;
            ++alloc_counts_.cuda_free;
        }
    }
    // Events and entry/status pools are always safe to free — the kernel
    // has returned (caller synced all streams before calling shutdown).
    if (d_status_pool_) {
        cudaFree(d_status_pool_);
        d_status_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
    }
    if (d_entries_pool_) {
        cudaFree(d_entries_pool_);
        d_entries_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
    }
    if (d_desc_pool_) {
        cudaFree(d_desc_pool_);
        d_desc_pool_ = nullptr;
        ++alloc_counts_.cuda_free;
    }
    for (auto& ev : events_) {
        if (ev) {
            cudaEventDestroy(static_cast<cudaEvent_t>(ev));
            ++alloc_counts_.cuda_event_destroy;
        }
    }
    events_.clear();

    cudaSetDevice(prev_dev);

    free_list_.clear();
    prp_aligned_ = nullptr;
    prp_aligned_bytes_ = 0;
    prp_pages_per_slot_ = 0;
    initialized_ = false;
}

// -----------------------------------------------------------------------
// acquire: lease a slot (zero CUDA calls)
// -----------------------------------------------------------------------

bool MetadataArena::acquire(Lease& out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (free_list_.empty()) {
        return false;
    }

    std::uint32_t slot = free_list_.front();
    free_list_.pop_front();

    out.slot_index = slot;
    out.event = events_[slot];
    out.d_entries = d_entries_pool_ + static_cast<std::size_t>(slot) * cfg_.max_entries_per_slot;
    out.d_status = d_status_pool_ + static_cast<std::size_t>(slot) * cfg_.max_entries_per_slot;
    out.prp_pages_devptr = static_cast<char*>(prp_aligned_) +
                           static_cast<std::size_t>(slot) * prp_pages_per_slot_ * cfg_.page_size;
    out.prp_ioaddrs_base = slot * prp_pages_per_slot_;
    out.prp_page_capacity = prp_pages_per_slot_;
    out.d_desc_pool = d_desc_pool_ + static_cast<std::size_t>(slot) * cfg_.max_entries_per_slot;

    return true;
}

// -----------------------------------------------------------------------
// release: return a slot for reuse
// -----------------------------------------------------------------------

void MetadataArena::release(std::uint32_t slot_index) {
    std::lock_guard<std::mutex> lock(mtx_);
    free_list_.push_back(slot_index);
}

// -----------------------------------------------------------------------
// release_with_timeout_leak: permanently consume a slot
// -----------------------------------------------------------------------

void MetadataArena::release_with_timeout_leak(std::uint32_t slot_index) {
    // Intentionally do NOT return the slot to the free list.
    // The timed-out NVMe command may still be in the controller SQ/CQ
    // and could DMA into the PRP-list pages after they are reused.
    // The CID was also not returned to the SQ, so that queue slot is
    // degraded until an abort/reset (future work).
    //
    // Bounded leak: at most cfg_.num_slots slots can be consumed this way.
    // After all slots are leaked, acquire() always returns false and
    // submit() returns RESOURCE_EXHAUSTED for every request.
    (void)slot_index;  // no-op: slot is simply not returned to free_list_
}

// -----------------------------------------------------------------------
// available: count free slots
// -----------------------------------------------------------------------

std::uint32_t MetadataArena::available() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<std::uint32_t>(free_list_.size());
}

} // namespace tutti::data_paths::local_nvme
