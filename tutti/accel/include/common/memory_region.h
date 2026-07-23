#pragma once
#include <cstdint>
#include <cstddef>
#include "memory_kind.h"

namespace tutti {

// Forward declaration
struct ExternalMemorySpec;

// Immutable runtime handle for registered memory.
// Created by IAccelerator::register_*, destroyed by unregister().
struct MemoryRegion {
    // Identity
    uint64_t    region_id;      // Unique across the runtime
    MemoryKind  kind;
    int         device_id;      // Which accelerator device owns this (or -1 for host-only)

    // Address views
    void*       host_ptr;       // nullptr if device-only
    void*       device_ptr;     // nullptr if host-only
    size_t      size;

    // External memory metadata (valid only if kind == EXTERNAL)
    ExternalMemorySpec* external;

    // Backend registration metadata (opaque to HAL)
    // - DMA ioaddrs (NVMe, RDMA)
    // - RDMA keys
    // Set by dma_map(), not by register_*()
    void* backend_private;
};

struct ExternalMemorySpec {
    enum class Source {
        APP_MANAGED,  // Caller allocated, HAL just tracks it
        DEVICE_IPC,   // Device-runtime IPC handle (e.g. cudaIpcOpenMemHandle)
        HOST_SHM,     // shm_open + mmap
        HOST_FD_MAP,  // fd + mmap
    } source;

    union {
        struct { /* empty */ } app_managed;
        struct { uint8_t handle[64]; } device_ipc;  // Opaque device IPC handle
        struct { int shm_fd; } host_shm;
        struct { int fd; off_t offset; } host_fd;
    };
};

} // namespace tutti
