#include "coordinator.h"
#include "coordinator_types.h"
#include "raw_device.h"
#include "../src/coordinator_impl.h"
#include "../src/buffer_registry.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "block_storage/include/block_storage.h"
#include "io_engine/include/io_engine.h"
#include <iostream>
#include <cstring>
#include <vector>

using namespace tutti;
using namespace tutti::coordinator;

// Mock implementations for testing
class MockBackendProvider : public backends::IBackendProvider {
public:
    bool initialize(VDevice*) override { return true; }
    void cleanup() override {}

    backends::BackendType backend_type() const override { return backends::BackendType::LOCAL_NVME; }
    const char* backend_name() const override { return "MockNVMe"; }
    size_t max_io_size() const override { return 131072; }
    backends::BackendMetadata metadata() const override {
        backends::BackendMetadata m{}; m.max_io_size = 131072; return m;
    }

    bool prepare_descriptors(const uint64_t*, const backends::SubSliceInfo*, uint32_t, backends::BufferDescriptor*) override { return true; }
    void release_descriptors(backends::BufferDescriptor*, uint32_t) override {}

    void* acquire_target_handle(const backends::StorageTarget&) override {
        return reinterpret_cast<void*>(0x1000);
    }
    void release_target_handle(void*) override {}

    void launch_batch_gpu_stream(void*, void*, const backends::BufferDescriptor*, uint32_t, bool) override {}

    backends::SubmissionResult submit_batch_cpu_sync(void*, const backends::BufferDescriptor*, uint32_t, bool) override {
        backends::SubmissionResult r{}; r.success = true; return r;
    }
    bool submit_batch_cpu_async(backends::IOFuture*, void*, const backends::BufferDescriptor*, uint32_t, bool) override { return true; }
    bool setup_coop_channel(const backends::CoopChannelConfig&, void*) override { return true; }
    bool poll_future(const backends::IOFuture&, backends::SubmissionResult*) override { return true; }
    bool wait_future(const backends::IOFuture&, uint32_t, backends::SubmissionResult*) override { return true; }
};

class MockAccelerator : public IAccelerator {
public:
    const char* vendor_name() const override { return "MockGPU"; }
    int device_count() const override { return 1; }
    bool set_device(int) override { return true; }
    int get_device() const override { return 0; }

    void* allocate_host(size_t size, MemoryKind) override { return malloc(size); }
    void* allocate_device(size_t size, MemoryKind, int) override { return malloc(size); }
    void free(void* ptr, MemoryKind) override { ::free(ptr); }

    MemoryRegion* register_host(void* host_ptr, size_t size) override {
        auto* r = new MemoryRegion{};
        r->host_ptr   = host_ptr;
        r->size       = size;
        r->region_id  = next_id_++;
        r->kind       = MemoryKind::HOST;
        return r;
    }
    MemoryRegion* register_device(void* dev_ptr, size_t size, int) override {
        auto* r = new MemoryRegion{};
        r->device_ptr = dev_ptr;
        r->size       = size;
        r->region_id  = next_id_++;
        r->kind       = MemoryKind::DEVICE;
        return r;
    }
    MemoryRegion* register_external(void*, void*, size_t, const ExternalMemorySpec&) override { return nullptr; }
    void unregister(MemoryRegion* r) override { delete r; }
    MemoryRegion* lookup(const void*) const override { return nullptr; }
    MemoryRegion* lookup_by_id(uint64_t) const override { return nullptr; }
    void* device_pointer_for(const void*) override { return nullptr; }

    AccelStream create_stream() override { return AccelStream(reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000 + stream_counter_++))); }
    void destroy_stream(AccelStream) override {}
    void synchronize_stream(AccelStream) override {}

    AccelEvent create_event() override { return AccelEvent(reinterpret_cast<void*>(0x3000)); }
    void destroy_event(AccelEvent) override {}
    void record_event(AccelEvent, AccelStream) override {}
    void wait_event(AccelStream, AccelEvent) override {}
    bool query_event(AccelEvent) override { return true; }

    bool memcpy_async(void* dst, const void* src, size_t size, AccelStream) override {
        memcpy(dst, src, size); return true;
    }
    void launch(void*, const Dim3&, const Dim3&, size_t, AccelStream, void**) override {}
    bool ipc_export(MemoryRegion*, IpcHandle*) override { return false; }
    MemoryRegion* ipc_import(const IpcHandle&, int) override { return nullptr; }

private:
    uint64_t next_id_      = 1;
    int      stream_counter_ = 0;
};

class MockBlockStorage : public block_storage::IBlockStorage {
public:
    bool initialize(const block_storage::BlockStorageConfig&,
                    backends::IBackendProvider*, IAccelerator*) override { return true; }
    void cleanup() override {}

    block_storage::GpuFileHandle* open_gpu_file(
        const std::string&, block_storage::FileOpenMode,
        uint64_t = 0, uint64_t = 0) override { return nullptr; }
    bool close_gpu_file(block_storage::GpuFileHandle*) override { return true; }
    bool delete_gpu_file(const std::string&) override { return true; }

    std::vector<block_storage::GpuFileHandle*> open_gpu_files_batch(
        const std::vector<std::string>&,
        const std::vector<block_storage::FileOpenMode>&,
        size_t /*count*/) override { return {}; }

    std::vector<block_storage::FileInfo> list_gpu_file_names() override { return {}; }

    backends::StorageTarget acquire_device_handle(block_storage::GpuFileHandle*, size_t) override {
        return backends::StorageTarget{};
    }
    bool release_device_handle(block_storage::GpuFileHandle*, size_t) override { return true; }
    bool sync_file(block_storage::GpuFileHandle*, void*) override { return true; }
    bool flush_metadata() override { return true; }
};

class MockIoEngine : public IIoEngine {
public:
    bool submit_batch(
        const std::vector<tutti::IoRequest>&, bool, AccelStream) override {
        return true;
    }

    bool submit_batch_async(
        const std::vector<tutti::IoRequest>&, bool, AccelStream) override {
        return true;
    }

    uint32_t max_entries_per_batch() const override {
        return 128;
    }

    uint32_t slice_fanout(const MemoryRegion*) const override {
        return 1;
    }
};

// Test functions
bool test_coordinator_construction() {
    std::cout << "Test: Coordinator construction... ";

    ICoordinator* coordinator = create_coordinator();
    if (!coordinator) {
        std::cout << "FAILED (null coordinator)" << std::endl;
        return false;
    }

    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_coordinator_initialization() {
    std::cout << "Test: Coordinator initialization... ";

    MockBackendProvider backend;
    MockAccelerator accel;
    MockBlockStorage storage;
    MockIoEngine io_engine;

    CoordinatorConfig config;
    config.backend_provider = &backend;
    config.accelerator = &accel;
    config.block_storage = &storage;
    config.io_engine = &io_engine;
    config.max_batch_size = 128;

    ICoordinator* coordinator = create_coordinator();
    bool init_result = coordinator->initialize(config);

    if (!init_result) {
        std::cout << "FAILED (init failed)" << std::endl;
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_buffer_registration() {
    std::cout << "Test: Buffer registration... ";

    MockBackendProvider backend;
    MockAccelerator accel;
    MockBlockStorage storage;
    MockIoEngine io_engine;

    CoordinatorConfig config;
    config.backend_provider = &backend;
    config.accelerator = &accel;
    config.block_storage = &storage;
    config.io_engine = &io_engine;

    ICoordinator* coordinator = create_coordinator();
    coordinator->initialize(config);

    char buffer[4096];
    MemoryRegion* region = coordinator->register_buffer(
        buffer, sizeof(buffer), MemoryKind::HOST);

    if (!region) {
        std::cout << "FAILED (registration failed)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    bool unregister_result = coordinator->unregister_buffer(region);
    if (!unregister_result) {
        std::cout << "FAILED (unregistration failed)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_buffer_registry() {
    std::cout << "Test: Buffer registry operations... ";

    BufferRegistry registry;

    char buffer[4096];
    MemoryRegion region{};
    region.host_ptr  = buffer;
    region.size      = sizeof(buffer);
    region.region_id = 1;

    bool add_result = registry.add_region(&region);
    if (!add_result) {
        std::cout << "FAILED (add failed)" << std::endl;
        return false;
    }

    MemoryRegion* found = registry.lookup_by_ptr(buffer);
    if (!found || found != &region) {
        std::cout << "FAILED (lookup failed)" << std::endl;
        return false;
    }

    bool remove_result = registry.remove_region(&region);
    if (!remove_result) {
        std::cout << "FAILED (remove failed)" << std::endl;
        return false;
    }

    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_max_batch_size_query() {
    std::cout << "Test: Max batch size query... ";

    MockBackendProvider backend;
    MockAccelerator accel;
    MockBlockStorage storage;
    MockIoEngine io_engine;

    CoordinatorConfig config;
    config.backend_provider = &backend;
    config.accelerator = &accel;
    config.block_storage = &storage;
    config.io_engine = &io_engine;
    config.max_batch_size = 128;

    ICoordinator* coordinator = create_coordinator();
    coordinator->initialize(config);

    uint32_t max_size = coordinator->max_batch_size();
    if (max_size != 128) {
        std::cout << "FAILED (expected 128, got " << max_size << ")" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_null_handling() {
    std::cout << "Test: Null pointer handling... ";

    MockBackendProvider backend;
    MockAccelerator accel;
    MockBlockStorage storage;
    MockIoEngine io_engine;

    CoordinatorConfig config;
    config.backend_provider = &backend;
    config.accelerator = &accel;
    config.block_storage = &storage;
    config.io_engine = &io_engine;

    ICoordinator* coordinator = create_coordinator();
    coordinator->initialize(config);

    MemoryRegion* null_region = coordinator->register_buffer(nullptr, 4096, MemoryKind::HOST);
    if (null_region != nullptr) {
        std::cout << "FAILED (null ptr accepted)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    bool unregister_null = coordinator->unregister_buffer(nullptr);
    if (unregister_null) {
        std::cout << "FAILED (null unregister succeeded)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== Layer 6 Coordinator Smoke Tests ===" << std::endl;

    int passed = 0;
    int total = 0;

    total++; if (test_coordinator_construction()) passed++;
    total++; if (test_coordinator_initialization()) passed++;
    total++; if (test_buffer_registration()) passed++;
    total++; if (test_buffer_registry()) passed++;
    total++; if (test_max_batch_size_query()) passed++;
    total++; if (test_null_handling()) passed++;

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << total << std::endl;

    return (passed == total) ? 0 : 1;
}
