// tutti/io_engine/tests/layer4_smoke_test.cpp
// Layer 4 Smoke Test: Verify IO Engine interfaces and basic operations

#include <iostream>
#include <vector>
#include <cstring>
#include "../src/io_engine_impl.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "accel/include/common/memory_region.h"

using namespace tutti;

//==============================================================================
// Mock Backend Provider for Testing
//==============================================================================
class MockBackendProvider : public backends::IBackendProvider {
public:
    backends::BackendMetadata metadata() const override {
        backends::BackendMetadata meta;
        meta.max_batch_size = 16;
        meta.max_io_size = 4096;
        return meta;
    }

    size_t max_io_size() const override {
        return 4096;
    }

    bool prepare_descriptors(
        const uint64_t* ioaddrs,
        const backends::SubSliceInfo* slices,
        uint32_t n_slices,
        backends::BufferDescriptor* out_descs) override {
        // Mock descriptor preparation
        for (uint32_t i = 0; i < n_slices; ++i) {
            out_descs[i].device_addr = ioaddrs[0] + slices[i].offset_bytes;
            out_descs[i].byte_length = slices[i].length_bytes;
        }
        return true;
    }

    void release_descriptors(
        const backends::BufferDescriptor* descs,
        uint32_t n_descs) override {
        // Mock release - no-op
    }

    void launch_batch_gpu_stream(
        void* stream,
        void* target_handle,
        const backends::BufferDescriptor* descs,
        uint32_t n_descs,
        bool is_read) override {
        // Mock kernel launch - no-op
        std::cout << "  [Mock] Launched batch with " << n_descs << " descriptors, is_read="
                  << is_read << std::endl;
    }

    // Other required methods (not used in smoke test)
    bool submit_batch_cpu_sync(void*, const backends::BufferDescriptor*, uint32_t, bool) override { return true; }
    bool submit_batch_cpu_async(void*, const backends::BufferDescriptor*, uint32_t, bool) override { return true; }
    bool setup_coop_channel(void*, void*, uint32_t) override { return true; }
    void* acquire_target_handle() override { return reinterpret_cast<void*>(0x1000); }
    void release_target_handle(void*) override {}
};

//==============================================================================
// Mock Accelerator for Testing
//==============================================================================
class MockAccelerator : public IAccelerator {
public:
    MockAccelerator() : device_buffer_(nullptr), event_completed_(false) {}

    const char* vendor_name() const override { return "MockGPU"; }
    int device_count() const override { return 1; }
    bool set_device(int) override { return true; }
    int get_device() const override { return 0; }

    void* allocate_device(size_t size, MemoryKind, int) override {
        device_buffer_ = malloc(size);
        return device_buffer_;
    }

    void* allocate_host(size_t size, MemoryKind) override {
        return malloc(size);
    }

    void free(void* ptr, MemoryKind) override {
        if (ptr == device_buffer_) {
            ::free(device_buffer_);
            device_buffer_ = nullptr;
        } else {
            ::free(ptr);
        }
    }

    bool memcpy_async(void* dst, const void* src, size_t size, AccelStream) override {
        memcpy(dst, src, size);
        return true;
    }

    AccelStream create_stream() override {
        AccelStream s;
        s.handle = reinterpret_cast<void*>(0x2000);
        return s;
    }

    void destroy_stream(AccelStream) override {}

    void synchronize_stream(AccelStream) override {
        std::cout << "  [Mock] Stream synchronized" << std::endl;
    }

    AccelEvent create_event() override {
        AccelEvent e;
        e.handle = reinterpret_cast<void*>(0x3000);
        return e;
    }

    void destroy_event(AccelEvent) override {}

    void record_event(AccelEvent, AccelStream) override {
        event_completed_ = true;
    }

    bool query_event(AccelEvent) override {
        return event_completed_;
    }

    void wait_event(AccelStream, AccelEvent) override {}

    MemoryRegion* register_host(void* host_ptr, size_t size) override {
        auto* region = new MemoryRegion();
        region->host_ptr = host_ptr;
        region->device_ptr = nullptr;
        region->size = size;
        region->backend_private = ioaddrs_;  // Mock ioaddrs
        return region;
    }

    MemoryRegion* register_device(void*, size_t, int) override { return nullptr; }
    MemoryRegion* register_external(void*, void*, size_t, const ExternalMemorySpec&) override { return nullptr; }

    void unregister(MemoryRegion* region) override {
        delete region;
    }

    MemoryRegion* lookup(const void*) const override { return nullptr; }
    MemoryRegion* lookup_by_id(uint64_t) const override { return nullptr; }
    void* device_pointer_for(const void*) override { return nullptr; }

    void launch(void*, const Dim3&, const Dim3&, size_t, AccelStream, void**) override {}

    bool ipc_export(MemoryRegion*, IpcHandle*) override { return false; }
    MemoryRegion* ipc_import(const IpcHandle&, int) override { return nullptr; }

private:
    void* device_buffer_;
    bool event_completed_;
    uint64_t ioaddrs_[16] = {0x100000, 0x101000, 0x102000, 0x103000};
};

//==============================================================================
// Test Cases
//==============================================================================

bool test_construction() {
    std::cout << "\n[TEST] IoEngineImpl Construction" << std::endl;

    MockBackendProvider backend;
    MockAccelerator accel;

    try {
        IoEngineImpl engine(&backend, &accel);
        std::cout << "  ✓ Engine constructed successfully" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cout << "  ✗ Construction failed: " << e.what() << std::endl;
        return false;
    }
}

bool test_null_backend() {
    std::cout << "\n[TEST] Null Backend Rejection" << std::endl;

    MockAccelerator accel;

    try {
        IoEngineImpl engine(nullptr, &accel);
        std::cout << "  ✗ Should have thrown on null backend" << std::endl;
        return false;
    } catch (const std::invalid_argument& e) {
        std::cout << "  ✓ Correctly rejected null backend: " << e.what() << std::endl;
        return true;
    }
}

bool test_capacity_queries() {
    std::cout << "\n[TEST] Capacity Query Methods" << std::endl;

    MockBackendProvider backend;
    MockAccelerator accel;
    IoEngineImpl engine(&backend, &accel);

    uint32_t max_entries = engine.max_entries_per_batch();
    std::cout << "  max_entries_per_batch() = " << max_entries << std::endl;

    if (max_entries != 16) {
        std::cout << "  ✗ Expected 16, got " << max_entries << std::endl;
        return false;
    }

    // Test slice_fanout
    char buffer[8192];
    MemoryRegion* region = accel.register_host(buffer, 8192);
    uint32_t fanout = engine.slice_fanout(region);
    std::cout << "  slice_fanout(8192 bytes, 4096 max_io) = " << fanout << std::endl;

    if (fanout != 2) {
        std::cout << "  ✗ Expected 2, got " << fanout << std::endl;
        accel.unregister(region);
        return false;
    }

    accel.unregister(region);
    std::cout << "  ✓ Capacity queries work correctly" << std::endl;
    return true;
}

bool test_submit_batch_blocking() {
    std::cout << "\n[TEST] submit_batch (Blocking)" << std::endl;

    MockBackendProvider backend;
    MockAccelerator accel;
    IoEngineImpl engine(&backend, &accel);

    char buffer[8192];
    MemoryRegion* region = accel.register_host(buffer, 8192);
    void* target = backend.acquire_target_handle();
    AccelStream stream = accel.create_stream();

    IoRequest req;
    req.region = region;
    req.target_handle = target;
    req.byte_offset = 0;
    req.byte_length = 4096;

    std::vector<IoRequest> requests = {req};

    bool result = engine.submit_batch(requests, true, stream);

    accel.destroy_stream(stream);
    backend.release_target_handle(target);
    accel.unregister(region);

    if (result) {
        std::cout << "  ✓ Blocking batch submission succeeded" << std::endl;
        return true;
    } else {
        std::cout << "  ✗ Blocking batch submission failed" << std::endl;
        return false;
    }
}

bool test_submit_batch_async() {
    std::cout << "\n[TEST] submit_batch_async (Async)" << std::endl;

    MockBackendProvider backend;
    MockAccelerator accel;
    IoEngineImpl engine(&backend, &accel);

    char buffer[8192];
    MemoryRegion* region = accel.register_host(buffer, 8192);
    void* target = backend.acquire_target_handle();
    AccelStream stream = accel.create_stream();

    IoRequest req;
    req.region = region;
    req.target_handle = target;
    req.byte_offset = 0;
    req.byte_length = 4096;

    std::vector<IoRequest> requests = {req};

    bool result = engine.submit_batch_async(requests, false, stream);

    // Simulate async completion check
    accel.synchronize_stream(stream);

    accel.destroy_stream(stream);
    backend.release_target_handle(target);
    accel.unregister(region);

    if (result) {
        std::cout << "  ✓ Async batch submission succeeded" << std::endl;
        return true;
    } else {
        std::cout << "  ✗ Async batch submission failed" << std::endl;
        return false;
    }
}

bool test_multi_region_batch() {
    std::cout << "\n[TEST] Multi-Region Batch Support" << std::endl;

    MockBackendProvider backend;
    MockAccelerator accel;
    IoEngineImpl engine(&backend, &accel);

    char buffer1[4096];
    char buffer2[4096];
    MemoryRegion* region1 = accel.register_host(buffer1, 4096);
    MemoryRegion* region2 = accel.register_host(buffer2, 4096);
    void* target = backend.acquire_target_handle();
    AccelStream stream = accel.create_stream();

    IoRequest req1;
    req1.region = region1;
    req1.target_handle = target;
    req1.byte_offset = 0;
    req1.byte_length = 2048;

    IoRequest req2;
    req2.region = region2;
    req2.target_handle = target;
    req2.byte_offset = 0;
    req2.byte_length = 2048;

    std::vector<IoRequest> requests = {req1, req2};

    bool result = engine.submit_batch(requests, true, stream);

    accel.destroy_stream(stream);
    backend.release_target_handle(target);
    accel.unregister(region1);
    accel.unregister(region2);

    if (result) {
        std::cout << "  ✓ Multi-region batch succeeded" << std::endl;
        return true;
    } else {
        std::cout << "  ✗ Multi-region batch failed" << std::endl;
        return false;
    }
}

//==============================================================================
// Main
//==============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Layer 4 IO Engine Smoke Test" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int total = 6;

    if (test_construction()) passed++;
    if (test_null_backend()) passed++;
    if (test_capacity_queries()) passed++;
    if (test_submit_batch_blocking()) passed++;
    if (test_submit_batch_async()) passed++;
    if (test_multi_region_batch()) passed++;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << "/" << total << " tests passed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
