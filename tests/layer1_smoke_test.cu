// tests/layer1_smoke_test.cu -- Layer 1 Accelerator HAL Smoke Tests
//
// Validates core IAccelerator functionality:
//   - Memory allocation/deallocation (host, device, pinned)
//   - Memory registration and lookup
//   - DMA mapping (ioaddrs array verification)
//   - Stream lifecycle (create, sync, destroy)
//   - Event lifecycle (create, record, wait, query, destroy)
//   - Memcpy async (H->D, D->H, D->D)
//   - Simple kernel launch via IAccelerator::launch()
//   - Device management (get/set device)

#include "iaccel.h"
#include "memory_kind.h"
#include "memory_region.h"
#include "accel_types.h"
#include "cuda_accelerator.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace tutti;

// Simple test kernel for kernel launch validation
__global__ void simple_add_kernel(int* a, int* b, int* c, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        c[idx] = a[idx] + b[idx];
    }
}

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
// Test 1: Device Management
// ---------------------------------------------------------------------------
bool test_device_management(IAccelerator* accel) {
    printf("\n[TEST] Device management (get/set device)\n");

    int device_count = accel->device_count();
    TEST_ASSERT(device_count > 0, "No devices found");
    printf("  Found %d device(s)\n", device_count);

    int initial_device = accel->get_device();
    printf("  Initial device: %d\n", initial_device);

    bool set_result = accel->set_device(0);
    TEST_ASSERT(set_result, "Failed to set device 0");

    int current_device = accel->get_device();
    TEST_ASSERT(current_device == 0, "Device mismatch after set");

    TEST_PASS("Device management operations succeeded");
}

// ---------------------------------------------------------------------------
// Test 2: Host Memory Allocation
// ---------------------------------------------------------------------------
bool test_host_memory(IAccelerator* accel) {
    printf("\n[TEST] Host memory allocation and deallocation\n");

    const size_t size = 4096;
    void* host_ptr = accel->allocate_host(size, MemoryKind::HOST);
    TEST_ASSERT(host_ptr != nullptr, "Host allocation failed");

    // Write pattern to verify accessibility
    memset(host_ptr, 0xAB, size);
    unsigned char* bytes = static_cast<unsigned char*>(host_ptr);
    TEST_ASSERT(bytes[0] == 0xAB && bytes[size-1] == 0xAB,
                "Host memory not writable");

    accel->free(host_ptr, MemoryKind::HOST);

    TEST_PASS("Host memory lifecycle succeeded");
}

// ---------------------------------------------------------------------------
// Test 3: Device Memory Allocation
// ---------------------------------------------------------------------------
bool test_device_memory(IAccelerator* accel) {
    printf("\n[TEST] Device memory allocation and deallocation\n");

    const size_t size = 4096;
    void* device_ptr = accel->allocate_device(size, MemoryKind::DEVICE, 0);
    TEST_ASSERT(device_ptr != nullptr, "Device allocation failed");

    accel->free(device_ptr, MemoryKind::DEVICE);

    TEST_PASS("Device memory lifecycle succeeded");
}

// ---------------------------------------------------------------------------
// Test 4: Pinned Host Memory Allocation
// ---------------------------------------------------------------------------
bool test_pinned_memory(IAccelerator* accel) {
    printf("\n[TEST] Pinned host memory allocation\n");

    const size_t size = 8192;
    void* pinned_ptr = accel->allocate_host(size, MemoryKind::PINNED_HOST);
    TEST_ASSERT(pinned_ptr != nullptr, "Pinned host allocation failed");

    // Verify we can write to it
    memset(pinned_ptr, 0xCD, size);
    unsigned char* bytes = static_cast<unsigned char*>(pinned_ptr);
    TEST_ASSERT(bytes[0] == 0xCD, "Pinned memory not writable");

    accel->free(pinned_ptr, MemoryKind::PINNED_HOST);

    TEST_PASS("Pinned host memory lifecycle succeeded");
}

// ---------------------------------------------------------------------------
// Test 5: Memory Registration
// ---------------------------------------------------------------------------
bool test_memory_registration(IAccelerator* accel) {
    printf("\n[TEST] Memory registration and unregistration\n");

    const size_t size = 4096;
    void* host_ptr = accel->allocate_host(size, MemoryKind::PINNED_HOST);
    TEST_ASSERT(host_ptr != nullptr, "Allocation for registration test failed");

    MemoryRegion* region = accel->register_host(host_ptr, size);
    TEST_ASSERT(region != nullptr, "Host memory registration failed");
    TEST_ASSERT(region->host_ptr == host_ptr, "Region host_ptr mismatch");
    TEST_ASSERT(region->size == size, "Region size mismatch");
    TEST_ASSERT(region->kind == MemoryKind::HOST, "Region kind mismatch");

    // Lookup by pointer
    MemoryRegion* found = accel->lookup(host_ptr);
    TEST_ASSERT(found != nullptr, "Lookup by pointer failed");
    TEST_ASSERT(found->region_id == region->region_id, "Lookup returned wrong region");

    // Lookup by ID
    MemoryRegion* found_by_id = accel->lookup_by_id(region->region_id);
    TEST_ASSERT(found_by_id != nullptr, "Lookup by ID failed");
    TEST_ASSERT(found_by_id->region_id == region->region_id, "Lookup by ID returned wrong region");

    accel->unregister(region);
    accel->free(host_ptr, MemoryKind::PINNED_HOST);

    TEST_PASS("Memory registration lifecycle succeeded");
}

// ---------------------------------------------------------------------------
// Test 6: DMA Mapping
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Test 6: Stream Lifecycle
// ---------------------------------------------------------------------------
bool test_stream_lifecycle(IAccelerator* accel) {
    printf("\n[TEST] Stream lifecycle (create, sync, query, destroy)\n");

    AccelStream stream = accel->create_stream();
    TEST_ASSERT(stream.handle != nullptr, "Stream creation failed");

    // Enqueue a memcpy operation
    const size_t size = 4096;
    void* device_ptr = accel->allocate_device(size, MemoryKind::DEVICE, 0);
    void* host_ptr = accel->allocate_host(size, MemoryKind::PINNED_HOST);
    TEST_ASSERT(device_ptr != nullptr && host_ptr != nullptr,
                "Allocation for stream test failed");

    memset(host_ptr, 0x55, size);
    bool memcpy_result = accel->memcpy_async(device_ptr, host_ptr, size, stream);
    TEST_ASSERT(memcpy_result, "Async memcpy on stream failed");

    // Synchronize
    accel->synchronize_stream(stream);

    accel->free(device_ptr, MemoryKind::DEVICE);
    accel->free(host_ptr, MemoryKind::PINNED_HOST);
    accel->destroy_stream(stream);

    TEST_PASS("Stream lifecycle operations succeeded");
}

// ---------------------------------------------------------------------------
// Test 8: Event Lifecycle
// ---------------------------------------------------------------------------
bool test_event_lifecycle(IAccelerator* accel) {
    printf("\n[TEST] Event lifecycle (create, record, wait, query, destroy)\n");

    AccelEvent event = accel->create_event();
    TEST_ASSERT(event.handle != nullptr, "Event creation failed");

    AccelStream stream = accel->create_stream();
    TEST_ASSERT(stream.handle != nullptr, "Stream creation for event test failed");

    // Enqueue work and record event
    const size_t size = 4096;
    void* device_ptr = accel->allocate_device(size, MemoryKind::DEVICE, 0);
    void* host_ptr = accel->allocate_host(size, MemoryKind::PINNED_HOST);
    TEST_ASSERT(device_ptr != nullptr && host_ptr != nullptr,
                "Allocation for event test failed");

    memset(host_ptr, 0x66, size);
    bool memcpy_result = accel->memcpy_async(device_ptr, host_ptr, size, stream);
    TEST_ASSERT(memcpy_result, "Memcpy for event test failed");

    accel->record_event(event, stream);

    // Synchronize and query
    accel->synchronize_stream(stream);
    bool is_complete = accel->query_event(event);
    printf("  Event query result: %s\n", is_complete ? "complete" : "pending");

    accel->free(device_ptr, MemoryKind::DEVICE);
    accel->free(host_ptr, MemoryKind::PINNED_HOST);
    accel->destroy_event(event);
    accel->destroy_stream(stream);

    TEST_PASS("Event lifecycle operations succeeded");
}

// ---------------------------------------------------------------------------
// Test 9: Memcpy Async H<->D
// ---------------------------------------------------------------------------
bool test_memcpy_async(IAccelerator* accel) {
    printf("\n[TEST] Memcpy async (H->D, D->H)\n");

    const size_t size = 512 * sizeof(int);
    const int n = 512;

    int* host_src = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* host_dst = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* device_ptr = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));

    TEST_ASSERT(host_src && host_dst && device_ptr,
                "Memory allocation for memcpy test failed");

    // Initialize source
    for (int i = 0; i < n; i++) {
        host_src[i] = i * 7;
    }
    memset(host_dst, 0, size);

    AccelStream stream = accel->create_stream();

    // H->D
    bool h2d_result = accel->memcpy_async(device_ptr, host_src, size, stream);
    TEST_ASSERT(h2d_result, "H->D memcpy failed");

    // D->H
    bool d2h_result = accel->memcpy_async(host_dst, device_ptr, size, stream);
    TEST_ASSERT(d2h_result, "D->H memcpy failed");

    accel->synchronize_stream(stream);

    // Verify
    bool data_valid = true;
    for (int i = 0; i < n; i++) {
        if (host_dst[i] != i * 7) {
            data_valid = false;
            break;
        }
    }
    TEST_ASSERT(data_valid, "Memcpy data verification failed");

    accel->destroy_stream(stream);
    accel->free(host_src, MemoryKind::PINNED_HOST);
    accel->free(host_dst, MemoryKind::PINNED_HOST);
    accel->free(device_ptr, MemoryKind::DEVICE);

    TEST_PASS("Memcpy async H->D and D->H succeeded");
}

// ---------------------------------------------------------------------------
// Test 10: Memcpy D->D
// ---------------------------------------------------------------------------
bool test_memcpy_device_to_device(IAccelerator* accel) {
    printf("\n[TEST] Memcpy async (D->D)\n");

    const size_t size = 512 * sizeof(int);
    const int n = 512;

    int* host_buffer = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* device_src = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));
    int* device_dst = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));

    TEST_ASSERT(host_buffer != nullptr && device_src != nullptr && device_dst != nullptr,
                "Memory allocation for D->D test failed");

    // Initialize source on device via host
    for (int i = 0; i < n; i++) {
        host_buffer[i] = i * 3;
    }

    AccelStream stream = accel->create_stream();

    // H->D to initialize device_src
    accel->memcpy_async(device_src, host_buffer, size, stream);

    // D->D copy
    bool d2d_result = accel->memcpy_async(device_dst, device_src, size, stream);
    TEST_ASSERT(d2d_result, "D->D memcpy failed");

    // D->H to verify
    memset(host_buffer, 0, size);
    accel->memcpy_async(host_buffer, device_dst, size, stream);
    accel->synchronize_stream(stream);

    // Verify
    bool data_valid = true;
    for (int i = 0; i < n; i++) {
        if (host_buffer[i] != i * 3) {
            data_valid = false;
            break;
        }
    }
    TEST_ASSERT(data_valid, "D->D memcpy data verification failed");

    accel->destroy_stream(stream);
    accel->free(host_buffer, MemoryKind::PINNED_HOST);
    accel->free(device_src, MemoryKind::DEVICE);
    accel->free(device_dst, MemoryKind::DEVICE);

    TEST_PASS("Memcpy async D->D succeeded");
}

// ---------------------------------------------------------------------------
// Test 11: Device Memory Alignment + Leak Regression
// ---------------------------------------------------------------------------
// Regression test for the allocate_device()/free() bug where the 64KB-aligned
// sub-pointer handed to the caller was never mapped back to the underlying
// cudaMalloc'd pointer, so free() either leaked the real allocation (kind ==
// DEVICE, since the aligned pointer discarded) — this checks both that the
// returned pointer is actually 64KB-aligned and that repeated alloc/free
// cycles do not leak device memory.
bool test_device_memory_alignment_and_leak(IAccelerator* accel) {
    printf("\n[TEST] Device memory alignment + leak regression\n");

    const size_t size = 4096;
    constexpr uintptr_t kAlign = 65536;

    void* p = accel->allocate_device(size, MemoryKind::DEVICE, 0);
    TEST_ASSERT(p != nullptr, "Device allocation failed");
    TEST_ASSERT(((uintptr_t)p % kAlign) == 0,
                "Returned device pointer is not 64KB-aligned");
    accel->free(p, MemoryKind::DEVICE);

    // Repeated alloc/free must not leak: measure free device memory before
    // and after a loop of alloc/free cycles.
    size_t free_before = 0, total_before = 0;
    cudaMemGetInfo(&free_before, &total_before);

    const int iterations = 200;
    for (int i = 0; i < iterations; i++) {
        void* q = accel->allocate_device(size, MemoryKind::DEVICE, 0);
        TEST_ASSERT(q != nullptr, "Device allocation failed in leak loop");
        TEST_ASSERT(((uintptr_t)q % kAlign) == 0,
                    "Device pointer not aligned in leak loop");
        accel->free(q, MemoryKind::DEVICE);
    }

    size_t free_after = 0, total_after = 0;
    cudaMemGetInfo(&free_after, &total_after);

    // Allow a small slack for driver-side bookkeeping/fragmentation, but a
    // leak of `iterations * (size + kAlign)` bytes (~12.8MB here) would
    // massively exceed this.
    const long drift = (long)free_before - (long)free_after;
    printf("  Free device memory before: %zu, after: %zu, drift: %ld bytes\n",
           free_before, free_after, drift);
    TEST_ASSERT(drift < (long)(4 * kAlign),
                "Device memory leaked across repeated alloc/free cycles");

    TEST_PASS("Device memory alignment and leak regression succeeded");
}

// ---------------------------------------------------------------------------
// Test 12: Kernel Launch
// ---------------------------------------------------------------------------
bool test_kernel_launch(IAccelerator* accel) {
    printf("\n[TEST] Simple kernel launch via IAccelerator::launch()\n");

    const int n = 256;
    const size_t size = n * sizeof(int);

    // Allocate memory
    int* h_a = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* h_b = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* h_c = static_cast<int*>(accel->allocate_host(size, MemoryKind::PINNED_HOST));
    int* d_a = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));
    int* d_b = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));
    int* d_c = static_cast<int*>(accel->allocate_device(size, MemoryKind::DEVICE, 0));

    TEST_ASSERT(h_a && h_b && h_c && d_a && d_b && d_c,
                "Memory allocation for kernel test failed");

    // Initialize host data
    for (int i = 0; i < n; i++) {
        h_a[i] = i;
        h_b[i] = i * 2;
        h_c[i] = 0;
    }

    AccelStream stream = accel->create_stream();

    // Copy to device
    accel->memcpy_async(d_a, h_a, size, stream);
    accel->memcpy_async(d_b, h_b, size, stream);

    // Launch kernel
    tutti::Dim3 grid_dim(1, 1, 1);
    tutti::Dim3 block_dim(256, 1, 1);

    void* kernel_args[] = { &d_a, &d_b, &d_c, const_cast<int*>(&n) };

    accel->launch(
        reinterpret_cast<void*>(simple_add_kernel),
        grid_dim,
        block_dim,
        0,  // shared memory
        stream,
        kernel_args
    );

    // Copy result back
    accel->memcpy_async(h_c, d_c, size, stream);
    accel->synchronize_stream(stream);

    // Verify results
    bool results_valid = true;
    for (int i = 0; i < n; i++) {
        int expected = h_a[i] + h_b[i];
        if (h_c[i] != expected) {
            printf("  Kernel result mismatch at index %d: expected %d, got %d\n",
                   i, expected, h_c[i]);
            results_valid = false;
            break;
        }
    }
    TEST_ASSERT(results_valid, "Kernel computation results incorrect");

    // Cleanup
    accel->destroy_stream(stream);
    accel->free(h_a, MemoryKind::PINNED_HOST);
    accel->free(h_b, MemoryKind::PINNED_HOST);
    accel->free(h_c, MemoryKind::PINNED_HOST);
    accel->free(d_a, MemoryKind::DEVICE);
    accel->free(d_b, MemoryKind::DEVICE);
    accel->free(d_c, MemoryKind::DEVICE);

    TEST_PASS("Kernel launch and execution succeeded");
}

// ---------------------------------------------------------------------------
// Main Test Runner
// ---------------------------------------------------------------------------
int main() {
    printf("=======================================================\n");
    printf(" Layer 1 Accelerator HAL Smoke Tests\n");
    printf("=======================================================\n\n");

    // Create CUDA accelerator
    CudaAccelerator* accel = new CudaAccelerator();
    printf("Accelerator initialized successfully\n");

    // Run all tests
    test_device_management(accel);
    test_host_memory(accel);
    test_device_memory(accel);
    test_pinned_memory(accel);
    test_memory_registration(accel);
    test_device_memory_alignment_and_leak(accel);
    test_stream_lifecycle(accel);
    test_event_lifecycle(accel);
    test_memcpy_async(accel);
    test_memcpy_device_to_device(accel);
    test_kernel_launch(accel);

    // Clean up
    delete accel;

    // Print summary
    printf("\n=======================================================\n");
    printf(" Test Summary\n");
    printf("=======================================================\n");
    printf("  Passed: %d\n", g_tests_passed);
    printf("  Failed: %d\n", g_tests_failed);
    printf("  Total:  %d\n", g_tests_passed + g_tests_failed);
    printf("=======================================================\n\n");

    if (g_tests_failed == 0) {
        printf("✓ All tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed.\n");
        return 1;
    }
}
