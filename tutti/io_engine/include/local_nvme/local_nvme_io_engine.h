// tutti/io_engine/include/local_nvme/local_nvme_io_engine.h
// Layer 4: IO Engine - Local NVMe IO Engine
//
// Concrete I/O engine implementation for local NVMe devices

#pragma once

#include "io_engine/include/io_engine.h"
#include "backends/nvme/include/batch_submitter.h"
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
        backends::nvme::IBatchSubmitter* backend,
        IAccelerator* accel,
        const LocalNvmeIoEngineConfig& config);

    virtual ~LocalNvmeIoEngine();

    // IIoEngine interface
    bool submit_batch(
        const std::vector<IoRequest>& requests,
        bool is_read,
        AccelStream stream) override;

    bool submit_batch_async(
        const std::vector<IoRequest>& requests,
        bool is_read,
        AccelStream stream) override;

    bool submit_one(
        const SingleShardIoRequest& req,
        bool                        is_read,
        AccelStream                 stream) override;

    uint32_t max_entries_per_batch() const override;

    uint32_t slice_fanout(const MemoryRegion* region) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace local_nvme
} // namespace tutti
