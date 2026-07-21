/**
 * layer2_smoke_test.cpp - Layer 2 (Device Manager) Smoke Test
 *
 * Tests the complete Layer 2 stack without requiring actual NVMe hardware.
 * Uses mock implementations to verify interfaces and allocation logic.
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>

// Layer 2 common interfaces
#include "common/device.h"
#include "common/vdevice.h"
#include "common/virtual_nvme.h"
#include "common/device_registry.h"
#include "common/lease_manager.h"

// Layer 2 NVMe implementation (root: nvme/include)
#include "local_nvme_device.h"
#include "nvme_queue_group.h"
#include "local_nvme_virtual.h"

// Mock types forward declarations
struct MockQueuePair {
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t ns_id;
};

// =============================================================================
// Mock Implementations
// =============================================================================

namespace tutti {

// Mock NvmeQueueGroup for testing
class MockNvmeQueueGroup : public NvmeQueueGroup {
public:
    MockNvmeQueueGroup(uint32_t n_qps) {
        // Allocate mock queue pairs
        mock_qps_ = new MockQueuePair[n_qps];
        for (uint32_t i = 0; i < n_qps; ++i) {
            mock_qps_[i].sq_tail = 0;
            mock_qps_[i].cq_head = 0;
            mock_qps_[i].ns_id = 1;
        }
        // Set base class members (now protected)
        n_qps_ = n_qps;
        d_qps_ = reinterpret_cast<void*>(mock_qps_);
    }

    ~MockNvmeQueueGroup() {
        delete[] mock_qps_;
    }

private:
    MockQueuePair* mock_qps_;
};

// Mock Device Registry for testing
class MockDeviceRegistry : public IDeviceRegistry {
public:
    MockDeviceRegistry() : opened_(false) {}

    ~MockDeviceRegistry() override {
        Close();
    }

    bool Open() override {
        if (opened_) return false;

        // Create mock devices
        for (int i = 0; i < 2; ++i) {
            auto* mock_dev = new Device();
            mock_dev->device_id = i;
            mock_dev->backend_type = BackendType::LOCAL_NVME;
            mock_dev->pci_addr = "0000:00:00.0";
            mock_dev->display_name = "Mock NVMe Device";

            // Create mock LocalNvmeDevice
            auto* ldev = new LocalNvmeDevice();
            ldev->device_id = i;
            ldev->ctrl = nullptr; // Mock - no real controller
            ldev->queue_group = new MockNvmeQueueGroup(16); // 16 queue pairs
            ldev->namespace_id = 1;
            ldev->blk_size = 4096;
            ldev->blk_size_log = 12;
            ldev->max_data_size = 1024 * 1024; // 1MB MDTS

            mock_dev->backend_private = ldev;
            devices_.push_back(mock_dev);
            local_devices_.push_back(ldev);
        }

        opened_ = true;
        return true;
    }

    void Close() override {
        if (!opened_) return;

        for (auto* ldev : local_devices_) {
            delete ldev->queue_group;
            delete ldev;
        }
        local_devices_.clear();

        for (auto* dev : devices_) {
            delete dev;
        }
        devices_.clear();

        opened_ = false;
    }

    int device_count() const override {
        return static_cast<int>(devices_.size());
    }

    const Device* device_at(int index) const override {
        if (index < 0 || index >= device_count()) {
            return nullptr;
        }
        return devices_[index];
    }

    const Device* find_by_id(uint32_t device_id) const override {
        for (const auto* dev : devices_) {
            if (dev->device_id == static_cast<int32_t>(device_id)) {
                return dev;
            }
        }
        return nullptr;
    }

    std::vector<const Device*> list() const override {
        return std::vector<const Device*>(devices_.begin(), devices_.end());
    }

private:
    bool opened_;
    std::vector<Device*> devices_;
    std::vector<LocalNvmeDevice*> local_devices_;
};

} // namespace tutti

// =============================================================================
// Test Functions
// =============================================================================

namespace {

int g_test_passed = 0;
int g_test_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "❌ FAILED: " << message << std::endl; \
            std::cerr << "   at " << __FILE__ << ":" << __LINE__ << std::endl; \
            g_test_failed++; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(test_func) \
    do { \
        std::cout << "\n[TEST] " << #test_func << std::endl; \
        if (test_func()) { \
            std::cout << "✅ PASS: " << #test_func << std::endl; \
            g_test_passed++; \
        } else { \
            g_test_failed++; \
        } \
    } while (0)

// Test 1: MockDeviceRegistry basic operations
bool test_mock_registry() {
    tutti::MockDeviceRegistry registry;

    // Test Open
    TEST_ASSERT(registry.Open(), "Registry should open successfully");
    TEST_ASSERT(registry.device_count() == 2, "Should have 2 mock devices");

    // Test device_at
    const tutti::Device* dev0 = registry.device_at(0);
    TEST_ASSERT(dev0 != nullptr, "device_at(0) should return valid device");
    TEST_ASSERT(dev0->device_id == 0, "Device 0 should have ID 0");

    const tutti::Device* dev1 = registry.device_at(1);
    TEST_ASSERT(dev1 != nullptr, "device_at(1) should return valid device");
    TEST_ASSERT(dev1->device_id == 1, "Device 1 should have ID 1");

    // Test find_by_id
    const tutti::Device* found = registry.find_by_id(0);
    TEST_ASSERT(found != nullptr, "find_by_id(0) should find device");
    TEST_ASSERT(found->device_id == 0, "Found device should have correct ID");

    // Test list
    auto devices = registry.list();
    TEST_ASSERT(devices.size() == 2, "list() should return 2 devices");

    // Test Close
    registry.Close();
    TEST_ASSERT(registry.device_count() == 0, "Should have 0 devices after close");

    return true;
}

// Test 2: VDevice structure
bool test_vdevice_struct() {
    tutti::VDevice vdev;

    // Test field assignment
    vdev.phys_device_id = 0;
    vdev.vdev_id = 0;
    vdev.d_qps = nullptr;
    vdev.queue_quota = 4;
    vdev.namespace_id = 1;
    vdev.blk_size = 4096;
    vdev.blk_size_log = 12;
    vdev.max_data_size = 1024 * 1024;
    vdev.caps = 0x1; // GPUDIRECT_CAPABLE

    TEST_ASSERT(vdev.phys_device_id == 0, "phys_device_id should be 0");
    TEST_ASSERT(vdev.queue_quota == 4, "queue_quota should be 4");
    TEST_ASSERT(vdev.blk_size == 4096, "blk_size should be 4096");
    TEST_ASSERT(vdev.caps & 0x1, "GPUDIRECT capability should be set");

    return true;
}

// Test 3: LocalNvmeVirtualRegistry allocation
bool test_allocation_basic() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);

    // Test initial available queues
    uint32_t available = allocator.available_queues(0);
    TEST_ASSERT(available == 16, "Should have 16 queues available initially");

    // Test allocation
    std::string error;
    tutti::VDevice* vdev = allocator.open_vdevice(0, 4, &error);
    TEST_ASSERT(vdev != nullptr, "Should allocate vDevice successfully");
    TEST_ASSERT(error.empty(), "Should have no error message");

    if (vdev) {
        TEST_ASSERT(vdev->phys_device_id == 0, "vDevice should have correct phys_device_id");
        TEST_ASSERT(vdev->queue_quota == 4, "vDevice should have quota of 4");
        TEST_ASSERT(vdev->d_qps != nullptr, "vDevice should have valid d_qps pointer");
        TEST_ASSERT(vdev->namespace_id == 1, "vDevice should have namespace_id 1");
        TEST_ASSERT(vdev->blk_size == 4096, "vDevice should have blk_size 4096");
    }

    // Test available queues after allocation
    available = allocator.available_queues(0);
    TEST_ASSERT(available == 12, "Should have 12 queues available after allocation");

    // Test deallocation
    allocator.close_vdevice(vdev);
    available = allocator.available_queues(0);
    TEST_ASSERT(available == 16, "Should have 16 queues available after deallocation");

    registry.Close();
    return true;
}

// Test 4: Multiple allocations
bool test_allocation_multiple() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);

    std::vector<tutti::VDevice*> vdevs;
    std::string error;

    // Allocate 4 vDevices with 4 queues each (16 total)
    for (int i = 0; i < 4; ++i) {
        tutti::VDevice* vdev = allocator.open_vdevice(0, 4, &error);
        TEST_ASSERT(vdev != nullptr, "Should allocate vDevice");
        if (vdev) vdevs.push_back(vdev);
    }

    // Should have exhausted all queues
    uint32_t available = allocator.available_queues(0);
    TEST_ASSERT(available == 0, "Should have 0 queues available");

    // Try to allocate one more (should fail)
    tutti::VDevice* vdev = allocator.open_vdevice(0, 4, &error);
    TEST_ASSERT(vdev == nullptr, "Should fail to allocate when pool exhausted");
    TEST_ASSERT(!error.empty(), "Should have error message");

    // Deallocate middle vDevice
    if (vdevs.size() > 1) {
        allocator.close_vdevice(vdevs[1]);
        available = allocator.available_queues(0);
        TEST_ASSERT(available == 4, "Should have 4 queues available after deallocation");
    }

    // Cleanup
    for (size_t i = 0; i < vdevs.size(); ++i) {
        if (i != 1) { // Skip already closed
            allocator.close_vdevice(vdevs[i]);
        }
    }

    registry.Close();
    return true;
}

// Test 5: Allocation failures
bool test_allocation_failures() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);
    std::string error;

    // Test zero quota
    tutti::VDevice* vdev = allocator.open_vdevice(0, 0, &error);
    TEST_ASSERT(vdev == nullptr, "Should fail with zero quota");
    TEST_ASSERT(error == "quota is zero", "Should have correct error message");

    // Test invalid device ID
    error.clear();
    vdev = allocator.open_vdevice(999, 4, &error);
    TEST_ASSERT(vdev == nullptr, "Should fail with invalid device ID");
    TEST_ASSERT(error == "phys_device_id not found", "Should have correct error message");

    // Test quota larger than available
    error.clear();
    vdev = allocator.open_vdevice(0, 100, &error);
    TEST_ASSERT(vdev == nullptr, "Should fail with excessive quota");
    TEST_ASSERT(error.find("insufficient") != std::string::npos,
                "Error should mention insufficient queues");

    registry.Close();
    return true;
}

// Test 6: Multi-device allocation
bool test_multi_device() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);
    std::string error;

    // Allocate from device 0
    tutti::VDevice* vdev0 = allocator.open_vdevice(0, 8, &error);
    TEST_ASSERT(vdev0 != nullptr, "Should allocate from device 0");
    if (vdev0) {
        TEST_ASSERT(vdev0->phys_device_id == 0, "vDevice should be from device 0");
    }

    // Allocate from device 1
    tutti::VDevice* vdev1 = allocator.open_vdevice(1, 8, &error);
    TEST_ASSERT(vdev1 != nullptr, "Should allocate from device 1");
    if (vdev1) {
        TEST_ASSERT(vdev1->phys_device_id == 1, "vDevice should be from device 1");
    }

    // Check available queues per device
    TEST_ASSERT(allocator.available_queues(0) == 8, "Device 0 should have 8 queues left");
    TEST_ASSERT(allocator.available_queues(1) == 8, "Device 1 should have 8 queues left");

    // Check pointer ranges are different
    if (vdev0 && vdev1) {
        TEST_ASSERT(vdev0->d_qps != vdev1->d_qps,
                   "Different devices should have different queue pointers");
    }

    allocator.close_vdevice(vdev0);
    allocator.close_vdevice(vdev1);

    registry.Close();
    return true;
}

// Test 7: Capabilities query
bool test_capabilities() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);

    // Query caps
    uint32_t caps = allocator.caps(0);
    TEST_ASSERT(caps & 0x1, "Device should have GPUDIRECT capability");

    // Invalid device
    caps = allocator.caps(999);
    TEST_ASSERT(caps == 0, "Invalid device should return 0 caps");

    registry.Close();
    return true;
}

// Test 8: Null pointer handling
bool test_null_handling() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);

    // close_vdevice with nullptr should be no-op (not crash)
    allocator.close_vdevice(nullptr);

    registry.Close();
    return true;
}

} // anonymous namespace

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Layer 2 (Device Manager) Smoke Test" << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(test_mock_registry);
    RUN_TEST(test_vdevice_struct);
    RUN_TEST(test_allocation_basic);
    RUN_TEST(test_allocation_multiple);
    RUN_TEST(test_allocation_failures);
    RUN_TEST(test_multi_device);
    RUN_TEST(test_capabilities);
    RUN_TEST(test_null_handling);

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Passed: " << g_test_passed << std::endl;
    std::cout << "Failed: " << g_test_failed << std::endl;
    std::cout << "Total:  " << (g_test_passed + g_test_failed) << std::endl;

    if (g_test_failed == 0) {
        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n❌ Some tests failed!" << std::endl;
        return 1;
    }
}
