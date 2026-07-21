/**
 * queue_acquire_helper_impl.cuh -- Device-side queue helper implementations
 *
 * Implementation of queue acquisition and NVMe command submission helpers.
 * This file is included by queue_acquire_helper.cuh when compiling with nvcc.
 */

#pragma once

#include <ctrl.h>            // libnvm: outer include, transitively brings queue.h
#include <queue.h>           // libnvm: QueuePair definition
#include <nvm_parallel_queue.h>  // libnvm: get_cid / put_cid / sq_enqueue / cq_poll / cq_dequeue
#include <nvm_cmd.h>         // libnvm: nvm_cmd_header / data_ptr / rw_blks
#include <nvm_io.h>          // libnvm: NVM_IO_READ / NVM_IO_WRITE opcodes

namespace tutti {

// Acquire one queue from the d_qps[] array.
// Hash-based selection: (blockDim.x * 32 + threadIdx.x) % n_qps
TUTTI_DEVICE TUTTI_FORCEINLINE
uint32_t acquire_queue(nvm_queue_t* d_qps, uint32_t n_qps) {
    // Legacy hash: spreads warps across queues
    return (blockDim.x * 32u + threadIdx.x) % n_qps;
}

// Submit one NVMe command to the SQ ring and ring the doorbell.
TUTTI_DEVICE TUTTI_FORCEINLINE
void issue_nvme_cmd(
    nvm_queue_t* qp,
    uint64_t prp1,
    uint64_t prp2,
    uint32_t n_blocks,
    uint64_t lba,
    uint8_t opcode,
    uint16_t* out_cid)
{
    nvm_cmd_t cmd;
    *out_cid = get_cid(&qp->sq);
    nvm_cmd_header(&cmd, *out_cid, opcode, qp->nvmNamespace);
    nvm_cmd_data_ptr(&cmd, prp1, prp2);
    nvm_cmd_rw_blks(&cmd, lba, n_blocks);
    sq_enqueue(&qp->sq, &cmd);
}

// Poll the CQ ring for completion of command `cid`.
TUTTI_DEVICE TUTTI_FORCEINLINE
void poll(nvm_queue_t* qp, uint16_t cid) {
    uint32_t cq_pos = cq_poll(&qp->cq, cid);
    cq_dequeue(&qp->cq, cq_pos, &qp->sq);
    put_cid(&qp->sq, cid);
}

} // namespace tutti
