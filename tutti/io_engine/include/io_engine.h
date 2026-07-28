// tutti/io_engine/include/io_engine.h
// Backend-neutral IO Engine interface
//
// Replaces NVMe-specific NvmeBatchInputTensor with generic IoRequest

#pragma once

#include "io_engine/include/io_types.h"
#include "accel/include/common/accel_types.h"
#include <vector>
#include <cstdint>

namespace tutti {

struct MemoryRegion;  // accel/include/common/memory_region.h
class IAccelerator;

class IIoEngine {
public:
    virtual ~IIoEngine() = default;

    //==========================================================================
    // Batch Submit (Blocking)
    //
    // Submit one uniform-direction batch and block until complete.
    // Internally:
    //   1. Fan out: IoRequest → SubSliceInfo[] (using max_io_size from backend)
    //   2. Backend: prepare_descriptors(ioaddrs, slices) → BufferDescriptor[]
    //   3. HAL: memcpy_async(descs CPU→GPU, stream)
    //   4. Backend: launch_batch_gpu_stream(stream, target_handle, descs, n, is_read)
    //   5. HAL: synchronize_stream(stream)
    //==========================================================================

    virtual bool submit_batch(
        const std::vector<IoRequest>& requests,  // Backend-neutral
        bool is_read,
        AccelStream stream) = 0;

    //==========================================================================
    // Batch Submit (Async)
    //
    // Returns after kernel launch is queued on stream.
    // Caller responsible for stream-sync or event observation.
    // Kernel-side failures surface on caller's eventual stream-sync.
    //==========================================================================

    virtual bool submit_batch_async(
        const std::vector<IoRequest>& requests,
        bool is_read,
        AccelStream stream) = 0;

    //==========================================================================
    // Single-Shard Submit (Blocking)
    //
    // Submit one shard IO end-to-end: acquire handle → fan-out → descriptors
    // → launch → complete.  Suitable for low-fan-out or scatter callers that
    // already know which shard to target.
    //==========================================================================

    virtual bool submit_one(
        const SingleShardIoRequest& req,
        bool                        is_read,
        AccelStream                 stream) = 0;

    //==========================================================================
    // Capacity / Planning
    //==========================================================================

    // Maximum entries one batch may flatten to after fan-out.
    // Callers must pack under this limit.
    virtual uint32_t max_entries_per_batch() const = 0;

    // How many sub-IOs a given MemoryRegion flattens to after fan-out.
    // Adapters use this to pack batches without exceeding max_entries_per_batch.
    // NEW METHOD (was buried in IMemorySubsystem::lookup_io_slice).
    virtual uint32_t slice_fanout(const MemoryRegion* region) const = 0;
};

}  // namespace tutti
