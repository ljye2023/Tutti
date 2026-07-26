#pragma once

/**
 * queue_acquire_helper.cuh -- Device-side queue mechanics
 *
 * Layer: device_manager (NVMe-specific)
 *
 * Device-side helpers for queue acquisition and NVMe command submission.
 * These operate on nvm_queue_t* (libnvm struct) exposed via NvmeVirtualDevice::d_qps.
 *
 * Concurrency model:
 *   - acquire_queue(): Hash-based queue selection (round-robin)
 *   - issue_nvme_cmd(): Compose SQE + ring doorbell
 *   - poll(): Busy-poll CQ for completion
 */

#include <cstdint>
#include "tutti/abstraction/accel.h"  // TUTTI_DEVICE, TUTTI_FORCEINLINE

struct nvm_queue_t;  // forward-decl (defined in libnvm queue.h)

namespace tutti {

// Acquire one queue from the d_qps[] array.
// Returns the index of the acquired queue (0..n_qps-1).
// Uses round-robin + atomic ticket to avoid contention.
TUTTI_DEVICE TUTTI_FORCEINLINE
uint32_t acquire_queue(nvm_queue_t* d_qps, uint32_t n_qps);

// Submit one NVMe command to the SQ ring and ring the doorbell.
// Blocking: waits for a free SQ slot if full.
// out_cid: receives the command ID for polling.
TUTTI_DEVICE TUTTI_FORCEINLINE
void issue_nvme_cmd(
    nvm_queue_t* qp,
    uint64_t prp1,
    uint64_t prp2,
    uint32_t n_blocks,
    uint64_t lba,
    uint8_t opcode,
    uint16_t* out_cid);

// Poll the CQ ring for completion of command `cid`.
// Blocking: spins until the command completes.
TUTTI_DEVICE TUTTI_FORCEINLINE
void poll(nvm_queue_t* qp, uint16_t cid);

} // namespace tutti

// Implementation included from separate header to keep interface clean
#ifdef __CUDACC__
#include "../src/queue_acquire_helper_impl.cuh"
#endif
