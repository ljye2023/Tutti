/**
 * layer6_hw_basic_test.cpp - Layer 6 (Coordinator) Hardware Basic Test
 *
 * Tests the coordinator with real hardware (NVMe + GPU) through the full stack.
 * Verifies the coordinator orchestrates all layers correctly for basic operations.
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>
#include <chrono>

// Layer 6 Coordinator
#include "coordinator.h"
#include "coordinator_types.h"
#include "raw_device.h"

// Layer 5 Block Storage
#include "block_storage/include/block_storage.h"
#include "block_storage/include/block_storage_types.h"

// Layer 4 IO Engine
#include "io_engine/include/io_engine.h"
#include "io_engine/include/local_nvme/local_nvme_io_engine.h"

// Layer 3 Backends
#include "backends/include/backend_provider.h"
#include "backends/include/backend_factory.h"

// Layer 2 Device Manager
#include "device_manager/include/common/device_registry.h"

// Layer 1 Accelerator HAL
#include "accel/include/cuda/cuda_accelerator.h"
#include "accel/include/common/iaccel.h"

using namespace tutti;
using namespace tutti::coordinator;

// Test configuration
struct TestConfig {
    uint32_t namespace_id = 1;
    uint64_t start_lba = 1000000;  // Start at 1M blocks to avoid filesystem area
    uint64_t test_blocks = 256;     // Test with 256 blocks (1MB at 4K blocks)
    size_t buffer_size = 1024 * 1024;  // 1MB buffer
};

class HardwareTest {
public:
    HardwareTest() : test_config_() {}

    bool initialize() {
        std::cout << "=== Initializing Layer 6 Hardware Test Stack ===" << std::endl;

        // Layer 1: Initialize Accelerator HAL
        std::cout << "[1/6] Initializing CUDA Accelerator..." << std::endl;
        accel_ = new CudaAccelerator();
        if (!accel_) {
            std::cerr << "Failed to create CUDA accelerator" << std::endl;
            return false;
        }

        int gpu_count = accel_->device_count();
        std::cout << "  Found " << gpu_count << " GPU(s)" << std::endl;
        if (gpu_count == 0) {
            std::cerr << "No GPUs available" << std::endl;
            return false;
        }

        accel_->set_device(0);
        std::cout << "  Using GPU 0: " << accel_->vendor_name() << std::endl;

        // Layer 2: Initialize Device Manager - using backend factory instead
        std::cout << "[2/6] Skipping Device Manager (will use backend factory)..." << std::endl;
        // Device manager is internal to backends in the new architecture
        device_registry_ = nullptr;

        // Layer 3: Initialize Backend via factory
        std::cout << "[3/6] Initializing Local NVMe Backend via factory..." << std::endl;

        auto backend_unique = backends::BackendFactory::create_backend(backends::BackendType::LOCAL_NVME);
        if (!backend_unique) {
            std::cerr << "Failed to create backend from factory" << std::endl;
            return false;
        }

        backend_ = backend_unique.release();

        if (!backend_->initialize()) {
            std::cerr << "Failed to initialize backend" << std::endl;
            return false;
        }
        std::cout << "  Backend initialized: " << backend_->backend_name() << std::endl;
        std::cout << "  Max IO size: " << backend_->max_io_size() << " bytes" << std::endl;

        // Layer 4: Initialize IO Engine
        std::cout << "[4/6] Initializing IO Engine..." << std::endl;

        io_engine_ = new local_nvme::LocalNvmeIoEngine(backend_, accel_,
            local_nvme::LocalNvmeIoEngineConfig{128});
        if (!io_engine_) {
            std::cerr << "Failed to create IO engine" << std::endl;
            return false;
        }

        std::cout << "  IO Engine initialized" << std::endl;
        std::cout << "  Max batch size: " << io_engine_->max_entries_per_batch() << std::endl;

        // Layer 5: Initialize Block Storage
        std::cout << "[5/6] Initializing Block Storage..." << std::endl;
        block_storage::BlockStorageConfig bs_config;
        bs_config.root_directory = "/tmp/tutti_layer6_test";
        bs_config.stripe_config.stripe_size = 1024 * 1024;  // 1MB stripe
        bs_config.max_open_files = 100;

        auto block_storage_unique = block_storage::create_block_storage();
        if (!block_storage_unique) {
            std::cerr << "Failed to create block storage" << std::endl;
            return false;
        }

        block_storage_ = block_storage_unique.release();

        if (!block_storage_->initialize(bs_config, backend_, accel_)) {
            std::cerr << "Failed to initialize block storage" << std::endl;
            return false;
        }
        std::cout << "  Block Storage initialized" << std::endl;

        // Layer 6: Initialize Coordinator
        std::cout << "[6/6] Initializing Coordinator..." << std::endl;
        CoordinatorConfig coord_config;
        coord_config.backend_provider = backend_;
        coord_config.accelerator = accel_;
        coord_config.block_storage = block_storage_;
        coord_config.io_engine = io_engine_;
        coord_config.max_batch_size = 128;

        coordinator_ = create_coordinator();
        if (!coordinator_) {
            std::cerr << "Failed to create coordinator" << std::endl;
            return false;
        }

        if (!coordinator_->initialize(coord_config)) {
            std::cerr << "Failed to initialize coordinator" << std::endl;
            return false;
        }
        std::cout << "  Coordinator initialized" << std::endl;
        std::cout << "  Max batch size: " << coordinator_->max_batch_size() << std::endl;

        std::cout << "\n✅ Full stack initialized successfully\n" << std::endl;
        return true;
    }

    bool test_buffer_registration() {
        std::cout << "=== Test 1: Buffer Registration via Coordinator ===" << std::endl;

        // Allocate host buffer
        void* host_buf = accel_->allocate_host(test_config_.buffer_size, MemoryKind::PINNED_HOST);
        if (!host_buf) {
            std::cerr << "Failed to allocate host buffer" << std::endl;
            return false;
        }

        // Fill with test pattern
        uint32_t* data = static_cast<uint32_t*>(host_buf);
        for (size_t i = 0; i < test_config_.buffer_size / sizeof(uint32_t); ++i) {
            data[i] = 0xDEADBEEF + i;
        }

        // Register via coordinator
        MemoryRegion* region = coordinator_->register_buffer(
            host_buf, test_config_.buffer_size, MemoryKind::PINNED_HOST);

        if (!region) {
            std::cerr << "Failed to register buffer" << std::endl;
            accel_->free(host_buf, MemoryKind::PINNED_HOST);
            return false;
        }

        std::cout << "✅ Buffer registered:" << std::endl;
        std::cout << "   Region ID: " << region->region_id << std::endl;
        std::cout << "   Size: " << region->size << " bytes" << std::endl;
        std::cout << "   Host ptr: " << region->host_ptr << std::endl;

        // Store for later tests
        test_buffer_ = host_buf;
        test_region_ = region;

        return true;
    }

    bool test_raw_device_access() {
        std::cout << "\n=== Test 2: Raw Device Access (namespace + LBA) ===" << std::endl;

        IRawDevice* raw_device = coordinator_->get_raw_device();
        if (!raw_device) {
            std::cerr << "Failed to get raw device interface" << std::endl;
            return false;
        }

        // List available namespaces
        auto namespaces = raw_device->list_namespaces();
        std::cout << "Available namespaces: ";
        for (auto ns : namespaces) {
            std::cout << ns << " ";
        }
        std::cout << std::endl;

        if (namespaces.empty()) {
            std::cerr << "No namespaces available" << std::endl;
            return false;
        }

        // Get namespace info
        uint32_t test_ns = namespaces[0];
        NamespaceInfo ns_info = raw_device->get_namespace_info(test_ns);
        std::cout << "Namespace " << test_ns << " info:" << std::endl;
        std::cout << "   Block size: " << ns_info.block_size << " bytes" << std::endl;
        std::cout << "   Capacity: " << ns_info.capacity_blocks << " blocks" << std::endl;
        std::cout << "   MDTS: " << ns_info.mdts_bytes << " bytes" << std::endl;

        // Acquire raw target
        RawTargetHandle* raw_target = raw_device->acquire_raw_target(
            test_ns,
            test_config_.start_lba,
            test_config_.test_blocks);

        if (!raw_target) {
            std::cerr << "Failed to acquire raw target" << std::endl;
            return false;
        }

        std::cout << "✅ Raw target acquired:" << std::endl;
        std::cout << "   Namespace: " << raw_target->namespace_id << std::endl;
        std::cout << "   Start LBA: " << raw_target->start_lba << std::endl;
        std::cout << "   Length: " << raw_target->length_blocks << " blocks" << std::endl;
        std::cout << "   Region ID: " << raw_target->region_id << std::endl;

        // Store for cleanup
        raw_target_ = raw_target;

        return true;
    }

    bool test_write_read_cycle() {
        std::cout << "\n=== Test 3: Write/Read Cycle via Coordinator ===" << std::endl;

        if (!test_buffer_ || !test_region_ || !raw_target_) {
            std::cerr << "Prerequisites not met" << std::endl;
            return false;
        }

        IRawDevice* raw_device = coordinator_->get_raw_device();

        // Prepare test data
        uint32_t* write_data = static_cast<uint32_t*>(test_buffer_);
        const uint32_t test_pattern_base = 0xCAFEBABE;
        for (size_t i = 0; i < test_config_.buffer_size / sizeof(uint32_t); ++i) {
            write_data[i] = test_pattern_base + i;
        }

        // Write via raw device
        std::cout << "Writing " << test_config_.buffer_size << " bytes..." << std::endl;
        auto write_start = std::chrono::high_resolution_clock::now();

        bool write_result = raw_device->submit_write(
            raw_target_,
            test_region_,
            0,  // byte offset within the LBA range
            test_config_.buffer_size,
            AccelStream());  // Use coordinator's default stream

        auto write_end = std::chrono::high_resolution_clock::now();

        if (!write_result) {
            std::cerr << "Write failed" << std::endl;
            return false;
        }

        auto write_us = std::chrono::duration_cast<std::chrono::microseconds>(
            write_end - write_start).count();
        double write_bw = (test_config_.buffer_size / 1024.0 / 1024.0) /
                          (write_us / 1000000.0);

        std::cout << "✅ Write completed in " << write_us << " µs" << std::endl;
        std::cout << "   Bandwidth: " << write_bw << " MB/s" << std::endl;

        // Clear buffer for read verification
        std::memset(test_buffer_, 0, test_config_.buffer_size);

        // Read back
        std::cout << "Reading " << test_config_.buffer_size << " bytes..." << std::endl;
        auto read_start = std::chrono::high_resolution_clock::now();

        bool read_result = raw_device->submit_read(
            raw_target_,
            test_region_,
            0,
            test_config_.buffer_size,
            AccelStream());

        auto read_end = std::chrono::high_resolution_clock::now();

        if (!read_result) {
            std::cerr << "Read failed" << std::endl;
            return false;
        }

        auto read_us = std::chrono::duration_cast<std::chrono::microseconds>(
            read_end - read_start).count();
        double read_bw = (test_config_.buffer_size / 1024.0 / 1024.0) /
                         (read_us / 1000000.0);

        std::cout << "✅ Read completed in " << read_us << " µs" << std::endl;
        std::cout << "   Bandwidth: " << read_bw << " MB/s" << std::endl;

        // Verify data
        uint32_t* read_data = static_cast<uint32_t*>(test_buffer_);
        bool data_ok = true;
        size_t mismatches = 0;

        for (size_t i = 0; i < test_config_.buffer_size / sizeof(uint32_t); ++i) {
            if (read_data[i] != test_pattern_base + i) {
                if (mismatches == 0) {
                    std::cerr << "Data mismatch at offset " << (i * sizeof(uint32_t))
                              << ": expected " << std::hex << (test_pattern_base + i)
                              << ", got " << read_data[i] << std::dec << std::endl;
                }
                mismatches++;
                data_ok = false;
                if (mismatches >= 10) break;  // Limit error output
            }
        }

        if (data_ok) {
            std::cout << "✅ Data verified successfully" << std::endl;
        } else {
            std::cerr << "❌ Data verification failed (" << mismatches << " mismatches)" << std::endl;
            return false;
        }

        return true;
    }

    bool cleanup() {
        std::cout << "\n=== Cleaning Up ===" << std::endl;

        bool success = true;

        // Release raw target
        if (raw_target_) {
            IRawDevice* raw_device = coordinator_->get_raw_device();
            if (raw_device && !raw_device->release_raw_target(raw_target_)) {
                std::cerr << "Warning: Failed to release raw target" << std::endl;
                success = false;
            }
            raw_target_ = nullptr;
        }

        // Unregister buffer
        if (test_region_) {
            if (!coordinator_->unregister_buffer(test_region_)) {
                std::cerr << "Warning: Failed to unregister buffer" << std::endl;
                success = false;
            }
            test_region_ = nullptr;
        }

        // Free buffer
        if (test_buffer_) {
            accel_->free(test_buffer_, MemoryKind::PINNED_HOST);
            test_buffer_ = nullptr;
        }

        // Cleanup coordinator
        if (coordinator_) {
            coordinator_->cleanup();
            destroy_coordinator(coordinator_);
            coordinator_ = nullptr;
        }

        // Cleanup block storage
        if (block_storage_) {
            block_storage_->cleanup();
            delete block_storage_;
            block_storage_ = nullptr;
        }

        // Cleanup IO engine
        if (io_engine_) {
            delete io_engine_;
            io_engine_ = nullptr;
        }

        // Cleanup backend
        if (backend_) {
            backend_->cleanup();
            delete backend_;
            backend_ = nullptr;
        }

        // Cleanup device registry
        if (device_registry_) {
            // Note: device_registry is nullptr in the new architecture
            device_registry_ = nullptr;
        }

        // Cleanup accelerator
        if (accel_) {
            delete accel_;
            accel_ = nullptr;
        }

        std::cout << (success ? "✅" : "⚠️") << " Cleanup complete" << std::endl;
        return success;
    }

private:
    TestConfig test_config_;

    // Layer instances
    IAccelerator* accel_ = nullptr;
    IDeviceRegistry* device_registry_ = nullptr;
    backends::IBackendProvider* backend_ = nullptr;
    IIoEngine* io_engine_ = nullptr;
    block_storage::IBlockStorage* block_storage_ = nullptr;
    ICoordinator* coordinator_ = nullptr;

    // Test resources
    void* test_buffer_ = nullptr;
    MemoryRegion* test_region_ = nullptr;
    RawTargetHandle* raw_target_ = nullptr;
};

int main(int /*argc*/, char** /*argv*/) {
    std::cout << "===============================================" << std::endl;
    std::cout << "  Layer 6 Coordinator Hardware Basic Test" << std::endl;
    std::cout << "===============================================\n" << std::endl;

    HardwareTest test;

    if (!test.initialize()) {
        std::cerr << "\n❌ Initialization failed" << std::endl;
        return 1;
    }

    bool all_passed = true;

    if (!test.test_buffer_registration()) {
        std::cerr << "\n❌ Test 1 failed: Buffer registration" << std::endl;
        all_passed = false;
    }

    if (!test.test_raw_device_access()) {
        std::cerr << "\n❌ Test 2 failed: Raw device access" << std::endl;
        all_passed = false;
    }

    if (!test.test_write_read_cycle()) {
        std::cerr << "\n❌ Test 3 failed: Write/Read cycle" << std::endl;
        all_passed = false;
    }

    if (!test.cleanup()) {
        std::cerr << "\n❌ Cleanup had warnings" << std::endl;
        all_passed = false;
    }

    std::cout << "\n===============================================" << std::endl;
    if (all_passed) {
        std::cout << "  ✅ ALL TESTS PASSED" << std::endl;
    } else {
        std::cout << "  ❌ SOME TESTS FAILED" << std::endl;
    }
    std::cout << "===============================================" << std::endl;

    return all_passed ? 0 : 1;
}
