#pragma once
#include "common/ivirtual_device.h"

// Include full libnvm typedef (not forward-declarable as struct)
#include <nvm_types.h>

namespace tutti {

/**
 * NvmeVirtualDevice -- IVirtualDevice subtype for LOCAL_NVME backends.
 *
 * Backends that handle NVMe storage receive an NvmeVirtualDevice from
 * IDeviceManager::open_vdevice(). After checking type() == LOCAL_NVME
 * they static_cast<NvmeVirtualDevice*> to access the NVMe-specific fields.
 *
 * The GPU-resident queue slice (d_qps) and the namespace view (namespace_id,
 * blk_size, max_data_size) must NOT be stored in IVirtualDevice because they
 * are NVMe-specific types/concerns. Keeping them here preserves the
 * vendor-neutral IVirtualDevice contract.
 *
 * Capability bits (caps_):
 *   bit 0: GPUDIRECT_CAPABLE (NvmeQueueGroup was successfully allocated)
 *   bits 1-31: reserved
 */
struct NvmeVirtualDevice : IVirtualDevice {
    // ── IVirtualDevice implementation ─────────────────────────────────────
    int32_t     phys_id()        const override { return phys_device_id_; }
    uint32_t    vdev_id()        const override { return vdev_id_;         }
    DeviceType type()           const override { return DeviceType::LOCAL_NVME; }
    uint32_t    resource_count() const override { return queue_quota;      }
    uint32_t    caps()           const override { return caps_;            }

    // ── NVMe-specific fields (read by NVMe backends only) ─────────────────

    // GPU-resident queue slice: d_qps[0..queue_quota-1] belong to this vdev.
    // Actually points to QueuePair[] on CUDA managed memory; typed nvm_queue_t*
    // for interface uniformity. Cast to QueuePair* to access .sq/.cq fields.
    nvm_queue_t* d_qps       = nullptr;
    uint32_t     queue_quota = 0;       // number of QPs in this slice

    // libnvm controller handle. Non-null only when allocated via the real
    // gRPC path (mock_mode=false). Needed by backends to call
    // nvm_dma_map_data_device() for IO buffer DMA registration.
    nvm_ctrl_t*  ctrl        = nullptr;

    // Namespace view (copied from the owning NvmePhysicalDevice)
    uint32_t namespace_id  = 0;
    uint32_t blk_size      = 0;
    uint32_t blk_size_log  = 0;
    size_t   max_data_size = 0;         // MDTS in bytes

    // ── Constructor (used by Direct/Daemon NvmeDeviceDriver) ──────────────
    NvmeVirtualDevice(int32_t phys_id, uint32_t vdev_id, uint32_t caps)
        : phys_device_id_(phys_id), vdev_id_(vdev_id), caps_(caps) {}

private:
    int32_t  phys_device_id_;
    uint32_t vdev_id_;
    uint32_t caps_;
};

} // namespace tutti
