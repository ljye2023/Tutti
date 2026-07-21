#pragma once
#include <cstdint>

namespace tutti {

// Identifies the storage backend for IO operations.
// Placeholder for now; will be used by upper layers (IO Engine, Coordinator).
// Will expand as additional storage backends are integrated.
enum class StorageTarget : uint32_t {
    LOCAL_NVME   = 0,  // Local NVMe SSD via kernel bypass (SPDK, libnvm)
    REMOTE_RDMA  = 1,  // Remote storage over RDMA
    LOCAL_FILE   = 2,  // Local filesystem (fallback for testing)
    REMOTE_HTTP  = 3,  // Remote object storage via HTTP(S)
    // Future: CEPH, GCS, S3, etc.
};

// Helper for string conversion (useful for logging/debugging)
inline const char* storage_target_to_string(StorageTarget target) {
    switch (target) {
        case StorageTarget::LOCAL_NVME:   return "LOCAL_NVME";
        case StorageTarget::REMOTE_RDMA:  return "REMOTE_RDMA";
        case StorageTarget::LOCAL_FILE:   return "LOCAL_FILE";
        case StorageTarget::REMOTE_HTTP:  return "REMOTE_HTTP";
        default:                          return "UNKNOWN";
    }
}

} // namespace tutti
