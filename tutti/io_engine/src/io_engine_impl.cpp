// tutti/io_engine/src/io_engine_impl.cpp
// Layer 4: IO Engine - Implementation

#include "io_engine_impl.h"

namespace tutti {

IoEngineImpl::IoEngineImpl(IBackendProvider* backend)
    : backend_(backend) {
}

IoEngineImpl::~IoEngineImpl() {
}

int IoEngineImpl::submit_batch(
    const IoRequest* requests,
    size_t num_requests,
    AccelStream stream)
{
    if (!backend_) {
        return -1;
    }

    return backend_->submit_io_batch(requests, num_requests, stream);
}

int IoEngineImpl::poll_completions(
    uint32_t* num_completed,
    AccelStream stream)
{
    // TODO: Implement completion polling
    (void)stream;
    *num_completed = 0;
    return 0;
}

} // namespace tutti
