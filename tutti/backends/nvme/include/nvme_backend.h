#ifndef TUTTI_BACKENDS_NVME_BACKEND_H_
#define TUTTI_BACKENDS_NVME_BACKEND_H_

#include "backends/include/backend.h"
#include "backends/include/backend_types.h"
#include "backends/include/storage_target.h"
#include "nvme_io_types.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tutti {

struct NvmeVirtualDevice;  // device_manager/nvme/include/nvme_virtual_device.h

} // namespace tutti

namespace tutti {
namespace backends {
namespace nvme {

class NvmeCommandBuilder;
class PrpPageCache;

// Concrete IBackend implementation for local NVMe transport.
//
// Inherits the device-agnostic IBackend contract: lifecycle (initialize /
// shutdown), vdevice roster, and metadata. NVMe-specific operations —
// descriptor preparation, target handle management, and submission — are
// exposed as non-virtual host methods (register-equivalent) or device-side
// helpers (submit_one, see nvme_device_helpers.cuh).
//
// ── Roster ───────────────────────────────────────────────────────────────────
//
//   initialize() opens cfg.vdevice_count vdevices from cfg.phys_id via the
//   Device Manager, downcasting each IVirtualDevice* to NvmeVirtualDevice*.
//   On any failure the partial roster is rolled back and initialize returns
//   false. A shared PRP cache is sized for the aggregate queue quota.
//
// ── NVMe-specific methods (non-virtual, call after initialize()) ──────────────
//
//   acquire_target_handle(target, hdl)
//       Build a GPU-resident NvmeFileDeviceHandle bound to the vdevice
//       selected by hdl (VDeviceHandle). Returns a device pointer (opaque
//       void*), or nullptr on failure.
//
//   release_target_handle(handle)
//       Free the GPU-resident handle and associated overflow extents.
//
//   prepare_descriptors(ioaddrs, slices, n, out_descs)
//       Build PRP descriptors from DMA addresses. Shared across all vdevices.
//
//   release_descriptors(descs, n)
//       Return PRP-list pages to cache.
//
//   launch_batch_gpu_stream(stream, target_handle, descs, n, is_read)
//       Launch the GPU batch submission kernel on the given CUDA stream.
//
//   submit_batch_cpu_sync(target_handle, descs, n, is_read)
//       CPU synchronous submission (bootstrap / testing).
class NvmeBackend : public IBackend {
public:
    NvmeBackend();
    ~NvmeBackend() override;

    NvmeBackend(const NvmeBackend&) = delete;
    NvmeBackend& operator=(const NvmeBackend&) = delete;

    // ── IBackend: lifecycle ──────────────────────────────────────────────────

    // Open cfg.vdevice_count NvmeVirtualDevice slices from cfg.phys_id.
    // Initialises PRP cache sized for total queue quota. Rolls back on partial
    // allocation failure.
    bool initialize(IDeviceManager* dm, const BackendConfig& cfg) override;

    // Return every vdevice to the Device Manager and free backend resources.
    // Idempotent: safe to call more than once.
    void shutdown() override;

    // ── IBackend: vdevice roster ─────────────────────────────────────────────

    uint32_t        vdevice_count()            const override;
    IVirtualDevice* vdevice_at(uint32_t i)     const override;
    VDeviceHandle   vdevice_handle_at(uint32_t i) const override;

    // ── IBackend: metadata ───────────────────────────────────────────────────

    BackendType     backend_type() const override { return BackendType::LOCAL_NVME; }
    const char*     backend_name() const override { return "local_nvme"; }
    BackendMetadata metadata()     const override;

    // ── NVMe-specific: descriptor preparation ───────────────────────────────

    // Build PRP descriptors from raw DMA addresses (one per sub-slice).
    // Returns true on success; false if MDTS exceeded or PRP cache exhausted.
    bool prepare_descriptors(
        const uint64_t*    ioaddrs,
        const SubSliceInfo* slices,
        uint32_t            n_slices,
        BufferDescriptor*   out_descs);

    // Return PRP-list pages used by the descriptors back to the cache.
    void release_descriptors(BufferDescriptor* descs, uint32_t n_descs);

    // ── NVMe-specific: target handle management ──────────────────────────────

    // Build a GPU-resident NvmeFileDeviceHandle bound to the vdevice at hdl.
    // Returns a device pointer (opaque void*), or nullptr on failure.
    void* acquire_target_handle(const StorageTarget& target, VDeviceHandle hdl);

    // Free the GPU-resident handle and its associated overflow extents.
    void release_target_handle(void* handle);

    // ── NVMe-specific: submission ────────────────────────────────────────────

    // Launch the GPU batch submission kernel on stream.
    void launch_batch_gpu_stream(
        void*                    stream,
        void*                    target_handle,
        const BufferDescriptor*  descs,
        uint32_t                 n_descs,
        bool                     is_read);

    // CPU synchronous submission (bootstrap / testing path).
    SubmissionResult submit_batch_cpu_sync(
        void*                   target_handle,
        const BufferDescriptor* descs,
        uint32_t                n_descs,
        bool                    is_read);

private:
    IDeviceManager*                   dm_       = nullptr;  // not owned
    std::vector<NvmeVirtualDevice*>   nvme_vdevices_;       // not owned (owned by DM)

    // Shared across all vdevices (sized for aggregate queue quota)
    std::unique_ptr<PrpPageCache>       prp_cache_;
    std::unique_ptr<NvmeCommandBuilder> descriptor_builder_;

    // Tracking acquired target handles for cleanup in shutdown()
    struct TargetHandleEntry {
        void*    device_ptr;        // GPU pointer to NvmeFileDeviceHandle
        void*    overflow_extents;  // GPU pointer to overflow extents (null if none)
        void*    inline_extents;    // GPU pointer to inline extent array
        uint64_t target_id;         // Source target identifier (debug)
    };
    std::unordered_map<void*, TargetHandleEntry> target_handles_;
    mutable std::mutex                           target_handles_mutex_;

    // Internal helpers
    NvmeVirtualDevice* nvme_vdev_at(uint32_t i) const;
    uint32_t           total_queue_quota() const;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_BACKEND_H_
