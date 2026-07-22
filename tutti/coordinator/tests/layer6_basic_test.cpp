#include "../include/coordinator.h"
#include "../include/coordinator_types.h"
#include "../include/raw_device.h"
#include <iostream>
#include <memory>

// Basic smoke test that verifies:
// 1. Headers compile
// 2. Types are well-formed
// 3. Factory functions work
// 4. Interfaces can be instantiated

using namespace tutti::coordinator;

int main() {
    std::cout << "=== Layer 6 Coordinator Basic Smoke Test ===" << std::endl;

    // Test 1: Verify types compile
    std::cout << "[1/5] Testing type definitions..." << std::endl;
    {
        RawTargetHandle handle;
        handle.namespace_id = 1;
        handle.start_lba = 0;
        handle.length_blocks = 1000;
        handle.region_id = 1;

        BatchSubmitResult result;
        result.success = true;
        result.completed_count = 10;
        result.failed_count = 0;
        result.error_code = 0;

        std::cout << "  ✓ RawTargetHandle, BatchSubmitResult" << std::endl;
    }

    // Test 2: Verify config types
    std::cout << "[2/5] Testing config types..." << std::endl;
    {
        CoordinatorConfig config;
        config.backend_provider = nullptr;
        config.accelerator = nullptr;
        config.block_storage = nullptr;
        config.io_engine = nullptr;
        config.max_batch_size = 128;

        std::cout << "  ✓ CoordinatorConfig" << std::endl;
    }

    // Test 3: Verify IoRequest from coordinator perspective
    std::cout << "[3/5] Testing IoRequest..." << std::endl;
    {
        tutti::IoRequest req;
        req.region = nullptr;
        req.byte_offset = 0;
        req.byte_length = 4096;

        std::cout << "  ✓ IoRequest" << std::endl;
    }

    // Test 4: Verify factory functions exist
    std::cout << "[4/5] Testing factory functions..." << std::endl;
    {
        ICoordinator* coordinator = create_coordinator();
        if (!coordinator) {
            std::cerr << "  ✗ create_coordinator() returned nullptr" << std::endl;
            return 1;
        }
        std::cout << "  ✓ create_coordinator()" << std::endl;

        destroy_coordinator(coordinator);
        std::cout << "  ✓ destroy_coordinator()" << std::endl;
    }

    // Test 5: Verify interface methods compile (not callable without deps)
    std::cout << "[5/5] Testing interface signatures..." << std::endl;
    {
        // Just verify the interfaces compile - we can't call methods without real dependencies
        std::cout << "  ✓ ICoordinator interface compiles" << std::endl;
        std::cout << "  ✓ IRawDevice interface compiles" << std::endl;
    }

    std::cout << "\n=== All Basic Tests Passed ===" << std::endl;
    std::cout << "Coordinator library is properly built and linked." << std::endl;

    return 0;
}
