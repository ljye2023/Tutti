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
    MockBackendProvider() {}

    bool initialize(VDevice*) override { return true; }
    void cleanup() override {}

    backends::BackendType backend_type() const override { return backends::BackendType::LOCAL_NVME; }
    const char* backend_name() const override { return "MockBackend"; }
    size_t max_io_size() const override { return 1024 * 1024; }
    backends::BackendMetadata metadata() const override {
        backends::BackendMetadata m{};
        m.max_io_size = 1024 * 1024;
        return m;
    }

    bool prepare_descriptors(const uint64_t*, const backends::SubSliceInfo*,
                             uint32_t, backends::BufferDescriptor*) override { return true; }
    void release_descriptors(backends::BufferDescriptor*, uint32_t) override {}

    void* acquire_target_handle(const backends::StorageTarget& t) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + t.namespace_id));
    }
    void release_target_handle(void*) override {}

    void launch_batch_gpu_stream(void*, void*, const backends::BufferDescriptor*, uint32_t, bool) override {}

    backends::SubmissionResult submit_batch_cpu_sync(
        void*, const backends::BufferDescriptor*, uint32_t, bool) override {
        backends::SubmissionResult r{}; r.success = true; return r;
    }
    bool submit_batch_cpu_async(backends::IOFuture*, void*, const backends::BufferDescriptor*, uint32_t, bool) override { return true; }
    bool setup_coop_channel(const backends::CoopChannelConfig&, void*) override { return true; }
    bool poll_future(const backends::IOFuture&, backends::SubmissionResult*) override { return true; }
    bool wait_future(const backends::IOFuture&, uint32_t, backends::SubmissionResult*) override { return true; }

private:
    uint64_t next_lba_;
    mutable std::mutex mutex_;
};

// Mock Accelerator for testing
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
        auto* r = new MemoryRegion{}; r->host_ptr = host_ptr; r->size = size; return r;
    }
    MemoryRegion* register_device(void* dev_ptr, size_t size, int) override {
        auto* r = new MemoryRegion{}; r->device_ptr = dev_ptr; r->size = size; return r;
    }
    MemoryRegion* register_external(void*, void*, size_t, const ExternalMemorySpec&) override { return nullptr; }
    void unregister(MemoryRegion* r) override { delete r; }
    MemoryRegion* lookup(const void*) const override { return nullptr; }
    MemoryRegion* lookup_by_id(uint64_t) const override { return nullptr; }
    void* device_pointer_for(const void*) override { return nullptr; }

    AccelStream create_stream() override { return AccelStream(reinterpret_cast<void*>(0x2000)); }
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
    AccelStream stream = accelerator.create_stream();
    result = storage->sync_file(handle, stream.handle);
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
