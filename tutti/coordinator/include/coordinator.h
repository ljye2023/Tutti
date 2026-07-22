#pragma once

#include "coordinator_types.h"
#include "raw_device.h"
#include "accel/include/common/accel_types.h"
#include "io_engine/include/io_types.h"
#include <cstdint>

namespace tutti {

namespace block_storage {
class IBlockStorage;
}

namespace coordinator {

class ICoordinator {
public:
    virtual ~ICoordinator() = default;

    virtual bool initialize(const CoordinatorConfig& config) = 0;

    virtual bool cleanup() = 0;

    virtual MemoryRegion* register_buffer(
        void* ptr,
        size_t size,
        MemoryKind kind) = 0;

    virtual bool unregister_buffer(MemoryRegion* region) = 0;

    virtual BatchSubmitResult submit_read_batch(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream = AccelStream()) = 0;

    virtual BatchSubmitResult submit_write_batch(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream = AccelStream()) = 0;

    virtual bool submit_read_batch_async(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream,
        BatchCompletionCallback callback,
        void* user_data) = 0;

    virtual bool submit_write_batch_async(
        IoRequest* requests,
        uint32_t count,
        AccelStream stream,
        BatchCompletionCallback callback,
        void* user_data) = 0;

    virtual block_storage::IBlockStorage* get_block_storage() = 0;

    virtual IRawDevice* get_raw_device() = 0;

    virtual uint32_t max_batch_size() const = 0;

    virtual uint32_t slice_fanout(MemoryRegion* region) const = 0;
};

ICoordinator* create_coordinator();

void destroy_coordinator(ICoordinator* coordinator);

} // namespace coordinator
} // namespace tutti
