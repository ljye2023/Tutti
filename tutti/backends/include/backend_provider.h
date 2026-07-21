#ifndef TUTTI_BACKENDS_BACKEND_PROVIDER_H_
#define TUTTI_BACKENDS_BACKEND_PROVIDER_H_

#include "backend_types.h"
#include <cstddef>
#include <cstdint>

// Forward declarations to avoid hard dependencies
namespace tutti {
struct VDevice;
}

// Include storage target definition (Layer 3 type)
#include "storage_target.h"

namespace tutti {
namespace backends {

// Core backend SPI interface consumed by IO Engine and Block/Raw Storage.
// Backends implement this interface to provide transport-specific IO capabilities.
//
// Lifecycle:
//   1. Construction via BackendFactory
//   2. initialize(VDevice*) - receive queue slice from Device Manager
//   3. Steady-state IO operations (descriptor prep, target handles, submission)
//   4. cleanup() - return resources to Device Manager
//
// Key responsibilities:
//   - Descriptor build: prepare_descriptors() converts ioaddrs to transport commands
//   - Target handle management: acquire/release GPU-resident target handles
//   - Submission modes: GPU stream (REQUIRED), CPU sync (REQUIRED), async/COOP (OPTIONAL)
//   - Metadata: backend type, name, capabilities, max IO size
//
// Threading: Implementations must be thread-safe for concurrent descriptor preparation
// and submission from multiple IO Engine threads.
class IBackendProvider {
public:
    virtual ~IBackendProvider() = default;

    // ========== LIFECYCLE ==========

    // Initialize backend with Device Manager resources.
    //
    // For NVMe backends: vdev != nullptr, contains queue slice + namespace view + caps.
    // Backend stores vdev pointer for steady-state operations. Ownership remains with caller.
    //
    // For non-NVMe backends (GDS, RDMA): vdev == nullptr, backend ignores and uses
    // its own resource initialization path.
    //
    // Called once at bootstrap after backend construction. Must be called before any
    // other operations except metadata queries.
    //
    // Returns true on success, false on failure (backend unusable if false).
    virtual bool initialize(VDevice* vdev) = 0;

    // Clean up backend resources and return them to Device Manager.
    //
    // Must release all target handles, free descriptor resources (PRP pages, etc.),
    // and return queue slice to Device Manager (via caller - backend does not call DM directly).
    //
    // After cleanup(), backend is unusable. No operations except destruction are valid.
    virtual void cleanup() = 0;

    // ========== DESCRIPTOR PREPARATION ==========

    // Prepare transport-specific descriptors from raw DMA addresses.
    //
    // Called by Memory Layer during tensor registration. Given ioaddrs (DMA bus addresses
    // from Accel HAL) and SubSliceInfo layout, produce BufferDescriptors ready for submission.
    //
    // For NVMe backends: construct PRP or SGL descriptors, allocate PRP-list pages from
    // internal cache, populate BufferDescriptor::prp1/prp2 fields.
    //
    // For RDMA backends: construct RDMA descriptors with rkeys and remote addresses.
    //
    // Backend owns descriptor lifetime. Caller must call release_descriptors() when done.
    //
    // ioaddrs: Array of DMA bus addresses (one per page/segment)
    // slices: Array of sub-slice layout info (offset, length, index)
    // n_slices: Number of sub-slices to process
    // out_descs: Output array of BufferDescriptors (caller-allocated, size >= n_slices)
    //
    // Returns true on success, false on failure (e.g., out of PRP pages, invalid addresses).
    virtual bool prepare_descriptors(
        const uint64_t* ioaddrs,
        const SubSliceInfo* slices,
        uint32_t n_slices,
        BufferDescriptor* out_descs) = 0;

    // Release descriptors and return internal resources to cache.
    //
    // Must be called when descriptors are no longer needed (tensor deregistration, error cleanup).
    // For NVMe: returns PRP-list pages to cache. For RDMA: may unpin memory or release rkeys.
    //
    // descs: Array of descriptors to release (from prior prepare_descriptors() call)
    // n_descs: Number of descriptors in array
    virtual void release_descriptors(BufferDescriptor* descs, uint32_t n_descs) = 0;

    // ========== TARGET HANDLE MANAGEMENT ==========

    // Acquire GPU-resident target handle from StorageTarget.
    //
    // Given StorageTarget (file extents, raw LBA range, or remote address), build
    // device-resident handle suitable for kernel consumption.
    //
    // For NVMe backends: allocate and populate NvmeFileDeviceHandle (extents + vdev reference),
    // cudaMalloc + cudaMemcpy to GPU, return device pointer.
    //
    // For RDMA backends: build remote address descriptor with rkeys.
    //
    // Handle lifetime: acquired once per file/target open, held for file lifetime,
    // released at file close via release_target_handle().
    //
    // Backend tracks allocated handles internally for cleanup validation.
    //
    // target: Storage target from namespace layer (file extents, LBA range, etc.)
    //
    // Returns opaque device pointer typed by backend, or nullptr on failure.
    // Caller wraps in TargetHandle with backend_type discriminator.
    virtual void* acquire_target_handle(const StorageTarget& target) = 0;

    // Release GPU-resident target handle and free resources.
    //
    // Frees GPU memory (cudaFree), releases overflow extents if present, removes from
    // backend's internal tracking map.
    //
    // handle: Device pointer returned by prior acquire_target_handle() call
    //
    // Must be idempotent (double-free is safe no-op). Invalid handles logged but not fatal.
    virtual void release_target_handle(void* handle) = 0;

    // ========== SUBMISSION MODES ==========

    // REQUIRED: Launch GPU batch submission kernel on AccelStream.
    //
    // Backend launches its own __global__ kernel on the provided stream. Backend picks
    // grid/block dimensions based on batch size and backend-specific tuning.
    //
    // Kernel uses Device Manager's device-side queue mechanics (acquire_queue, issue_nvme_cmd,
    // poll) to submit and complete IOs. Completion is stream-ordered - kernel exit implies
    // all IOs complete.
    //
    // This is the primary production IO path. Must be implemented by all backends.
    //
    // stream: AccelStream (opaque void* from abstraction layer, cast to cudaStream_t internally)
    // target_handle: Device pointer from acquire_target_handle() (backend casts to typed pointer)
    // descs: Array of BufferDescriptors from prepare_descriptors() (device-accessible memory)
    // n_descs: Number of descriptors in batch
    // is_read: true for READ operations, false for WRITE operations
    virtual void launch_batch_gpu_stream(
        void* stream,
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;

    // REQUIRED: Synchronous CPU submission - prepare, submit, poll completion.
    //
    // CPU-side path for bootstrap, metadata operations, and testing. No GPU kernel involvement.
    //
    // Backend resolves LBAs, builds transport commands (NVMe SQE, RDMA WR), submits via
    // host API (libnvm nvm_cmd_read/write, ibv_post_send), polls completion queue until done.
    //
    // Blocks until all IOs complete or first failure. Returns success/failure status.
    //
    // Used during bootstrap before GPU is ready, for metadata operations, and in tests.
    // Must be implemented by all backends.
    //
    // target_handle: Device pointer from acquire_target_handle() (backend casts to typed pointer)
    // descs: Array of BufferDescriptors from prepare_descriptors() (host-accessible memory)
    // n_descs: Number of descriptors in batch
    // is_read: true for READ operations, false for WRITE operations
    //
    // Returns SubmissionResult with success flag and completion counts.
    virtual SubmissionResult submit_batch_cpu_sync(
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;

    // OPTIONAL: Asynchronous CPU submission - returns immediately with IOFuture.
    //
    // Submit IOs on CPU thread pool or async submission queue. Returns IOFuture handle
    // that caller can poll or wait on for completion.
    //
    // Backends may return false if async mode is unsupported. Caller falls back to
    // submit_batch_cpu_sync() if this returns false.
    //
    // future: Output parameter for IOFuture handle (caller-allocated)
    // target_handle: Device pointer from acquire_target_handle()
    // descs: Array of BufferDescriptors from prepare_descriptors()
    // n_descs: Number of descriptors in batch
    // is_read: true for READ operations, false for WRITE operations
    //
    // Returns true if submission succeeded (future is valid), false if unsupported or failed.
    virtual bool submit_batch_cpu_async(
        IOFuture* future,
        void* target_handle,
        const BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) = 0;

    // OPTIONAL: Setup cooperative kernel channel for COOP submission mode.
    //
    // Allocate device-side channel for cooperative kernel IO submission. Kernels enqueue
    // IO requests to channel, backend's persistent kernel or thread pool services requests.
    //
    // Backends may return false if COOP mode is unsupported. Caller falls back to
    // launch_batch_gpu_stream() if this returns false.
    //
    // config: COOP channel configuration (channel capacity, timeout, etc.)
    // target_handle: Device pointer from acquire_target_handle()
    //
    // Returns true if channel setup succeeded, false if unsupported or failed.
    virtual bool setup_coop_channel(
        const CoopChannelConfig& config,
        void* target_handle) = 0;

    // Poll IOFuture for completion (OPTIONAL - only if submit_batch_cpu_async supported).
    //
    // Check if async submission has completed. Non-blocking poll.
    //
    // future: IOFuture handle from prior submit_batch_cpu_async() call
    // out_result: Output parameter for submission result (only valid if completed)
    //
    // Returns true if completed (out_result valid), false if still pending.
    virtual bool poll_future(const IOFuture& future, SubmissionResult* out_result) = 0;

    // Wait for IOFuture completion (OPTIONAL - only if submit_batch_cpu_async supported).
    //
    // Block until async submission completes or timeout expires.
    //
    // future: IOFuture handle from prior submit_batch_cpu_async() call
    // timeout_ms: Timeout in milliseconds (0 = no timeout, wait indefinitely)
    // out_result: Output parameter for submission result
    //
    // Returns true if completed before timeout, false if timed out.
    virtual bool wait_future(
        const IOFuture& future,
        uint32_t timeout_ms,
        SubmissionResult* out_result) = 0;

    // ========== METADATA ==========

    // Get backend type identifier.
    virtual BackendType backend_type() const = 0;

    // Get human-readable backend name (e.g., "local_nvme", "rdma", "gds").
    virtual const char* backend_name() const = 0;

    // Get maximum single IO size in bytes.
    //
    // For NVMe: typically VDevice MDTS (controller max data transfer size).
    // For RDMA: max inline data or max SGE size.
    // For GDS: cuFile max IO size.
    virtual size_t max_io_size() const = 0;

    // Get full backend metadata (name, type, caps, limits).
    virtual BackendMetadata metadata() const = 0;
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_BACKEND_PROVIDER_H_
