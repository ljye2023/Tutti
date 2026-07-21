#include "backends/include/storage_target.h"
#include "nvme_backend.h"
#include "nvme_target_handle.h"
#include "device_manager/include/common/vdevice.h"
#include <cstdint>
#include <cuda_runtime.h>
#include <cstring>
#include <cstdio>

namespace tutti {
namespace backends {
namespace nvme {

void* NvmeBackend::acquire_target_handle(const StorageTarget& target) {
    if (!validate_vdev()) {
        fprintf(stderr, "[NvmeBackend] ERROR: acquire_target_handle called with invalid vdev\n");
        return nullptr;
    }

    // Validate target kind
    if (target.kind != StorageTargetKind::NVME_FILE &&
        target.kind != StorageTargetKind::NVME_RAW) {
        fprintf(stderr, "[NvmeBackend] ERROR: Unsupported target kind: %u\n",
                static_cast<uint32_t>(target.kind));
        return nullptr;
    }

    // Allocate host-side handle template
    NvmeFileDeviceHandle host_handle;
    memset(&host_handle, 0, sizeof(host_handle));

    // Populate identity fields
    host_handle.file_id = target.target_id;
    host_handle.logical_size_bytes = target.logical_size_bytes;

    // Populate NVMe namespace parameters
    host_handle.namespace_id = target.namespace_id;
    host_handle.nvme_block_size = target.nvme_block_size;
    host_handle.nvme_block_size_log = target.nvme_block_size_log;

    // Populate extent mapping
    host_handle.num_extents = target.num_extents;
    host_handle.extents_overflow = nullptr;

    // Set VDevice reference
    host_handle.vdev = vdev_;

    // Copy extents: inline-small pattern (8 extents inline, overflow for more)
    void* overflow_extents = nullptr;

    if (target.kind == StorageTargetKind::NVME_FILE) {
        if (target.num_extents == 0 || target.extents == nullptr) {
            fprintf(stderr, "[NvmeBackend] ERROR: NVME_FILE target has no extents\n");
            return nullptr;
        }

        // Allocate inline extent array (part of the handle structure)
        // Note: extents is a pointer in the structure, needs allocation
        LbaExtent inline_extents[NvmeFileDeviceHandle::MAX_INLINE_EXTENTS];
        uint32_t inline_count = std::min(target.num_extents,
                                         NvmeFileDeviceHandle::MAX_INLINE_EXTENTS);

        // Copy inline extents
        for (uint32_t i = 0; i < inline_count; ++i) {
            inline_extents[i] = target.extents[i];
        }

        // Allocate overflow extents if needed
        if (target.num_extents > NvmeFileDeviceHandle::MAX_INLINE_EXTENTS) {
            size_t overflow_count = target.num_extents - NvmeFileDeviceHandle::MAX_INLINE_EXTENTS;
            size_t overflow_size = overflow_count * sizeof(LbaExtent);

            cudaError_t err = cudaMalloc(&overflow_extents, overflow_size);
            if (err != cudaSuccess) {
                fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc overflow extents failed: %s\n",
                        cudaGetErrorString(err));
                return nullptr;
            }

            // Copy overflow extents to GPU
            err = cudaMemcpy(overflow_extents,
                           &target.extents[NvmeFileDeviceHandle::MAX_INLINE_EXTENTS],
                           overflow_size,
                           cudaMemcpyHostToDevice);
            if (err != cudaSuccess) {
                fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy overflow extents failed: %s\n",
                        cudaGetErrorString(err));
                cudaFree(overflow_extents);
                return nullptr;
            }

            host_handle.extents_overflow = static_cast<LbaExtent*>(overflow_extents);
        }

        // Note: The actual inline extents array is allocated as part of the device handle
        // We'll need to fix the structure to have an embedded array instead of pointer
        // For now, allocate extents array separately
        LbaExtent* d_inline_extents = nullptr;
        cudaError_t err = cudaMalloc(&d_inline_extents,
                                    inline_count * sizeof(LbaExtent));
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc inline extents failed: %s\n",
                    cudaGetErrorString(err));
            if (overflow_extents) cudaFree(overflow_extents);
            return nullptr;
        }

        err = cudaMemcpy(d_inline_extents, inline_extents,
                        inline_count * sizeof(LbaExtent),
                        cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy inline extents failed: %s\n",
                    cudaGetErrorString(err));
            cudaFree(d_inline_extents);
            if (overflow_extents) cudaFree(overflow_extents);
            return nullptr;
        }

        host_handle.extents = d_inline_extents;

    } else if (target.kind == StorageTargetKind::NVME_RAW) {
        // Raw LBA range - create single extent
        LbaExtent* d_extent = nullptr;
        cudaError_t err = cudaMalloc(&d_extent, sizeof(LbaExtent));
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc raw extent failed: %s\n",
                    cudaGetErrorString(err));
            return nullptr;
        }

        LbaExtent raw_extent;
        raw_extent.start_lba = target.start_lba;
        raw_extent.length_blocks = target.length_blocks;

        err = cudaMemcpy(d_extent, &raw_extent, sizeof(LbaExtent),
                        cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy raw extent failed: %s\n",
                    cudaGetErrorString(err));
            cudaFree(d_extent);
            return nullptr;
        }

        host_handle.extents = d_extent;
        host_handle.num_extents = 1;
    }

    // Allocate device handle
    void* device_handle = nullptr;
    cudaError_t err = cudaMalloc(&device_handle, sizeof(NvmeFileDeviceHandle));
    if (err != cudaSuccess) {
        fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc device handle failed: %s\n",
                cudaGetErrorString(err));
        if (host_handle.extents) cudaFree(host_handle.extents);
        if (overflow_extents) cudaFree(overflow_extents);
        return nullptr;
    }

    // Copy host template to device
    err = cudaMemcpy(device_handle, &host_handle, sizeof(NvmeFileDeviceHandle),
                    cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy device handle failed: %s\n",
                cudaGetErrorString(err));
        cudaFree(device_handle);
        if (host_handle.extents) cudaFree(host_handle.extents);
        if (overflow_extents) cudaFree(overflow_extents);
        return nullptr;
    }

    // Track handle for cleanup
    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        TargetHandleMetadata meta;
        meta.device_ptr = device_handle;
        meta.overflow_extents = overflow_extents;
        meta.num_extents = target.num_extents;
        meta.target_id = target.target_id;
        target_handles_[device_handle] = meta;
    }

    fprintf(stderr, "[NvmeBackend] Acquired target handle: target_id=%lu, num_extents=%u, "
            "overflow=%s\n",
            target.target_id, target.num_extents,
            overflow_extents ? "yes" : "no");

    return device_handle;
}

void NvmeBackend::release_target_handle(void* handle) {
    if (handle == nullptr) {
        return;
    }

    TargetHandleMetadata meta;
    bool found = false;

    // Remove from tracking map
    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        auto it = target_handles_.find(handle);
        if (it != target_handles_.end()) {
            meta = it->second;
            target_handles_.erase(it);
            found = true;
        }
    }

    if (!found) {
        fprintf(stderr, "[NvmeBackend] WARNING: release_target_handle called with "
                "untracked handle %p\n", handle);
        return;
    }

    // Need to read the device handle to free the extents pointer
    NvmeFileDeviceHandle host_handle;
    cudaError_t err = cudaMemcpy(&host_handle, handle, sizeof(NvmeFileDeviceHandle),
                                cudaMemcpyDeviceToHost);
    if (err == cudaSuccess) {
        // Free inline extents array
        if (host_handle.extents != nullptr) {
            cudaFree(host_handle.extents);
        }
    }

    // Free overflow extents if present
    if (meta.overflow_extents != nullptr) {
        cudaFree(meta.overflow_extents);
    }

    // Free device handle
    cudaFree(meta.device_ptr);

    fprintf(stderr, "[NvmeBackend] Released target handle: target_id=%lu\n",
            meta.target_id);
}

} // namespace nvme
} // namespace backends
} // namespace tutti
