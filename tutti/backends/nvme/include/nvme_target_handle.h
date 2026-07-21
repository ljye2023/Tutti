#ifndef TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_
#define TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_

#include <cstdint>
#include "device_manager/include/common/vdevice.h"
#include "backends/include/storage_target.h"

namespace tutti {
namespace backends {
namespace nvme {

// GPU-resident file handle consumed by device kernels.
//
// Design: inline-small (8 extents) + overflow pattern. 99% of files fit in inline storage.
// Overflow extents allocated separately on GPU for large fragmented files.
//
// Lifetime: allocated by acquire_target_handle via cudaMalloc, held for file lifetime,
// freed by release_target_handle via cudaFree.
//
// Memory layout: POD struct, ~200 bytes typical (8 inline extents). Scales to 124 extents
// max via overflow pointer.
struct NvmeFileDeviceHandle {
    // File identity and size
    uint64_t file_id;                // Source file identifier for debugging
    uint64_t logical_size_bytes;     // User-visible file size

    // NVMe namespace parameters
    uint32_t nvme_block_size;        // Namespace block size (typically 4096)
    uint32_t nvme_block_size_log;    // log2(block_size) for fast LBA conversion
    uint32_t namespace_id;           // NVMe namespace id for SQE construction

    // Extent mapping
    uint32_t num_extents;            // Total extent count
    LbaExtent* extents;              // Inline extents (8 elements)
    LbaExtent* extents_overflow;     // GPU pointer to overflow extents (nullptr if num_extents <= 8)

    // Device Manager reference
    VDevice* vdev;                   // Reference to DM-provided vDevice (contains d_qps, queue_quota)

    // Inline extent storage (8 extents covers 99% of files)
    static constexpr uint32_t MAX_INLINE_EXTENTS = 8;
};

// Raw LBA range handle for direct block device access (future)
struct NvmeRawDeviceHandle {
    uint64_t start_lba;              // Starting LBA of range
    uint64_t length_blocks;          // Length of range in blocks
    uint32_t namespace_id;           // NVMe namespace id
    uint32_t nvme_block_size;        // Namespace block size
    uint32_t nvme_block_size_log;    // log2(block_size)
    VDevice* vdev;                   // Reference to DM-provided vDevice
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_
