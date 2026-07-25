#pragma once
/**
 * daemon_nvme_queue_alloc.h -- CUDA queue allocation helper for DaemonNvmeDeviceDriver.
 *
 * This header is included by both the pure-C++ driver (.cpp) and the CUDA
 * implementation (.cu).  It uses only C/C++ types so it compiles under both
 * host-only and nvcc translation units.
 *
 * Caller sequence (mirrors nvmeservice_client_io.cu):
 *
 *   // After gRPC Connect + nvm_ctrl_attach_client + nvm_create_group:
 *   DaemonQueueAllocArgs args = { ctrl, group_id, n_queues, queue_depth,
 *                                 namespace_id, cuda_device, blk_size, blk_size_log };
 *   DaemonQueueSet qs{};
 *   int rc = daemon_nvme_alloc_queues(&args, &qs);
 *   // qs.d_qps is now a device pointer to QueuePair[n_queues], typed as nvm_queue_t*
 *   // (queue_acquire_helper_impl.cuh treats it as QueuePair* -- see comment there).
 *
 *   // On vdevice free:
 *   daemon_nvme_free_queues(&qs);
 */

#include <nvm_types.h>  // nvm_ctrl_t, nvm_queue_t

#ifdef __cplusplus
extern "C" {
#endif

/** Input parameters for one queue-pair allocation batch. */
struct DaemonQueueAllocArgs {
    nvm_ctrl_t* ctrl;
    uint32_t    group_id;
    uint32_t    n_queues;       // Number of SQ+CQ pairs to request
    uint32_t    queue_depth;    // Entries per ring (from sess->queue_depth)
    uint32_t    namespace_id;   // NVMe namespace id (from sess->namespace_id)
    int         cuda_device;    // CUDA device index
    uint32_t    blk_size;
    uint32_t    blk_size_log;
    uint32_t    page_size;      // Controller page size (from ctrl->page_size)
};

/**
 * Result / cleanup handle.
 *
 * d_qps is typed as nvm_queue_t* for the NvmeVirtualDevice interface, but it
 * actually points to a QueuePair[] on CUDA managed memory.  queue_acquire_helper_impl.cuh
 * casts it back to QueuePair* when it accesses ->sq, ->cq, ->nvmNamespace.
 *
 * opaque owns all resources allocated by daemon_nvme_alloc_queues().  Pass it
 * verbatim to daemon_nvme_free_queues() for cleanup.
 */
struct DaemonQueueSet {
    nvm_queue_t* d_qps;     // device-accessible QueuePair[] (typed as nvm_queue_t*)
    uint32_t     n_queues;
    void*        opaque;    // internal cleanup handle; do not inspect
};

/**
 * Allocate n_queues SQ+CQ pairs on the GPU, register their ring buffers with the
 * snvme kernel driver, and issue a single NVM_ADD_USER_QUEUE ioctl batch.
 *
 * Fills out->d_qps with a device-accessible pointer to a QueuePair[] array.
 * Returns 0 on success, positive errno on failure.
 *
 * Thread-safety: each call is independent; multiple calls on the same ctrl are
 * safe as long as the group has capacity (max NVM_MAX_QUEUES_PER_GROUP = 16).
 */
int daemon_nvme_alloc_queues(const struct DaemonQueueAllocArgs* args,
                             struct DaemonQueueSet*             out);

/**
 * Free all GPU buffers and DMA mappings allocated by daemon_nvme_alloc_queues().
 * Caller must ensure no GPU kernels are accessing qs->d_qps when this is called.
 */
void daemon_nvme_free_queues(struct DaemonQueueSet* qs);

#ifdef __cplusplus
}
#endif
