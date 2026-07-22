#include "raw_device_impl.h"
#include "io_engine/include/io_engine.h"
#include "backends/include/backend_types.h"
#include <cstring>

namespace tutti {
namespace coordinator {

RawDeviceImpl::RawDeviceImpl(backends::IBackendProvider* backend_provider, tutti::IIoEngine* io_engine)
    : backend_provider_(backend_provider), io_engine_(io_engine) {
}

RawDeviceImpl::~RawDeviceImpl() {
    std::lock_guard<std::mutex> lock(handle_lock_);

    if (!handle_map_.empty()) {
        // Log warning: raw targets not properly released
    }

    for (auto& [handle, target] : handle_map_) {
        if (backend_provider_ && handle->target_handle) {
            backend_provider_->release_target_handle(handle->target_handle);
        }
        delete target;
        delete handle;
    }
    handle_map_.clear();
}

RawTargetHandle* RawDeviceImpl::acquire_raw_target(
    uint32_t namespace_id,
    uint64_t start_lba,
    uint64_t length_blocks) {

    if (!backend_provider_) {
        return nullptr;
    }

    auto* target = new backends::StorageTarget();
    target->kind = backends::StorageTargetKind::NVME_RAW;
    target->namespace_id = namespace_id;
    target->start_lba = start_lba;
    target->length_blocks = length_blocks;

    void* target_handle = backend_provider_->acquire_target_handle(*target);
    if (!target_handle) {
        delete target;
        return nullptr;
    }

    auto* handle = new RawTargetHandle(namespace_id, start_lba, length_blocks);
    handle->target_handle = target_handle;
    handle->region_id = next_region_id_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(handle_lock_);
    handle_map_[handle] = target;

    return handle;
}

bool RawDeviceImpl::release_raw_target(RawTargetHandle* handle) {
    if (!handle) {
        return false;
    }

    std::lock_guard<std::mutex> lock(handle_lock_);

    auto it = handle_map_.find(handle);
    if (it == handle_map_.end()) {
        return false;
    }

    backends::StorageTarget* target = it->second;

    if (backend_provider_ && handle->target_handle) {
        backend_provider_->release_target_handle(handle->target_handle);
    }

    handle_map_.erase(it);
    delete target;
    delete handle;

    return true;
}

bool RawDeviceImpl::submit_read(
    RawTargetHandle* handle,
    MemoryRegion* buffer,
    uint64_t byte_offset,
    uint64_t byte_length,
    AccelStream stream) {

    if (!handle || !buffer || !io_engine_) {
        return false;
    }

    tutti::IoRequest req;
    req.region = buffer;
    req.target_handle = handle->target_handle;
    req.byte_offset = byte_offset;
    req.byte_length = byte_length;

    std::vector<tutti::IoRequest> requests = {req};
    return io_engine_->submit_batch(requests, true, stream);
}

bool RawDeviceImpl::submit_write(
    RawTargetHandle* handle,
    MemoryRegion* buffer,
    uint64_t byte_offset,
    uint64_t byte_length,
    AccelStream stream) {

    if (!handle || !buffer || !io_engine_) {
        return false;
    }

    tutti::IoRequest req;
    req.region = buffer;
    req.target_handle = handle->target_handle;
    req.byte_offset = byte_offset;
    req.byte_length = byte_length;

    std::vector<tutti::IoRequest> requests = {req};
    return io_engine_->submit_batch(requests, false, stream);
}

BatchSubmitResult RawDeviceImpl::submit_read_batch(
    RawTargetHandle* handle,
    IoRequest* requests,
    uint32_t count,
    AccelStream stream) {

    if (!handle || !requests || count == 0 || !io_engine_) {
        return BatchSubmitResult(false, 0, count, -1);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (requests[i].target_handle != handle->target_handle) {
            return BatchSubmitResult(false, 0, count, -2);
        }
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

    bool success = io_engine_->submit_batch(req_vec, true, stream);

    return BatchSubmitResult(success, success ? count : 0, success ? 0 : count, success ? 0 : -3);
}

BatchSubmitResult RawDeviceImpl::submit_write_batch(
    RawTargetHandle* handle,
    IoRequest* requests,
    uint32_t count,
    AccelStream stream) {

    if (!handle || !requests || count == 0 || !io_engine_) {
        return BatchSubmitResult(false, 0, count, -1);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (requests[i].target_handle != handle->target_handle) {
            return BatchSubmitResult(false, 0, count, -2);
        }
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

    bool success = io_engine_->submit_batch(req_vec, false, stream);

    return BatchSubmitResult(success, success ? count : 0, success ? 0 : count, success ? 0 : -3);
}

NamespaceInfo RawDeviceImpl::get_namespace_info(uint32_t namespace_id) {
    {
        std::lock_guard<std::mutex> lock(namespace_cache_lock_);
        auto it = namespace_info_cache_.find(namespace_id);
        if (it != namespace_info_cache_.end()) {
            return it->second;
        }
    }

    // Query backend for namespace metadata
    // For now, return placeholder values
    NamespaceInfo info(namespace_id, 4096, 0, 131072);

    {
        std::lock_guard<std::mutex> lock(namespace_cache_lock_);
        namespace_info_cache_[namespace_id] = info;
    }

    return info;
}

std::vector<uint32_t> RawDeviceImpl::list_namespaces() {
    if (!backend_provider_) {
        return {};
    }

    // Query backend for available namespaces
    // For now, return placeholder
    return {1};
}

} // namespace coordinator
} // namespace tutti
