// Include accel headers first
#include <sys/types.h>  // for off_t used in memory_region.h
#include "accel/include/common/iaccel.h"
#include "accel/include/common/accel_types.h"
#include "accel/include/common/memory_kind.h"
#include "accel/include/common/memory_region.h"

#include "block_storage.h"
#include "block_storage_types.h"
#include "storage_config.h"
#include "backends/include/backend_provider.h"
#include "backends/include/storage_target.h"
#include "coordinator/include/raw_device.h"
#include "coordinator/include/coordinator_types.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
#include <map>
#include <filesystem>

using namespace tutti::block_storage;
using namespace tutti;

// Note: IAccelerator is in tutti:: namespace, not tutti::accel::

// Mock Backend Provider for testing
class MockBackendProvider : public backends::IBackendProvider {
public:
    MockBackendProvider() : device_count_(4), next_lba_(1000) {}

    bool initialize(VDevice* vdev) override {
        (void)vdev;
        return true;
    }
    void cleanup() override {}

    bool prepare_descriptors(const uint64_t* ioaddrs, const backends::SubSliceInfo* slices,
                            uint32_t n_slices, backends::BufferDescriptor* out_descs) override {
        (void)ioaddrs; (void)slices; (void)n_slices; (void)out_descs;
        return true;
    }

    void release_descriptors(backends::BufferDescriptor* descs, uint32_t n_descs) override {
        (void)descs; (void)n_descs;
    }

    void* acquire_target_handle(const backends::StorageTarget& target) override {
        return reinterpret_cast<void*>(0x1000 + target.namespace_id);
    }

    void release_target_handle(void* handle) override {
        (void)handle;
    }

    void launch_batch_gpu_stream(void* stream, void* target_handle,
                                 const backends::BufferDescriptor* descs, uint32_t n_descs,
                                 bool is_read) override {
        (void)stream; (void)target_handle; (void)descs; (void)n_descs; (void)is_read;
    }

    backends::SubmissionResult submit_batch_cpu_sync(void* target_handle,
                                                      const backends::BufferDescriptor* descs,
                                                      uint32_t n_descs, bool is_read) override {
        (void)target_handle; (void)descs; (void)n_descs; (void)is_read;
        backends::SubmissionResult result;
        result.success = true;
        result.completed_count = n_descs;
        result.failed_count = 0;
        result.error_code = 0;
        return result;
    }

    bool submit_batch_cpu_async(backends::IOFuture* future, void* target_handle,
                                const backends::BufferDescriptor* descs,
                                uint32_t n_descs, bool is_read) override {
        (void)future; (void)target_handle; (void)descs; (void)n_descs; (void)is_read;
        return false; // Not supported
    }

    bool setup_coop_channel(const backends::CoopChannelConfig& config,
                           void* target_handle) override {
        (void)config; (void)target_handle;
        return false; // Not supported
    }

    bool poll_future(const backends::IOFuture& future,
                    backends::SubmissionResult* out_result) override {
        (void)future; (void)out_result;
        return false; // Not supported
    }

    bool wait_future(const backends::IOFuture& future, uint32_t timeout_ms,
                    backends::SubmissionResult* out_result) override {
        (void)future; (void)timeout_ms; (void)out_result;
        return false; // Not supported
    }

    backends::BackendType backend_type() const override {
        return backends::BackendType::LOCAL_NVME;
    }

    const char* backend_name() const override { return "MockBackend"; }

    size_t max_io_size() const override { return 1024 * 1024; }

    backends::BackendMetadata metadata() const override {
        backends::BackendMetadata meta;
        meta.name = "MockBackend";
        meta.type = backends::BackendType::LOCAL_NVME;
        meta.capabilities = 0;
        meta.max_io_size = 1024 * 1024;
        meta.max_batch_size = 128;
        meta.alignment_bytes = 4096;
        return meta;
    }

private:
    size_t device_count_;
    uint64_t next_lba_;
};

// Mock Accelerator for testing
class MockAccelerator : public tutti::IAccelerator {
public:
    const char* vendor_name() const override { return "MockVendor"; }
    int device_count() const override { return 1; }
    bool set_device(int device_id) override { (void)device_id; return true; }
    int get_device() const override { return 0; }

    void* allocate_host(size_t size, tutti::MemoryKind kind) override {
        (void)kind;
        return malloc(size);
    }

    void* allocate_device(size_t size, tutti::MemoryKind kind, int device_id) override {
        (void)kind; (void)device_id;
        return malloc(size);
    }

    void free(void* ptr, tutti::MemoryKind kind) override {
        (void)kind;
        ::free(ptr);
    }

    tutti::MemoryRegion* register_host(void* host_ptr, size_t size) override {
        (void)host_ptr; (void)size;
        return nullptr;
    }

    tutti::MemoryRegion* register_device(void* device_ptr, size_t size, int device_id) override {
        (void)device_ptr; (void)size; (void)device_id;
        return nullptr;
    }

    tutti::MemoryRegion* register_external(void* host_ptr, void* device_ptr,
                                          size_t size, const tutti::ExternalMemorySpec& spec) override {
        (void)host_ptr; (void)device_ptr; (void)size; (void)spec;
        return nullptr;
    }

    void unregister(tutti::MemoryRegion* region) override {
        (void)region;
    }

    tutti::MemoryRegion* lookup(const void* ptr) const override {
        (void)ptr;
        return nullptr;
    }

    tutti::MemoryRegion* lookup_by_id(uint64_t region_id) const override {
        (void)region_id;
        return nullptr;
    }

    void* device_pointer_for(const void* host_ptr) override {
        return const_cast<void*>(host_ptr);
    }

    tutti::AccelStream create_stream() override {
        return tutti::AccelStream{reinterpret_cast<void*>(0x2000)};
    }

    void destroy_stream(tutti::AccelStream stream) override {
        (void)stream;
    }

    void synchronize_stream(tutti::AccelStream stream) override {
        (void)stream;
    }

    tutti::AccelEvent create_event() override {
        return tutti::AccelEvent{reinterpret_cast<void*>(0x3000)};
    }

    void destroy_event(tutti::AccelEvent event) override {
        (void)event;
    }

    void record_event(tutti::AccelEvent event, tutti::AccelStream stream) override {
        (void)event; (void)stream;
    }

    void wait_event(tutti::AccelStream stream, tutti::AccelEvent event) override {
        (void)stream; (void)event;
    }

    bool query_event(tutti::AccelEvent event) override {
        (void)event;
        return true;
    }

    bool memcpy_async(void* dst, const void* src, size_t size, tutti::AccelStream stream) override {
        (void)stream;
        memcpy(dst, src, size);
        return true;
    }

    void launch(void* kernel_func, const tutti::Dim3& grid, const tutti::Dim3& block,
               size_t shared_mem_bytes, tutti::AccelStream stream, void** kernel_args) override {
        (void)kernel_func; (void)grid; (void)block; (void)shared_mem_bytes;
        (void)stream; (void)kernel_args;
    }

    bool ipc_export(tutti::MemoryRegion* region, tutti::IpcHandle* out_handle) override {
        (void)region; (void)out_handle;
        return false;
    }

    tutti::MemoryRegion* ipc_import(const tutti::IpcHandle& handle, int device_id) override {
        (void)handle; (void)device_id;
        return nullptr;
    }
};

// Mock Raw Device for testing
class MockRawDevice : public tutti::coordinator::IRawDevice {
public:
    MockRawDevice() : namespace_count_(4) {
        // Initialize mock namespaces
        for (uint32_t i = 1; i <= namespace_count_; ++i) {
            tutti::coordinator::NamespaceInfo info;
            info.namespace_id = i;
            info.block_size = 512;
            info.capacity_blocks = 1024 * 1024 * 1024;  // 512 GB per namespace
            info.mdts_bytes = 1024 * 1024;
            namespaces_[i] = info;
        }
    }

    tutti::coordinator::RawTargetHandle* acquire_raw_target(
        uint32_t namespace_id,
        uint64_t start_lba,
        uint64_t length_blocks) override {

        auto* handle = new tutti::coordinator::RawTargetHandle(namespace_id, start_lba, length_blocks);
        handle->target_handle = reinterpret_cast<void*>(0x5000 + namespace_id);
        return handle;
    }

    bool release_raw_target(tutti::coordinator::RawTargetHandle* handle) override {
        if (handle) {
            delete handle;
            return true;
        }
        return false;
    }

    bool submit_read(
        tutti::coordinator::RawTargetHandle* handle,
        tutti::MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        tutti::AccelStream stream = tutti::AccelStream()) override {
        (void)handle; (void)buffer; (void)byte_offset; (void)byte_length; (void)stream;
        return true;
    }

    bool submit_write(
        tutti::coordinator::RawTargetHandle* handle,
        tutti::MemoryRegion* buffer,
        uint64_t byte_offset,
        uint64_t byte_length,
        tutti::AccelStream stream = tutti::AccelStream()) override {
        (void)handle; (void)buffer; (void)byte_offset; (void)byte_length; (void)stream;
        return true;
    }

    tutti::coordinator::BatchSubmitResult submit_read_batch(
        tutti::coordinator::RawTargetHandle* handle,
        tutti::coordinator::IoRequest* requests,
        uint32_t count,
        tutti::AccelStream stream = tutti::AccelStream()) override {
        (void)handle; (void)requests; (void)stream;
        return tutti::coordinator::BatchSubmitResult(true, count, 0, 0);
    }

    tutti::coordinator::BatchSubmitResult submit_write_batch(
        tutti::coordinator::RawTargetHandle* handle,
        tutti::coordinator::IoRequest* requests,
        uint32_t count,
        tutti::AccelStream stream = tutti::AccelStream()) override {
        (void)handle; (void)requests; (void)stream;
        return tutti::coordinator::BatchSubmitResult(true, count, 0, 0);
    }

    tutti::coordinator::NamespaceInfo get_namespace_info(uint32_t namespace_id) override {
        auto it = namespaces_.find(namespace_id);
        if (it != namespaces_.end()) {
            return it->second;
        }
        return tutti::coordinator::NamespaceInfo();
    }

    std::vector<uint32_t> list_namespaces() override {
        std::vector<uint32_t> ns_ids;
        for (uint32_t i = 1; i <= namespace_count_; ++i) {
            ns_ids.push_back(i);
        }
        return ns_ids;
    }

private:
    uint32_t namespace_count_;
    std::map<uint32_t, tutti::coordinator::NamespaceInfo> namespaces_;
};

void test_mock_backend_provider() {
    std::cout << "Test: mock_backend_provider..." << std::endl;

    MockBackendProvider provider;

    // Test backend metadata
    assert(provider.backend_type() == backends::BackendType::LOCAL_NVME);
    assert(std::string(provider.backend_name()) == "MockBackend");
    assert(provider.max_io_size() == 1024 * 1024);

    // Test target handle acquisition
    backends::StorageTarget target;
    target.namespace_id = 1;
    void* handle = provider.acquire_target_handle(target);
    assert(handle != nullptr);

    provider.release_target_handle(handle);

    std::cout << "  PASS" << std::endl;
}

void test_block_storage_construction() {
    std::cout << "Test: block_storage_construction..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_file_create_open() {
    std::cout << "Test: file_create_open..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_create";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create a new file
    GpuFileHandle* handle = storage->open_gpu_file(
        "test_file.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);
    assert(handle->file_id != INVALID_FILE_ID);

    // Close the file
    result = storage->close_gpu_file(handle);
    assert(result);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_file_close_delete() {
    std::cout << "Test: file_close_delete..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_delete";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create and close file
    GpuFileHandle* handle = storage->open_gpu_file(
        "delete_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);

    result = storage->close_gpu_file(handle);
    assert(result);

    // Delete the file
    result = storage->delete_gpu_file("delete_test.dat");
    assert(result);

    // Try to open deleted file - should fail
    handle = storage->open_gpu_file("delete_test.dat", FileOpenMode::READ_ONLY);
    assert(handle == nullptr);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_acquire_release_handle() {
    std::cout << "Test: acquire_release_handle..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_handle";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create file
    GpuFileHandle* handle = storage->open_gpu_file(
        "handle_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
    assert(handle != nullptr);

    // Acquire device handle for first shard
    backends::StorageTarget target = storage->acquire_device_handle(handle, 0);
    assert(target.namespace_id >= 0);

    // Release device handle
    result = storage->release_device_handle(handle, 0);
    assert(result);

    storage->close_gpu_file(handle);
    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_stripe_allocation() {
    std::cout << "Test: stripe_allocation..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_stripe";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create file large enough to span multiple devices
    uint64_t file_size = 2 * 1024 * 1024;  // 2 MB
    GpuFileHandle* handle = storage->open_gpu_file(
        "stripe_test.dat", FileOpenMode::CREATE_NEW, 0, file_size);
    assert(handle != nullptr);

    storage->close_gpu_file(handle);
    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_directory_persistence() {
    std::cout << "Test: directory_persistence..." << std::endl;

    std::string root_dir = "/tmp/tutti_test_storage_persist";

    // Create files in first instance
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

        GpuFileHandle* handle = storage->open_gpu_file(
            "persist_test.dat", FileOpenMode::CREATE_NEW, 0, 1024 * 1024);
        assert(handle != nullptr);

        storage->close_gpu_file(handle);
        storage->flush_metadata();
        storage->cleanup();
    }

    // Reopen and verify files exist
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

        std::vector<FileInfo> files = storage->list_gpu_file_names();
        assert(files.size() >= 1);

        storage->cleanup();
    }

    std::cout << "  PASS" << std::endl;
}

void test_batch_operations() {
    std::cout << "Test: batch_operations..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_batch";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Create batch of files
    std::vector<std::string> names = {"batch1.dat", "batch2.dat", "batch3.dat"};
    std::vector<FileOpenMode> modes = {
        FileOpenMode::CREATE_NEW,
        FileOpenMode::CREATE_NEW,
        FileOpenMode::CREATE_NEW
    };

    std::vector<GpuFileHandle*> handles = storage->open_gpu_files_batch(names, modes, 3);
    assert(handles.size() == 3);

    for (auto handle : handles) {
        assert(handle != nullptr);
        storage->close_gpu_file(handle);
    }

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_null_handling() {
    std::cout << "Test: null_handling..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_null";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Test null handle operations
    result = storage->close_gpu_file(nullptr);
    assert(!result);

    result = storage->sync_file(nullptr, nullptr);
    assert(!result);

    backends::StorageTarget target = storage->acquire_device_handle(nullptr, 0);
    assert(target.start_lba == 0);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

void test_invalid_params() {
    std::cout << "Test: invalid_params..." << std::endl;

    auto storage = create_block_storage();
    assert(storage != nullptr);

    BlockStorageConfig config;
    config.root_directory = "/tmp/tutti_test_storage_invalid";
    config.stripe_config.stripe_size = 256 * 1024;
    config.max_open_files = 100;

    MockBackendProvider provider;
    MockAccelerator accelerator;

    bool result = storage->initialize(config, &provider, &accelerator);
    assert(result);

    // Test empty file name
    GpuFileHandle* handle = storage->open_gpu_file("", FileOpenMode::CREATE_NEW);
    assert(handle == nullptr);

    // Test opening non-existent file in READ_ONLY mode
    handle = storage->open_gpu_file("nonexistent.dat", FileOpenMode::READ_ONLY);
    assert(handle == nullptr);

    // Test deleting non-existent file
    result = storage->delete_gpu_file("nonexistent.dat");
    assert(!result);

    storage->cleanup();

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "Running Block Storage Layer 5 Smoke Tests" << std::endl;
    std::cout << "==========================================" << std::endl;

    // Clean up test directories from previous runs
    std::filesystem::remove_all("/tmp/tutti_test_storage");
    std::filesystem::remove_all("/tmp/tutti_test_storage_create");
    std::filesystem::remove_all("/tmp/tutti_test_storage_delete");
    std::filesystem::remove_all("/tmp/tutti_test_storage_handle");
    std::filesystem::remove_all("/tmp/tutti_test_storage_stripe");
    std::filesystem::remove_all("/tmp/tutti_test_storage_persist");
    std::filesystem::remove_all("/tmp/tutti_test_storage_batch");

    test_mock_backend_provider();
    test_block_storage_construction();
    test_file_create_open();
    test_file_close_delete();
    test_acquire_release_handle();
    test_stripe_allocation();
    test_directory_persistence();
    test_batch_operations();
    test_null_handling();
    test_invalid_params();

    std::cout << std::endl;
    std::cout << "All smoke tests PASSED!" << std::endl;

    return 0;
}
