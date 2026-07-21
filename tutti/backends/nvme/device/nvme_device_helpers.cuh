#include "backends/include/storage_target.h"
#ifndef TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_
#define TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_

#include "nvme_target_handle.h"
#include "backends/include/backend_types.h"
#include "device_manager/include/common/vdevice.h"
// TODO: Add proper storage types
#include <cstdint>
#include <cstdint>

// Device-side addressing helpers - private to backend, not exposed through SPI.
//
// These functions consume Device Manager's device-side queue mechanics:
//   - acquire_queue(vdev->d_qps, vdev->queue_quota, preferred_index)
//   - issue_nvme_cmd(qp, sqe)
//   - poll(qp, cid)
//
// Key design: Backend does NOT own queue mechanics - it consumes DM's device API.

namespace tutti {
namespace backends {
namespace nvme {
namespace device {

// Resolve logical file offset to NVMe LBA and block count.
//
// Walks inline extents[], falls back to extents_overflow if needed.
// Translates logical offset to (LBA, block_count) pair for NVMe command.
//
// handle: GPU-resident file handle
// logical_offset: Byte offset within file
// out_lba: Output LBA (returned)
// out_block_count: Output block count (returned)
//
// Returns true on success, false if offset is out of bounds or invalid.
__device__ inline bool resolve_lba(
    NvmeFileDeviceHandle* handle,
    uint64_t logical_offset,
    uint64_t& out_lba,
    uint32_t& out_block_count)
{
    if (handle == nullptr || handle->extents == nullptr) {
        return false;
    }

    // Convert byte offset to block offset
    uint64_t logical_block = logical_offset >> handle->nvme_block_size_log;

    // Walk extents to find containing extent
    uint64_t current_logical_block = 0;

    for (uint32_t i = 0; i < handle->num_extents; ++i) {
        const LbaExtent* extent;

        // Load extent from inline or overflow array
        if (i < NvmeFileDeviceHandle::MAX_INLINE_EXTENTS) {
            extent = &handle->extents[i];
        } else {
            if (handle->extents_overflow == nullptr) {
                return false;
            }
            extent = &handle->extents_overflow[i - NvmeFileDeviceHandle::MAX_INLINE_EXTENTS];
        }

        // Check if logical_block falls within this extent
        uint64_t extent_end = current_logical_block + extent->length_blocks;
        if (logical_block >= current_logical_block && logical_block < extent_end) {
            // Found containing extent
            uint64_t offset_within_extent = logical_block - current_logical_block;
            out_lba = extent->start_lba + offset_within_extent;

            // Calculate remaining blocks in extent
            uint64_t remaining_blocks = extent->length_blocks - offset_within_extent;
            out_block_count = static_cast<uint32_t>(remaining_blocks);

            return true;
        }

        current_logical_block = extent_end;
    }

    // Offset out of bounds
    return false;
}

// Submit a single NVMe READ command.
//
// Resolves LBA, acquires queue from DM, builds READ SQE, submits via DM's issue_nvme_cmd().
//
// handle: GPU-resident file handle
// logical_offset: Byte offset within file
// prp1: First PRP entry (DMA address)
// prp2: Second PRP entry or PRP-list pointer
// nbytes: Transfer length in bytes
//
// Returns command ID (CID) on success, negative value on failure.
__device__ inline int submit_read_one(
    NvmeFileDeviceHandle* handle,
    uint64_t logical_offset,
    uint64_t prp1,
    uint64_t prp2,
    uint32_t nbytes)
{
    // Resolve LBA from logical offset
    uint64_t lba;
    uint32_t block_count;
    if (!resolve_lba(handle, logical_offset, lba, block_count)) {
        return -1;
    }

    // Calculate number of blocks for transfer
    uint32_t transfer_blocks = (nbytes + handle->nvme_block_size - 1) >> handle->nvme_block_size_log;
    if (transfer_blocks > block_count) {
        // Transfer spans multiple extents - not supported in single command
        return -2;
    }

    // TODO: Integrate with Device Manager's device-side queue mechanics
    // This requires:
    //   1. acquire_queue(handle->vdev->d_qps, handle->vdev->queue_quota, preferred_index)
    //   2. Build NVMe READ SQE with: namespace_id, lba, transfer_blocks, prp1, prp2
    //   3. issue_nvme_cmd(qp, &sqe)
    //   4. Return CID for polling

    // Placeholder: return success
    return 0;
}

// Submit a single NVMe WRITE command.
//
// Same as submit_read_one but WRITE opcode.
__device__ inline int submit_write_one(
    NvmeFileDeviceHandle* handle,
    uint64_t logical_offset,
    uint64_t prp1,
    uint64_t prp2,
    uint32_t nbytes)
{
    // Resolve LBA from logical offset
    uint64_t lba;
    uint32_t block_count;
    if (!resolve_lba(handle, logical_offset, lba, block_count)) {
        return -1;
    }

    // Calculate number of blocks for transfer
    uint32_t transfer_blocks = (nbytes + handle->nvme_block_size - 1) >> handle->nvme_block_size_log;
    if (transfer_blocks > block_count) {
        // Transfer spans multiple extents - not supported in single command
        return -2;
    }

    // TODO: Integrate with Device Manager's device-side queue mechanics
    // Same as submit_read_one but with WRITE opcode

    // Placeholder: return success
    return 0;
}

} // namespace device
} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_DEVICE_HELPERS_CUH_
