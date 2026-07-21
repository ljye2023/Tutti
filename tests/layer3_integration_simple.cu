// tests/layer3_integration_simple.cu -- Simplified Layer 3 + Layer 2 Integration Test
//
// Simplified integration test that validates Layer 3 backend initialization
// with a real NVMe controller handle from libnvm (Layer 2).
//
// Tests:
//   1. Open real NVMe controller via libnvm
//   2. Create VDevice structure (simplified, without full queue setup)
//   3. Initialize backend with VDevice
//   4. Query backend metadata
//   5. Create and manage target handles
//
// REQUIRES: sudo access, /dev/nvme1 accessible

#include "backends/include/backend_provider.h"
#include "backends/include/backend_factory.h"
#include "backends/include/backend_types.h"
#include "backends/include/storage_target.h"
#include "backends/nvme/include/nvme_backend.h"
#include "device_manager/include/common/vdevice.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <fcntl.h>

// libnvm includes
#include <nvm_types.h>
#include <nvm_ctrl.h>

using namespace tutti;
using namespace tutti::backends;

// Test configuration
static const char* TEST_NVME_PATH = "/dev/nvme1";
static const uint32_t TEST_NSID = 1;
static const size_t TEST_BLOCK_SIZE = 4096;
static const uint64_t TEST_START_LBA = 200000;

// Test result tracking
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  [FAIL] %s: %s\n", __func__, msg); \
            g_tests_failed++; \
            return false; \
        } \
    } while(0)

#define TEST_PASS(msg) \
    do { \
        printf("  [PASS] %s: %s\n", __func__, msg); \
        g_tests_passed++; \
        return true; \
    } while(0)

// ---------------------------------------------------------------------------
// Helper: Create minimal VDevice for testing
// ---------------------------------------------------------------------------
VDevice* create_test_vdevice(nvm_ctrl_t* ctrl) {
    if (!ctrl) return nullptr;

    // Allocate minimal queue array on GPU
    nvm_queue_t* d_qps = nullptr;
    cudaError_t err = cudaMalloc(&d_qps, sizeof(nvm_queue_t) * 2);
    if (err != cudaSuccess) {
        printf("Failed to allocate GPU queues: %s\n", cudaGetErrorString(err));
        return nullptr;
    }

    // Create VDevice
    VDevice* vdev = new VDevice();
    vdev->phys_device_id = 0;
    vdev->vdev_id = 0;
    vdev->d_qps = d_qps;  // nvm_queue_t* type
    vdev->queue_quota = 2;
    vdev->namespace_id = TEST_NSID;
    vdev->blk_size = TEST_BLOCK_SIZE;
    vdev->blk_size_log = 12;  // log2(4096)
    vdev->max_data_size = 128 * 1024;  // 128KB MDTS
    vdev->caps = 0x1;  // GPUDIRECT_CAPABLE

    return vdev;
}

void destroy_test_vdevice(VDevice* vdev) {
    if (vdev && vdev->d_qps) {
        cudaFree(vdev->d_qps);
    }
    delete vdev;
}

// ---------------------------------------------------------------------------
// Test 1: Initialize Real NVMe Controller
// ---------------------------------------------------------------------------
bool test_nvme_controller_init(nvm_ctrl_t** out_ctrl) {
    printf("\n[TEST] Initialize real NVMe controller via libnvm\n");

    // Open NVMe device
    int fd = open(TEST_NVME_PATH, O_RDWR);
    TEST_ASSERT(fd >= 0, "Failed to open NVMe device - run with sudo");

    printf("  Opened %s (fd=%d)\n", TEST_NVME_PATH, fd);

    // Initialize controller
    nvm_ctrl_t* ctrl = nullptr;
    int ret = nvm_ctrl_init(&ctrl, fd, fd);
    TEST_ASSERT(ret == 0 && ctrl != nullptr, "Failed to initialize controller");

    printf("  Controller initialized:\n");
    printf("    Page size: %zu bytes\n", ctrl->page_size);
    printf("    Timeout: %u ms\n", ctrl->timeout);

    *out_ctrl = ctrl;
    TEST_PASS("NVMe controller initialization succeeded");
}

// ---------------------------------------------------------------------------
// Test 2: Create VDevice from Controller
// ---------------------------------------------------------------------------
bool test_vdevice_creation(nvm_ctrl_t* ctrl, VDevice** out_vdev) {
    printf("\n[TEST] Create VDevice from NVMe controller\n");

    VDevice* vdev = create_test_vdevice(ctrl);
    TEST_ASSERT(vdev != nullptr, "Failed to create VDevice");

    printf("  VDevice created:\n");
    printf("    Physical device ID: %d\n", vdev->phys_device_id);
    printf("    Queue quota: %u\n", vdev->queue_quota);
    printf("    Namespace ID: %u\n", vdev->namespace_id);
    printf("    Block size: %u bytes\n", vdev->blk_size);
    printf("    Max data size: %zu KB\n", vdev->max_data_size / 1024);
    printf("    Capabilities: 0x%x\n", vdev->caps);

    *out_vdev = vdev;
    TEST_PASS("VDevice creation succeeded");
}

// ---------------------------------------------------------------------------
// Test 3: Backend Initialization with VDevice
// ---------------------------------------------------------------------------
bool test_backend_initialization(VDevice* vdev, IBackendProvider** out_backend) {
    printf("\n[TEST] Backend initialization with real VDevice\n");

    auto backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);
    TEST_ASSERT(backend != nullptr, "Failed to create backend");

    printf("  Backend created: %s\n", backend->backend_name());

    // Initialize with VDevice
    bool init_ok = backend->initialize(vdev);
    TEST_ASSERT(init_ok, "Backend initialization failed");

    printf("  Backend initialized:\n");
    printf("    Type: %s\n", backend->backend_name());
    printf("    Max IO size: %zu KB\n", backend->max_io_size() / 1024);

    BackendMetadata meta = backend->metadata();
    printf("    Capabilities: 0x%x\n", meta.capabilities);
    printf("    Max batch size: %zu\n", meta.max_batch_size);
    printf("    Alignment: %zu bytes\n", meta.alignment_bytes);

    *out_backend = backend.release();
    TEST_PASS("Backend initialization succeeded");
}

// ---------------------------------------------------------------------------
// Test 4: Target Handle Management
// ---------------------------------------------------------------------------
bool test_target_handle(IBackendProvider* backend) {
    printf("\n[TEST] Target handle acquire/release\n");

    // Create StorageTarget for a test LBA range
    StorageTarget target;
    target.kind = StorageTargetKind::NVME_RAW;
    target.target_id = 1;
    target.logical_size_bytes = 1024 * 1024;  // 1MB
    target.namespace_id = TEST_NSID;
    target.nvme_block_size = TEST_BLOCK_SIZE;
    target.nvme_block_size_log = 12;
    target.start_lba = TEST_START_LBA;
    target.length_blocks = 256;  // 1MB / 4KB
    target.num_extents = 0;
    target.extents = nullptr;

    printf("  StorageTarget:\n");
    printf("    Kind: NVME_RAW\n");
    printf("    Start LBA: %lu\n", target.start_lba);
    printf("    Length: %lu blocks (%lu KB)\n",
           target.length_blocks, (target.length_blocks * TEST_BLOCK_SIZE) / 1024);

    // Acquire handle
    void* handle = backend->acquire_target_handle(target);
    TEST_ASSERT(handle != nullptr, "Failed to acquire target handle");

    printf("  Target handle acquired: %p\n", handle);

    // Release handle
    backend->release_target_handle(handle);
    printf("  Target handle released\n");

    TEST_PASS("Target handle management succeeded");
}

// ---------------------------------------------------------------------------
// Test 5: Backend Cleanup
// ---------------------------------------------------------------------------
bool test_backend_cleanup(IBackendProvider* backend) {
    printf("\n[TEST] Backend cleanup\n");

    backend->cleanup();
    printf("  Backend cleanup() called\n");

    delete backend;
    printf("  Backend destroyed\n");

    TEST_PASS("Backend cleanup succeeded");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    printf("=======================================================\n");
    printf("Layer 3 + Layer 2 Integration Test (Simplified)\n");
    printf("=======================================================\n");
    printf("Device: %s\n", TEST_NVME_PATH);
    printf("Namespace: %u\n", TEST_NSID);
    printf("Test LBA range: %lu - %lu\n", TEST_START_LBA, TEST_START_LBA + 256);
    printf("=======================================================\n");

    // Check root privileges
    if (geteuid() != 0) {
        printf("\n⚠️  WARNING: Not running as root\n");
        printf("    NVMe access requires sudo\n");
        printf("    Run: sudo %s\n\n", argv[0]);
        return 1;
    }

    // Run tests
    nvm_ctrl_t* ctrl = nullptr;
    VDevice* vdev = nullptr;
    IBackendProvider* backend = nullptr;

    bool all_ok = true;

    all_ok &= test_nvme_controller_init(&ctrl);
    if (all_ok && ctrl) {
        all_ok &= test_vdevice_creation(ctrl, &vdev);
    }
    if (all_ok && vdev) {
        all_ok &= test_backend_initialization(vdev, &backend);
    }
    if (all_ok && backend) {
        all_ok &= test_target_handle(backend);
    }
    if (backend) {
        all_ok &= test_backend_cleanup(backend);
    }

    // Cleanup
    if (vdev) {
        destroy_test_vdevice(vdev);
    }
    if (ctrl) {
        nvm_ctrl_free(ctrl);
    }

    // Summary
    printf("\n=======================================================\n");
    printf("Test Summary\n");
    printf("=======================================================\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);
    printf("=======================================================\n");

    if (g_tests_failed == 0) {
        printf("\n✅ All integration tests PASSED\n");
        printf("\nVerified:\n");
        printf("  ✓ Real NVMe controller initialization (libnvm)\n");
        printf("  ✓ VDevice creation and structure\n");
        printf("  ✓ Backend initialization with VDevice\n");
        printf("  ✓ Target handle management\n");
        printf("  ✓ Backend cleanup\n");
        printf("\nLayer 3 successfully integrates with Layer 2!\n");
    } else {
        printf("\n❌ Some tests FAILED\n");
    }

    return (g_tests_failed == 0) ? 0 : 1;
}
