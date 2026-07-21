#ifndef TUTTI_BACKENDS_STORAGE_TARGET_H_
#define TUTTI_BACKENDS_STORAGE_TARGET_H_

#include <cstdint>
#include <cstddef>

namespace tutti {
namespace backends {

// LBA extent - physical block range on NVMe device
// Maps logical file offset to physical LBA on NVMe namespace
struct LbaExtent {
    uint64_t start_lba;       // Starting LBA on NVMe namespace
    uint64_t length_blocks;   // Length in blocks
    uint64_t logical_offset;  // Logical offset in file (in blocks)
};

// Storage target kind - identifies how to interpret target data
enum class StorageTargetKind : uint32_t {
    NVME_FILE = 0,      // File on NVMe with LBA extents
    NVME_RAW = 1,       // Raw LBA range on NVMe
    RDMA_REMOTE = 2,    // Remote RDMA target
    LOCAL_FILE = 3      // Local filesystem file (fallback)
};

// Storage target descriptor - emitted by namespace producers, consumed by backends.
//
// Contains all information needed to build a backend-specific target handle:
//   - Target kind (file, raw, remote)
//   - File/namespace metadata (size, block size, namespace id)
//   - Extent mapping (LBA extents for files, LBA range for raw)
//
// Backends convert this into GPU-resident handles (e.g., NvmeFileDeviceHandle).
struct StorageTarget {
    StorageTargetKind kind;

    // File/namespace identity
    uint64_t target_id;              // Unique identifier (file id, inode, etc.)
    uint64_t logical_size_bytes;     // User-visible size

    // NVMe-specific fields (valid for NVME_FILE, NVME_RAW)
    uint32_t namespace_id;           // NVMe namespace id
    uint32_t nvme_block_size;        // Namespace block size (typically 4096)
    uint32_t nvme_block_size_log;    // log2(block_size) for fast conversion

    // Extent mapping (NVME_FILE)
    uint32_t num_extents;            // Number of extents
    const LbaExtent* extents;        // Pointer to extent array (host memory)

    // Raw LBA range (NVME_RAW)
    uint64_t start_lba;              // Starting LBA
    uint64_t length_blocks;          // Length in blocks

    // RDMA-specific fields (valid for RDMA_REMOTE)
    uint64_t remote_addr;            // Remote virtual address
    uint32_t rkey;                   // Remote key

    // Default constructor
    StorageTarget()
        : kind(StorageTargetKind::NVME_FILE)
        , target_id(0)
        , logical_size_bytes(0)
        , namespace_id(0)
        , nvme_block_size(4096)
        , nvme_block_size_log(12)
        , num_extents(0)
        , extents(nullptr)
        , start_lba(0)
        , length_blocks(0)
        , remote_addr(0)
        , rkey(0)
    {}
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_STORAGE_TARGET_H_
