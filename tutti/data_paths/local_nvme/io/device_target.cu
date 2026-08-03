// tutti/data_paths/local_nvme/io/device_target.cu
//
// Device target handle build/free — ported from main's
// HostFsBackedNvmeStorage::build_handle_template_ / release path.

#include "tutti/data_paths/local_nvme/io/device_target.h"

#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>

namespace tutti::data_paths::local_nvme {

bool build_device_target(
    const DeviceTargetHandle& host_template,
    const DeviceLbaExtent* overflow_extents,
    uint32_t n_overflow,
    uint32_t cuda_device,
    DeviceTargetHandle** out_handle,
    void** out_overflow_dev)
{
    *out_handle = nullptr;
    *out_overflow_dev = nullptr;

    cudaError_t cerr;

    // 1. Allocate overflow buffer (if needed) — done first so we can
    //    clean up on failure before allocating the main handle.
    void* overflow_dev = nullptr;
    if (n_overflow > 0 && overflow_extents != nullptr) {
        cerr = cudaSetDevice(cuda_device);
        if (cerr != cudaSuccess) {
            std::fprintf(stderr,
                "[device_target] cudaSetDevice(%u): %s\n",
                cuda_device, cudaGetErrorString(cerr));
            return false;
        }
        cerr = cudaMalloc(&overflow_dev,
            (std::size_t)n_overflow * sizeof(DeviceLbaExtent));
        if (cerr != cudaSuccess) {
            std::fprintf(stderr,
                "[device_target] cudaMalloc(overflow, %u extents): %s\n",
                n_overflow, cudaGetErrorString(cerr));
            return false;
        }
        cerr = cudaMemcpy(overflow_dev, overflow_extents,
            (std::size_t)n_overflow * sizeof(DeviceLbaExtent),
            cudaMemcpyHostToDevice);
        if (cerr != cudaSuccess) {
            std::fprintf(stderr,
                "[device_target] cudaMemcpy(overflow): %s\n",
                cudaGetErrorString(cerr));
            cudaFree(overflow_dev);
            return false;
        }
    }

    // 2. Fill the overflow pointer in the template.
    DeviceTargetHandle tmpl = host_template;
    tmpl.extents_overflow = (DeviceLbaExtent*)overflow_dev;

    // 3. Allocate device handle and H2D copy.
    cerr = cudaSetDevice(cuda_device);
    if (cerr != cudaSuccess) {
        std::fprintf(stderr,
            "[device_target] cudaSetDevice(%u): %s\n",
            cuda_device, cudaGetErrorString(cerr));
        if (overflow_dev) cudaFree(overflow_dev);
        return false;
    }

    DeviceTargetHandle* dev_handle = nullptr;
    cerr = cudaMalloc(&dev_handle, sizeof(DeviceTargetHandle));
    if (cerr != cudaSuccess) {
        std::fprintf(stderr,
            "[device_target] cudaMalloc(handle): %s\n",
            cudaGetErrorString(cerr));
        if (overflow_dev) cudaFree(overflow_dev);
        return false;
    }

    cerr = cudaMemcpy(dev_handle, &tmpl, sizeof(DeviceTargetHandle),
                      cudaMemcpyHostToDevice);
    if (cerr != cudaSuccess) {
        std::fprintf(stderr,
            "[device_target] cudaMemcpy(handle): %s\n",
            cudaGetErrorString(cerr));
        cudaFree(dev_handle);
        if (overflow_dev) cudaFree(overflow_dev);
        return false;
    }

    *out_handle = dev_handle;
    *out_overflow_dev = overflow_dev;
    return true;
}

void free_device_target(
    DeviceTargetHandle* handle,
    void* overflow_dev,
    uint32_t /*cuda_device*/)
{
    if (overflow_dev != nullptr) {
        cudaFree(overflow_dev);
    }
    if (handle != nullptr) {
        cudaFree(handle);
    }
}

} // namespace tutti::data_paths::local_nvme
