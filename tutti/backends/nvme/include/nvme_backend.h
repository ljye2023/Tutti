#ifndef TUTTI_BACKENDS_NVME_BACKEND_H_
#define TUTTI_BACKENDS_NVME_BACKEND_H_

#include "backends/include/backend_provider.h"
#include "nvme_command_builder.h"
#include "prp_page_cache.h"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace tutti {
namespace backends {
namespace nvme {

// Concrete IBackendProvider implementation for NVMe transport.
//
// Responsibilities:
//   - Consume VDevice from Device Manager at initialize()
//   - Build PRP/SGL descriptors from ioaddrs via NvmeCommandBuilder
//   - Manage GPU-resident target handles (NvmeFileDeviceHandle)
//   - Launch GPU batch submission kernel on AccelStream
//   - Provide CPU synchronous submission path for bootstrap/testing
//
// Dependencies (consuming downward):
//   - Device Manager: VDevice (queues + namespace view + caps) at initialize()
//   - Accelerator HAL: AccelStream, dma_map, cudaMalloc/cudaMemcpy wrappers
//   - Abstraction: TUTTI_DEVICE, TUTTI_GLOBAL, TUTTI_LAUNCH_KERNEL macros
//
// Consumed by (upward):
//   - IO Engine: calls prepare_descriptors, launch_batch_gpu_stream, submit_batch_cpu_sync
//   - Block Storage: calls acquire_target_handle, release_target_handle
//
// Key design notes:
//   - Receives VDevice once at initialize(); steady-state IO never calls DM
//   - Owns PRP-list page cache internally (two-tier: GPU L1 + host-pinned L2)
//   - Launches backend-specific submit_batch_kernel on GPU stream
//   - Target handle is NvmeFileDeviceHandle: extents + reference to vdev->d_qps
//   - Steady-state IO uses DM's device-side queue mechanics (no hot-path DM calls)
class NvmeBackend : public IBackendProvider {
public:
    NvmeBackend();
    ~NvmeBackend() override;

    // Disable copy/move - backend manages GPU resources
    NvmeBackend(const NvmeBackend&) = delete;
    NvmeBackend& operator=(const NvmeBackend&) = delete;

    // ========== LIFECYCLE ==========

    // Initialize backend with Device Manager VDevice.
    //
    // Stores vdev pointer, initializes PRP cache with queue_quota sizing:
    //   L1 pool: queue_quota * 2 pages (GPU-resident)
    //   L2 pool: queue_quota * 8 pages (host-pinned)
    //
    // vdev: VDevice pointer from Device Manager (non-null for NVMe backends)
    //
    // Returns true on success, false on failure (backend unusable if false).
    bool initialize(VDevice* vdev) override;

    // Clean up backend resources.
    //
    // Releases all target handles, frees PRP cache, clears vdev reference.
    // Does NOT call Device Manager directly - caller returns vdev to DM.
    void cleanup() override;

    // ========== DESCRIPTOR PREPARATION ==========

    // Prepare PRP descriptors from raw DMA addresses.
    //
    // Calls descriptor_builder_->build_prp_descriptors() for PRP construction.
    // Falls back to SGL if controller supports and transfer exceeds PRP limits (future).
    //
    // Returns true on success, false on failure (out of PRP pages, MDTS exceeded).
    bool prepare_descriptors(
        const uint64_t* ioaddrs,
        const SubSliceInfo* slices,
        uint32_t n_slices,
        BufferDescriptor* out_descs) override;

    // Release descriptors and return PRP-list pages to cache.
    void release_descriptors(BufferDescriptor* descs, uint32_t n_descs) override;

    // ========== TARGET HANDLE MANAGEMENT ==========

    // Acquire GPU-resident target handle from StorageTarget.
    //
    // Algorithm:
    //   1. Validate target kind (NVME_FILE or NVME_RAW only)
    //   2. Allocate host-side NvmeFileDeviceHandle on stack
    //   3. Populate fields: file_id, logical_size_bytes, nvme_block_size from target + vdev
    //   4. Copy extents: if num_extents <= 8, inline; else inline + overflow cudaMalloc
    //   5. Set vdev pointer to backend's vdev_ member (reference, not ownership)
    //   6. cudaMalloc device handle, cudaMemcpy host template to device
    //   7. Track in target_handles_ map for cleanup
    //
    // Returns device pointer (opaque void*), or nullptr on failure.
    void* acquire_target_handle(const StorageTarget& target) override;

    // Release GPU-resident target handle and free resources.
    //
    // cudaFree GPU handle, free overflow extents if present, remove from tracking map.
    void release_target_handle(void* handle) override;

    // ========== SUBMISSION MODES ==========

    // REQUIRED: Launch GPU batch submission kernel on AccelStream.
    //
    // Casts target_handle to NvmeFileDeviceHandle*, computes grid/block dimensions,
    // launches submit_batch_kernel<<<>>> on stream.
    //
    // Grid/block sizing: block_size = 256, grid_size = (n_descs + 255) / 256.
    // Kernel uses DM's device-side queue mechanics (acquire_queue, issue_nvme_cmd, poll).
    void launch_batch_gpu_stream(
        void* stream,
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) override;

    // REQUIRED: CPU synchronous submission.
    //
    // CPU-side: resolve LBAs, build NVMe commands, call libnvm nvm_cmd_read/write,
    // poll completion queue, return success/failure.
    //
    // Used for bootstrap, metadata operations, and testing.
    SubmissionResult submit_batch_cpu_sync(
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) override;

    // OPTIONAL: Asynchronous CPU submission - unsupported in v0.2.
    bool submit_batch_cpu_async(
        IOFuture* future,
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) override;

    // OPTIONAL: Setup COOP channel - unsupported in v0.2.
    bool setup_coop_channel(
        const CoopChannelConfig& config,
        void* target_handle) override;

    // OPTIONAL: Poll IOFuture - unsupported in v0.2.
    bool poll_future(const IOFuture& future, SubmissionResult* out_result) override;

    // OPTIONAL: Wait for IOFuture - unsupported in v0.2.
    bool wait_future(
        const IOFuture& future,
        uint32_t timeout_ms,
        SubmissionResult* out_result) override;

    // ========== METADATA ==========

    BackendType backend_type() const override { return BackendType::LOCAL_NVME; }
    const char* backend_name() const override { return "local_nvme"; }
    size_t max_io_size() const override;
    BackendMetadata metadata() const override;

private:
    // Device Manager reference
    VDevice* vdev_;

    // PRP-list page cache (two-tier: GPU L1 + host-pinned L2)
    std::unique_ptr<PrpPageCache> prp_cache_;

    // Descriptor builder (PRP/SGL construction logic)
    std::unique_ptr<NvmeCommandBuilder> descriptor_builder_;

    // Target handle tracking (for cleanup validation)
    struct TargetHandleMetadata {
        void* device_ptr;            // GPU pointer to NvmeFileDeviceHandle
        void* overflow_extents;      // GPU pointer to overflow extents (nullptr if none)
        uint32_t num_extents;        // Total extent count
        uint64_t target_id;          // Source target identifier
    };
    std::unordered_map<void*, TargetHandleMetadata> target_handles_;
    std::mutex target_handles_mutex_;

    // Internal helpers
    bool validate_vdev() const;
    size_t compute_l1_cache_size() const;
    size_t compute_l2_cache_size() const;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_BACKEND_H_
