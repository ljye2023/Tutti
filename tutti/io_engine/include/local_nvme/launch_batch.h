// tutti/io_engine/include/local_nvme/launch_batch.h
// Layer 4: IO Engine - Batch Launch Helpers
//
// Host-side helpers for launching batched I/O operations

#pragma once

#include "accel/include/common/accel_types.h"
#include "io_engine/include/io_types.h"
#include <cstddef>

namespace tutti {
namespace local_nvme {

// Batch launch configuration
struct BatchLaunchConfig {
    uint32_t num_threads;
    uint32_t threads_per_block;
    AccelStream stream;
};

// Launch a batch of I/O requests on the GPU
int launch_io_batch(
    const IoRequest* requests,
    size_t num_requests,
    const BatchLaunchConfig& config);

} // namespace local_nvme
} // namespace tutti
