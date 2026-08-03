#pragma once

// tutti/data_paths/local_nvme/io/device_target.h
//
// GPU-resident NVMe target handle — ported from main's NvmeFileDeviceHandle.
//
// Lives entirely in GPU memory. Built by LocalNvmeDataPath::open(),
// freed by LocalNvmeDataPath::close().  Never mutated after construction.
//
// Extent storage: 8 inline extents + overflow buffer for >8.

#include <cstdint>

// Forward declaration of libnvm's QueuePair.
struct QueuePair;

namespace tutti::data_paths::local_nvme {

inline constexpr uint32_t kDeviceTargetInlineExtents = 8;

// Block-unit extent — matches source LbaExtent layout exactly.
struct DeviceLbaExtent {
    uint64_t start_lba;
    uint64_t length_blocks;
};

// GPU-resident target handle. POD; built host-side then cudaMemcpy'd.
struct DeviceTargetHandle {
    uint64_t   file_id;               // target token (for debugging)
    uint64_t   logical_size_bytes;
    uint32_t   header_bytes;          // always 0 (no in-band header)
    uint32_t   nvme_block_size;
    uint32_t   nvme_block_size_log;
    uint32_t   namespace_id;
    uint32_t   num_extents;
    DeviceLbaExtent extents[kDeviceTargetInlineExtents];
    DeviceLbaExtent* extents_overflow; // nullptr when num_extents <= inline cap
    QueuePair* d_qps;                 // borrowed from NvmeQueueGroup
    uint32_t   num_d_qps;
    uint32_t   reserved0;
};

// Build a device target handle from host-side state and cudaMalloc it.
// Returns the GPU pointer via out_handle.  Also returns the overflow
// GPU pointer (or nullptr) via out_overflow for later cleanup.
//
// On failure returns false; any partial CUDA allocations are cleaned up.
bool build_device_target(
    const DeviceTargetHandle& host_template,
    const DeviceLbaExtent* overflow_extents,  // nullptr if none
    uint32_t n_overflow,
    uint32_t cuda_device,
    DeviceTargetHandle** out_handle,
    void** out_overflow_dev);

// Free a device target handle and its overflow buffer.
void free_device_target(
    DeviceTargetHandle* handle,
    void* overflow_dev,
    uint32_t cuda_device);

} // namespace tutti::data_paths::local_nvme
