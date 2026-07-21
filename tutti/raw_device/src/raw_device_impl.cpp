// tutti/raw_device/src/raw_device_impl.cpp
// Layer 5: Raw Device - Implementation

#include "raw_device_impl.h"

namespace tutti {

RawDeviceImpl::RawDeviceImpl(IIoEngine* io_engine)
    : io_engine_(io_engine) {
}

RawDeviceImpl::~RawDeviceImpl() {
}

int RawDeviceImpl::open_namespace(uint32_t nsid) {
    // TODO: Implement namespace opening
    (void)nsid;
    return 0;
}

void RawDeviceImpl::close_namespace(uint32_t nsid) {
    // TODO: Implement namespace closing
    (void)nsid;
}

int RawDeviceImpl::read(
    uint32_t nsid,
    void* buffer,
    uint64_t lba,
    uint32_t num_blocks,
    AccelStream stream)
{
    if (!buffer) {
        return -1;
    }

    // TODO: Convert LBA to IoRequest and submit
    (void)nsid;
    (void)lba;
    (void)num_blocks;
    (void)stream;

    return 0;
}

int RawDeviceImpl::write(
    uint32_t nsid,
    const void* buffer,
    uint64_t lba,
    uint32_t num_blocks,
    AccelStream stream)
{
    if (!buffer) {
        return -1;
    }

    // TODO: Convert LBA to IoRequest and submit
    (void)nsid;
    (void)lba;
    (void)num_blocks;
    (void)stream;

    return 0;
}

} // namespace tutti
