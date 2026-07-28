#ifndef TUTTI_BACKENDS_NVME_BATCH_SUBMITTER_H_
#define TUTTI_BACKENDS_NVME_BATCH_SUBMITTER_H_

#include "backends/include/backend_types.h"   // BackendMetadata, VDeviceHandle
#include "backends/include/storage_target.h"  // StorageTarget
#include "nvme_io_types.h"                    // SubSliceInfo, BufferDescriptor

#include <cstdint>

namespace tutti {
namespace backends {
namespace nvme {

// Narrow submission SPI consumed by the Layer 4 IO Engine.
//
// Why this exists: the IO Engine only needs four operations from a backend to
// orchestrate a batch -- read its capabilities, turn DMA addresses into PRP
// descriptors, hand those descriptors to a GPU kernel, and return the PRP-list
// pages afterwards. Those four are NVMe-specific (they traffic in nvme::
// SubSliceInfo / BufferDescriptor), so they deliberately do NOT belong on the
// device-agnostic IBackend contract.
//
// Extracting them into a virtual interface lets the engine hold an
// IBatchSubmitter* instead of a concrete NvmeBackend*, which keeps the engine
// unit-testable with a lightweight mock (no CUDA, no libnvm, no real vdevices).
// NvmeBackend implements this alongside IBackend:
//
//     class NvmeBackend : public IBackend, public IBatchSubmitter { ... };
//
// This is honest about the coupling: the interface is scoped under nvme:: and
// speaks NVMe descriptor types. It is NOT a device-agnostic abstraction. When a
// second transport needs the engine, a genuinely neutral descriptor type would
// be required first -- out of scope for the single-backend phase.
class IBatchSubmitter {
public:
    virtual ~IBatchSubmitter() = default;

    // Capability record (max_io_size, max_batch_size, alignment, ...).
    virtual BackendMetadata metadata() const = 0;

    // Build PRP descriptors from raw DMA addresses (one per sub-slice).
    // Returns true on success; false if MDTS exceeded or PRP cache exhausted.
    virtual bool prepare_descriptors(
        const uint64_t*     ioaddrs,
        const SubSliceInfo* slices,
        uint32_t            n_slices,
        BufferDescriptor*   out_descs) = 0;

    // Return the PRP-list pages used by the descriptors back to the cache.
    virtual void release_descriptors(BufferDescriptor* descs, uint32_t n_descs) = 0;

    // Resolve a StorageTarget + VDeviceHandle into an opaque GPU-resident handle
    // (e.g. NvmeFileDeviceHandle).  Returns nullptr on failure.
    // Implementations are expected to cache the result keyed by target + vdev.
    virtual void* acquire_target_handle(
        const backends::StorageTarget& target,
        backends::VDeviceHandle        hdl) = 0;

    // Launch the GPU batch submission kernel on the given CUDA stream.
    virtual void launch_batch_gpu_stream(
        void*                   stream,
        void*                   target_handle,
        const BufferDescriptor* descs,
        uint32_t                n_descs,
        bool                    is_read) = 0;
};

} // namespace nvme
} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_NVME_BATCH_SUBMITTER_H_
