#pragma once
#include <atomic>
#include <string>
#include "common/iphysical_device.h"
#include <nvm_types.h>  // nvm_ctrl_t

namespace tutti {

class NvmeQueueGroup;

/**
 * NvmePhysicalDevice -- IPhysicalDevice for LOCAL_NVME.
 *
 * Represents one NVMe controller from this process's perspective.
 * In direct mode: ctrl_ is non-null (process owns the device).
 * In daemon mode: ctrl_ is null (DeviceService owns it; this is a view).
 *
 * Resource accounting: process_grant_ = total QPs granted; allocated_ = in-use.
 */
class NvmePhysicalDevice : public IPhysicalDevice {
public:
    NvmePhysicalDevice(int32_t id, std::string pci_addr, std::string display_name,
                        uint32_t caps, uint32_t process_grant);
    ~NvmePhysicalDevice() override = default;

    // IPhysicalDevice implementation
    int32_t id() const override { return id_; }
    DeviceType type() const override { return DeviceType::LOCAL_NVME; }
    std::string_view pci_addr() const override { return pci_addr_; }
    std::string_view display_name() const override { return display_name_; }
    uint32_t process_grant() const override { return process_grant_; }
    uint32_t available_grant() const override { return process_grant_ - allocated_.load(); }
    uint32_t caps() const override { return caps_; }

    // Allocation tracking (called by driver on alloc/free_vdevice)
    void reserve(uint32_t count) { allocated_ += count; }
    void release(uint32_t count) { allocated_ -= count; }

    // NVMe-specific fields (read by NVMe driver only)
    nvm_ctrl_t*      ctrl = nullptr;         // direct mode only
    NvmeQueueGroup*  queue_group = nullptr;  // direct mode only
    uint32_t         namespace_id = 0;
    uint32_t         blk_size = 0;
    uint32_t         blk_size_log = 0;
    size_t           max_data_size = 0;

private:
    int32_t id_;
    std::string pci_addr_;
    std::string display_name_;
    uint32_t caps_;
    uint32_t process_grant_;
    std::atomic<uint32_t> allocated_{0};
};

} // namespace tutti
