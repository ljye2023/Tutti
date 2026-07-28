// tutti/io_engine/src/io_engine_impl.h
// Layer 4: IO Engine - Implementation (private header)

#pragma once

#include "io_engine/include/io_engine.h"
#include "io_engine/include/io_types.h"
#include "backends/nvme/include/batch_submitter.h"  // nvme::IBatchSubmitter, nvme::BufferDescriptor
#include "accel/include/common/iaccel.h"
#include <vector>
#include <memory>

namespace tutti {

// Async operation context for deferred cleanup
struct AsyncBatchContext {
    std::vector<backends::nvme::BufferDescriptor> descriptors;
    backends::nvme::IBatchSubmitter* backend;  // not owned
    AccelEvent completion_event;

    AsyncBatchContext(backends::nvme::IBatchSubmitter* b, IAccelerator* accel)
        : backend(b), completion_event(accel->create_event()) {}
};

class IoEngineImpl : public IIoEngine {
public:
    IoEngineImpl(backends::nvme::IBatchSubmitter* backend, IAccelerator* accel);
    virtual ~IoEngineImpl();

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
    backends::nvme::IBatchSubmitter* backend_;  // not owned
    IAccelerator* accel_;        // not owned

    backends::nvme::BufferDescriptor* d_descs_;  // GPU-resident scratch for descriptors
    uint32_t max_batch_entries_;
    size_t   max_io_size_;       // cached from metadata() at construction

    // Track pending async operations for deferred cleanup
    std::vector<std::shared_ptr<AsyncBatchContext>> pending_async_ops_;

    // Poll and cleanup completed async operations
    void cleanup_completed_async_ops();
};

} // namespace tutti
