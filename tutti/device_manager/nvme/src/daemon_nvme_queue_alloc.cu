/**
 * daemon_nvme_queue_alloc.cu -- CUDA queue allocation for DaemonNvmeDeviceDriver.
 *
 * Mirrors the libnvm bring-up in nvmeservice_client_io.cu, but allocates N
 * queue pairs in one batch and returns them as a QueuePair[] on managed memory
 * (accessible from both CPU and GPU).
 *
 * Why managed (not plain cudaMalloc)?
 *   QueuePair contains C++ members (DmaPtr, BufferPtr) whose constructors must
 *   run on the CPU.  cudaMallocManaged gives us CPU-constructible, GPU-readable
 *   unified memory.  The GPU-visible fields (sq.db, sq.vaddr, cq.db, etc.) are
 *   written on the CPU side after nvm_add_user_queue returns, so there is no
 *   race.
 *
 * Note: this TU must NOT include any gRPC / protobuf headers.  nvcc chokes on
 * their C++17 inline-static-variable traits.  All gRPC interaction lives in
 * daemon_nvme_device_driver.cpp.
 */

#include "daemon_nvme_queue_alloc.h"
// ctrl.h (libnvm internal) includes queue.h at line 29 AFTER its own forward
// declarations, so including ctrl.h first resolves the QueuePair dependency.
// Including queue.h before ctrl.h creates a circular include (queue.h brings
// in ctrl.h, ctrl.h #include-guards queue.h, then ctrl.h uses QueuePair that
// queue.h hasn't finished defining yet).
#include "ctrl.h"        // Controller; transitively pulls in queue.h + buffer.h
#include <nvm_ctrl.h>    // nvm_create_group / nvm_add_user_queue / nvm_destroy_group
#include <nvm_dma.h>     // nvm_dma_map_ring_device / nvm_dma_unmap
#include <ioctl.h>       // nvm_ioctl_add_user_queue / NVM_MAX_QUEUES_PER_GROUP

#include <cuda_runtime.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>       // placement new

// ---------------------------------------------------------------------------
// Internal cleanup handle
// ---------------------------------------------------------------------------

struct DaemonQueueInternals {
    QueuePair**  qpairs;        // [n_queues] QueuePair* (each on managed memory)
    uint32_t     n_queues;
};

// ---------------------------------------------------------------------------
// daemon_nvme_alloc_queues
// ---------------------------------------------------------------------------

extern "C"
int daemon_nvme_alloc_queues(const DaemonQueueAllocArgs* args, DaemonQueueSet* out)
{
    if (!args || !out || !args->ctrl || args->n_queues == 0) {
        return EINVAL;
    }
    if (args->n_queues > NVM_MAX_QUEUES_PER_GROUP) {
        std::fprintf(stderr,
            "[daemon_nvme_alloc_queues] n_queues=%u exceeds NVM_MAX_QUEUES_PER_GROUP=%d\n",
            args->n_queues, NVM_MAX_QUEUES_PER_GROUP);
        return EINVAL;
    }

    const uint32_t N = args->n_queues;
    int rc = 0;

    if (cudaSetDevice(args->cuda_device) != cudaSuccess) {
        std::fprintf(stderr,
            "[daemon_nvme_alloc_queues] cudaSetDevice(%d) failed\n", args->cuda_device);
        return EFAULT;
    }

    // -----------------------------------------------------------------------
    // Allocate internal bookkeeping
    // -----------------------------------------------------------------------
    auto* internals = new (std::nothrow) DaemonQueueInternals{};
    if (!internals) return ENOMEM;
    internals->n_queues = N;
    internals->qpairs = new (std::nothrow) QueuePair*[N]{};
    if (!internals->qpairs) {
        delete internals;
        return ENOMEM;
    }

    // Managed memory block for the QueuePair array:
    //   GPU kernels read it; CPU code constructs it.
    QueuePair* qpairs_dev = nullptr;
    cudaError_t cerr = cudaMallocManaged(reinterpret_cast<void**>(&qpairs_dev),
                                         N * sizeof(QueuePair));
    if (cerr != cudaSuccess) {
        std::fprintf(stderr, "[daemon_nvme_alloc_queues] cudaMallocManaged(QueuePair[%u]): %s\n",
                     N, cudaGetErrorString(cerr));
        delete[] internals->qpairs;
        delete internals;
        return ENOMEM;
    }

    // -----------------------------------------------------------------------
    // Construct QueuePair objects in-place (B3 ctor: allocates ring DMA via
    // create_ring_Dma which calls nvm_dma_map_ring_device internally).
    // defer_gpu_init=true: we call init_gpu_specific_struct AFTER
    // nvm_add_user_queue so doorbell offsets are available.
    // -----------------------------------------------------------------------
    uint32_t constructed = 0;
    for (uint32_t i = 0; i < N; ++i) {
        try {
            new (&qpairs_dev[i]) QueuePair(
                args->ctrl,
                (uint32_t)args->cuda_device,
                (uint16_t)i,          // qp_id within this vdevice (0-based)
                (uint64_t)args->queue_depth,
                args->group_id,
                /*defer_gpu_init=*/true);
            internals->qpairs[i] = &qpairs_dev[i];
            ++constructed;
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[daemon_nvme_alloc_queues] QueuePair[%u] ctor failed: %s\n", i, e.what());
            rc = EFAULT;
            break;
        }
    }
    if (rc != 0) goto out_destruct;

    // -----------------------------------------------------------------------
    // Build NVM_ADD_USER_QUEUE batch (all N pairs at once).
    // QueuePair ctor filled sq_mem / cq_mem with ring DMA; we read their
    // vaddr back as the SQ/CQ ring GPU virtual addresses.
    // -----------------------------------------------------------------------
    {
        struct nvm_ioctl_add_user_queue add_req;
        std::memset(&add_req, 0, sizeof(add_req));
        add_req.group_id = args->group_id;
        add_req.nr_pairs = N;

        for (uint32_t i = 0; i < N; ++i) {
            add_req.pairs[i].sq_vaddr =
                (uint64_t)(uintptr_t)qpairs_dev[i].sq_mem.get()->vaddr;
            add_req.pairs[i].cq_vaddr =
                (uint64_t)(uintptr_t)qpairs_dev[i].cq_mem.get()->vaddr;
        }

        rc = nvm_add_user_queue(args->ctrl, &add_req);
        if (rc != 0) {
            std::fprintf(stderr,
                "[daemon_nvme_alloc_queues] nvm_add_user_queue failed errno=%d\n", rc);
            goto out_destruct;
        }

        // Get GPU-accessible doorbell pointer for BAR0.
        void* bar0_gpu = nullptr;
        if (cudaHostGetDevicePointer(&bar0_gpu,
                                     (void*)args->ctrl->mm_ptr, 0) != cudaSuccess) {
            std::fprintf(stderr,
                "[daemon_nvme_alloc_queues] cudaHostGetDevicePointer(BAR0) failed\n");
            rc = EFAULT;
            goto out_destruct;
        }

        // Populate the nvm_queue_t fields that GPU code needs, then call
        // init_gpu_specific_struct (allocates ticket/cid/head_mark/pos_locks arrays).
        for (uint32_t i = 0; i < N; ++i) {
            QueuePair& qp = qpairs_dev[i];
            const auto& out_p = add_req.out_pairs[i];

            // SQ ring
            qp.sq.no          = (uint16_t)out_p.qid;
            qp.sq.es          = 64;                   // SQE size always 64 B
            qp.sq.phase       = 0;
            qp.sq.local       = 0;
            qp.sq.vaddr       = qp.sq_mem.get()->vaddr;
            qp.sq.ioaddr      = qp.sq_mem.get()->ioaddrs[0];
            qp.sq.db          = (volatile uint32_t*)(
                                    (char*)bar0_gpu + out_p.sq_doorbell_offset);

            // CQ ring
            qp.cq.no          = (uint16_t)out_p.qid;
            qp.cq.es          = 16;                   // CQE size always 16 B
            qp.cq.phase       = 1;                    // initial phase bit = 1
            qp.cq.local       = 0;
            qp.cq.vaddr       = qp.cq_mem.get()->vaddr;
            qp.cq.ioaddr      = qp.cq_mem.get()->ioaddrs[0];
            qp.cq.db          = (volatile uint32_t*)(
                                    (char*)bar0_gpu + out_p.cq_doorbell_offset);

            // Namespace / block metadata used by issue_nvme_cmd.
            qp.nvmNamespace        = args->namespace_id;
            qp.block_size          = args->blk_size;
            qp.block_size_log      = args->blk_size_log;
            qp.block_size_minus_1  = (args->blk_size > 0) ? (args->blk_size - 1) : 0;
            qp.pageSize            = args->page_size;

            // GPU-side ticket/cid/head_mark/pos_locks arrays.
            qp.init_gpu_specific_struct((uint32_t)args->cuda_device);
        }
    }

    // Success path.
    out->d_qps    = reinterpret_cast<nvm_queue_t*>(qpairs_dev);
    out->n_queues = N;
    out->opaque   = internals;
    return 0;

out_destruct:
    // Destroy already-constructed QueuePair objects (their DmaPtr dtors unmap).
    for (uint32_t i = 0; i < constructed; ++i) {
        qpairs_dev[i].~QueuePair();
    }
    cudaFree(qpairs_dev);
    delete[] internals->qpairs;
    delete internals;
    return (rc != 0) ? rc : EFAULT;
}

// ---------------------------------------------------------------------------
// daemon_nvme_free_queues
// ---------------------------------------------------------------------------

extern "C"
void daemon_nvme_free_queues(DaemonQueueSet* qs)
{
    if (!qs || !qs->opaque) return;

    auto* internals = static_cast<DaemonQueueInternals*>(qs->opaque);
    const uint32_t N = internals->n_queues;

    // The QueuePair array lives on managed memory.  Retrieve the base pointer
    // from the first entry (all entries are in the same managed block).
    if (N > 0 && internals->qpairs[0] != nullptr) {
        QueuePair* base = internals->qpairs[0];
        // Destroy each QueuePair: DmaPtr / BufferPtr dtors run nvm_dma_unmap
        // and cudaFree for the ring + tracking buffers.
        for (uint32_t i = 0; i < N; ++i) {
            base[i].~QueuePair();
        }
        cudaFree(base);
    }

    delete[] internals->qpairs;
    delete internals;

    qs->d_qps   = nullptr;
    qs->n_queues = 0;
    qs->opaque  = nullptr;
}
