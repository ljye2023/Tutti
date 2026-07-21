# NVMe Backend - Layer 3 Transport Adapter

## Overview

The NVMe backend is a concrete implementation of `IBackendProvider` that provides NVMe transport capabilities for local NVMe SSDs. It consumes `VDevice` from the Device Manager (Layer 2) and provides NVMe-specific command construction, descriptor management, and IO submission.

## Architecture

```
Layer 3 (Backends)
  ├── backends/include/          - Common backend SPI
  │   ├── backend_provider.h     - IBackendProvider interface
  │   ├── backend_factory.h      - Backend factory for runtime selection
  │   └── backend_types.h        - Backend-agnostic types
  └── backends/nvme/             - NVMe backend implementation
      ├── include/               - NVMe backend public headers
      ├── src/                   - Host-side implementation
      └── device/                - Device-side kernels
```

## Key Components

### Core Classes

1. **NvmeBackend** (`nvme/include/nvme_backend.h`)
   - Concrete `IBackendProvider` implementation
   - Manages VDevice reference from Device Manager
   - Owns PRP-list page cache (two-tier: GPU L1 + host-pinned L2)
   - Provides descriptor construction and batch submission

2. **NvmeFileDeviceHandle** (`nvme/include/nvme_target_handle.h`)
   - GPU-resident file handle consumed by device kernels
   - Inline-small pattern: 8 extents inline, overflow for large files
   - Contains VDevice reference for queue access

3. **PrpPageCache** (`nvme/include/prp_page_cache.h`)
   - Two-tier cache for PRP-list pages
   - L1: GPU-resident pages (fast path)
   - L2: Host-pinned pages (fallback)
   - Amortizes allocation cost across operations

4. **NvmeCommandBuilder** (`nvme/include/nvme_command_builder.h`)
   - Host-side PRP/SGL descriptor construction
   - Converts DMA ioaddrs to NVMe PRP descriptors
   - Handles SINGLE/DUAL/LIST PRP patterns

### Device-Side Components

1. **nvme_device_helpers.cuh** (`device/nvme_device_helpers.cuh`)
   - Device-side addressing: `resolve_lba()` walks extents
   - Submission helpers: `submit_read_one()`, `submit_write_one()`
   - Calls DOWN to Device Manager's device-side queue API

2. **submit_batch_kernel.cu** (`device/submit_batch_kernel.cu`)
   - GPU batch submission kernel
   - One thread per descriptor
   - Uses DM's device-side queue mechanics for submission

## Lifecycle

### Initialization
```cpp
// 1. Backend created via BackendFactory
IBackendProvider* backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);

// 2. Initialize with VDevice from Device Manager
VDevice* vdev = device_manager->allocate_vdevice(queue_quota);
backend->initialize(vdev);

// 3. Backend stores vdev, initializes PRP cache with queue_quota sizing
//    L1 pool: queue_quota * 2 pages (GPU-resident)
//    L2 pool: queue_quota * 8 pages (host-pinned)
```

### Descriptor Preparation
```cpp
// Called by Memory Layer during tensor registration
backend->prepare_descriptors(ioaddrs, slices, n_slices, out_descs);

// Constructs PRP descriptors:
//   SINGLE: prp1 = ioaddrs[0], prp2 = 0
//   DUAL:   prp1 = ioaddrs[0], prp2 = ioaddrs[1]
//   LIST:   prp1 = ioaddrs[0], prp2 = PRP-list page from cache
```

### Target Handle Management
```cpp
// Called by Block Storage at file open
StorageTarget target = namespace_layer->resolve_file(file_id);
void* handle = backend->acquire_target_handle(target);

// Builds NvmeFileDeviceHandle:
//   1. Populate identity, namespace params, extents
//   2. Set vdev reference (not ownership transfer)
//   3. cudaMalloc + cudaMemcpy to GPU
//   4. Track in target_handles_ map

// Called at file close
backend->release_target_handle(handle);
```

### IO Submission
```cpp
// GPU path (primary production IO)
backend->launch_batch_gpu_stream(stream, handle, descs, n_descs, is_read);

// Launches submit_batch_kernel<<<>>> on stream
// Kernel uses DM's device-side queue mechanics:
//   - acquire_queue(vdev->d_qps, vdev->queue_quota)
//   - issue_nvme_cmd(qp, sqe)
//   - poll(qp, cid)

// CPU sync path (bootstrap, metadata, testing)
SubmissionResult result = backend->submit_batch_cpu_sync(handle, descs, n_descs, is_read);
```

### Cleanup
```cpp
backend->cleanup();

// Releases all target handles, frees PRP cache, clears vdev reference
// Caller returns vdev to Device Manager
```

## Dependencies

### Downward (Consuming)
- **Device Manager**: VDevice (queues + namespace view + caps) at initialize()
- **Accelerator HAL**: AccelStream, dma_map, cudaMalloc/cudaMemcpy wrappers
- **Abstraction**: TUTTI_DEVICE, TUTTI_GLOBAL, TUTTI_LAUNCH_KERNEL macros

### Upward (Consumed By)
- **IO Engine**: calls prepare_descriptors, launch_batch_gpu_stream, submit_batch_cpu_sync
- **Block Storage**: calls acquire_target_handle, release_target_handle

## Key Design Notes

1. **VDevice Once**: Backend receives VDevice once at initialize(). Steady-state IO never calls Device Manager - all queue access via device-side helpers.

2. **Queue Mechanics Ownership**: Backend does NOT own queue mechanics. It consumes DM's device-side API (acquire_queue, issue_nvme_cmd, poll) through device helpers.

3. **PRP Cache**: Two-tier design amortizes allocation cost. L1 sized by queue_quota for typical load, L2 absorbs bursts.

4. **Target Handle Reference**: NvmeFileDeviceHandle stores VDevice* reference (not ownership). Lifetime: VDevice outlives all target handles.

5. **Backend Pluggability**: IO Engine holds IBackendProvider*, never NVMe-specific types. Each transfer mode written once in IO Engine, not per backend.

## Build Configuration

```cmake
# In tutti/backends/nvme/CMakeLists.txt
add_library(tutti_backend_nvme
    src/nvme_backend.cpp
    src/nvme_target_handle.cpp
    src/nvme_submission.cpp
    src/prp_page_cache.cpp
    src/nvme_command_builder.cpp
    src/nvme_backend_registration.cpp
    device/submit_batch_kernel.cu
)

target_link_libraries(tutti_backend_nvme
    PUBLIC tutti_backend_interface
    PRIVATE tutti_device_manager nvme_storage_types
)
```

## Integration with Device Manager

The NVMe backend consumes Device Manager's device-side queue mechanics through thin wrappers in `nvme_device_helpers.cuh`:

```cuda
__device__ int submit_read_one(NvmeFileDeviceHandle* handle, ...) {
    // 1. Resolve LBA from logical offset (walk extents)
    uint64_t lba;
    resolve_lba(handle, logical_offset, lba, block_count);

    // 2. Acquire queue from VDevice (DM's device-side API)
    QueuePair* qp = acquire_queue(handle->vdev->d_qps, 
                                   handle->vdev->queue_quota, 
                                   preferred_index);

    // 3. Build NVMe READ SQE
    NvmeSqe sqe;
    build_read_sqe(&sqe, handle->namespace_id, lba, transfer_blocks, prp1, prp2);

    // 4. Submit via DM's device-side API
    int cid = issue_nvme_cmd(qp, &sqe);

    // 5. Poll completion
    poll(qp, cid);

    return cid;
}
```

## Future Enhancements

1. **SGL Support**: NVMe 1.2+ scatter-gather lists for large transfers
2. **CPU Async Submission**: Async CPU path with IOFuture (v0.3+)
3. **COOP Mode**: Cooperative kernel channel setup (v0.3+)
4. **Libnvm Integration**: Full CPU sync submission via libnvm nvm_cmd_read/write
5. **Multi-extent Commands**: Handle transfers spanning multiple extents
6. **Error Reporting**: Detailed error reporting in SubmissionResult

## Testing

Unit tests should cover:
- PRP descriptor construction (SINGLE/DUAL/LIST patterns)
- PRP cache allocation/free (L1/L2 tiers)
- Target handle lifecycle (acquire/release with inline/overflow extents)
- LBA resolution (walk extents, bounds checking)
- Backend initialization/cleanup

Integration tests should verify:
- End-to-end IO with real NVMe device via VDevice
- GPU batch submission kernel correctness
- CPU sync submission path
- Multi-file concurrent access
