// tutti/device_manager/include/queue_acquire_helper.cuh
// Layer 2: Device Manager - Device-side Queue Helpers
//
// Device-side helpers for acquiring and managing queue resources

#pragma once

#include "tutti/abstraction/accel.h"
#include "tutti/device_manager/include/vdevice.h"

namespace tutti {

// Device-side queue acquisition context
struct QueueAcquireContext {
    VDevice* vdev;
    uint32_t thread_id;
    uint32_t num_threads;

    TUTTI_DEVICE uint32_t get_queue_index() const;
    TUTTI_DEVICE void* get_submission_queue() const;
    TUTTI_DEVICE void* get_completion_queue() const;
};

// Device-side queue acquisition helper
TUTTI_DEVICE inline uint32_t acquire_queue_for_thread(
    const VDevice& vdev,
    uint32_t thread_id,
    uint32_t num_threads)
{
    // Round-robin queue assignment
    return vdev.queue_slice_start + (thread_id % vdev.queue_slice_len);
}

} // namespace tutti
