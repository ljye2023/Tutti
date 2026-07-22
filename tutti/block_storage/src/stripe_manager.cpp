#include "stripe_manager.h"
#include "backends/include/backend_provider.h"
#include "coordinator/include/raw_device.h"
#include "coordinator/include/coordinator_types.h"

#include <algorithm>
#include <cmath>

namespace tutti {
namespace block_storage {

StripeManager::StripeManager()
    : backend_provider_(nullptr), raw_device_(nullptr), next_device_index_(0) {
}

StripeManager::~StripeManager() {
}

bool StripeManager::initialize(backends::IBackendProvider* backend_provider,
                               coordinator::IRawDevice* raw_device,
                               const StripeConfig& config) {
    if (!backend_provider) {
        return false;
    }

    backend_provider_ = backend_provider;
    raw_device_ = raw_device;
    config_ = config;

    // If no raw device provided, we can still initialize in mock mode
    if (!raw_device_) {
        // Mock mode: create synthetic namespace list for testing
        available_namespaces_ = {1, 2, 3, 4};  // Mock 4 namespaces

        // Initialize mock LBA allocators
        lba_allocators_.clear();
        for (uint32_t ns_id : available_namespaces_) {
            DeviceLbaAllocator allocator;
            allocator.namespace_id = ns_id;
            allocator.total_blocks = 1024ULL * 1024 * 1024;  // Mock 512 GB per namespace
            allocator.next_free_lba = 0;
            allocator.allocated_blocks = 0;
            lba_allocators_[ns_id] = allocator;
        }

        next_device_index_ = 0;
        return true;
    }

    // Query available namespaces from raw device interface
    available_namespaces_ = raw_device_->list_namespaces();

    if (available_namespaces_.empty()) {
        return false;
    }

    // Initialize LBA allocators for each namespace
    lba_allocators_.clear();
    for (uint32_t ns_id : available_namespaces_) {
        coordinator::NamespaceInfo ns_info = raw_device_->get_namespace_info(ns_id);

        DeviceLbaAllocator allocator;
        allocator.namespace_id = ns_id;
        allocator.total_blocks = ns_info.capacity_blocks;
        allocator.next_free_lba = 0;  // Start from LBA 0
        allocator.allocated_blocks = 0;

        lba_allocators_[ns_id] = allocator;
    }

    next_device_index_ = 0;
    return true;
}

std::vector<FileShard> StripeManager::allocate_shards(uint64_t file_size,
                                                      uint64_t stripe_size) {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    std::vector<FileShard> shards;

    if (!backend_provider_ || available_namespaces_.empty()) {
        return shards;
    }

    // Use provided stripe_size, or fall back to config default
    if (stripe_size == 0) {
        stripe_size = config_.stripe_size;
    }

    // Calculate number of shards needed
    size_t shard_count = static_cast<size_t>(
        std::ceil(static_cast<double>(file_size) / stripe_size));

    // Limit to max_shards_per_file
    if (shard_count > config_.max_shards_per_file) {
        shard_count = config_.max_shards_per_file;
        // Recalculate stripe_size to fit
        stripe_size = (file_size + shard_count - 1) / shard_count;
    }

    // Limit to available namespaces/devices
    if (shard_count > available_namespaces_.size()) {
        shard_count = available_namespaces_.size();
        stripe_size = (file_size + shard_count - 1) / shard_count;
    }

    shards.reserve(shard_count);

    // Allocate shards using round-robin device selection with load balancing
    uint64_t remaining_size = file_size;
    for (size_t i = 0; i < shard_count; ++i) {
        // Select namespace with least allocations (load balancing)
        uint32_t selected_ns_id = available_namespaces_[0];
        uint64_t min_allocation = lba_allocators_[selected_ns_id].allocated_blocks;

        for (uint32_t ns_id : available_namespaces_) {
            auto& allocator = lba_allocators_[ns_id];
            if (allocator.allocated_blocks < min_allocation) {
                min_allocation = allocator.allocated_blocks;
                selected_ns_id = ns_id;
            }
        }

        auto& allocator = lba_allocators_[selected_ns_id];

        // Calculate shard size
        uint64_t shard_size = std::min(stripe_size, remaining_size);
        uint64_t length_blocks = (shard_size + 511) / 512;  // Convert to 512-byte blocks

        // Check if namespace has enough space
        if (allocator.next_free_lba + length_blocks > allocator.total_blocks) {
            // Not enough space in this namespace, skip allocation
            // In production, this should handle space exhaustion more gracefully
            break;
        }

        // Allocate LBA range from the namespace
        uint64_t start_lba = allocator.next_free_lba;

        // Create shard - device_id is the namespace_id for now
        FileShard shard(selected_ns_id, selected_ns_id, start_lba, length_blocks);
        shards.push_back(shard);

        // Update allocator tracking
        allocator.next_free_lba += length_blocks;
        allocator.allocated_blocks += length_blocks;

        remaining_size -= shard_size;

        if (remaining_size == 0) {
            break;
        }
    }

    return shards;
}

bool StripeManager::deallocate_shards(const std::vector<FileShard>& shards) {
    std::lock_guard<std::mutex> lock(allocation_mutex_);

    if (!backend_provider_) {
        return false;
    }

    // Deallocate LBA ranges by updating allocator tracking
    for (const auto& shard : shards) {
        auto it = lba_allocators_.find(shard.device_id);
        if (it == lba_allocators_.end()) {
            // Unknown namespace, skip
            continue;
        }

        auto& allocator = it->second;

        // Update allocation tracking
        if (allocator.allocated_blocks >= shard.length_blocks) {
            allocator.allocated_blocks -= shard.length_blocks;
        }

        // Note: This is a simple allocator that doesn't reclaim freed LBA ranges
        // for reuse. In production, this should maintain a free list or bitmap
        // to track which LBA ranges are available for allocation.
        // For now, we only update the allocated_blocks counter.
    }

    return true;
}

size_t StripeManager::get_device_count() const {
    return available_namespaces_.size();
}

}  // namespace block_storage
}  // namespace tutti
