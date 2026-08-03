// tutti/data_paths/local_nvme/io/submit_one.cu
//
// Host launcher for the one-thread-per-entry NVMe IO kernel.
// Ported from main's nvme_batch_xfer_kernel.cu launch shape. The launcher
// accepts the full per-operation entry batch and spans as many CUDA blocks
// as required.

#include "tutti/data_paths/local_nvme/io/submit_one.cuh"

#include <cuda_runtime.h>

namespace tutti::data_paths::local_nvme {

cudaError_t launch_submit_one(
    const DeviceSubmitEntry* d_entries,
    EntryCompletionStatus*   d_status,
    std::uint32_t            count,
    std::uint32_t            cq_poll_budget,
    std::uint32_t            inject_flag,
    void*                    stream)
{
    cudaStream_t s = static_cast<cudaStream_t>(stream);
    std::uint32_t threads = 256;
    std::uint32_t blocks = (count + threads - 1) / threads;
    if (blocks == 0) blocks = 1;
    submit_one_kernel<<<blocks, threads, 0, s>>>(d_entries, d_status, count,
                                                  cq_poll_budget, inject_flag);
    cudaError_t err = cudaGetLastError();
    return err;
}

// Fill kernel: writes val to the first n bytes of buf.
// GPU kernel writes are visible to NVMe DMA (unlike cudaMemsetAsync
// which may stay in L2 cache and not be visible to the NVMe controller).
__global__
void fill_pattern_kernel(unsigned char* buf, unsigned char val, std::uint64_t n)
{
    std::uint64_t tid = threadIdx.x + (std::uint64_t)blockIdx.x * blockDim.x;
    if (tid < n) buf[tid] = val;
}

void launch_fill_pattern(void* buf, unsigned char val, std::uint64_t n,
                          void* stream)
{
    cudaStream_t s = static_cast<cudaStream_t>(stream);
    std::uint32_t threads = 256;
    std::uint32_t blocks = (std::uint32_t)((n + threads - 1) / threads);
    fill_pattern_kernel<<<blocks, threads, 0, s>>>(
        (unsigned char*)buf, val, n);
}

} // namespace tutti::data_paths::local_nvme
