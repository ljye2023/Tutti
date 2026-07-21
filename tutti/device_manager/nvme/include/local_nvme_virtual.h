#pragma once
#include "common/virtual_nvme.h"
#include <vector>
#include <mutex>

namespace tutti {

class IDeviceRegistry;  // forward-decl

// Concrete Level-2 allocator for local NVMe devices.
class LocalNvmeVirtualRegistry : public IVirtualNvme {
public:
    // Does NOT own registry — caller keeps it alive.
    explicit LocalNvmeVirtualRegistry(IDeviceRegistry* registry);
    ~LocalNvmeVirtualRegistry() override;

    VDevice* open_vdevice(int32_t phys_id, uint32_t quota, std::string* error) override;
    void close_vdevice(VDevice* vdev) override;
    uint32_t available_queues(int32_t phys_id) const override;
    uint32_t caps(int32_t phys_id) const override;

private:
    struct PerDeviceState {
        int32_t phys_id;
        nvm_queue_t* d_qps_base;  // from LocalNvmeDevice::queue_group->d_qps()
        uint32_t total_qps;
        std::vector<bool> allocated;  // free-list: allocated[i] = true if QP i is in use
    };

    IDeviceRegistry* registry_;  // not owned
    std::vector<PerDeviceState> devices_;
    std::vector<VDevice> vdevices_;  // storage for returned VDevice*
    mutable std::mutex mutex_;
};

} // namespace tutti
