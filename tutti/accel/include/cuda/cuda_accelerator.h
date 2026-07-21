#pragma once
#include "../common/iaccel.h"
#include <cuda_runtime.h>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace tutti {

// CUDA implementation of the IAccelerator interface.
// Provides GPU memory management, DMA mapping via libnvm, and stream/event lifecycle.
//
// Thread safety: All public methods are thread-safe via internal mutex protection.
class CudaAccelerator : public IAccelerator {
public:
    CudaAccelerator();
    ~CudaAccelerator() override;

    //==========================================================================
    // Identity
    //==========================================================================

    const char* vendor_name() const override { return "CUDA"; }
    int device_count() const override;
    bool set_device(int device_id) override;
    int get_device() const override;

    //==========================================================================
    // Memory Allocation
    //==========================================================================

    void* allocate_host(size_t size, MemoryKind kind) override;
    void* allocate_device(size_t size, MemoryKind kind, int device_id) override;
    void free(void* ptr, MemoryKind kind) override;

    //==========================================================================
    // MemoryRegion Registry
    //==========================================================================

    MemoryRegion* register_host(void* host_ptr, size_t size) override;
    MemoryRegion* register_device(void* device_ptr, size_t size, int device_id) override;
    MemoryRegion* register_external(
        void* host_ptr,
        void* device_ptr,
        size_t size,
        const ExternalMemorySpec& spec) override;
    void unregister(MemoryRegion* region) override;
    MemoryRegion* lookup(const void* ptr) const override;
    MemoryRegion* lookup_by_id(uint64_t region_id) const override;

    //==========================================================================
    // Host ↔ Device Pointer Translation
    //==========================================================================

    void* device_pointer_for(const void* host_ptr) override;

    //==========================================================================
    // Stream Lifecycle
    //==========================================================================

    AccelStream create_stream() override;
    void destroy_stream(AccelStream stream) override;
    void synchronize_stream(AccelStream stream) override;

    //==========================================================================
    // Event Lifecycle
    //==========================================================================

    AccelEvent create_event() override;
    void destroy_event(AccelEvent event) override;
    void record_event(AccelEvent event, AccelStream stream) override;
    void wait_event(AccelStream stream, AccelEvent event) override;
    bool query_event(AccelEvent event) override;

    //==========================================================================
    // Transfer
    //==========================================================================

    bool memcpy_async(
        void* dst,
        const void* src,
        size_t size,
        AccelStream stream) override;

    //==========================================================================
    // Kernel Launch
    //==========================================================================

    void launch(
        void* kernel_func,
        const Dim3& grid,
        const Dim3& block,
        size_t shared_mem_bytes,
        AccelStream stream,
        void** kernel_args) override;

    //==========================================================================
    // IPC
    //==========================================================================

    bool ipc_export(MemoryRegion* region, IpcHandle* out_handle) override;
    MemoryRegion* ipc_import(const IpcHandle& handle, int device_id) override;

private:
    // Per-region registry entry
    struct RegionEntry {
        MemoryRegion region;
    };

    // Registry state
    mutable std::mutex registry_mutex_;
    std::unordered_map<uint64_t, RegionEntry> regions_by_id_;
    std::unordered_map<uintptr_t, uint64_t> ptr_to_region_id_;  // ptr -> region_id
    uint64_t next_region_id_;

    // Device allocation tracking: aligned ptr -> underlying cudaMalloc ptr.
    // allocate_device(DEVICE) over-allocates and returns a 64KB-aligned
    // sub-pointer; cudaFree() requires the exact pointer cudaMalloc returned,
    // so free() must recover it from here.
    mutable std::mutex alloc_mutex_;
    std::unordered_map<uintptr_t, void*> aligned_to_raw_;

    // Helper methods
    uint64_t allocate_region_id();
    void add_ptr_mapping(void* ptr, uint64_t region_id);
    void remove_ptr_mappings(const MemoryRegion& region);
};

} // namespace tutti
