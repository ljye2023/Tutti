// tutti/types/io_types.h -- IO request and slice descriptors
//
// Backend-neutral types used by IO Engine for IO orchestration.
// Replaces NVMe-specific types like NvmeBatchInputTensor.

#pragma once

#include <cstdint>
#include <cstddef>
#include "tutti/accel/include/accel_types.h"
#include "tutti/types/storage_target.h"

namespace tutti {

// Forward declarations
struct MemoryRegion;

// ---------------------------------------------------------------------------
// Sub-slice descriptor
// ---------------------------------------------------------------------------

// Describes a single IO slice after fan-out.
// The IO Engine splits large requests into max_io_size chunks,
// producing one SubSliceInfo per chunk.
struct SubSliceInfo {
    size_t region_byte_offset;    // Offset within the MemoryRegion
    size_t byte_length;           // Length of this slice
    size_t ioaddr_index;          // Index into MemoryRegion::dma_ioaddrs array
    uint64_t storage_offset;      // Offset within the storage target (bytes or LBA)
};

// ---------------------------------------------------------------------------
// IO request
// ---------------------------------------------------------------------------

// A single IO request from Block Storage or Raw Device to IO Engine.
// Replaces NvmeBatchInputTensor (which was NVMe-specific).
struct IoRequest {
    MemoryRegion* region;         // Memory region (contains DMA ioaddrs)
    void* target_handle;          // Backend-private target handle (opaque)
    size_t byte_offset;           // Starting offset within region
    size_t byte_length;           // Total length of this request
    uint64_t storage_offset;      // Starting offset in storage (file offset or LBA)
    bool is_read;                 // true = read, false = write
};

// ---------------------------------------------------------------------------
// Buffer descriptor
// ---------------------------------------------------------------------------

// Backend-specific descriptor passed to device kernels.
// For NVMe: contains PRP/SGL entries.
// For RDMA: contains RDMA work request fields.
//
// This is an opaque type at the IO Engine level; only backends know its layout.
struct BufferDescriptor {
    uint64_t prp1;                // Primary buffer pointer (or first SGL entry)
    uint64_t prp2;                // Secondary buffer pointer (or SGL list pointer)
    uint32_t byte_length;         // Length of this descriptor
    uint32_t reserved;            // Alignment padding
};

static_assert(sizeof(BufferDescriptor) == 24, "BufferDescriptor layout mismatch");

}  // namespace tutti
