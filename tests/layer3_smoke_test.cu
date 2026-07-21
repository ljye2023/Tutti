// tests/layer3_smoke_test.cu -- Layer 3 Backends SPI Smoke Tests (Simplified)
//
// Validates core IBackendProvider functionality:
//   - Backend factory (creation, registration)
//   - Backend metadata queries
//
// This is a simplified version that tests Layer 3 without full Layer 2 integration.
// Full integration tests will be added once Layer 2 API is finalized.

#include "backends/include/backend_provider.h"
#include "backends/include/backend_factory.h"
#include "backends/include/backend_types.h"
#include "backends/include/storage_target.h"
#include "backends/nvme/include/nvme_backend.h"
#include "backends/nvme/include/nvme_command_builder.h"
#include "backends/nvme/include/nvme_target_handle.h"
#include "backends/nvme/include/prp_page_cache.h"

#include <cstdio>
#include <cstring>
#include <memory>

using namespace tutti::backends;
using namespace tutti::backends::nvme;

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
// Test 1: Backend Factory - Create NVMe Backend
// ---------------------------------------------------------------------------
bool test_backend_factory() {
    printf("\n[TEST] Backend factory creation\n");

    auto backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);
    TEST_ASSERT(backend != nullptr, "Failed to create LOCAL_NVME backend");

    printf("  Backend type: %d\n", static_cast<int>(backend->backend_type()));
    printf("  Backend name: %s\n", backend->backend_name());

    TEST_ASSERT(backend->backend_type() == BackendType::LOCAL_NVME,
                "Backend type mismatch");
    TEST_ASSERT(strcmp(backend->backend_name(), "local_nvme") == 0,
                "Backend name mismatch");

    // backend will be automatically deleted by unique_ptr
    TEST_PASS("Backend factory creation succeeded");
}

// ---------------------------------------------------------------------------
// Test 2: Backend Metadata Queries (without initialization)
// ---------------------------------------------------------------------------
bool test_backend_metadata() {
    printf("\n[TEST] Backend metadata queries\n");

    auto backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);
    TEST_ASSERT(backend != nullptr, "Failed to create backend");

    // Query metadata before initialization (should work)
    BackendType type = backend->backend_type();
    const char* name = backend->backend_name();
    BackendMetadata metadata = backend->metadata();

    printf("  Backend type: %d\n", static_cast<int>(type));
    printf("  Backend name: %s\n", name);
    printf("  Metadata name: %s\n", metadata.name);
    printf("  Metadata type: %d\n", static_cast<int>(metadata.type));
    printf("  Capabilities: 0x%x\n", metadata.capabilities);
    printf("  Max IO size: %zu bytes\n", metadata.max_io_size);
    printf("  Max batch size: %zu\n", metadata.max_batch_size);
    printf("  Alignment: %zu bytes\n", metadata.alignment_bytes);

    TEST_ASSERT(type == BackendType::LOCAL_NVME, "Type mismatch");
    TEST_ASSERT(strcmp(name, "local_nvme") == 0, "Name mismatch");
    TEST_ASSERT(strcmp(metadata.name, "local_nvme") == 0, "Metadata name mismatch");
    TEST_ASSERT(metadata.type == BackendType::LOCAL_NVME, "Metadata type mismatch");

    // backend will be automatically deleted by unique_ptr
    TEST_PASS("Backend metadata queries succeeded");
}

// ---------------------------------------------------------------------------
// Test 3: StorageTarget Creation
// ---------------------------------------------------------------------------
bool test_storage_target() {
    printf("\n[TEST] StorageTarget creation\n");

    // Create a RAW NVMe storage target
    StorageTarget target;
    target.kind = StorageTargetKind::NVME_RAW;
    target.target_id = 1;
    target.logical_size_bytes = 1024 * 1024;  // 1MB
    target.namespace_id = 1;
    target.nvme_block_size = 4096;
    target.nvme_block_size_log = 12;
    target.start_lba = 100000;
    target.length_blocks = 256;
    target.num_extents = 0;
    target.extents = nullptr;

    printf("  Target kind: %d\n", static_cast<int>(target.kind));
    printf("  Target ID: %lu\n", target.target_id);
    printf("  Logical size: %lu bytes\n", target.logical_size_bytes);
    printf("  Namespace ID: %u\n", target.namespace_id);
    printf("  Block size: %u bytes\n", target.nvme_block_size);
    printf("  Start LBA: %lu\n", target.start_lba);
    printf("  Length: %lu blocks\n", target.length_blocks);

    TEST_ASSERT(target.kind == StorageTargetKind::NVME_RAW, "Kind mismatch");
    TEST_ASSERT(target.nvme_block_size == 4096, "Block size mismatch");
    TEST_ASSERT(target.length_blocks == 256, "Length mismatch");

    TEST_PASS("StorageTarget creation succeeded");
}

// ---------------------------------------------------------------------------
// Test 4: BackendTypes Structures
// ---------------------------------------------------------------------------
bool test_backend_types() {
    printf("\n[TEST] Backend types structures\n");

    // Test SubSliceInfo
    SubSliceInfo slice;
    slice.offset_bytes = 0;
    slice.length_bytes = 4096;
    slice.slice_index = 0;

    printf("  SubSliceInfo: offset=%lu, length=%u, index=%u\n",
           slice.offset_bytes, slice.length_bytes, slice.slice_index);

    // Test BufferDescriptor
    BufferDescriptor desc;
    desc.prp1 = 0x1000;
    desc.prp2 = 0x2000;
    desc.storage_offset = 0;
    desc.data_length = 4096;
    desc.descriptor_flags = 0;
    desc.backend_private = nullptr;

    printf("  BufferDescriptor: prp1=0x%lx, prp2=0x%lx, length=%u\n",
           desc.prp1, desc.prp2, desc.data_length);

    // Test SubmissionResult
    SubmissionResult result;
    result.success = true;
    result.completed_count = 10;
    result.failed_count = 0;
    result.error_code = 0;

    printf("  SubmissionResult: success=%d, completed=%u, failed=%u\n",
           result.success, result.completed_count, result.failed_count);

    TEST_ASSERT(slice.length_bytes == 4096, "Slice length mismatch");
    TEST_ASSERT(desc.data_length == 4096, "Descriptor length mismatch");
    TEST_ASSERT(result.success == true, "Result success mismatch");

    TEST_PASS("Backend types structures succeeded");
}

// ---------------------------------------------------------------------------
// Test 5: Multiple Backend Instances
// ---------------------------------------------------------------------------
bool test_multiple_backends() {
    printf("\n[TEST] Multiple backend instances\n");

    auto backend1 = BackendFactory::create_backend(BackendType::LOCAL_NVME);
    auto backend2 = BackendFactory::create_backend(BackendType::LOCAL_NVME);

    TEST_ASSERT(backend1 != nullptr, "Failed to create backend1");
    TEST_ASSERT(backend2 != nullptr, "Failed to create backend2");
    TEST_ASSERT(backend1.get() != backend2.get(), "Backends should be different instances");

    printf("  Backend1: %p, name=%s\n", backend1.get(), backend1->backend_name());
    printf("  Backend2: %p, name=%s\n", backend2.get(), backend2->backend_name());

    // backends will be automatically deleted by unique_ptr
    TEST_PASS("Multiple backend instances succeeded");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    printf("=======================================================\n");
    printf("Layer 3 Backend SPI Smoke Tests (Simplified)\n");
    printf("=======================================================\n");
    printf("Note: Full integration tests require Layer 2 setup\n");
    printf("This test validates Layer 3 APIs independently\n");
    printf("=======================================================\n");

    // Run tests
    test_backend_factory();
    test_backend_metadata();
    test_storage_target();
    test_backend_types();
    test_multiple_backends();

    // Summary
    printf("\n=======================================================\n");
    printf("Test Summary\n");
    printf("=======================================================\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);
    printf("=======================================================\n");

    if (g_tests_failed == 0) {
        printf("\n✅ All Layer 3 API tests PASSED\n");
        printf("\nNext steps:\n");
        printf("  1. Setup Layer 2 (Device Manager) with /dev/nvme1n1\n");
        printf("  2. Run full integration test with VDevice\n");
        printf("  3. Test descriptor preparation and submission\n");
    }

    return (g_tests_failed == 0) ? 0 : 1;
}
