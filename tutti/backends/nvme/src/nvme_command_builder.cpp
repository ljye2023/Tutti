#include "backends/include/storage_target.h"
#include "nvme_command_builder.h"
#include "prp_page_cache.h"
#include <cuda_runtime.h>
#include <cstring>

namespace tutti {
namespace backends {
namespace nvme {

NvmeCommandBuilder::NvmeCommandBuilder(size_t page_size, size_t mdts, PrpPageCache* prp_cache)
    : page_size_(page_size)
    , mdts_(mdts)
    , prp_cache_(prp_cache)
{
}

bool NvmeCommandBuilder::build_prp_descriptors(
    const uint64_t* ioaddrs,
    const SubSliceInfo* slices,
    uint32_t n_slices,
    BufferDescriptor* out_descs)
{
    if (ioaddrs == nullptr || slices == nullptr || out_descs == nullptr) {
        return false;
    }

    for (uint32_t i = 0; i < n_slices; ++i) {
        const SubSliceInfo& slice = slices[i];
        BufferDescriptor& desc = out_descs[i];

        // Validate transfer size against MDTS
        if (slice.length_bytes > mdts_) {
            return false;
        }

        // Calculate number of pages needed
        uint32_t num_pages = calculate_num_pages(
            ioaddrs[slice.slice_index], slice.length_bytes);

        // Initialize descriptor
        desc.storage_offset = slice.offset_bytes;
        desc.data_length = slice.length_bytes;
        desc.backend_private = nullptr;

        if (num_pages == 0) {
            // Empty transfer - should not happen
            desc.prp1 = 0;
            desc.prp2 = 0;
            desc.descriptor_flags = PRP_SINGLE;
        } else if (num_pages == 1) {
            // SINGLE: prp1 = first page, prp2 = 0
            desc.prp1 = ioaddrs[slice.slice_index];
            desc.prp2 = 0;
            desc.descriptor_flags = PRP_SINGLE;
        } else if (num_pages == 2) {
            // DUAL: prp1 = first page, prp2 = second page
            desc.prp1 = ioaddrs[slice.slice_index];
            desc.prp2 = ioaddrs[slice.slice_index + 1];
            desc.descriptor_flags = PRP_DUAL;
        } else {
            // LIST: prp1 = first page, prp2 = PRP-list page
            desc.prp1 = ioaddrs[slice.slice_index];

            // Build PRP-list page with remaining addresses
            void* prp_list = build_prp_list_page(
                &ioaddrs[slice.slice_index + 1], num_pages - 1);

            if (prp_list == nullptr) {
                // Out of PRP pages - clean up already allocated descriptors
                release_descriptors(out_descs, i);
                return false;
            }

            desc.prp2 = reinterpret_cast<uint64_t>(prp_list);
            desc.descriptor_flags = PRP_LIST;
            desc.backend_private = prp_list;  // Track for cleanup
        }
    }

    return true;
}

bool NvmeCommandBuilder::build_sgl_descriptors(
    const uint64_t* ioaddrs,
    const SubSliceInfo* slices,
    uint32_t n_slices,
    BufferDescriptor* out_descs)
{
    // SGL support is future enhancement (NVMe 1.2+)
    return false;
}

void NvmeCommandBuilder::release_descriptors(BufferDescriptor* descs, uint32_t n_descs) {
    if (descs == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < n_descs; ++i) {
        BufferDescriptor& desc = descs[i];

        // Release PRP-list page if present
        if (desc.descriptor_flags == PRP_LIST && desc.backend_private != nullptr) {
            prp_cache_->free_gpu_page(desc.backend_private);
            desc.backend_private = nullptr;
        }

        // Clear descriptor
        desc.prp1 = 0;
        desc.prp2 = 0;
        desc.descriptor_flags = 0;
    }
}

void* NvmeCommandBuilder::build_prp_list_page(const uint64_t* ioaddrs, uint32_t num_pages) {
    if (ioaddrs == nullptr || num_pages == 0) {
        return nullptr;
    }

    // Allocate PRP-list page from cache
    void* prp_list = prp_cache_->allocate_gpu_page();
    if (prp_list == nullptr) {
        return nullptr;
    }

    // Build PRP list on host
    size_t prp_list_size = num_pages * sizeof(uint64_t);
    uint64_t* host_prp_list = new uint64_t[num_pages];

    for (uint32_t i = 0; i < num_pages; ++i) {
        host_prp_list[i] = ioaddrs[i];
    }

    // Copy to GPU
    cudaError_t err = cudaMemcpy(prp_list, host_prp_list, prp_list_size, cudaMemcpyHostToDevice);
    delete[] host_prp_list;

    if (err != cudaSuccess) {
        prp_cache_->free_gpu_page(prp_list);
        return nullptr;
    }

    return prp_list;
}

uint32_t NvmeCommandBuilder::calculate_num_pages(uint64_t start_addr, uint32_t length) const {
    if (length == 0) {
        return 0;
    }

    // Calculate offset within first page
    uint64_t page_offset = start_addr & (page_size_ - 1);

    // Calculate total bytes including offset
    uint64_t total_bytes = page_offset + length;

    // Calculate number of pages (round up)
    uint32_t num_pages = (total_bytes + page_size_ - 1) / page_size_;

    return num_pages;
}

} // namespace nvme
} // namespace backends
} // namespace tutti
