// tutti/io_engine/src/local_nvme_io_engine.cpp
// Layer 4: IO Engine - Local NVMe IO Engine Implementation

#include "io_engine/include/local_nvme/local_nvme_io_engine.h"
#include "io_engine_impl.h"

namespace tutti {
namespace local_nvme {

// PIMPL implementation delegates to IoEngineImpl
struct LocalNvmeIoEngine::Impl {
    IoEngineImpl engine_impl;

    Impl(backends::nvme::IBatchSubmitter* backend, IAccelerator* accel)
        : engine_impl(backend, accel) {}
};

LocalNvmeIoEngine::LocalNvmeIoEngine(
    backends::nvme::IBatchSubmitter* backend,
    IAccelerator* accel,
    const LocalNvmeIoEngineConfig& config)
    : impl_(std::make_unique<Impl>(backend, accel)) {
    // Config could be used for future extensions (polling, inflight limits)
    // For now, delegate directly to IoEngineImpl
}

LocalNvmeIoEngine::~LocalNvmeIoEngine() = default;

bool LocalNvmeIoEngine::submit_batch(
    const std::vector<IoRequest>& requests,
    bool is_read,
    AccelStream stream) {
    return impl_->engine_impl.submit_batch(requests, is_read, stream);
}

bool LocalNvmeIoEngine::submit_batch_async(
    const std::vector<IoRequest>& requests,
    bool is_read,
    AccelStream stream) {
    return impl_->engine_impl.submit_batch_async(requests, is_read, stream);
}

bool LocalNvmeIoEngine::submit_one(
    const SingleShardIoRequest& req,
    bool is_read,
    AccelStream stream) {
    return impl_->engine_impl.submit_one(req, is_read, stream);
}

uint32_t LocalNvmeIoEngine::max_entries_per_batch() const {
    return impl_->engine_impl.max_entries_per_batch();
}

uint32_t LocalNvmeIoEngine::slice_fanout(const MemoryRegion* region) const {
    return impl_->engine_impl.slice_fanout(region);
}

} // namespace local_nvme
} // namespace tutti
