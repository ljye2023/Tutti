#include "coordinator.h"
#include "coordinator_types.h"
#include "raw_device.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "block_storage/include/block_storage.h"
#include "io_engine/include/io_engine.h"
#include <iostream>
#include <vector>
#include <cstring>

using namespace tutti;
using namespace tutti::coordinator;

// Mock implementations (aligned to current interfaces)
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
        return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + counter_++));
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

private:
    mutable int counter_ = 0;
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
        r->host_ptr  = host_ptr;
        r->size      = size;
        r->region_id = next_id_++;
        r->kind      = MemoryKind::HOST;
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
    uint64_t next_id_       = 1;
    int      stream_counter_ = 0;
};

class MockBlockStorage : public block_storage::IBlockStorage {
public:
    bool initialize(const block_storage::BlockStorageConfig&,
                    backends::IBackendProvider*, IAccelerator*) override { return true; }
    void cleanup() override {}

    block_storage::GpuFileHandle* open_gpu_file(
        const std::string&, block_storage::FileOpenMode,
        uint64_t = 0, uint64_t = 0) override {
        return reinterpret_cast<block_storage::GpuFileHandle*>(0x3000);
    }
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
        const std::vector<tutti::IoRequest>& requests, bool, AccelStream) override {
        submitted_count_ += requests.size();
        return true;
    }

    bool submit_batch_async(
        const std::vector<tutti::IoRequest>& requests, bool, AccelStream) override {
        submitted_count_ += requests.size();
        return true;
    }

    uint32_t max_entries_per_batch() const override { return 128; }

    uint32_t slice_fanout(const MemoryRegion*) const override { return 1; }

    uint32_t get_submitted_count() const { return submitted_count_; }

private:
    uint32_t submitted_count_ = 0;
};

// Test functions
bool test_multi_buffer_registration() {
    std::cout << "Test: Multiple buffer registration... ";

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

    std::vector<char> buffer1(4096);
    std::vector<char> buffer2(8192);
    std::vector<char> buffer3(16384);

    MemoryRegion* region1 = coordinator->register_buffer(
        buffer1.data(), buffer1.size(), MemoryKind::HOST);
    MemoryRegion* region2 = coordinator->register_buffer(
        buffer2.data(), buffer2.size(), MemoryKind::HOST);
    MemoryRegion* region3 = coordinator->register_buffer(
        buffer3.data(), buffer3.size(), MemoryKind::HOST);

    if (!region1 || !region2 || !region3) {
        std::cout << "FAILED (registration failed)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->unregister_buffer(region1);
    coordinator->unregister_buffer(region2);
    coordinator->unregister_buffer(region3);

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_raw_device_interface() {
    std::cout << "Test: Raw device interface access... ";

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

    IRawDevice* raw_device = coordinator->get_raw_device();
    if (!raw_device) {
        std::cout << "FAILED (null raw device)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    RawTargetHandle* handle = raw_device->acquire_raw_target(1, 0, 1000);
    if (!handle) {
        std::cout << "FAILED (acquire failed)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    bool release_result = raw_device->release_raw_target(handle);
    if (!release_result) {
        std::cout << "FAILED (release failed)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_block_storage_access() {
    std::cout << "Test: Block storage access... ";

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

    block_storage::IBlockStorage* block_storage = coordinator->get_block_storage();
    if (!block_storage) {
        std::cout << "FAILED (null block storage)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_batch_submit_validation() {
    std::cout << "Test: Batch submit validation... ";

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

    std::vector<char> buffer(4096);
    MemoryRegion* region = coordinator->register_buffer(
        buffer.data(), buffer.size(), MemoryKind::HOST);

    coordinator::IoRequest req;
    req.region = region;
    req.target_handle = reinterpret_cast<void*>(0x1000);
    req.byte_offset = 0;
    req.byte_length = 4096;

    BatchSubmitResult result = coordinator->submit_read_batch(&req, 1, AccelStream{});

    if (!result.success) {
        std::cout << "FAILED (submit failed)" << std::endl;
        coordinator->unregister_buffer(region);
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->unregister_buffer(region);
    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_slice_fanout_query() {
    std::cout << "Test: Slice fanout query... ";

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

    std::vector<char> buffer(4096);
    MemoryRegion* region = coordinator->register_buffer(
        buffer.data(), buffer.size(), MemoryKind::HOST);

    uint32_t fanout = coordinator->slice_fanout(region);
    if (fanout != 1) {
        std::cout << "FAILED (expected fanout=1, got " << fanout << ")" << std::endl;
        coordinator->unregister_buffer(region);
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    coordinator->unregister_buffer(region);
    coordinator->cleanup();
    destroy_coordinator(coordinator);
    std::cout << "PASSED" << std::endl;
    return true;
}

bool test_namespace_query() {
    std::cout << "Test: Namespace query via raw device... ";

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

    IRawDevice* raw_device = coordinator->get_raw_device();

    std::vector<uint32_t> namespaces = raw_device->list_namespaces();
    if (namespaces.empty()) {
        std::cout << "FAILED (no namespaces)" << std::endl;
        coordinator->cleanup();
        destroy_coordinator(coordinator);
        return false;
    }

    NamespaceInfo info = raw_device->get_namespace_info(1);
    if (info.namespace_id != 1) {
        std::cout << "FAILED (wrong namespace id)" << std::endl;
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
    std::cout << "=== Layer 6 Coordinator Integration Tests ===" << std::endl;

    int passed = 0;
    int total = 0;

    total++; if (test_multi_buffer_registration()) passed++;
    total++; if (test_raw_device_interface()) passed++;
    total++; if (test_block_storage_access()) passed++;
    total++; if (test_batch_submit_validation()) passed++;
    total++; if (test_slice_fanout_query()) passed++;
    total++; if (test_namespace_query()) passed++;

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << total << std::endl;

    return (passed == total) ? 0 : 1;
}
