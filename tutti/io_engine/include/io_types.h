#pragma once
#include <cstdint>

namespace tutti {

struct MemoryRegion;  // forward-decl

// Backend-neutral IO request (replaces NvmeBatchInputTensor).
struct IoRequest {
    MemoryRegion* region;         // Registered source/destination buffer
    void*         target_handle;  // Opaque, produced by IBackendProvider::acquire_target_handle
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

} // namespace tutti
