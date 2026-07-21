// tutti/accel/include/memory_kind.h -- Memory allocation kinds
//
// Enumerates the types of memory that can be allocated via IAccelerator.

#pragma once

#include <cstdint>

namespace tutti {

// ---------------------------------------------------------------------------
// Memory allocation kind
// ---------------------------------------------------------------------------

enum class MemoryKind : uint8_t {
    // Host-side memory (pageable, not accessible from device)
    HOST = 0,

    // Host-side memory (pinned, DMA-capable, device-accessible via PCIe)
    PINNED_HOST = 1,

    // Device-side memory (on-GPU VRAM)
    DEVICE = 2,

    // Unified memory (managed by runtime, accessible from both host and device)
    MANAGED = 3,

    // External memory (registered from user pointer, not owned by allocator)
    EXTERNAL = 4,
};

// ---------------------------------------------------------------------------
// Memory access flags
// ---------------------------------------------------------------------------

// Flags for memory registration and mapping operations
enum MemoryAccessFlags : uint32_t {
    ACCESS_NONE = 0,
    ACCESS_READ = 1 << 0,     // Memory can be read by device
    ACCESS_WRITE = 1 << 1,    // Memory can be written by device
    ACCESS_HOST_MAPPED = 1 << 2,  // Device memory should be host-accessible
};

}  // namespace tutti
