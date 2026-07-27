#ifndef TUTTI_BACKENDS_NVME_IO_TYPES_H_
#define TUTTI_BACKENDS_NVME_IO_TYPES_H_

#include <cstdint>
#include <cstddef>

namespace tutti {
namespace backends {
namespace nvme {

// Sub-slice layout info - describes one contiguous region within a larger IO.
//
// When a tensor's physical layout is fragmented or when batching multiple small
// IOs, each sub-slice maps to one NVMe command. The command builder uses this to
// construct PRP/SGL descriptors from the flat ioaddrs array.
struct SubSliceInfo {
    uint64_t offset_bytes;   // Logical offset within the storage target
    uint32_t length_bytes;   // Length of this sub-slice in bytes
    uint32_t slice_index;    // Starting index into ioaddrs array for this sub-slice
};

// NVMe buffer descriptor - one per NVMe command.
//
// Built by NvmeCommandBuilder from ioaddrs + SubSliceInfo. Contains PRP entries
// and metadata needed for device-side submission. The descriptor_flags field
// encodes the PRP kind (SINGLE/DUAL/LIST) for cleanup logic.
struct BufferDescriptor {
    uint64_t storage_offset;      // Logical offset in storage (bytes)
    uint32_t data_length;         // Transfer length (bytes)

    // PRP entries (NVMe command DPTR field)
    uint64_t prp1;                // First PRP entry (first page DMA address)
    uint64_t prp2;                // Second PRP entry or PRP-list pointer

    uint32_t descriptor_flags;    // PRP kind: 0=SINGLE, 1=DUAL, 2=LIST
    void*    backend_private;     // Opaque backend data (e.g., PRP-list page for cleanup)
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_IO_TYPES_H_
