/**
 * layer6_smoke_test_simple.cpp - Simple Layer 6 Smoke Test
 *
 * Tests coordinator interface compilation and basic API surface without
 * requiring full hardware stack initialization.
 */

#include <iostream>
#include <cstring>

// Layer 6 Coordinator
#include "coordinator.h"
#include "coordinator_types.h"
#include "raw_device.h"

using namespace tutti::coordinator;

int main() {
    std::cout << "=== Layer 6 Coordinator Simple Smoke Test ===" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    // Test 1: CoordinatorConfig structure
    std::cout << "[1/6] CoordinatorConfig structure..." << std::endl;
    {
        CoordinatorConfig config;
        config.backend_provider = nullptr;
        config.accelerator = nullptr;
        config.block_storage = nullptr;
        config.io_engine = nullptr;
        config.max_batch_size = 256;

        if (!config.is_valid()) {
            std::cout << "  ✓ is_valid() correctly returns false for incomplete config" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ is_valid() should return false" << std::endl;
            failed++;
        }
    }

    // Test 2: RawTargetHandle structure
    std::cout << "[2/6] RawTargetHandle structure..." << std::endl;
    {
        RawTargetHandle handle(1, 1000, 2000);

        if (handle.namespace_id == 1 &&
            handle.start_lba == 1000 &&
            handle.length_blocks == 2000) {
            std::cout << "  ✓ Constructor initializes fields correctly" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ Constructor failed" << std::endl;
            failed++;
        }
    }

    // Test 3: BatchSubmitResult structure
    std::cout << "[3/6] BatchSubmitResult structure..." << std::endl;
    {
        BatchSubmitResult result(true, 10, 0, 0);

        if (result.success &&
            result.completed_count == 10 &&
            result.failed_count == 0 &&
            result.error_code == 0) {
            std::cout << "  ✓ Constructor sets fields correctly" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ Constructor failed" << std::endl;
            failed++;
        }
    }

    // Test 4: IoRequest structure
    std::cout << "[4/6] IoRequest from coordinator namespace..." << std::endl;
    {
        IoRequest req;
        req.region = nullptr;
        req.target_handle = nullptr;
        req.byte_offset = 4096;
        req.byte_length = 8192;

        if (req.byte_offset == 4096 && req.byte_length == 8192) {
            std::cout << "  ✓ IoRequest fields accessible" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ IoRequest assignment failed" << std::endl;
            failed++;
        }
    }

    // Test 5: Factory functions exist
    std::cout << "[5/6] Factory functions..." << std::endl;
    {
        ICoordinator* coordinator = create_coordinator();
        if (coordinator) {
            std::cout << "  ✓ create_coordinator() returns non-null" << std::endl;
            destroy_coordinator(coordinator);
            std::cout << "  ✓ destroy_coordinator() completed" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ create_coordinator() returned null" << std::endl;
            failed++;
        }
    }

    // Test 6: NamespaceInfo structure
    std::cout << "[6/6] NamespaceInfo structure..." << std::endl;
    {
        NamespaceInfo info(1, 4096, 1000000, 131072);

        if (info.namespace_id == 1 &&
            info.block_size == 4096 &&
            info.capacity_blocks == 1000000 &&
            info.mdts_bytes == 131072) {
            std::cout << "  ✓ Constructor initializes fields correctly" << std::endl;
            passed++;
        } else {
            std::cout << "  ✗ Constructor failed" << std::endl;
            failed++;
        }
    }

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << (passed + failed) << std::endl;
    std::cout << "Failed: " << failed << "/" << (passed + failed) << std::endl;
    std::cout << std::endl;

    if (failed == 0) {
        std::cout << "✅ All smoke tests passed" << std::endl;
        std::cout << "Coordinator types and factory functions are correctly defined." << std::endl;
        return 0;
    } else {
        std::cout << "❌ Some tests failed" << std::endl;
        return 1;
    }
}
