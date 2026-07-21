#ifndef TUTTI_BACKENDS_NVME_COMMAND_BUILDER_H_
#define TUTTI_BACKENDS_NVME_COMMAND_BUILDER_H_

#include "backends/include/backend_types.h"
#include <cstdint>
#include <cstddef>

namespace tutti {
namespace backends {
namespace nvme {

class PrpPageCache;

// Host-side PRP/SGL descriptor construction from raw ioaddrs.
//
// Converts DMA bus addresses (ioaddrs from Accel HAL) into NVMe-specific
// PRP or SGL descriptors suitable for submission.
//
// PRP construction logic:
//   SINGLE: data_length <= page_size
//           prp1 = ioaddrs[0], prp2 = 0
//
//   DUAL:   page_size < data_length <= 2*page_size
//           prp1 = ioaddrs[0], prp2 = ioaddrs[1]
//
//   LIST:   data_length > 2*page_size
//           prp1 = ioaddrs[0]
//           prp2 = PRP-list page from cache
//           populate list page with ioaddrs[1..n]
//
// Thread safety: stateless descriptor construction (thread-safe).
// PRP cache access is internally synchronized.
class NvmeCommandBuilder {
public:
    // page_size: typically 4096 bytes (NVMe standard)
    // mdts: max data transfer size from VDevice (controller MDTS)
    // prp_cache: reference to backend's PRP-list page cache
    NvmeCommandBuilder(size_t page_size, size_t mdts, PrpPageCache* prp_cache);

    // Build PRP descriptors from ioaddrs and sub-slice layout.
    //
    // For each sub-slice:
    //   1. Determine PRP kind (SINGLE/DUAL/LIST) based on data_length
    //   2. Set prp1 = first page ioaddr
    //   3. Set prp2 = second page ioaddr (DUAL) or PRP-list page (LIST)
    //   4. For LIST: populate PRP-list page with remaining ioaddrs
    //
    // ioaddrs: Array of DMA bus addresses (one per page/segment)
    // slices: Array of sub-slice layout info (offset, length, index)
    // n_slices: Number of sub-slices to process
    // out_descs: Output array of BufferDescriptors (caller-allocated, size >= n_slices)
    //
    // Returns true on success, false on failure (e.g., out of PRP pages, MDTS exceeded).
    bool build_prp_descriptors(
        const uint64_t* ioaddrs,
        const SubSliceInfo* slices,
        uint32_t n_slices,
        BufferDescriptor* out_descs);

    // Build SGL descriptors (NVMe 1.2+ scatter-gather lists).
    // Future enhancement - currently returns false (unsupported).
    bool build_sgl_descriptors(
        const uint64_t* ioaddrs,
        const SubSliceInfo* slices,
        uint32_t n_slices,
        BufferDescriptor* out_descs);

    // Release descriptors and return PRP-list pages to cache.
    //
    // Must be called when descriptors are no longer needed (tensor deregistration,
    // error cleanup). Returns PRP-list pages to cache for reuse.
    //
    // descs: Array of descriptors to release (from prior build_prp_descriptors call)
    // n_descs: Number of descriptors in array
    void release_descriptors(BufferDescriptor* descs, uint32_t n_descs);

private:
    const size_t page_size_;
    const size_t mdts_;
    PrpPageCache* prp_cache_;

    // PRP descriptor kind flags (stored in BufferDescriptor::descriptor_flags)
    enum PrpKind : uint32_t {
        PRP_SINGLE = 0,  // Single page, prp2 = 0
        PRP_DUAL = 1,    // Two pages, prp2 = second page
        PRP_LIST = 2     // Multiple pages, prp2 = PRP-list page
    };

    // Build PRP-list page for LIST transfers
    // Returns device pointer to PRP-list page, or nullptr on failure
    void* build_prp_list_page(const uint64_t* ioaddrs, uint32_t num_pages);

    // Calculate number of pages needed for transfer
    uint32_t calculate_num_pages(uint64_t start_addr, uint32_t length) const;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_COMMAND_BUILDER_H_
