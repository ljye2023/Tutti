// tutti/device_manager/src/local_nvme_virtual.h
// Layer 2: Device Manager - Local NVMe Virtual Registry (private header)

#pragma once

#include "tutti/device_manager/include/virtual_nvme.h"
#include "tutti/device_manager/include/vdevice.h"
#include <vector>
#include <memory>

namespace tutti {

// Local NVMe virtual registry implementation
class LocalNvmeVirtualRegistry : public IVirtualNvme {
public:
    LocalNvmeVirtualRegistry();
    virtual ~LocalNvmeVirtualRegistry();

    // IVirtualNvme interface
    int initialize() override;
    void shutdown() override;

    VDevice allocate_vdevice(uint32_t num_queues) override;
    void free_vdevice(const VDevice& vdev) override;

private:
    bool initialized_;
    std::vector<VDevice> allocated_devices_;
};

} // namespace tutti
