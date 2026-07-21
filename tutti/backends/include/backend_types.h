#ifndef TUTTI_BACKENDS_BACKEND_TYPES_H_
#define TUTTI_BACKENDS_BACKEND_TYPES_H_

#include <cstdint>
#include <cstddef>

namespace tutti {
namespace backends {

// Backend type identifier - moved from device_manager for Layer 3 independence
enum class BackendType {
    LOCAL_NVME = 0,  // Local NVMe via Device Manager VDevice
    RDMA = 1,        // RDMA-capable remote storage
    GDS = 2,         // NVIDIA GPUDirect Storage
    UNKNOWN = 255
};

// Backend capability flags
enum BackendCapability : uint32_t {
    SUPPORTS_GPUDIRECT = 1 << 0,  // Can bypass CPU for data movement
    SUPPORTS_COOP = 1 << 1,       // Supports cooperative kernel mode
    SUPPORTS_ASYNC = 1 << 2,      // Supports CPU async submission
    SUPPORTS_SGL = 1 << 3,        // Supports scatter-gather lists
    SUPPORTS_METADATA = 1 << 4    // Supports metadata pass-through
};

// Backend metadata - returned by IBackendProvider methods
struct BackendMetadata {
    const char* name;           // Human-readable backend name
    BackendType type;           // Backend type enum
    uint32_t capabilities;      // Bitfield of BackendCapability flags
    size_t max_io_size;         // Maximum single IO size in bytes
    size_t max_batch_size;      // Maximum batch descriptor count
    size_t alignment_bytes;     // Required buffer alignment
};

// Sub-slice layout information for descriptor construction
struct SubSliceInfo {
    uint64_t offset_bytes;      // Offset within the larger buffer
    uint32_t length_bytes;      // Length of this sub-slice
    uint32_t slice_index;       // Index for tracking/debugging
};

// Buffer descriptor - transport-agnostic IO descriptor
// Backends populate this during prepare_descriptors()
struct BufferDescriptor {
    uint64_t prp1;              // First PRP entry (or SGL descriptor address)
    uint64_t prp2;              // Second PRP entry or PRP-list pointer
    uint64_t storage_offset;    // Logical offset within the target (file/LBA space)
    uint32_t data_length;       // Transfer length in bytes
    uint32_t descriptor_flags;  // Backend-specific flags (PRP kind, SGL type, etc.)
    void* backend_private;      // Backend-private metadata pointer (e.g., cached pages)
};

// Target handle wrapper - opaque pointer with type discriminator
struct TargetHandle {
    void* handle;               // Opaque backend-private handle pointer
    BackendType backend_type;   // Type discriminator for safe casting
    uint64_t target_id;         // Source target identifier for debugging

    TargetHandle() : handle(nullptr), backend_type(BackendType::UNKNOWN), target_id(0) {}

    TargetHandle(void* h, BackendType type, uint64_t id = 0)
        : handle(h), backend_type(type), target_id(id) {}

    bool is_valid() const { return handle != nullptr; }
};

// Submission result for synchronous operations
struct SubmissionResult {
    bool success;               // Overall success flag
    uint32_t completed_count;   // Number of IOs completed
    uint32_t failed_count;      // Number of IOs failed
    int error_code;             // First error code encountered (0 = success)

    SubmissionResult()
        : success(false), completed_count(0), failed_count(0), error_code(0) {}
};

// IO future handle for asynchronous operations (OPTIONAL path)
struct IOFuture {
    void* backend_private;      // Backend-specific future handle
    BackendType backend_type;   // Type discriminator

    IOFuture() : backend_private(nullptr), backend_type(BackendType::UNKNOWN) {}

    bool is_valid() const { return backend_private != nullptr; }
};

// COOP channel setup parameters (OPTIONAL path)
struct CoopChannelConfig {
    void* device_channel;       // Device-side channel pointer
    size_t channel_capacity;    // Max outstanding requests in channel
    uint32_t timeout_ms;        // Timeout for channel operations
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_BACKEND_TYPES_H_
