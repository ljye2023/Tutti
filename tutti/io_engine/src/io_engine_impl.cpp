// tutti/io_engine/src/io_engine_impl.cpp
// Layer 4: IO Engine - Implementation

#include "io_engine_impl.h"
#include "io_engine/include/io_types.h"
#include "accel/include/common/memory_region.h"
#include "backends/include/backend_types.h"
#include "backends/include/backend_provider.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <memory>

namespace tutti {

namespace {
    // Page size for ioaddr index calculation (standard 4KB page)
    constexpr size_t PAGE_SIZE = 4096;
}

IoEngineImpl::IoEngineImpl(backends::IBackendProvider* backend, IAccelerator* accel)
    : backend_(backend)
    , accel_(accel)
    , d_descs_(nullptr)
    , max_batch_entries_(0) {

    // Validate input pointers
    if (!backend_) {
        throw std::invalid_argument("IoEngineImpl: backend cannot be null");
    }
    if (!accel_) {
        throw std::invalid_argument("IoEngineImpl: accel cannot be null");
    }

    // Query backend capabilities
    backends::BackendMetadata meta = backend_->metadata();
    max_batch_entries_ = static_cast<uint32_t>(meta.max_batch_size);

    // Allocate GPU-resident descriptor scratch buffer
    // Size based on max batch entries from backend
    if (max_batch_entries_ > 0) {
        size_t desc_buffer_size = max_batch_entries_ * sizeof(backends::BufferDescriptor);
        d_descs_ = static_cast<backends::BufferDescriptor*>(
            accel_->allocate_device(desc_buffer_size, MemoryKind::DEVICE, accel_->get_device())
        );

        if (!d_descs_) {
            throw std::runtime_error("IoEngineImpl: failed to allocate GPU descriptor buffer");
        }
    }
}

IoEngineImpl::~IoEngineImpl() {
    // Clean up any remaining async operations
    cleanup_completed_async_ops();

    // Force cleanup of any still-pending async operations
    for (auto& ctx : pending_async_ops_) {
        if (ctx) {
            backend_->release_descriptors(ctx->descriptors.data(),
                                         static_cast<uint32_t>(ctx->descriptors.size()));
            accel_->destroy_event(ctx->completion_event);
        }
    }
    pending_async_ops_.clear();

    // Free GPU-resident descriptor buffer
    if (d_descs_) {
        accel_->free(d_descs_, MemoryKind::DEVICE);
        d_descs_ = nullptr;
    }
}

bool IoEngineImpl::submit_batch(
    const std::vector<IoRequest>& requests,
    bool is_read,
    AccelStream stream) {

    // Validate state
    if (!backend_ || !accel_ || !d_descs_) {
        return false;
    }

    // Handle empty batch
    if (requests.empty()) {
        return true;
    }

    // Get backend's max IO size for fan-out
    size_t max_io = backend_->max_io_size();
    if (max_io == 0) {
        return false;
    }

    // Fan out requests into transport-sized sub-IOs
    // Track which region each slice belongs to for multi-region support
    std::vector<backends::SubSliceInfo> slices;
    std::vector<const MemoryRegion*> slice_regions;
    slices.reserve(requests.size());  // Conservative estimate
    slice_regions.reserve(requests.size());

    for (const IoRequest& req : requests) {
        // Validate request
        if (!req.region || !req.target_handle || req.byte_length == 0) {
            return false;
        }

        // Validate region has DMA mapping
        const uint64_t* ioaddrs = static_cast<const uint64_t*>(req.region->backend_private);
        if (!ioaddrs) {
            return false;  // Region not DMA-mapped
        }

        // Split this request into chunks of max_io size
        for (uint64_t off = 0; off < req.byte_length; off += max_io) {
            uint64_t remaining = req.byte_length - off;
            uint64_t chunk_size = std::min(static_cast<uint64_t>(max_io), remaining);

            backends::SubSliceInfo slice;
            slice.offset_bytes = req.byte_offset + off;
            slice.length_bytes = static_cast<uint32_t>(chunk_size);
            slice.slice_index = static_cast<uint32_t>(slices.size());

            slices.push_back(slice);
            slice_regions.push_back(req.region);
        }
    }

    // Check if fanned-out batch exceeds capacity
    if (slices.size() > max_batch_entries_) {
        return false;
    }

    // Prepare CPU-side descriptor array
    std::vector<backends::BufferDescriptor> descs(slices.size());

    // Build descriptors for each slice using its corresponding region
    for (size_t i = 0; i < slices.size(); ++i) {
        const uint64_t* ioaddrs = static_cast<const uint64_t*>(
            slice_regions[i]->backend_private);

        if (!backend_->prepare_descriptors(ioaddrs, &slices[i], 1, &descs[i])) {
            return false;
        }
    }

    // Stage descriptors CPU→GPU via async memcpy
    if (!accel_->memcpy_async(d_descs_, descs.data(),
                              descs.size() * sizeof(backends::BufferDescriptor),
                              stream)) {
        // Release descriptors on failure
        backend_->release_descriptors(descs.data(), static_cast<uint32_t>(descs.size()));
        return false;
    }

    // Use first request's target_handle for the batch
    // All requests in batch should target the same device
    const IoRequest& first_req = requests[0];

    // Launch backend kernel on GPU stream
    backend_->launch_batch_gpu_stream(
        stream.handle,
        first_req.target_handle,
        d_descs_,
        static_cast<uint32_t>(slices.size()),
        is_read
    );

    // Synchronize stream - wait for kernel completion
    accel_->synchronize_stream(stream);

    // Release descriptors after IO completes
    backend_->release_descriptors(descs.data(), static_cast<uint32_t>(descs.size()));

    return true;
}

bool IoEngineImpl::submit_batch_async(
    const std::vector<IoRequest>& requests,
    bool is_read,
    AccelStream stream) {

    // Validate state
    if (!backend_ || !accel_ || !d_descs_) {
        return false;
    }

    // Handle empty batch
    if (requests.empty()) {
        return true;
    }

    // Get backend's max IO size for fan-out
    size_t max_io = backend_->max_io_size();
    if (max_io == 0) {
        return false;
    }

    // Fan out requests into transport-sized sub-IOs
    // Track which region each slice belongs to for multi-region support
    std::vector<backends::SubSliceInfo> slices;
    std::vector<const MemoryRegion*> slice_regions;
    slices.reserve(requests.size());
    slice_regions.reserve(requests.size());

    for (const IoRequest& req : requests) {
        // Validate request
        if (!req.region || !req.target_handle || req.byte_length == 0) {
            return false;
        }

        // Validate region has DMA mapping
        const uint64_t* ioaddrs = static_cast<const uint64_t*>(req.region->backend_private);
        if (!ioaddrs) {
            return false;  // Region not DMA-mapped
        }

        // Split this request into chunks of max_io size
        for (uint64_t off = 0; off < req.byte_length; off += max_io) {
            uint64_t remaining = req.byte_length - off;
            uint64_t chunk_size = std::min(static_cast<uint64_t>(max_io), remaining);

            backends::SubSliceInfo slice;
            slice.offset_bytes = req.byte_offset + off;
            slice.length_bytes = static_cast<uint32_t>(chunk_size);
            slice.slice_index = static_cast<uint32_t>(slices.size());

            slices.push_back(slice);
            slice_regions.push_back(req.region);
        }
    }

    // Check if fanned-out batch exceeds capacity
    if (slices.size() > max_batch_entries_) {
        return false;
    }

    // Prepare CPU-side descriptor array
    std::vector<backends::BufferDescriptor> descs(slices.size());

    // Build descriptors for each slice using its corresponding region
    for (size_t i = 0; i < slices.size(); ++i) {
        const uint64_t* ioaddrs = static_cast<const uint64_t*>(
            slice_regions[i]->backend_private);

        if (!backend_->prepare_descriptors(ioaddrs, &slices[i], 1, &descs[i])) {
            return false;
        }
    }

    // Stage descriptors CPU→GPU via async memcpy
    if (!accel_->memcpy_async(d_descs_, descs.data(),
                              descs.size() * sizeof(backends::BufferDescriptor),
                              stream)) {
        backend_->release_descriptors(descs.data(), static_cast<uint32_t>(descs.size()));
        return false;
    }

    // Use first request's target_handle for the batch
    // All requests in batch should target the same device
    const IoRequest& first_req = requests[0];

    // Launch backend kernel on GPU stream
    backend_->launch_batch_gpu_stream(
        stream.handle,
        first_req.target_handle,
        d_descs_,
        static_cast<uint32_t>(slices.size()),
        is_read
    );

    // Create async context for deferred cleanup
    auto ctx = std::make_shared<AsyncBatchContext>(backend_, accel_);
    ctx->descriptors = std::move(descs);

    // Record event after kernel launch for completion tracking
    accel_->record_event(ctx->completion_event, stream);

    // Store context for later cleanup
    pending_async_ops_.push_back(ctx);

    // Clean up any completed operations from previous submissions
    cleanup_completed_async_ops();

    return true;
}

void IoEngineImpl::cleanup_completed_async_ops() {
    // Poll all pending operations and release completed ones
    auto it = pending_async_ops_.begin();
    while (it != pending_async_ops_.end()) {
        if (accel_->query_event((*it)->completion_event)) {
            // Operation completed - release descriptors and event
            backend_->release_descriptors((*it)->descriptors.data(),
                                         static_cast<uint32_t>((*it)->descriptors.size()));
            accel_->destroy_event((*it)->completion_event);
            it = pending_async_ops_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t IoEngineImpl::max_entries_per_batch() const {
    return max_batch_entries_;
}

uint32_t IoEngineImpl::slice_fanout(const MemoryRegion* region) const {
    if (!region || !backend_) {
        return 0;
    }

    size_t max_io = backend_->max_io_size();
    if (max_io == 0) {
        return 0;
    }

    // Compute ceiling division: ceil(region->size / max_io)
    // This tells adapters how many sub-IOs this region will produce
    return static_cast<uint32_t>((region->size + max_io - 1) / max_io);
}

} // namespace tutti
