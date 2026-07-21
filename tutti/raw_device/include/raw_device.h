// tutti/raw_device/include/raw_device.h
// Raw Device interface - direct namespace + LBA access

#pragma once

#include "tutti/types/storage_target.h"
#include "tutti/accel/include/accel_types.h"
#include <cstdint>

namespace tutti {

class IRawDevice {
public:
    virtual ~IRawDevice() = default;

    // Acquire a raw device target (namespace + LBA range)
    virtual StorageTarget acquire_raw_target(uint32_t device_index,
                                            uint32_t namespace_id,
                                            uint64_t start_lba,
                                            uint64_t lba_count,
                                            AccelStream stream) = 0;

    // Release a raw device target
    virtual bool release_raw_target(const StorageTarget& target,
                                   AccelStream stream) = 0;

    // Query namespace properties
    virtual uint32_t get_lba_size(uint32_t device_index,
                                  uint32_t namespace_id) const = 0;
};

}  // namespace tutti
