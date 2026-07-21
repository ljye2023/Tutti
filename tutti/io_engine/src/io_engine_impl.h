// tutti/io_engine/src/io_engine_impl.h
// Layer 4: IO Engine - Implementation (private header)

#pragma once

#include "io_engine/include/io_engine.h"
#include "io_engine/include/io_types.h"
#include "backends/include/backend_provider.h"
#include "backends/include/backend_types.h"
#include "accel/include/common/iaccel.h"
#include <vector>
#include <memory>

namespace tutti {

// Forward declarations from backends namespace
namespace backends {
    class IBackendProvider;
}

// Async operation context for deferred cleanup
struct AsyncBatchContext {
    std::vector<backends::BufferDescriptor> descriptors;
    backends::IBackendProvider* backend;  // not owned
    AccelEvent completion_event;

    AsyncBatchContext(backends::IBackendProvider* b, IAccelerator* accel)
        : backend(b), completion_event(accel->create_event()) {}
};

class IoEngineImpl : public IIoEngine {
public:
    IoEngineImpl(backends::IBackendProvider* backend, IAccelerator* accel);
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

    uint32_t max_entries_per_batch() const override;

    uint32_t slice_fanout(const MemoryRegion* region) const override;

private:
    backends::IBackendProvider* backend_;  // not owned
    IAccelerator* accel_;        // not owned

    backends::BufferDescriptor* d_descs_;  // GPU-resident scratch for descriptors
    uint32_t max_batch_entries_;

    // Track pending async operations for deferred cleanup
    std::vector<std::shared_ptr<AsyncBatchContext>> pending_async_ops_;

    // Poll and cleanup completed async operations
    void cleanup_completed_async_ops();
};

} // namespace tutti
