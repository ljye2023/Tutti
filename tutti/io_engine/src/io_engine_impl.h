// tutti/io_engine/src/io_engine_impl.h
// Layer 4: IO Engine - Implementation (private header)

#pragma once

#include "tutti/io_engine/include/io_engine.h"
#include "tutti/backends/include/backend_provider.h"
#include <memory>

namespace tutti {

class IoEngineImpl : public IIoEngine {
public:
    IoEngineImpl(IBackendProvider* backend);
    virtual ~IoEngineImpl();

    // IIoEngine interface
    int submit_batch(
        const IoRequest* requests,
        size_t num_requests,
        AccelStream stream) override;

    int poll_completions(
        uint32_t* num_completed,
        AccelStream stream) override;

private:
    IBackendProvider* backend_;
};

} // namespace tutti
