#include "coordinator_impl.h"
#include <cstring>

namespace tutti {
namespace coordinator {

CoordinatorImpl::CoordinatorImpl() = default;

CoordinatorImpl::~CoordinatorImpl() {
    if (initialized_) {
        cleanup();
    }
}

bool CoordinatorImpl::initialize(const CoordinatorConfig& config) {
    std::lock_guard<std::mutex> lock(init_lock_);

    if (initialized_) {
        return false;
    }

    if (!config.is_valid()) {
        return false;
    }

    config_ = config;
    backend_provider_ = config.backend_provider;
    accelerator_ = config.accelerator;
    block_storage_ = config.block_storage;
    io_engine_ = config.io_engine;

    if (!config.default_stream.is_valid()) {
        default_stream_ = accelerator_->create_stream();
        owns_default_stream_ = true;
    } else {
        default_stream_ = config.default_stream;
        owns_default_stream_ = false;
    }

    raw_device_impl_ = std::make_unique<RawDeviceImpl>(backend_provider_, io_engine_);
    if (!raw_device_impl_) {
        if (owns_default_stream_) {
            accelerator_->destroy_stream(default_stream_);
        }
        return false;
    }

    initialized_ = true;
    return true;
}

bool CoordinatorImpl::cleanup() {
    std::lock_guard<std::mutex> lock(init_lock_);

    if (!initialized_) {
        return false;
    }

    if (!buffer_registry_.is_empty()) {
        // Error: buffers still registered
        return false;
    }

    {
        std::lock_guard<std::mutex> targets_lock(targets_lock_);
        if (!open_raw_targets_.empty()) {
            // Error: raw targets still open
            return false;
        }
    }

    raw_device_impl_.reset();

    if (owns_default_stream_) {
        accelerator_->destroy_stream(default_stream_);
    }

    initialized_ = false;
    return true;
}

MemoryRegion* CoordinatorImpl::register_buffer(
    void* ptr,
    size_t size,
    MemoryKind kind) {

    if (!initialized_ || !ptr || size == 0) {
        return nullptr;
    }

    MemoryRegion* region = nullptr;

    switch (kind) {
        case MemoryKind::HOST:
            region = accelerator_->register_host(ptr, size);
            break;
        case MemoryKind::DEVICE:
            region = accelerator_->register_device(ptr, size, 0);
            break;
        case MemoryKind::EXTERNAL:
            region = accelerator_->register_external(ptr, nullptr, size, ExternalMemorySpec());
            break;
        default:
            return nullptr;
    }

    if (!region) {
        return nullptr;
    }

    if (!buffer_registry_.add_region(region)) {
        accelerator_->unregister(region);
        return nullptr;
    }

    return region;
}

bool CoordinatorImpl::unregister_buffer(MemoryRegion* region) {
    if (!initialized_ || !region) {
        return false;
    }

    if (!buffer_registry_.remove_region(region)) {
        return false;
    }

    accelerator_->unregister(region);
    return true;
}

bool CoordinatorImpl::validate_requests(
    IoRequest* requests,
    uint32_t count) const {

    if (!requests || count == 0) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const auto& req = requests[i];

        if (!req.region) {
            return false;
        }

        MemoryRegion* registered = buffer_registry_.lookup_by_ptr(req.region->host_ptr);
        if (!registered) {
            return false;
        }
    }

    return true;
}

BatchSubmitResult CoordinatorImpl::submit_read_batch(
    IoRequest* requests,
    uint32_t count,
    AccelStream stream) {

    if (!initialized_) {
        return BatchSubmitResult(false, 0, count, -1);
    }

    if (!validate_requests(requests, count)) {
        return BatchSubmitResult(false, 0, count, -2);
    }

    AccelStream target_stream = stream.is_valid() ? stream : default_stream_;

    std::vector<tutti::IoRequest> req_vec;
    for (uint32_t i = 0; i < count; ++i) {
        tutti::IoRequest req;
        req.region = requests[i].region;
        req.target_handle = requests[i].target_handle;
        req.byte_offset = requests[i].byte_offset;
        req.byte_length = requests[i].byte_length;
        req_vec.push_back(req);
    }

    bool success = io_engine_->submit_batch(req_vec, true, target_stream);

    return BatchSubmitResult(success, success ? count : 0, success ? 0 : count, success ? 0 : -3);
}

BatchSubmitResult CoordinatorImpl::submit_write_batch(
    IoRequest* requests,
    uint32_t count,
    AccelStream stream) {

    if (!initialized_) {
        return BatchSubmitResult(false, 0, count, -1);
    }

    if (!validate_requests(requests, count)) {
        return BatchSubmitResult(false, 0, count, -2);
    }

    AccelStream target_stream = stream.is_valid() ? stream : default_stream_;

    std::vector<tutti::IoRequest> req_vec;
    for (uint32_t i = 0; i < count; ++i) {
        tutti::IoRequest req;
        req.region = requests[i].region;
        req.target_handle = requests[i].target_handle;
        req.byte_offset = requests[i].byte_offset;
        req.byte_length = requests[i].byte_length;
        req_vec.push_back(req);
    }

    bool success = io_engine_->submit_batch(req_vec, false, target_stream);

    return BatchSubmitResult(success, success ? count : 0, success ? 0 : count, success ? 0 : -3);
}

bool CoordinatorImpl::submit_read_batch_async(
    IoRequest* requests,
    uint32_t count,
    AccelStream stream,
    BatchCompletionCallback callback,
    void* user_data) {

    if (!initialized_ || !stream.is_valid() || !callback) {
        return false;
    }

    if (!validate_requests(requests, count)) {
        return false;
    }

    std::vector<tutti::IoRequest> req_vec;
    for (uint32_t i = 0; i < count; ++i) {
        tutti::IoRequest req;
        req.region = requests[i].region;
        req.target_handle = requests[i].target_handle;
        req.byte_offset = requests[i].byte_offset;
        req.byte_length = requests[i].byte_length;
        req_vec.push_back(req);
    }

    bool launch_success = io_engine_->submit_batch_async(req_vec, true, stream);

    if (!launch_success) {
        return false;
    }

    // Note: In a real implementation, we would record an event on the stream
    // and register a callback to be invoked when the event completes.
    // For now, this is a simplified implementation.

    return true;
}

bool CoordinatorImpl::submit_write_batch_async(
    IoRequest* requests,
    uint32_t count,
    AccelStream stream,
    BatchCompletionCallback callback,
    void* user_data) {

    if (!initialized_ || !stream.is_valid() || !callback) {
        return false;
    }

    if (!validate_requests(requests, count)) {
        return false;
    }

    std::vector<tutti::IoRequest> req_vec;
    for (uint32_t i = 0; i < count; ++i) {
        tutti::IoRequest req;
        req.region = requests[i].region;
        req.target_handle = requests[i].target_handle;
        req.byte_offset = requests[i].byte_offset;
        req.byte_length = requests[i].byte_length;
        req_vec.push_back(req);
    }

    bool launch_success = io_engine_->submit_batch_async(req_vec, false, stream);

    if (!launch_success) {
        return false;
    }

    // Note: Event-based callback mechanism would be implemented here

    return true;
}

block_storage::IBlockStorage* CoordinatorImpl::get_block_storage() {
    return block_storage_;
}

IRawDevice* CoordinatorImpl::get_raw_device() {
    return raw_device_impl_.get();
}

uint32_t CoordinatorImpl::max_batch_size() const {
    if (!initialized_ || !io_engine_) {
        return 0;
    }
    return io_engine_->max_entries_per_batch();
}

uint32_t CoordinatorImpl::slice_fanout(MemoryRegion* region) const {
    if (!initialized_ || !io_engine_ || !region) {
        return 0;
    }
    return io_engine_->slice_fanout(region);
}

ICoordinator* create_coordinator() {
    return new CoordinatorImpl();
}

void destroy_coordinator(ICoordinator* coordinator) {
    delete coordinator;
}

} // namespace coordinator
} // namespace tutti
