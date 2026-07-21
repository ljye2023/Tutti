#pragma once
#include <cstdint>
#include <cstddef>
#include "accel_types.h"
#include "memory_kind.h"
#include "memory_region.h"

namespace tutti {

// Accelerator Hardware Abstraction Layer Interface
// Vendor-neutral interface for GPU/accelerator operations
class IAccelerator {
public:
    virtual ~IAccelerator() = default;

    //==========================================================================
    // Identity
    //==========================================================================

    virtual const char* vendor_name() const = 0;
    virtual int device_count() const = 0;
    virtual bool set_device(int device_id) = 0;
    virtual int get_device() const = 0;

    //==========================================================================
    // Memory Allocation
    //==========================================================================

    virtual void* allocate_host(size_t size, MemoryKind kind) = 0;
    virtual void* allocate_device(size_t size, MemoryKind kind, int device_id) = 0;
    virtual void free(void* ptr, MemoryKind kind) = 0;

    //==========================================================================
    // MemoryRegion Registry
    //==========================================================================

    virtual MemoryRegion* register_host(void* host_ptr, size_t size) = 0;
    virtual MemoryRegion* register_device(void* device_ptr, size_t size, int device_id) = 0;
    virtual MemoryRegion* register_external(
        void* host_ptr,
        void* device_ptr,
        size_t size,
        const ExternalMemorySpec& spec) = 0;
    virtual void unregister(MemoryRegion* region) = 0;
    virtual MemoryRegion* lookup(const void* ptr) const = 0;
    virtual MemoryRegion* lookup_by_id(uint64_t region_id) const = 0;

    //==========================================================================
    // Host ↔ Device Pointer Translation
    //==========================================================================

    virtual void* device_pointer_for(const void* host_ptr) = 0;

    //==========================================================================
    // Stream Lifecycle
    //==========================================================================

    virtual AccelStream create_stream() = 0;
    virtual void destroy_stream(AccelStream stream) = 0;
    virtual void synchronize_stream(AccelStream stream) = 0;

    //==========================================================================
    // Event Lifecycle
    //==========================================================================

    virtual AccelEvent create_event() = 0;
    virtual void destroy_event(AccelEvent event) = 0;
    virtual void record_event(AccelEvent event, AccelStream stream) = 0;
    virtual void wait_event(AccelStream stream, AccelEvent event) = 0;
    virtual bool query_event(AccelEvent event) = 0;

    //==========================================================================
    // Transfer
    //==========================================================================

    virtual bool memcpy_async(
        void* dst,
        const void* src,
        size_t size,
        AccelStream stream) = 0;

    //==========================================================================
    // Kernel Launch
    //==========================================================================

    virtual void launch(
        void* kernel_func,
        const Dim3& grid,
        const Dim3& block,
        size_t shared_mem_bytes,
        AccelStream stream,
        void** kernel_args) = 0;

    //==========================================================================
    // IPC
    //==========================================================================

    virtual bool ipc_export(MemoryRegion* region, IpcHandle* out_handle) = 0;
    virtual MemoryRegion* ipc_import(const IpcHandle& handle, int device_id) = 0;
};

} // namespace tutti
