// tutti/io_engine/include/io_engine.h
// Backend-neutral IO Engine interface
//
// Replaces NVMe-specific NvmeBatchInputTensor with generic IoRequest

#pragma once

#include "tutti/types/io_types.h"
#include "tutti/accel/include/accel_types.h"
#include <vector>

namespace tutti {

class IIoEngine {
public:
    virtual ~IIoEngine() = default;

    // Submit a batch of IO requests
    // Returns true on success
    virtual bool submit_batch(const std::vector<IoRequest>& requests,
                              bool is_read,
                              AccelStream stream) = 0;

    // Get maximum IO size supported by backend
    virtual size_t max_io_size() const = 0;

    // Slice a large IO into sub-slices (fan-out logic moved from memory/)
    virtual size_t slice_fanout(const IoRequest& request,
                                SubSliceInfo* slices_out,
                                size_t max_slices) const = 0;
};

}  // namespace tutti
