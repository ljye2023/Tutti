// tutti/raw_device/src/raw_device_impl.h
// Layer 5: Raw Device - Implementation (private header)

#pragma once

#include "tutti/raw_device/include/raw_device.h"
#include "tutti/io_engine/include/io_engine.h"
#include <memory>

namespace tutti {

class RawDeviceImpl : public IRawDevice {
public:
    RawDeviceImpl(IIoEngine* io_engine);
    virtual ~RawDeviceImpl();

    // IRawDevice interface
    int open_namespace(uint32_t nsid) override;
    void close_namespace(uint32_t nsid) override;

    int read(
        uint32_t nsid,
        void* buffer,
        uint64_t lba,
        uint32_t num_blocks,
        AccelStream stream) override;

    int write(
        uint32_t nsid,
        const void* buffer,
        uint64_t lba,
        uint32_t num_blocks,
        AccelStream stream) override;

private:
    IIoEngine* io_engine_;
};

} // namespace tutti
