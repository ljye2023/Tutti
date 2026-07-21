// tutti/io_engine/include/local_nvme/local_nvme_io_engine.h
// Layer 4: IO Engine - Local NVMe IO Engine
//
// Concrete I/O engine implementation for local NVMe devices

#pragma once

#include "tutti/io_engine/include/io_engine.h"
#include "tutti/backends/include/backend_provider.h"
#include <memory>

namespace tutti {
namespace local_nvme {

// Local NVMe I/O engine configuration
struct LocalNvmeIoEngineConfig {
    uint32_t max_batch_size;
    uint32_t max_inflight_batches;
    bool enable_polling;
};

// Local NVMe I/O engine implementation
class LocalNvmeIoEngine : public IIoEngine {
public:
    LocalNvmeIoEngine(
        IBackendProvider* backend,
        const LocalNvmeIoEngineConfig& config);

    virtual ~LocalNvmeIoEngine();

    // IIoEngine interface
    int submit_batch(
        const IoRequest* requests,
        size_t num_requests,
        AccelStream stream) override;

    int poll_completions(
        uint32_t* num_completed,
        AccelStream stream) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace local_nvme
} // namespace tutti
