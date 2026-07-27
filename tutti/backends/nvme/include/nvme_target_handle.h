#ifndef TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_
#define TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_

#include <cstdint>
#include "backends/include/storage_target.h"
#include <nvm_types.h>   // nvm_queue_t (needed for d_qps field type)

namespace tutti {
namespace backends {
namespace nvme {

// GPU-resident file handle consumed by device kernels.
//
// Layout: inline-small (8 extents) + overflow pattern. 99 % of files fit in
// the inline storage; the overflow pointer is null in that case.
//
// Lifetime: allocated by NvmeBackend::acquire_target_handle() via cudaMalloc,
// held for the duration of IO, freed by release_target_handle() via cudaFree.
//
// All pointer fields must point to CUDA-accessible memory (managed, device,
// or pinned). In particular, NvmeVirtualDevice (host heap) must NOT be stored
// here — its runtime-relevant fields are copied inline at acquisition time.
struct NvmeFileDeviceHandle {
    // File identity and size
    uint64_t file_id;                // Source file identifier for debugging
    uint64_t logical_size_bytes;     // User-visible file size

    // NVMe namespace parameters
    uint32_t nvme_block_size;        // Namespace block size (typically 4096)
    uint32_t nvme_block_size_log;    // log2(block_size) for fast LBA conversion
    uint32_t namespace_id;           // NVMe namespace id for SQE construction

    // Extent mapping
    uint32_t   num_extents;          // Total extent count
    LbaExtent* extents;              // GPU pointer to inline extents array
    LbaExtent* extents_overflow;     // GPU pointer to overflow extents (nullptr if <= 8)

    // Queue slice — copied from NvmeVirtualDevice at acquisition time so the
    // GPU kernel never needs to dereference a host-heap pointer.
    nvm_queue_t* d_qps;              // GPU-resident QueuePair[] (managed memory)
    uint32_t     queue_quota;        // Number of queue pairs in d_qps

    // Inline extent capacity
    static constexpr uint32_t MAX_INLINE_EXTENTS = 8;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_TARGET_HANDLE_H_
