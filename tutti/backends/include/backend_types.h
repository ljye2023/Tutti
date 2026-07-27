#ifndef TUTTI_BACKENDS_BACKEND_TYPES_H_
#define TUTTI_BACKENDS_BACKEND_TYPES_H_

#include <cstdint>
#include <cstddef>

namespace tutti {
namespace backends {

// Backend type identifier - one per transport family.
// Mirrors DeviceType (Layer 2) but stays independent so Layer 3 does not
// depend on the device_manager enum for its public identity.
enum class BackendType {
    LOCAL_NVME = 0,  // Local NVMe via Device Manager vdevices
    RDMA = 1,        // RDMA-capable remote storage
    GDS = 2,         // NVIDIA GPUDirect Storage
    MOCK = 3,        // In-memory test backend (device-agnostic; no transport)
    UNKNOWN = 255
};

// Backend capability flags.
enum BackendCapability : uint32_t {
    SUPPORTS_GPUDIRECT = 1 << 0,  // Can bypass CPU for data movement
    SUPPORTS_COOP = 1 << 1,       // Supports cooperative kernel mode
    SUPPORTS_ASYNC = 1 << 2,      // Supports CPU async submission
    SUPPORTS_SGL = 1 << 3,        // Supports scatter-gather lists
    SUPPORTS_METADATA = 1 << 4    // Supports metadata pass-through
};

// Backend metadata - returned by IBackend::metadata().
struct BackendMetadata {
    const char* name;           // Human-readable backend name
    BackendType type;           // Backend type enum
    uint32_t capabilities;      // Bitfield of BackendCapability flags
    size_t max_io_size;         // Maximum single IO size in bytes
    size_t max_batch_size;      // Maximum batch descriptor count
    size_t alignment_bytes;     // Required buffer alignment
};

// Handle identifying one vdevice within a backend's roster.
//
// A backend acquires N vdevices from IDeviceManager at initialize() and keeps
// them in a dense array. VDeviceHandle is the caller-facing token that selects
// which vdevice an operation (register / submit_one) targets. It carries only
// the dense index; the backend maps it back to the concrete IVirtualDevice*
// (and any transport-specific, device-side resources) internally.
//
// This keeps the selection token transport-neutral: NVMe's submit_one is a
// device-side (__device__) function, so the host interface only needs an index,
// not a pointer to device resources.
struct VDeviceHandle {
    static constexpr uint32_t INVALID = 0xFFFFFFFFu;

    uint32_t index;  // Dense index into the backend's vdevice roster

    VDeviceHandle() : index(INVALID) {}
    explicit VDeviceHandle(uint32_t i) : index(i) {}

    bool is_valid() const { return index != INVALID; }
};

// Configuration for backend bring-up.
//
// Passed to IBackend::initialize(). Tells the backend which physical device to
// slice and how to carve its vdevice roster from the Device Manager grant.
//
// The backend calls IDeviceManager::open_vdevice(phys_id, quota_per_vdevice)
// vdevice_count times, populating its roster. Transport-specific tuning that is
// not device-agnostic belongs in the concrete backend, not here.
struct BackendConfig {
    int32_t  phys_id;             // Physical device id (from IDeviceManager registry)
    uint32_t vdevice_count;       // How many vdevices to open on this backend
    uint32_t quota_per_vdevice;   // Resource units per vdevice (e.g. NVMe queue pairs)

    BackendConfig()
        : phys_id(-1), vdevice_count(1), quota_per_vdevice(1) {}
};

// Result of a synchronous submission (host-side paths, e.g. bootstrap/tests).
//
// Device-side submit_one paths report completion through their own transport
// mechanics; this struct is for host-side callers only.
struct SubmissionResult {
    bool success;               // Overall success flag
    uint32_t completed_count;   // Number of IOs completed
    uint32_t failed_count;      // Number of IOs failed
    int error_code;             // First error code encountered (0 = success)

    SubmissionResult()
        : success(false), completed_count(0), failed_count(0), error_code(0) {}
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_BACKEND_TYPES_H_
