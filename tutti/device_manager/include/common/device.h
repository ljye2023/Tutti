#pragma once
#include <cstdint>
#include <string>

namespace tutti {

// Backend type enumeration
enum class BackendType {
    LOCAL_NVME = 0,
    RDMA = 1,
    GDS = 2
};

// Device structure representing a physical storage device
struct Device {
    int32_t device_id;
    BackendType backend_type;
    std::string pci_addr;
    std::string display_name;
    void* backend_private;  // Opaque pointer to backend-specific data

    Device()
        : device_id(-1),
          backend_type(BackendType::LOCAL_NVME),
          backend_private(nullptr) {}
};

} // namespace tutti
