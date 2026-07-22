#pragma once

#include "../include/coordinator.h"
#include "../include/coordinator_types.h"
#include "buffer_registry.h"
#include "batch_builder.h"
#include "raw_device_impl.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "block_storage/include/block_storage.h"
#include "io_engine/include/io_engine.h"
#include <mutex>
#include <unordered_set>

namespace tutti {
namespace coordinator {

class CoordinatorImpl : public ICoordinator {
public:
    CoordinatorImpl();
    ~CoordinatorImpl() override;

    bool initialize(const CoordinatorConfig& config) override;

    bool cleanup() override;

    MemoryRegion* register_buffer(
        void* ptr,
        size_t size,
        MemoryKind kind) override;

    bool unregister_buffer(MemoryRegion* region) override;

    BatchSubmitResult submit_read_batch(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream) override;

    BatchSubmitResult submit_write_batch(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream) override;

    bool submit_read_batch_async(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream,
        BatchCompletionCallback callback,
        void* user_data) override;

    bool submit_write_batch_async(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream,
        BatchCompletionCallback callback,
        void* user_data) override;

    block_storage::IBlockStorage* get_block_storage() override;

    IRawDevice* get_raw_device() override;

    uint32_t max_batch_size() const override;

    uint32_t slice_fanout(MemoryRegion* region) const override;

private:
    bool initialized_ = false;
    mutable std::mutex init_lock_;

    CoordinatorConfig config_;
    backends::IBackendProvider* backend_provider_ = nullptr;
    IAccelerator* accelerator_ = nullptr;
    block_storage::IBlockStorage* block_storage_ = nullptr;
    tutti::IIoEngine* io_engine_ = nullptr;

    AccelStream default_stream_;
    bool owns_default_stream_ = false;

    BufferRegistry buffer_registry_;
    std::unique_ptr<RawDeviceImpl> raw_device_impl_;
    BatchBuilder batch_builder_;

    std::unordered_set<RawTargetHandle*> open_raw_targets_;
    mutable std::mutex targets_lock_;

    bool validate_requests(IoRequest* requests, uint32_t count) const;
};

} // namespace coordinator
} // namespace tutti
