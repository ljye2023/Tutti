// tutti/device_manager/include/nvme_queue_group.h
// Layer 2: Device Manager - NVMe Queue Group
//
// Represents a group of NVMe submission/completion queues

#pragma once

#include <cstdint>
#include <memory>

namespace tutti {

// NVMe queue pair (SQ + CQ)
struct NvmeQueuePair {
    void* submission_queue;
    void* completion_queue;
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t queue_depth;
    uint16_t queue_id;
};

// NVMe queue group (multiple queue pairs)
class NvmeQueueGroup {
public:
    NvmeQueueGroup(uint32_t num_queues, uint32_t queue_depth);
    ~NvmeQueueGroup();

    // Queue access
    NvmeQueuePair* get_queue(uint32_t index);
    uint32_t get_num_queues() const;
    uint32_t get_queue_depth() const;

    // Queue operations
    int allocate_queues(int device_fd);
    void free_queues();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tutti
