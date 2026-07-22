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

// Mock implementations (same as smoke test)
class MockBackendProvider : public backends::IBackendProvider {
public:
    void* acquire_target_handle(backends::StorageTarget* target) override {
        return reinterpret_cast<void*>(0x1000 + (counter_++));
    }

    bool release_target_handle(void* handle) override {
        return true;
    }

    uint32_t max_io_size() const override {
        return 131072;
    }

private:
    int counter_ = 0;
};

class MockAccelerator : public IAccelerator {
public:
    MemoryRegion* register_host_memory(void* ptr, size_t size) override {
        auto* region = new MemoryRegion();
        region->ptr = ptr;
        region->size = size;
        region->region_id = next_id_++;
        region->kind = MemoryKind::HOST;
        return region;
    }

    MemoryRegion* register_device_memory(void* ptr, size_t size) override {
        auto* region = new MemoryRegion();
        region->ptr = ptr;
        region->size = size;
        region->region_id = next_id_++;
        region->kind = MemoryKind::DEVICE;
        return region;
    }

    MemoryRegion* register_external_memory(void* ptr, size_t size) override {
        auto* region = new MemoryRegion();
        region->ptr = ptr;
        region->size = size;
        region->region_id = next_id_++;
        region->kind = MemoryKind::EXTERNAL;
        return region;
    }

    bool unregister_memory(MemoryRegion* region) override {
        delete region;
        return true;
    }

    HalStream* create_stream() override {
        return reinterpret_cast<HalStream*>(0x2000 + (stream_counter_++));
    }

    bool destroy_stream(HalStream* stream) override {
        return true;
    }

private:
    uint64_t next_id_ = 1;
    int stream_counter_ = 0;
};

class MockBlockStorage : public block_storage::IBlockStorage {
public:
    bool initialize(const block_storage::BlockStorageConfig&) override {
        return true;
    }

    bool shutdown() override {
        return true;
    }

    block_storage::GpuFileHandle* open_gpu_file(
        const std::string&, block_storage::FileOpenMode) override {
        return reinterpret_cast<block_storage::GpuFileHandle*>(0x3000);
    }

    bool close_gpu_file(block_storage::GpuFileHandle*) override {
        return true;
    }
};

class MockIoEngine : public IIoEngine {
public:
    bool submit_batch(
        const std::vector<IoRequest>& requests, bool is_read, AccelStream stream) override {
        submitted_count_ += requests.size();
        return true;
    }

    bool submit_batch_async(
        const std::vector<IoRequest>& requests, bool is_read, AccelStream stream) override {
        submitted_count_ += requests.size();
        return true;
    }

    uint32_t max_entries_per_batch() const override {
        return 128;
    }

    uint32_t slice_fanout(const MemoryRegion*) const override {
        return 1;
    }

    uint32_t get_submitted_count() const {
        return submitted_count_;
    }

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

    BatchSubmitResult result = coordinator->submit_read_batch(&req, 1, nullptr);

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
