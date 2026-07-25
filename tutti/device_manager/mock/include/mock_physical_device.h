#pragma once
#include <atomic>
#include <string>
#include "common/iphysical_device.h"

namespace tutti {

/**
 * MockPhysicalDevice -- in-memory IPhysicalDevice, no real hardware.
 *
 * A first-class backend peer to NvmePhysicalDevice: exercises the
 * vendor-neutral contract and gives higher layers a device to allocate
 * against in unit tests, CI, and bring-up on machines with no NVMe.
 */
class MockPhysicalDevice : public IPhysicalDevice {
public:
    MockPhysicalDevice(int32_t id, DeviceType type, std::string pci_addr,
                       std::string display_name, uint32_t caps, uint32_t grant)
        : id_(id), type_(type), pci_addr_(std::move(pci_addr)),
          display_name_(std::move(display_name)), caps_(caps), grant_(grant) {}

    int32_t          id()              const override { return id_; }
    DeviceType       type()            const override { return type_; }
    std::string_view pci_addr()        const override { return pci_addr_; }
    std::string_view display_name()    const override { return display_name_; }
    uint32_t         process_grant()   const override { return grant_; }
    uint32_t         available_grant() const override { return grant_ - allocated_.load(); }
    uint32_t         caps()            const override { return caps_; }

    // Accounting hooks used by MockDeviceDriver on alloc/free.
    void reserve(uint32_t n) { allocated_ += n; }
    void release(uint32_t n) { allocated_ -= n; }

private:
    int32_t     id_;
    DeviceType  type_;
    std::string pci_addr_;
    std::string display_name_;
    uint32_t    caps_;
    uint32_t    grant_;
    std::atomic<uint32_t> allocated_{0};
};

} // namespace tutti
