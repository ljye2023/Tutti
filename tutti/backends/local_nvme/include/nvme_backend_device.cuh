// tutti/backends/local_nvme/include/nvme_backend_device.cuh
// Layer 3: Local NVMe Backend - Device-side helpers
//
// Device-side structures and helpers for NVMe backend operations

#pragma once

#include "tutti/abstraction/accel.h"
#include "tutti/types/io_types.h"

namespace tutti {
namespace local_nvme {

// PRP (Physical Region Page) entry for NVMe commands
struct PrpEntry {
    uint64_t addr;
};

// Device-side PRP list builder
struct PrpBuilder {
    PrpEntry* prp_list;
    size_t capacity;
    size_t count;

    TUTTI_DEVICE void add_entry(uint64_t addr);
    TUTTI_DEVICE bool is_full() const;
};

// Device-side batch submission context
struct BatchSubmitContext {
    void* submission_queue;
    void* completion_queue;
    uint32_t sq_tail;
    uint32_t sq_head;

    TUTTI_DEVICE int submit_command(const void* nvme_cmd);
    TUTTI_DEVICE int poll_completion(uint32_t cid);
};

} // namespace local_nvme
} // namespace tutti
