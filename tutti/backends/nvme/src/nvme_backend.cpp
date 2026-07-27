// nvme_backend.cpp -- NvmeBackend lifecycle implementation

#include "nvme_backend.h"
#include "prp_page_cache.h"
#include "nvme_command_builder.h"

#include "backends/include/backend_factory.h"
#include "common/idevice_manager.h"
#include "common/ivirtual_device.h"
#include "device_manager/nvme/include/nvme_virtual_device.h"

#include <cuda_runtime.h>
#include <cstdio>

namespace tutti {
namespace backends {
namespace nvme {

NvmeBackend::NvmeBackend() = default;

NvmeBackend::~NvmeBackend() {
    shutdown();
}

bool NvmeBackend::initialize(IDeviceManager* dm, const BackendConfig& cfg) {
    if (dm == nullptr) {
        fprintf(stderr, "[NvmeBackend] ERROR: initialize called with null dm\n");
        return false;
    }
    if (cfg.vdevice_count == 0 || cfg.quota_per_vdevice == 0) {
        fprintf(stderr, "[NvmeBackend] ERROR: invalid config (vdevice_count=%u, quota=%u)\n",
                cfg.vdevice_count, cfg.quota_per_vdevice);
        return false;
    }

    dm_ = dm;
    nvme_vdevices_.reserve(cfg.vdevice_count);

    for (uint32_t i = 0; i < cfg.vdevice_count; ++i) {
        std::string err;
        IVirtualDevice* vdev = dm_->open_vdevice(cfg.phys_id, cfg.quota_per_vdevice, &err);
        if (vdev == nullptr) {
            fprintf(stderr, "[NvmeBackend] ERROR: open_vdevice(%d, %u) failed: %s\n",
                    cfg.phys_id, cfg.quota_per_vdevice, err.c_str());
            // Roll back everything opened so far
            for (IVirtualDevice* opened : nvme_vdevices_) {
                dm_->close_vdevice(opened);
            }
            nvme_vdevices_.clear();
            dm_ = nullptr;
            return false;
        }

        // NVMe backend requires NvmeVirtualDevice
        if (vdev->type() != DeviceType::LOCAL_NVME) {
            fprintf(stderr, "[NvmeBackend] ERROR: open_vdevice returned non-NVMe device (type=%u)\n",
                    static_cast<unsigned>(vdev->type()));
            dm_->close_vdevice(vdev);
            for (IVirtualDevice* opened : nvme_vdevices_) {
                dm_->close_vdevice(opened);
            }
            nvme_vdevices_.clear();
            dm_ = nullptr;
            return false;
        }

        nvme_vdevices_.push_back(static_cast<NvmeVirtualDevice*>(vdev));
    }

    // Initialise shared PRP cache sized for aggregate queue quota
    const uint32_t total_quota = total_queue_quota();
    const size_t l1_size = total_quota * 2;
    const size_t l2_size = l1_size * 4;
    prp_cache_ = std::make_unique<PrpPageCache>(4096, l1_size, l2_size);

    // Use namespace params from the first vdevice (all vdevices share the same
    // physical namespace on one backend). Defensive guard for mock mode: if
    // DaemonNvmeDeviceDriver ran in mock_mode, blk_size / max_data_size default
    // to 0; substitute sane defaults to avoid division by zero in descriptor builder.
    NvmeVirtualDevice* first = nvme_vdevices_[0];
    const uint32_t blk_size = (first->blk_size > 0) ? first->blk_size : 4096;
    const size_t   mdts     = (first->max_data_size > 0) ? first->max_data_size : (512 * 1024);
    descriptor_builder_ = std::make_unique<NvmeCommandBuilder>(
        blk_size, mdts, prp_cache_.get());

    fprintf(stderr, "[NvmeBackend] Initialized: phys_id=%d, vdevice_count=%u, "
            "quota_per_vdevice=%u, blk_size=%u, mdts=%zu\n",
            cfg.phys_id, cfg.vdevice_count, cfg.quota_per_vdevice,
            first->blk_size, first->max_data_size);

    return true;
}

void NvmeBackend::shutdown() {
    if (dm_ == nullptr) {
        return;  // idempotent: never initialized or already shut down
    }

    // Release any outstanding target handles
    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        for (auto& kv : target_handles_) {
            const TargetHandleEntry& entry = kv.second;
            if (entry.inline_extents)  cudaFree(entry.inline_extents);
            if (entry.overflow_extents) cudaFree(entry.overflow_extents);
            if (entry.device_ptr)      cudaFree(entry.device_ptr);
        }
        target_handles_.clear();
    }

    descriptor_builder_.reset();
    prp_cache_.reset();

    for (IVirtualDevice* vdev : nvme_vdevices_) {
        dm_->close_vdevice(vdev);
    }
    nvme_vdevices_.clear();
    dm_ = nullptr;

    fprintf(stderr, "[NvmeBackend] Shut down\n");
}

uint32_t NvmeBackend::vdevice_count() const {
    return static_cast<uint32_t>(nvme_vdevices_.size());
}

IVirtualDevice* NvmeBackend::vdevice_at(uint32_t i) const {
    if (i >= nvme_vdevices_.size()) return nullptr;
    return nvme_vdevices_[i];
}

VDeviceHandle NvmeBackend::vdevice_handle_at(uint32_t i) const {
    if (i >= nvme_vdevices_.size()) return VDeviceHandle{};  // invalid
    return VDeviceHandle{i};
}

BackendMetadata NvmeBackend::metadata() const {
    BackendMetadata m;
    m.name = backend_name();
    m.type = BackendType::LOCAL_NVME;
    m.capabilities = SUPPORTS_GPUDIRECT;
    m.max_io_size   = nvme_vdevices_.empty() ? 0 : nvme_vdevices_[0]->max_data_size;
    m.max_batch_size  = 4096;
    m.alignment_bytes = nvme_vdevices_.empty() ? 4096 : nvme_vdevices_[0]->blk_size;
    return m;
}

bool NvmeBackend::prepare_descriptors(
    const uint64_t*    ioaddrs,
    const SubSliceInfo* slices,
    uint32_t            n_slices,
    BufferDescriptor*   out_descs)
{
    if (!descriptor_builder_) return false;
    return descriptor_builder_->build_prp_descriptors(ioaddrs, slices, n_slices, out_descs);
}

void NvmeBackend::release_descriptors(BufferDescriptor* descs, uint32_t n_descs) {
    if (descriptor_builder_) {
        descriptor_builder_->release_descriptors(descs, n_descs);
    }
}

NvmeVirtualDevice* NvmeBackend::nvme_vdev_at(uint32_t i) const {
    if (i >= nvme_vdevices_.size()) return nullptr;
    return nvme_vdevices_[i];
}

uint32_t NvmeBackend::total_queue_quota() const {
    uint32_t total = 0;
    for (const NvmeVirtualDevice* vdev : nvme_vdevices_) {
        total += vdev->queue_quota;
    }
    return total;
}

} // namespace nvme
} // namespace backends
} // namespace tutti
