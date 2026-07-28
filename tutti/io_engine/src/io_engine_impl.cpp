// tutti/io_engine/src/io_engine_impl.cpp
// Layer 4: IO Engine - Implementation

#include "io_engine_impl.h"
#include "io_engine/include/io_types.h"
#include "accel/include/common/memory_region.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <memory>

namespace tutti {

namespace {
    // Page size for ioaddr index calculation (standard 4KB page)
    constexpr size_t PAGE_SIZE = 4096;
}

IoEngineImpl::IoEngineImpl(backends::nvme::IBatchSubmitter* backend, IAccelerator* accel)
    : backend_(backend)
    , accel_(accel)
    , d_descs_(nullptr)
    , max_batch_entries_(0)
    , max_io_size_(0) {

    // Validate input pointers
    if (!backend_) {
        throw std::invalid_argument("IoEngineImpl: backend cannot be null");
    }
    if (!accel_) {
        throw std::invalid_argument("IoEngineImpl: accel cannot be null");
    }

    // Query backend capabilities once. max_io_size derives from vdevice MDTS,
    // which is fixed after the backend's roster is initialized, so caching it
    // here (rather than calling per-submit) is correct.
    backends::BackendMetadata meta = backend_->metadata();
    max_batch_entries_ = static_cast<uint32_t>(meta.max_batch_size);
    max_io_size_       = meta.max_io_size;

    // Allocate GPU-resident descriptor scratch buffer
    // Size based on max batch entries from backend
    if (max_batch_entries_ > 0) {
        size_t desc_buffer_size = max_batch_entries_ * sizeof(backends::nvme::BufferDescriptor);
        d_descs_ = static_cast<backends::nvme::BufferDescriptor*>(
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
    size_t max_io = max_io_size_;
    if (max_io == 0) {
        return false;
    }

    // Fan out requests into transport-sized sub-IOs
    // Track which region each slice belongs to for multi-region support
    std::vector<backends::nvme::SubSliceInfo> slices;
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

            backends::nvme::SubSliceInfo slice;
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
    std::vector<backends::nvme::BufferDescriptor> descs(slices.size());

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
                              descs.size() * sizeof(backends::nvme::BufferDescriptor),
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
    size_t max_io = max_io_size_;
    if (max_io == 0) {
        return false;
    }

    // Fan out requests into transport-sized sub-IOs
    // Track which region each slice belongs to for multi-region support
    std::vector<backends::nvme::SubSliceInfo> slices;
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

            backends::nvme::SubSliceInfo slice;
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
    std::vector<backends::nvme::BufferDescriptor> descs(slices.size());

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
                              descs.size() * sizeof(backends::nvme::BufferDescriptor),
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

    size_t max_io = max_io_size_;
    if (max_io == 0) {
        return 0;
    }

    // Compute ceiling division: ceil(region->size / max_io)
    // This tells adapters how many sub-IOs this region will produce
    return static_cast<uint32_t>((region->size + max_io - 1) / max_io);
}

bool IoEngineImpl::submit_one(
    const SingleShardIoRequest& req,
    bool is_read,
    AccelStream stream) {

    // Validate state and inputs.
    if (!backend_ || !accel_ || !d_descs_) { return false; }
    if (!req.region || req.length == 0)     { return false; }
    if (!req.vdev.is_valid())               { return false; }
    if (max_io_size_ == 0)                  { return false; }

    // DMA address array for PRP building. Must be present (region registered).
    const uint64_t* ioaddrs =
        static_cast<const uint64_t*>(req.region->backend_private);
    if (!ioaddrs) { return false; }

    // Acquire (or fetch cached) GPU-resident target handle bound to req.vdev.
    // NvmeBackend caches these keyed by (target_id, start_lba, vdev_index);
    // the call is safe to invoke on every submit.
    void* handle = backend_->acquire_target_handle(req.shard_target, req.vdev);
    if (!handle) { return false; }

    // MDTS fan-out: split request into transport-sized sub-IOs.
    std::vector<backends::nvme::SubSliceInfo> slices;
    for (uint64_t off = 0; off < req.length; off += max_io_size_) {
        uint64_t chunk = std::min(
            static_cast<uint64_t>(max_io_size_), req.length - off);
        backends::nvme::SubSliceInfo s;
        s.offset_bytes = req.logical_offset + off;  // physical offset within shard
        s.length_bytes = static_cast<uint32_t>(chunk);
        s.slice_index  = static_cast<uint32_t>(slices.size());
        slices.push_back(s);
    }

    if (slices.size() > max_batch_entries_) { return false; }

    // Build PRP descriptors from DMA addresses.
    std::vector<backends::nvme::BufferDescriptor> descs(slices.size());
    for (size_t i = 0; i < slices.size(); ++i) {
        if (!backend_->prepare_descriptors(ioaddrs, &slices[i], 1, &descs[i])) {
            return false;
        }
    }

    // Stage descriptors CPU→GPU asynchronously.
    if (!accel_->memcpy_async(d_descs_, descs.data(),
                              descs.size() * sizeof(backends::nvme::BufferDescriptor),
                              stream)) {
        backend_->release_descriptors(descs.data(),
                                      static_cast<uint32_t>(descs.size()));
        return false;
    }

    // Launch the GPU submission kernel (one kernel covers all slices).
    backend_->launch_batch_gpu_stream(
        stream.handle, handle,
        d_descs_, static_cast<uint32_t>(slices.size()), is_read);

    // Block until complete, then return PRP pages (target handle stays cached).
    accel_->synchronize_stream(stream);
    backend_->release_descriptors(descs.data(),
                                  static_cast<uint32_t>(descs.size()));
    return true;
}

} // namespace tutti
