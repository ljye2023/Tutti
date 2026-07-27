#ifndef TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_
#define TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_

// Device-side NVMe IO helpers -- private to the NVMe backend.
//
// Include order note:
//   <ctrl.h> must come before <queue.h>.  ctrl.h transitively #includes
//   queue.h (which defines QueuePair); if queue.h is included first its
//   include guard fires when ctrl.h tries to pull it in, leaving QueuePair
//   undefined at the point ctrl.h uses it at line ~308.  Including ctrl.h
//   first lets the ctrl.h -> queue.h -> QueuePair chain resolve before
//   ctrl.h needs QueuePair.
#include <ctrl.h>                     // libnvm/src: outer include → pulls queue.h
#include <queue.h>                    // libnvm/src: QueuePair { .sq, .cq, .nvmNamespace }
#include <nvm_parallel_queue.h>       // get_cid, sq_enqueue, cq_poll, cq_dequeue, put_cid
#include <nvm_cmd.h>                  // nvm_cmd_header/data_ptr/rw_blks; NVM_IO_READ/WRITE

#include "nvme_target_handle.h"       // NvmeFileDeviceHandle, LbaExtent
#include "nvme_io_types.h"            // BufferDescriptor, SubSliceInfo
#include "backends/include/storage_target.h" // LbaExtent

#include <cstdint>

namespace tutti {
namespace backends {
namespace nvme {
namespace device {

// ── LBA resolution ──────────────────────────────────────────────────────────

// Resolve logical file offset (bytes) to NVMe LBA + remaining block count.
//
// Walks inline extents[], falls back to extents_overflow for large files.
// Returns true on success; false if offset is out of range.
__device__ inline bool resolve_lba(
    NvmeFileDeviceHandle* handle,
    uint64_t              logical_offset,
    uint64_t&             out_lba,
    uint32_t&             out_block_count)
{
    if (handle == nullptr || handle->extents == nullptr) {
        return false;
    }

    uint64_t logical_block = logical_offset >> handle->nvme_block_size_log;

    uint64_t cur_logical = 0;
    for (uint32_t i = 0; i < handle->num_extents; ++i) {
        const LbaExtent* ext =
            (i < NvmeFileDeviceHandle::MAX_INLINE_EXTENTS)
                ? &handle->extents[i]
                : (handle->extents_overflow
                       ? &handle->extents_overflow[
                             i - NvmeFileDeviceHandle::MAX_INLINE_EXTENTS]
                       : nullptr);

        if (ext == nullptr) return false;

        uint64_t ext_end = cur_logical + ext->length_blocks;
        if (logical_block >= cur_logical && logical_block < ext_end) {
            uint64_t off = logical_block - cur_logical;
            out_lba         = ext->start_lba + off;
            out_block_count = static_cast<uint32_t>(ext->length_blocks - off);
            return true;
        }
        cur_logical = ext_end;
    }
    return false;  // out of bounds
}

// ── Internal: one NVMe command, blocking until CQE arrives ───────────────────

// Hash-based queue selection: spreads global threads across queue_quota rings.
__device__ inline uint32_t select_queue(uint32_t queue_quota) {
    uint32_t gtid = blockIdx.x * blockDim.x + threadIdx.x;
    return gtid % queue_quota;
}

// Build and submit one NVMe READ or WRITE command; spin-poll for completion.
// Returns CID (>= 0) on success; negative error code on failure.
__device__ inline int submit_one_impl(
    NvmeFileDeviceHandle* handle,
    uint64_t              logical_offset,
    uint64_t              prp1,
    uint64_t              prp2,
    uint32_t              nbytes,
    uint8_t               opcode)
{
    // 1. Resolve LBA
    uint64_t lba;
    uint32_t block_count;
    if (!resolve_lba(handle, logical_offset, lba, block_count)) return -1;

    uint32_t transfer_blocks =
        (nbytes + handle->nvme_block_size - 1) >> handle->nvme_block_size_log;
    if (transfer_blocks > block_count) return -2;  // crosses extent boundary

    // 2. Select queue pair.
    //    d_qps is typed nvm_queue_t* but points to QueuePair[] in managed memory.
    QueuePair* qps = reinterpret_cast<QueuePair*>(handle->d_qps);
    uint32_t   qi  = select_queue(handle->queue_quota);
    QueuePair* qp  = &qps[qi];

    // 3. Acquire CID + build NVMe SQE
    nvm_cmd_t cmd;
    uint16_t  cid = get_cid(&qp->sq);
    nvm_cmd_header(&cmd, cid, opcode, qp->nvmNamespace);
    nvm_cmd_data_ptr(&cmd, prp1, prp2);
    nvm_cmd_rw_blks(&cmd, lba, static_cast<uint16_t>(transfer_blocks));

    // 4. Enqueue SQE (rings SQ doorbell internally via ticket protocol)
    sq_enqueue(&qp->sq, &cmd);

    // 5. Spin-poll CQ; retire CQE and release CID
    uint32_t cq_pos = cq_poll(&qp->cq, cid);
    cq_dequeue(&qp->cq, static_cast<uint16_t>(cq_pos), &qp->sq);
    put_cid(&qp->sq, cid);

    return static_cast<int>(cid);
}

// ── Public entry points ──────────────────────────────────────────────────────

// Submit one NVMe READ command; spin until the CQE arrives.
// handle:         GPU-resident file handle (from NvmeBackend::acquire_target_handle)
// logical_offset: byte offset within the file / raw range
// prp1, prp2:     PRP entries from NvmeBackend::prepare_descriptors
// nbytes:         transfer length in bytes
// Returns CID >= 0 on success; negative on failure.
__device__ inline int submit_read_one(
    NvmeFileDeviceHandle* handle,
    uint64_t              logical_offset,
    uint64_t              prp1,
    uint64_t              prp2,
    uint32_t              nbytes)
{
    return submit_one_impl(handle, logical_offset, prp1, prp2,
                           nbytes, NVM_IO_READ);
}

// Submit one NVMe WRITE command; spin until the CQE arrives.
__device__ inline int submit_write_one(
    NvmeFileDeviceHandle* handle,
    uint64_t              logical_offset,
    uint64_t              prp1,
    uint64_t              prp2,
    uint32_t              nbytes)
{
    return submit_one_impl(handle, logical_offset, prp1, prp2,
                           nbytes, NVM_IO_WRITE);
}

} // namespace device
} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_
