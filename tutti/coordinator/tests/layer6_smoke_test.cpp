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
    void* acquire_target_handle(backends::StorageTarget* target) override {
        return reinterpret_cast<void*>(0x1000);
    }

    bool release_target_handle(void* handle) override {
        return true;
    }

    uint32_t max_io_size() const override {
        return 131072;
    }
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
        return reinterpret_cast<HalStream*>(0x2000);
    }

    bool destroy_stream(HalStream* stream) override {
        return true;
    }

private:
    uint64_t next_id_ = 1;
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
        return nullptr;
    }

    bool close_gpu_file(block_storage::GpuFileHandle*) override {
        return true;
    }
};

class MockIoEngine : public IIoEngine {
public:
    bool submit_batch(
        const std::vector<IoRequest>&, bool, AccelStream) override {
        return true;
    }

    bool submit_batch_async(
        const std::vector<IoRequest>&, bool, AccelStream) override {
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
    MemoryRegion region;
    region.ptr = buffer;
    region.size = sizeof(buffer);
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
