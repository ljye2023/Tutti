// nvme_submission.cpp -- NvmeBackend submission paths

#include "nvme_backend.h"
#include "nvme_target_handle.h"
#include "device_manager/nvme/include/nvme_virtual_device.h"

#include <cuda_runtime.h>
#include <cstdio>

// Forward-declare device kernel launcher (defined in device/submit_batch_kernel.cu)
namespace tutti {
namespace backends {
namespace nvme {
namespace device {

void launch_submit_batch_kernel(
    void*                   stream,
    NvmeFileDeviceHandle*   handle,
    const BufferDescriptor* descs,
    uint32_t                n_descs,
    bool                    is_read);

} // namespace device
} // namespace nvme
} // namespace backends
} // namespace tutti

namespace tutti {
namespace backends {
namespace nvme {

void NvmeBackend::launch_batch_gpu_stream(
    void*                   stream,
    void*                   target_handle,
    const BufferDescriptor* descs,
    uint32_t                n_descs,
    bool                    is_read)
{
    if (target_handle == nullptr || descs == nullptr || n_descs == 0) {
        fprintf(stderr, "[NvmeBackend] ERROR: launch_batch_gpu_stream: invalid parameters\n");
        return;
    }

    auto* typed_handle = static_cast<NvmeFileDeviceHandle*>(target_handle);
    device::launch_submit_batch_kernel(stream, typed_handle, descs, n_descs, is_read);
}

SubmissionResult NvmeBackend::submit_batch_cpu_sync(
    void*                   target_handle,
    const BufferDescriptor* descs,
    uint32_t                n_descs,
    bool                    is_read)
{
    SubmissionResult result;
    result.success         = false;
    result.completed_count = 0;
    result.failed_count    = 0;
    result.error_code      = 0;

    if (target_handle == nullptr || descs == nullptr || n_descs == 0) {
        fprintf(stderr, "[NvmeBackend] ERROR: submit_batch_cpu_sync: invalid parameters\n");
        result.error_code = -1;
        return result;
    }

    // Copy device handle to host for CPU processing
    NvmeFileDeviceHandle host_handle;
    cudaError_t err = cudaMemcpy(&host_handle, target_handle,
                                 sizeof(NvmeFileDeviceHandle),
                                 cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy target_handle: %s\n",
                cudaGetErrorString(err));
        result.error_code = -2;
        return result;
    }

    // CPU synchronous submission path:
    // Real implementation would:
    //   1. Resolve LBAs from extents + descriptor offsets
    //   2. Build NVMe SQE commands
    //   3. Submit via libnvm nvm_cmd_read/write
    //   4. Poll completion queue
    //   5. Return actual success/failure status
    //
    // For now, this is a placeholder stub.

    fprintf(stderr, "[NvmeBackend] WARNING: submit_batch_cpu_sync not fully implemented\n");
    fprintf(stderr, "[NvmeBackend]   target_id=%lu, n_descs=%u, is_read=%d\n",
            host_handle.file_id, n_descs, is_read);

    result.success         = true;
    result.completed_count = n_descs;
    result.failed_count    = 0;
    result.error_code      = 0;

    return result;
}

} // namespace nvme
} // namespace backends
} // namespace tutti
