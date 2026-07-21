// tutti/coordinator/include/device.h
// Layer 6: Coordinator - Device Handle
//
// High-level device handle for Coordinator

#pragma once

#include "tutti/device_manager/include/vdevice.h"
#include <string>

namespace tutti {

// Device handle for Coordinator
struct Device {
    VDevice vdev;
    std::string device_path;
    int device_fd;
    bool is_initialized;

    Device() : device_fd(-1), is_initialized(false) {}
};

} // namespace tutti
