#include "nvme_device_helpers.cuh"
#include "nvme_target_handle.h"
#include "nvme_io_types.h"
#include <cuda_runtime.h>

namespace tutti {
namespace backends {
namespace nvme {
namespace device {

// GPU batch submission kernel - launched by launch_batch_gpu_stream.
//
// Each thread processes one descriptor:
//   1. Load descriptor (offset, length, PRP entries)
//   2. Call submit_read_one() or submit_write_one() based on is_read flag
//   3. Poll completion via DM's poll(qp, cid)
//   4. (Optional) Write completion status to output array
//
// Grid/block sizing: block_size = 256, grid_size = (n_descs + 255) / 256
// Single-dimensional launch for simplicity.
__global__ void submit_batch_kernel(
    NvmeFileDeviceHandle* handle,
    const BufferDescriptor* descs,
    uint32_t n_descs,
    bool is_read)
{
    // Calculate global thread ID
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    // Guard: return if beyond descriptor array
    if (tid >= n_descs) {
        return;
    }

    // Load descriptor
    const BufferDescriptor& desc = descs[tid];

    // Submit IO command
    int cid;
    if (is_read) {
        cid = submit_read_one(
            handle,
            desc.storage_offset,
            desc.prp1,
            desc.prp2,
            desc.data_length);
    } else {
        cid = submit_write_one(
            handle,
            desc.storage_offset,
            desc.prp1,
            desc.prp2,
            desc.data_length);
    }

    // Check submission result
    if (cid < 0) {
        // Submission failed
        // TODO: Report error to output array if provided
        return;
    }

    // Poll completion
    // TODO: Call DM's poll(qp, cid) in loop until complete
    // For now, assume synchronous completion (placeholder)

    // TODO: Write completion status to output array if provided
}

// Host-side kernel launcher.
//
// Computes grid/block dimensions and launches submit_batch_kernel on stream.
//
// stream: AccelStream (cast to cudaStream_t)
// handle: GPU-resident target handle (NvmeFileDeviceHandle*)
// descs: Array of BufferDescriptors (device-accessible memory)
// n_descs: Number of descriptors in batch
// is_read: true for READ, false for WRITE
void launch_submit_batch_kernel(
    void* stream,
    NvmeFileDeviceHandle* handle,
    const BufferDescriptor* descs,
    uint32_t n_descs,
    bool is_read)
{
    if (handle == nullptr || descs == nullptr || n_descs == 0) {
        return;
    }

    // Compute grid/block dimensions
    const uint32_t block_size = 256;  // Warp-aligned
    const uint32_t grid_size = (n_descs + block_size - 1) / block_size;

    // Cast stream to cudaStream_t
    cudaStream_t cuda_stream = static_cast<cudaStream_t>(stream);

    // Launch kernel
    submit_batch_kernel<<<grid_size, block_size, 0, cuda_stream>>>(
        handle, descs, n_descs, is_read);

    // Check for launch errors (non-blocking)
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        // Note: Can't use fprintf in device code, but this is host code
        fprintf(stderr, "[NvmeBackend] ERROR: Kernel launch failed: %s\n",
                cudaGetErrorString(err));
    }
}

} // namespace device
} // namespace nvme
} // namespace backends
} // namespace tutti
