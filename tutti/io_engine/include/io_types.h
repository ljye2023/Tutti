#pragma once
#include <cstdint>
#include "backends/include/storage_target.h"
#include "backends/include/backend_types.h"

namespace tutti {

struct MemoryRegion;  // forward-decl

// Backend-neutral IO request (replaces NvmeBatchInputTensor).
struct IoRequest {
    MemoryRegion* region;         // Registered source/destination buffer
    void*         target_handle;  // Opaque, produced by NvmeBackend::acquire_target_handle (via IBatchSubmitter)
    uint64_t      byte_offset;    // Offset into the logical target (file offset or LBA offset)
    uint64_t      byte_length;    // Bytes to transfer
};

// Describes one transport-sized sub-IO within a fanned-out batch.
// IO Engine produces these when splitting a large IoRequest.
struct SubSliceInfo {
    uint64_t region_byte_offset;  // Offset within MemoryRegion
    uint64_t byte_length;         // Sub-IO size (≤ backend's max_io_size)
    uint32_t ioaddr_index;        // Index into MemoryRegion::dma_ioaddrs[]
};


// Single-shard IO request used by IIoEngine::submit_one.
// Carries all routing information needed to acquire a target handle and issue
// the IO without a pre-built target_handle field.
struct SingleShardIoRequest {
    MemoryRegion*           region;          // registered buffer (DMA-mapped)
    uint64_t                logical_offset;  // byte offset within this shard
    uint64_t                length;          // bytes to transfer
    backends::StorageTarget shard_target;    // physical descriptor for this shard
    backends::VDeviceHandle vdev;            // which vdevice to route to
};

} // namespace tutti
