#pragma once
#include <cstdint>
#include <string>
#include "common/vdevice.h"

namespace tutti {

// Level-2 allocator: carves vDevices from the in-process QP grant.
class IVirtualNvme {
public:
    virtual ~IVirtualNvme() = default;

    // Carve a VDevice from phys_device_id's QP pool.
    // quota: number of QPs to reserve for this backend.
    // Returns nullptr if:
    //   - phys_id unknown
    //   - quota == 0
    //   - insufficient QPs remain in the pool
    // Error message written to *error if provided.
    virtual VDevice* open_vdevice(
        int32_t phys_id,
        uint32_t quota,
        std::string* error = nullptr) = 0;

    // Return the QP slice back to the pool. No-op on nullptr.
    // The VDevice* becomes invalid after this call.
    virtual void close_vdevice(VDevice* vdev) = 0;

    // Remaining unallocated QPs for a given physical device.
    virtual uint32_t available_queues(int32_t phys_id) const = 0;

    // Capability bitmask of the underlying physical device.
    virtual uint32_t caps(int32_t phys_id) const = 0;
};

} // namespace tutti
