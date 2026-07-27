// nvme_target_handle.cpp -- NvmeBackend target handle management

#include "nvme_backend.h"
#include "nvme_target_handle.h"
#include "backends/include/storage_target.h"
#include "device_manager/nvme/include/nvme_virtual_device.h"

#include <cuda_runtime.h>
#include <cstring>
#include <cstdio>

namespace tutti {
namespace backends {
namespace nvme {

void* NvmeBackend::acquire_target_handle(const StorageTarget& target, VDeviceHandle hdl) {
    NvmeVirtualDevice* ndev = nvme_vdev_at(hdl.index);
    if (ndev == nullptr) {
        fprintf(stderr, "[NvmeBackend] ERROR: acquire_target_handle: invalid VDeviceHandle %u\n",
                hdl.index);
        return nullptr;
    }

    if (target.kind != StorageTargetKind::NVME_FILE &&
        target.kind != StorageTargetKind::NVME_RAW) {
        fprintf(stderr, "[NvmeBackend] ERROR: unsupported target kind %u\n",
                static_cast<uint32_t>(target.kind));
        return nullptr;
    }

    // Build host-side template
    NvmeFileDeviceHandle host_handle;
    memset(&host_handle, 0, sizeof(host_handle));

    host_handle.file_id            = target.target_id;
    host_handle.logical_size_bytes = target.logical_size_bytes;
    host_handle.namespace_id       = target.namespace_id;
    host_handle.nvme_block_size    = target.nvme_block_size;
    host_handle.nvme_block_size_log = target.nvme_block_size_log;
    host_handle.num_extents        = target.num_extents;
    host_handle.extents_overflow   = nullptr;

    // Copy queue fields inline so the GPU kernel never dereferences a host pointer
    host_handle.d_qps       = ndev->d_qps;
    host_handle.queue_quota = ndev->queue_quota;

    void* d_inline_extents   = nullptr;
    void* d_overflow_extents = nullptr;

    if (target.kind == StorageTargetKind::NVME_FILE) {
        if (target.num_extents == 0 || target.extents == nullptr) {
            fprintf(stderr, "[NvmeBackend] ERROR: NVME_FILE target has no extents\n");
            return nullptr;
        }

        // -- Inline extents (up to MAX_INLINE_EXTENTS) --
        uint32_t inline_count =
            (target.num_extents < NvmeFileDeviceHandle::MAX_INLINE_EXTENTS)
                ? target.num_extents
                : NvmeFileDeviceHandle::MAX_INLINE_EXTENTS;

        cudaError_t err = cudaMalloc(&d_inline_extents, inline_count * sizeof(LbaExtent));
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc inline extents: %s\n",
                    cudaGetErrorString(err));
            return nullptr;
        }
        err = cudaMemcpy(d_inline_extents, target.extents,
                         inline_count * sizeof(LbaExtent), cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy inline extents: %s\n",
                    cudaGetErrorString(err));
            cudaFree(d_inline_extents);
            return nullptr;
        }
        host_handle.extents = static_cast<LbaExtent*>(d_inline_extents);

        // -- Overflow extents --
        if (target.num_extents > NvmeFileDeviceHandle::MAX_INLINE_EXTENTS) {
            uint32_t overflow_count =
                target.num_extents - NvmeFileDeviceHandle::MAX_INLINE_EXTENTS;

            err = cudaMalloc(&d_overflow_extents, overflow_count * sizeof(LbaExtent));
            if (err != cudaSuccess) {
                fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc overflow extents: %s\n",
                        cudaGetErrorString(err));
                cudaFree(d_inline_extents);
                return nullptr;
            }
            err = cudaMemcpy(d_overflow_extents,
                             &target.extents[NvmeFileDeviceHandle::MAX_INLINE_EXTENTS],
                             overflow_count * sizeof(LbaExtent), cudaMemcpyHostToDevice);
            if (err != cudaSuccess) {
                fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy overflow extents: %s\n",
                        cudaGetErrorString(err));
                cudaFree(d_overflow_extents);
                cudaFree(d_inline_extents);
                return nullptr;
            }
            host_handle.extents_overflow = static_cast<LbaExtent*>(d_overflow_extents);
        }

    } else {  // NVME_RAW -- single synthetic extent
        LbaExtent raw_extent;
        raw_extent.start_lba      = target.start_lba;
        raw_extent.length_blocks  = target.length_blocks;
        raw_extent.logical_offset = 0;

        cudaError_t err = cudaMalloc(&d_inline_extents, sizeof(LbaExtent));
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc raw extent: %s\n",
                    cudaGetErrorString(err));
            return nullptr;
        }
        err = cudaMemcpy(d_inline_extents, &raw_extent,
                         sizeof(LbaExtent), cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy raw extent: %s\n",
                    cudaGetErrorString(err));
            cudaFree(d_inline_extents);
            return nullptr;
        }
        host_handle.extents     = static_cast<LbaExtent*>(d_inline_extents);
        host_handle.num_extents = 1;
    }

    // -- Allocate and populate GPU handle --
    void* d_handle = nullptr;
    cudaError_t err = cudaMalloc(&d_handle, sizeof(NvmeFileDeviceHandle));
    if (err != cudaSuccess) {
        fprintf(stderr, "[NvmeBackend] ERROR: cudaMalloc device handle: %s\n",
                cudaGetErrorString(err));
        if (d_inline_extents)   cudaFree(d_inline_extents);
        if (d_overflow_extents) cudaFree(d_overflow_extents);
        return nullptr;
    }
    err = cudaMemcpy(d_handle, &host_handle,
                     sizeof(NvmeFileDeviceHandle), cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        fprintf(stderr, "[NvmeBackend] ERROR: cudaMemcpy device handle: %s\n",
                cudaGetErrorString(err));
        cudaFree(d_handle);
        if (d_inline_extents)   cudaFree(d_inline_extents);
        if (d_overflow_extents) cudaFree(d_overflow_extents);
        return nullptr;
    }

    // Track for cleanup
    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        TargetHandleEntry entry;
        entry.device_ptr       = d_handle;
        entry.inline_extents   = d_inline_extents;
        entry.overflow_extents = d_overflow_extents;
        entry.target_id        = target.target_id;
        target_handles_[d_handle] = entry;
    }

    fprintf(stderr, "[NvmeBackend] Acquired target handle: target_id=%lu, "
            "num_extents=%u, vdev_id=%u, overflow=%s\n",
            target.target_id, target.num_extents, ndev->vdev_id(),
            d_overflow_extents ? "yes" : "no");

    return d_handle;
}

void NvmeBackend::release_target_handle(void* handle) {
    if (handle == nullptr) return;

    TargetHandleEntry entry;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(target_handles_mutex_);
        auto it = target_handles_.find(handle);
        if (it != target_handles_.end()) {
            entry = it->second;
            target_handles_.erase(it);
            found = true;
        }
    }

    if (!found) {
        fprintf(stderr, "[NvmeBackend] WARNING: release_target_handle: "
                "untracked handle %p\n", handle);
        return;
    }

    if (entry.inline_extents)   cudaFree(entry.inline_extents);
    if (entry.overflow_extents) cudaFree(entry.overflow_extents);
    cudaFree(entry.device_ptr);

    fprintf(stderr, "[NvmeBackend] Released target handle: target_id=%lu\n",
            entry.target_id);
}

} // namespace nvme
} // namespace backends
} // namespace tutti
