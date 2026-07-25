#pragma once
#include <cstdint>
#include "common/ivirtual_device.h"

namespace tutti {

/**
 * MockVirtualDevice -- in-memory IVirtualDevice (peer to NvmeVirtualDevice).
 *
 * Carries only the generic slice metadata; no transport-specific fields.
 */
class MockVirtualDevice : public IVirtualDevice {
public:
    MockVirtualDevice(int32_t phys_id, uint32_t vdev_id, DeviceType type,
                      uint32_t resource_count, uint32_t caps)
        : phys_id_(phys_id), vdev_id_(vdev_id), type_(type),
          resource_count_(resource_count), caps_(caps) {}

    int32_t    phys_id()        const override { return phys_id_; }
    uint32_t   vdev_id()        const override { return vdev_id_; }
    DeviceType type()           const override { return type_; }
    uint32_t   resource_count() const override { return resource_count_; }
    uint32_t   caps()           const override { return caps_; }

private:
    int32_t    phys_id_;
    uint32_t   vdev_id_;
    DeviceType type_;
    uint32_t   resource_count_;
    uint32_t   caps_;
};

} // namespace tutti
