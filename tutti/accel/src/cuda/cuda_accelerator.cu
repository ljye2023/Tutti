#include "../../include/cuda/cuda_accelerator.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>

namespace tutti {

namespace {

// Check CUDA error and log
inline bool check_cuda(cudaError_t err, const char* context) {
    if (err != cudaSuccess) {
        fprintf(stderr, "[CudaAccelerator] %s failed: %s\n",
                context, cudaGetErrorString(err));
        return false;
    }
    return true;
}

// Convert tutti::Dim3 to CUDA dim3
inline ::dim3 to_cuda_dim3(const tutti::Dim3& d) {
    return ::dim3(d.x, d.y, d.z);
}

} // anonymous namespace

//=============================================================================
// Construction / Destruction
//=============================================================================

CudaAccelerator::CudaAccelerator()
    : next_region_id_(1)
{
}

CudaAccelerator::~CudaAccelerator() {
    std::lock_guard<std::mutex> lock(registry_mutex_);

    regions_by_id_.clear();
    ptr_to_region_id_.clear();
}

//=============================================================================
// Identity
//=============================================================================

int CudaAccelerator::device_count() const {
    int count = 0;
    cudaGetDeviceCount(&count);
    return count;
}

bool CudaAccelerator::set_device(int device_id) {
    return check_cuda(cudaSetDevice(device_id), "cudaSetDevice");
}

int CudaAccelerator::get_device() const {
    int device = 0;
    cudaGetDevice(&device);
    return device;
}

//=============================================================================
// Memory Allocation
//=============================================================================

void* CudaAccelerator::allocate_host(size_t size, MemoryKind kind) {
    void* ptr = nullptr;

    if (kind == MemoryKind::HOST) {
        ptr = malloc(size);
    } else if (kind == MemoryKind::PINNED_HOST) {
        if (!check_cuda(cudaHostAlloc(&ptr, size, cudaHostAllocDefault),
                       "cudaHostAlloc")) {
            return nullptr;
        }
    } else {
        fprintf(stderr, "[CudaAccelerator] allocate_host: invalid kind %u\n",
                (unsigned)kind);
        return nullptr;
    }

    return ptr;
}

void* CudaAccelerator::allocate_device(size_t size, MemoryKind kind, int device_id) {
    void* ptr = nullptr;

    // Save current device
    int prev_device = get_device();
    if (!set_device(device_id)) {
        return nullptr;
    }

    if (kind == MemoryKind::DEVICE) {
        // Over-allocate for 64KB alignment (preferred by nvm_dma_map_data_device)
        constexpr size_t kAlign = 65536;
        void* raw = nullptr;
        if (!check_cuda(cudaMalloc(&raw, size + kAlign), "cudaMalloc")) {
            set_device(prev_device);
            return nullptr;
        }
        uintptr_t aligned_addr = ((uintptr_t)raw + kAlign - 1) & ~(uintptr_t)(kAlign - 1);
        ptr = (void*)aligned_addr;

        // cudaFree() requires the exact pointer cudaMalloc returned, so we
        // must remember raw to free it later even though we hand out the
        // aligned sub-pointer.
        {
            std::lock_guard<std::mutex> lock(alloc_mutex_);
            aligned_to_raw_[aligned_addr] = raw;
        }
    } else if (kind == MemoryKind::MANAGED) {
        if (!check_cuda(cudaMallocManaged(&ptr, size), "cudaMallocManaged")) {
            set_device(prev_device);
            return nullptr;
        }
    } else {
        fprintf(stderr, "[CudaAccelerator] allocate_device: invalid kind %u\n",
                (unsigned)kind);
        set_device(prev_device);
        return nullptr;
    }

    // Restore previous device
    set_device(prev_device);
    return ptr;
}

void CudaAccelerator::free(void* ptr, MemoryKind kind) {
    if (ptr == nullptr) return;

    if (kind == MemoryKind::HOST) {
        ::free(ptr);
    } else if (kind == MemoryKind::PINNED_HOST) {
        cudaFreeHost(ptr);
    } else if (kind == MemoryKind::DEVICE) {
        // allocate_device(DEVICE) hands out a 64KB-aligned sub-pointer of a
        // larger cudaMalloc'd block; recover and free the original pointer.
        void* raw = ptr;
        {
            std::lock_guard<std::mutex> lock(alloc_mutex_);
            auto it = aligned_to_raw_.find((uintptr_t)ptr);
            if (it != aligned_to_raw_.end()) {
                raw = it->second;
                aligned_to_raw_.erase(it);
            }
        }
        cudaFree(raw);
    } else if (kind == MemoryKind::MANAGED) {
        cudaFree(ptr);
    } else {
        fprintf(stderr, "[CudaAccelerator] free: invalid kind %u\n", (unsigned)kind);
    }
}

//=============================================================================
// MemoryRegion Registry
//=============================================================================

uint64_t CudaAccelerator::allocate_region_id() {
    return next_region_id_++;
}

void CudaAccelerator::add_ptr_mapping(void* ptr, uint64_t region_id) {
    if (ptr != nullptr) {
        ptr_to_region_id_[(uintptr_t)ptr] = region_id;
    }
}

void CudaAccelerator::remove_ptr_mappings(const MemoryRegion& region) {
    if (region.host_ptr != nullptr) {
        ptr_to_region_id_.erase((uintptr_t)region.host_ptr);
    }
    if (region.device_ptr != nullptr && region.device_ptr != region.host_ptr) {
        ptr_to_region_id_.erase((uintptr_t)region.device_ptr);
    }
}

MemoryRegion* CudaAccelerator::register_host(void* host_ptr, size_t size) {
    if (host_ptr == nullptr || size == 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(registry_mutex_);

    uint64_t region_id = allocate_region_id();
    RegionEntry entry;
    entry.region.region_id = region_id;
    entry.region.kind = MemoryKind::HOST;
    entry.region.cuda_device = -1;
    entry.region.host_ptr = host_ptr;
    entry.region.device_ptr = nullptr;
    entry.region.size = size;
    entry.region.external = nullptr;
    entry.region.backend_private = nullptr;

    add_ptr_mapping(host_ptr, region_id);
    regions_by_id_[region_id] = std::move(entry);

    return &regions_by_id_[region_id].region;
}

MemoryRegion* CudaAccelerator::register_device(void* device_ptr, size_t size, int device_id) {
    if (device_ptr == nullptr || size == 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(registry_mutex_);

    uint64_t region_id = allocate_region_id();
    RegionEntry entry;
    entry.region.region_id = region_id;
    entry.region.kind = MemoryKind::DEVICE;
    entry.region.cuda_device = device_id;
    entry.region.host_ptr = nullptr;
    entry.region.device_ptr = device_ptr;
    entry.region.size = size;
    entry.region.external = nullptr;
    entry.region.backend_private = nullptr;

    add_ptr_mapping(device_ptr, region_id);
    regions_by_id_[region_id] = std::move(entry);

    return &regions_by_id_[region_id].region;
}

MemoryRegion* CudaAccelerator::register_external(
    void* host_ptr,
    void* device_ptr,
    size_t size,
    const ExternalMemorySpec& spec)
{
    if (size == 0 || (host_ptr == nullptr && device_ptr == nullptr)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(registry_mutex_);

    uint64_t region_id = allocate_region_id();
    RegionEntry entry;
    entry.region.region_id = region_id;
    entry.region.kind = MemoryKind::EXTERNAL;
    entry.region.cuda_device = (device_ptr != nullptr) ? get_device() : -1;
    entry.region.host_ptr = host_ptr;
    entry.region.device_ptr = device_ptr;
    entry.region.size = size;

    // Store external spec
    entry.region.external = new ExternalMemorySpec(spec);
    entry.region.backend_private = nullptr;

    add_ptr_mapping(host_ptr, region_id);
    add_ptr_mapping(device_ptr, region_id);
    regions_by_id_[region_id] = std::move(entry);

    return &regions_by_id_[region_id].region;
}

void CudaAccelerator::unregister(MemoryRegion* region) {
    if (region == nullptr) return;

    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = regions_by_id_.find(region->region_id);
    if (it == regions_by_id_.end()) {
        return;
    }

    RegionEntry& entry = it->second;

    // Clean up external spec if present
    if (entry.region.external != nullptr) {
        delete entry.region.external;
    }

    // Remove pointer mappings
    remove_ptr_mappings(entry.region);

    // Remove from registry
    regions_by_id_.erase(it);
}

MemoryRegion* CudaAccelerator::lookup(const void* ptr) const {
    if (ptr == nullptr) return nullptr;

    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = ptr_to_region_id_.find((uintptr_t)ptr);
    if (it == ptr_to_region_id_.end()) {
        return nullptr;
    }

    auto region_it = regions_by_id_.find(it->second);
    if (region_it == regions_by_id_.end()) {
        return nullptr;
    }

    return const_cast<MemoryRegion*>(&region_it->second.region);
}

MemoryRegion* CudaAccelerator::lookup_by_id(uint64_t region_id) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = regions_by_id_.find(region_id);
    if (it == regions_by_id_.end()) {
        return nullptr;
    }

    return const_cast<MemoryRegion*>(&it->second.region);
}

//=============================================================================
// Host ↔ Device Pointer Translation
//=============================================================================

void* CudaAccelerator::device_pointer_for(const void* host_ptr) {
    if (host_ptr == nullptr) return nullptr;

    void* device_ptr = nullptr;
    cudaError_t err = cudaHostGetDevicePointer(&device_ptr, const_cast<void*>(host_ptr), 0);

    if (err != cudaSuccess) {
        return nullptr;
    }

    return device_ptr;
}

//=============================================================================
// Stream Lifecycle
//=============================================================================

AccelStream CudaAccelerator::create_stream() {
    cudaStream_t stream = nullptr;
    if (!check_cuda(cudaStreamCreate(&stream), "cudaStreamCreate")) {
        return AccelStream(nullptr);
    }
    return AccelStream(stream);
}

void CudaAccelerator::destroy_stream(AccelStream stream) {
    if (stream.handle != nullptr) {
        cudaStreamDestroy(static_cast<cudaStream_t>(stream.handle));
    }
}

void CudaAccelerator::synchronize_stream(AccelStream stream) {
    if (stream.handle != nullptr) {
        cudaStreamSynchronize(static_cast<cudaStream_t>(stream.handle));
    }
}

//=============================================================================
// Event Lifecycle
//=============================================================================

AccelEvent CudaAccelerator::create_event() {
    cudaEvent_t event = nullptr;
    if (!check_cuda(cudaEventCreate(&event), "cudaEventCreate")) {
        return AccelEvent(nullptr);
    }
    return AccelEvent(event);
}

void CudaAccelerator::destroy_event(AccelEvent event) {
    if (event.handle != nullptr) {
        cudaEventDestroy(static_cast<cudaEvent_t>(event.handle));
    }
}

void CudaAccelerator::record_event(AccelEvent event, AccelStream stream) {
    if (event.handle != nullptr) {
        cudaEventRecord(static_cast<cudaEvent_t>(event.handle),
                       static_cast<cudaStream_t>(stream.handle));
    }
}

void CudaAccelerator::wait_event(AccelStream stream, AccelEvent event) {
    if (stream.handle != nullptr && event.handle != nullptr) {
        cudaStreamWaitEvent(static_cast<cudaStream_t>(stream.handle),
                           static_cast<cudaEvent_t>(event.handle), 0);
    }
}

bool CudaAccelerator::query_event(AccelEvent event) {
    if (event.handle == nullptr) return true;

    cudaError_t err = cudaEventQuery(static_cast<cudaEvent_t>(event.handle));
    return (err == cudaSuccess);
}

//=============================================================================
// Transfer
//=============================================================================

bool CudaAccelerator::memcpy_async(
    void* dst,
    const void* src,
    size_t size,
    AccelStream stream)
{
    cudaError_t err = cudaMemcpyAsync(
        dst, src, size, cudaMemcpyDefault,
        static_cast<cudaStream_t>(stream.handle));

    return check_cuda(err, "cudaMemcpyAsync");
}

//=============================================================================
// Kernel Launch
//=============================================================================

void CudaAccelerator::launch(
    void* kernel_func,
    const Dim3& grid,
    const Dim3& block,
    size_t shared_mem_bytes,
    AccelStream stream,
    void** kernel_args)
{
    ::dim3 cuda_grid = to_cuda_dim3(grid);
    ::dim3 cuda_block = to_cuda_dim3(block);
    cudaStream_t cuda_stream = static_cast<cudaStream_t>(stream.handle);

    cudaError_t err = cudaLaunchKernel(
        kernel_func,
        cuda_grid,
        cuda_block,
        kernel_args,
        shared_mem_bytes,
        cuda_stream);

    check_cuda(err, "cudaLaunchKernel");
}

//=============================================================================
// IPC
//=============================================================================

bool CudaAccelerator::ipc_export(MemoryRegion* region, IpcHandle* out_handle) {
    if (region == nullptr || out_handle == nullptr || region->device_ptr == nullptr) {
        return false;
    }

    cudaIpcMemHandle_t cuda_handle;
    cudaError_t err = cudaIpcGetMemHandle(&cuda_handle, region->device_ptr);

    if (err != cudaSuccess) {
        fprintf(stderr, "[CudaAccelerator] cudaIpcGetMemHandle failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }

    // Copy CUDA IPC handle to our IpcHandle
    static_assert(sizeof(cudaIpcMemHandle_t) <= IpcHandle::MAX_HANDLE_SIZE,
                  "IpcHandle too small for cudaIpcMemHandle_t");
    memcpy(out_handle->data, &cuda_handle, sizeof(cudaIpcMemHandle_t));

    return true;
}

MemoryRegion* CudaAccelerator::ipc_import(const IpcHandle& handle, int device_id) {
    // Save current device
    int prev_device = get_device();
    if (!set_device(device_id)) {
        return nullptr;
    }

    cudaIpcMemHandle_t cuda_handle;
    memcpy(&cuda_handle, handle.data, sizeof(cudaIpcMemHandle_t));

    void* device_ptr = nullptr;
    cudaError_t err = cudaIpcOpenMemHandle(&device_ptr, cuda_handle,
                                          cudaIpcMemLazyEnablePeerAccess);

    if (err != cudaSuccess) {
        fprintf(stderr, "[CudaAccelerator] cudaIpcOpenMemHandle failed: %s\n",
                cudaGetErrorString(err));
        set_device(prev_device);
        return nullptr;
    }

    // Restore previous device
    set_device(prev_device);

    // Register as external memory with CUDA IPC source
    ExternalMemorySpec spec;
    spec.source = ExternalMemorySpec::Source::CUDA_IPC;
    memcpy(spec.cuda_ipc.handle, handle.data, sizeof(spec.cuda_ipc.handle));

    // Note: We don't know the size from the handle alone
    // This is a limitation - caller should provide size
    // For now, we'll register with size 0 (caller must manage)
    return register_external(nullptr, device_ptr, 0, spec);
}

} // namespace tutti
