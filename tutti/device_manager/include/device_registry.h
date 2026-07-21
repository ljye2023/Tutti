// tutti/device_manager/include/device_registry.h
// Layer 2: Device Manager - Device Registry Interface
//
// Interface for discovering and managing storage devices

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace tutti {

// Device information structure
struct DeviceInfo {
    std::string device_path;     // e.g., "/dev/nvme0n1"
    std::string serial_number;
    uint64_t capacity_bytes;
    uint32_t block_size;
    uint32_t num_queues;
    bool supports_p2p;
};

// Device registry interface
class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;

    // Device discovery
    virtual int discover_devices() = 0;
    virtual std::vector<DeviceInfo> list_devices() const = 0;

    // Device access
    virtual int open_device(const std::string& device_path) = 0;
    virtual void close_device(int device_id) = 0;
};

} // namespace tutti
