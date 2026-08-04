// tutti/data_paths/local_nvme/metadata/prp_page_cache.cpp
//
// PrpPageCache init/shutdown implementation (needs CUDA + libnvm).

#include "tutti/data_paths/local_nvme/metadata/prp_page_cache.h"

#include <tutti/cuda_like.h>
#include <nvm_dma.h>

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace tutti::data_paths::local_nvme {

bool PrpPageCache::init(const Config& cfg, nvm_ctrl_t* ctrl) {
    if (initialized_) return false;
    if (cfg.capacity == 0) { cfg_ = cfg; return true; }
    if (ctrl == nullptr) return false;

    cfg_ = cfg;
    ctrl_ = ctrl;

    int prev_dev = -1;
    cudaGetDevice(&prev_dev);
    cudaSetDevice(cfg_.cuda_device);

    // Allocate pool: capacity * page_size bytes, 64 KiB-aligned.
    std::size_t user_bytes = static_cast<std::size_t>(cfg_.capacity) * cfg_.page_size;
    std::size_t aligned_bytes = (user_bytes + 65535) & ~static_cast<std::size_t>(65535);
    if (aligned_bytes == 0) aligned_bytes = 65536;

    cudaError_t ce = cudaMalloc(&pool_raw_, aligned_bytes + 65536);
    if (ce != cudaSuccess) {
        std::fprintf(stderr, "[prp_cache] init: cudaMalloc failed\n");
        cudaSetDevice(prev_dev);
        return false;
    }

    std::uintptr_t raw_addr = reinterpret_cast<std::uintptr_t>(pool_raw_);
    std::uintptr_t aligned_addr = (raw_addr + 65535) & ~static_cast<std::uintptr_t>(65535);
    pool_aligned_ = reinterpret_cast<void*>(aligned_addr);

    // DMA-map the entire pool.
    int rc = nvm_dma_map_data_device(&pool_dma_, ctrl_, pool_aligned_,
                                     aligned_bytes);
    if (rc != 0 || pool_dma_ == nullptr) {
        std::fprintf(stderr, "[prp_cache] init: nvm_dma_map_data_device failed: rc=%d\n", rc);
        cudaFree(pool_raw_);
        pool_raw_ = nullptr;
        pool_aligned_ = nullptr;
        cudaSetDevice(prev_dev);
        return false;
    }

    // Initialize entries + free list.
    entries_.resize(cfg_.capacity);
    free_list_.clear();
    for (std::uint32_t i = 0; i < cfg_.capacity; ++i)
        free_list_.push_back(i);
    index_.clear();
    lru_.clear();
    lru_pos_.clear();
    stats_ = {};

    cudaSetDevice(prev_dev);
    initialized_ = true;
    return true;
}

void PrpPageCache::shutdown() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!initialized_) return;

    int prev_dev = -1;
    cudaGetDevice(&prev_dev);
    cudaSetDevice(cfg_.cuda_device);

    // DMA unmap FIRST, then free GPU memory (lives/dies together).
    if (pool_dma_) {
        nvm_dma_unmap(pool_dma_);
        pool_dma_ = nullptr;
    }
    if (pool_raw_) {
        cudaFree(pool_raw_);
        pool_raw_ = nullptr;
    }
    pool_aligned_ = nullptr;

    cudaSetDevice(prev_dev);

    entries_.clear();
    free_list_.clear();
    index_.clear();
    lru_.clear();
    lru_pos_.clear();
    stats_ = {};
    initialized_ = false;
}

} // namespace tutti::data_paths::local_nvme
