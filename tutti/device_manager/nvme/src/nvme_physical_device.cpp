// nvme_physical_device.cpp -- NvmePhysicalDevice constructor

#include "nvme_physical_device.h"

namespace tutti {

NvmePhysicalDevice::NvmePhysicalDevice(int32_t     id,
                                        std::string pci_addr,
                                        std::string display_name,
                                        uint32_t    caps,
                                        uint32_t    process_grant)
    : id_(id)
    , pci_addr_(std::move(pci_addr))
    , display_name_(std::move(display_name))
    , caps_(caps)
    , process_grant_(process_grant)
{}

} // namespace tutti
