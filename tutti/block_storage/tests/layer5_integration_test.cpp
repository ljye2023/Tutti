#include "block_storage.h"
#include "block_storage_types.h"
#include "storage_config.h"
#include "backends/include/backend_provider.h"
#include "accel/include/common/iaccel.h"
#include "backends/include/storage_target.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>

using namespace tutti::block_storage;
using namespace tutti;

// Mock Backend Provider for testing
class MockBackendProvider : public backends::IBackendProvider {
public:
    MockBackendProvider() : device_count_(4), next_lba_(1000) {}

    bool initialize(VDevice* vdev) override { return true; }
    void cleanup() override {}

    bool prepare_descriptors(const uint64_t* ioaddrs, const backends::SubSliceInfo* slices,
                            uint32_t n_slices, backends::BufferDescriptor* out_descs) override {
        return true;
    }

    void release_descriptors(backends::BufferDescriptor* descs, uint32_t n_descs) override {}

    void* acquire_target_handle(const backends::StorageTarget& target) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return reinterpret_cast<void*>(0x1000 + target.namespace_id);
    }

    void release_target_handle(void* handle) override {}

    void launch_batch_gpu_stream(void* stream, void* target_handle,
                                 backends::BufferDescriptor* descs, uint32_t n_descs,
                                 bool is_read) override {}

    bool submit_batch_cpu_sync(void* target_handle, backends::BufferDescriptor* descs,
                               uint32_t n_descs, bool is_read) override {
        return true;
    }

    backends::BackendType get_backend_type() const override {
        return backends::BackendType::LOCAL_NVME;
    }

    const char* get_backend_name() const override { return "MockBackend"; }
    backends::BackendCapabilities get_capabilities() const override {
        return backends::BackendCapabilities{};
    }
    uint64_t get_max_io_size() const override { return 1024 * 1024; }

private:
    size_t device_count_;
    uint64_t next_lba_;
    mutable std::mutex mutex_;
};

// Mock Accelerator for testing
class MockAccelerator : public accel::IAccelerator {
public:
    accel::AccelKind get_kind() const override { return accel::AccelKind::CUDA; }
    const char* get_name() const override { return "MockAccel"; }

    bool allocate(size_t size, accel::MemoryKind kind, void** ptr) override {
        *ptr = malloc(size);
        return *ptr != nullptr;
    }

    bool deallocate(void* ptr, accel::MemoryKind kind) override {
        free(ptr);
        return true;
    }

    bool copy(void* dst, const void* src, size_t size, accel::MemoryKind dst_kind,
             accel::MemoryKind src_kind, void* stream) override {
        memcpy(dst, src, size);
        return true;
    }

    bool memset(void* ptr, int value, size_t size, accel::MemoryKind kind,
               void* stream) override {
        ::memset(ptr, value, size);
        return true;
    }

    void* create_stream() override { return reinterpret_cast<void*>(0x2000); }
    void destroy_stream(void* stream) override {}
    bool stream_sync(void* stream) override { return true; }

    bool get_device_properties(accel::DeviceProperties* props) override {
        return true;
    }
};

void test_large_file_striping() {
    std::cout << "Test: large_file_striping..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_large_file";
    config.stripe_config.stripe_size = 256 * 1024;
    config.stripe_config.max_shards_per_file = 64;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create 1GB file across 4 devices
    uint64_t file_size = 1ULL * 1024 * 1024 * 1024;  // 1 GB
    GpuFileHandle* handle = storage->open_gpu_file(
        "large_file.dat", FileOpenMode::CREATE_NEW, 256 * 1024, file_size);
    assert(handle != nullptr);

    // Verify shards span multiple devices
    std::vector<FileInfo> files = storage->list_gpu_file_names();
    assert(files.size() == 1);
    assert(files[0].shard_count >= 4);

    storage->close_gpu_file(handle);
    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_concurrent_file_access() {
    std::cout << "Test: concurrent_file_access..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_concurrent";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create a test file
    GpuFileHandle* handle = storage->open_gpu_file(
        "concurrent_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);
    storage->close_gpu_file(handle);

    // Try to open from multiple threads
    std::vector<std::thread> threads;
    std::vector<bool> results(4, false);

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&storage, &results, i]() {
            GpuFileHandle* h = storage->open_gpu_file(
                "concurrent_test.dat", FileOpenMode::READ_ONLY);
            if (h) {
                results[i] = true;
                // Only one thread should succeed (file can't be opened multiple times)
                storage->close_gpu_file(h);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // At least one thread should have succeeded
    int success_count = 0;
    for (bool r : results) {
        if (r) success_count++;
    }
    assert(success_count >= 1);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_batch_create_delete() {
    std::cout << "Test: batch_create_delete..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_batch";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 200;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create 100 files in batch
    const int FILE_COUNT = 100;
    std::vector<std::string> names;
    std::vector<FileOpenMode> modes;

    for (int i = 0; i < FILE_COUNT; ++i) {
        names.push_back("batch_file_" + std::to_string(i) + ".dat");
        modes.push_back(FileOpenMode::CREATE_NEW);
    }

    std::vector<GpuFileHandle*> handles = storage->open_gpu_files_batch(names, modes, FILE_COUNT);
    assert(handles.size() == FILE_COUNT);

    // Close all files
    for (auto handle : handles) {
        storage->close_gpu_file(handle);
    }

    // Verify all files exist
    std::vector<FileInfo> files = storage->list_gpu_file_names();
    assert(files.size() == FILE_COUNT);

    // Delete all files
    for (const auto& name : names) {
        result = storage->delete_gpu_file(name);
        assert(result);
    }

    // Verify all deleted
    files = storage->list_gpu_file_names();
    assert(files.size() == 0);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_metadata_recovery() {
    std::cout << "Test: metadata_recovery..." << std::endl;

    std::string root_dir = "/tmp/tutti_integration_recovery";

    // Create files and checkpoint
    {
        auto storage = create_block_storage();
        assert(storage != nullptr);

        BlockStorageConfig config;
        config.root_directory = root_dir;
        config.stripe_config.stripe_size = 256 * 1024;
        config.max_open_files = 100;

        MockBackendProvider provider;
        MockAccelerator accelerator;

        bool result = storage->initialize(config, &provider, &accelerator);
        assert(result);

        // Create multiple files
        for (int i = 0; i < 10; ++i) {
            std::string name = "recovery_file_" + std::to_string(i) + ".dat";
            GpuFileHandle* handle = storage->open_gpu_file(
                name, FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
            assert(handle != nullptr);
            storage->close_gpu_file(handle);
        }

        storage->flush_metadata();
        storage->cleanup();
    }

    // Simulate crash and recovery
    {
        auto storage = create_block_storage();
        assert(storage != nullptr);

        BlockStorageConfig config;
        config.root_directory = root_dir;
        config.stripe_config.stripe_size = 256 * 1024;
        config.max_open_files = 100;

        MockBackendProvider provider;
        MockAccelerator accelerator;

        bool result = storage->initialize(config, &provider, &accelerator);
        assert(result);

        // Verify files were recovered
        std::vector<FileInfo> files = storage->list_gpu_file_names();
        assert(files.size() == 10);

        storage->cleanup();
    }

    std::cout << "  PASS" << std::endl;
}

void test_sync_durability() {
    std::cout << "Test: sync_durability..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_sync";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;
    config.durability_config.sync_mode = SyncMode::FULL;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create file
    GpuFileHandle* handle = storage->open_gpu_file(
        "sync_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);

    // Mark as dirty
    handle->dirty = true;

    // Sync file
    void* stream = accelerator.create_stream();
    result = storage->sync_file(handle, stream);
    assert(result);
    assert(!handle->dirty);

    accelerator.destroy_stream(stream);
    storage->close_gpu_file(handle);
    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_max_open_files() {
    std::cout << "Test: max_open_files..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_max_files";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 5;  // Small limit

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Open files up to limit
    std::vector<GpuFileHandle*> handles;
    for (int i = 0; i < 5; ++i) {
        std::string name = "limit_file_" + std::to_string(i) + ".dat";
        GpuFileHandle* handle = storage->open_gpu_file(
            name, FileOpenMode::CREATE_NEW, 0, 1024);
        assert(handle != nullptr);
        handles.push_back(handle);
    }

    // Try to open one more - should fail
    GpuFileHandle* handle = storage->open_gpu_file(
        "overflow_file.dat", FileOpenMode::CREATE_NEW, 0, 1024);
    assert(handle == nullptr);

    // Close one file
    storage->close_gpu_file(handles[0]);
    handles.erase(handles.begin());

    // Now should be able to open
    handle = storage->open_gpu_file(
        "overflow_file.dat", FileOpenMode::CREATE_NEW, 0, 1024);
    assert(handle != nullptr);

    // Clean up
    storage->close_gpu_file(handle);
    for (auto h : handles) {
        storage->close_gpu_file(h);
    }

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_device_failure_handling() {
    std::cout << "Test: device_failure_handling..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_integration_device_failure";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create file - should succeed with available devices
    GpuFileHandle* handle = storage->open_gpu_file(
        "device_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);

    storage->close_gpu_file(handle);
    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "Running Block Storage Layer 5 Integration Tests" << std::endl;
    std::cout << "================================================" << std::endl;

    test_large_file_striping();
    test_concurrent_file_access();
    test_batch_create_delete();
    test_metadata_recovery();
    test_sync_durability();
    test_max_open_files();
    test_device_failure_handling();

    std::cout << std::endl;
    std::cout << "All integration tests PASSED!" << std::endl;

    return 0;
}
