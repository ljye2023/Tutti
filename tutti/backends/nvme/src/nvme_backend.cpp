#include "backends/include/storage_target.h"
#include "nvme_backend.h"
#include "nvme_target_handle.h"
#include "device_manager/include/common/vdevice.h"
// TODO: Add proper storage types
#include <cstdint>
#include <cuda_runtime.h>
#include <cstring>
#include <cstring>
#include <cstdio>

// Forward declaration of device kernel (defined in device/submit_batch_kernel.cu)
namespace tutti {
namespace backends {
namespace nvme {
namespace device {

void launch_submit_batch_kernel(
    void* stream,
    NvmeFileDeviceHandle* handle,
    const BufferDescriptor* descs,
    uint32_t n_descs,
    bool is_read);

} // namespace device
} // namespace nvme
} // namespace backends
} // namespace tutti

namespace tutti {
namespace backends {
namespace nvme {

NvmeBackend::NvmeBackend()
    : vdev_(nullptr)
    , prp_cache_(nullptr)
    , descriptor_builder_(nullptr)
{
}

NvmeBackend::~NvmeBackend() {
    cleanup();
}

bool NvmeBackend::initialize(VDevice* vdev) {
    if (vdev == nullptr) {
        fprintf(stderr, "[NvmeBackend] ERROR: initialize called with null vdev\n");
        return false;
    }

    vdev_ = vdev;

    // Validate VDevice parameters
    if (!validate_vdev()) {
        fprintf(stderr, "[NvmeBackend] ERROR: VDevice validation failed\n");
        vdev_ = nullptr;
        return false;
    }

    // Initialize PRP cache with queue_quota sizing
    size_t l1_size = compute_l1_cache_size();
    size_t l2_size = compute_l2_cache_size();

    prp_cache_ = std::make_unique<PrpPageCache>(4096, l1_size, l2_size);

    // Initialize descriptor builder
    descriptor_builder_ = std::make_unique<NvmeCommandBuilder>(
        vdev_->blk_size, vdev_->max_data_size, prp_cache_.get());

    fprintf(stderr, "[NvmeBackend] Initialized: vdev_id=%u, queue_quota=%u, "
            "blk_size=%u, mdts=%zu, l1_cache=%zu, l2_cache=%zu\n",
            vdev_->vdev_id, vdev_->queue_quota, vdev_->blk_size,
            vdev_->max_data_size, l1_size, l2_size);

    return true;
}

void NvmeBackend::cleanup() {
    // Release all target handles
    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        for (auto& entry : target_handles_) {
            const TargetHandleMetadata& meta = entry.second;

            // Free overflow extents if present
            if (meta.overflow_extents != nullptr) {
                cudaFree(meta.overflow_extents);
            }

            // Free device handle
            if (meta.device_ptr != nullptr) {
                cudaFree(meta.device_ptr);
            }
        }
        target_handles_.clear();
    }

    // Free descriptor builder (returns PRP pages to cache)
    descriptor_builder_.reset();

    // Free PRP cache (releases all GPU/host memory)
    prp_cache_.reset();

    // Clear VDevice reference (ownership remains with Device Manager)
    vdev_ = nullptr;

    fprintf(stderr, "[NvmeBackend] Cleaned up\n");
}

bool NvmeBackend::prepare_descriptors(
    const uint64_t* ioaddrs,
    const SubSliceInfo* slices,
    uint32_t n_slices,
    BufferDescriptor* out_descs)
{
    if (!validate_vdev()) {
        return false;
    }

    // Use descriptor builder for PRP construction
    return descriptor_builder_->build_prp_descriptors(ioaddrs, slices, n_slices, out_descs);
}

void NvmeBackend::release_descriptors(BufferDescriptor* descs, uint32_t n_descs) {
    if (descriptor_builder_) {
        descriptor_builder_->release_descriptors(descs, n_descs);
    }
}

// Target handle methods implemented in nvme_target_handle.cpp
// Submission methods implemented in nvme_submission.cpp

bool NvmeBackend::submit_batch_cpu_async(
    IOFuture* future,
    void* target_handle,
    const BufferDescriptor* descs,
    uint32_t n_descs,
    bool is_read)
{
    // Async CPU submission unsupported in v0.2
    return false;
}

bool NvmeBackend::setup_coop_channel(
    const CoopChannelConfig& config,
    void* target_handle)
{
    // COOP mode unsupported in v0.2
    return false;
}

bool NvmeBackend::poll_future(const IOFuture& future, SubmissionResult* out_result) {
    // Async CPU submission unsupported in v0.2
    return false;
}

bool NvmeBackend::wait_future(
    const IOFuture& future,
    uint32_t timeout_ms,
    SubmissionResult* out_result)
{
    // Async CPU submission unsupported in v0.2
    return false;
}

size_t NvmeBackend::max_io_size() const {
    return vdev_ ? vdev_->max_data_size : 0;
}

BackendMetadata NvmeBackend::metadata() const {
    BackendMetadata meta;
    meta.name = backend_name();
    meta.type = backend_type();
    meta.capabilities = SUPPORTS_GPUDIRECT;  // NVMe backend supports GPUDirect
    meta.max_io_size = max_io_size();
    meta.max_batch_size = 4096;  // Typical batch size limit
    meta.alignment_bytes = vdev_ ? vdev_->blk_size : 4096;
    return meta;
}

bool NvmeBackend::validate_vdev() const {
    if (vdev_ == nullptr) {
        return false;
    }

    if (vdev_->d_qps == nullptr) {
        fprintf(stderr, "[NvmeBackend] ERROR: VDevice has null d_qps\n");
        return false;
    }

    if (vdev_->queue_quota == 0) {
        fprintf(stderr, "[NvmeBackend] ERROR: VDevice has zero queue_quota\n");
        return false;
    }

    if (vdev_->blk_size == 0 || vdev_->max_data_size == 0) {
        fprintf(stderr, "[NvmeBackend] ERROR: VDevice has invalid block size or MDTS\n");
        return false;
    }

    return true;
}

size_t NvmeBackend::compute_l1_cache_size() const {
    // L1 pool: ~2 pages per queue typical
    return vdev_ ? (vdev_->queue_quota * 2) : 64;
}

size_t NvmeBackend::compute_l2_cache_size() const {
    // L2 pool: 4x L1 for burst absorption
    return compute_l1_cache_size() * 4;
}

} // namespace nvme
} // namespace backends
} // namespace tutti
