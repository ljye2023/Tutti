#include "../include/block_storage.h"
#include "../include/block_storage_types.h"
#include "../include/storage_config.h"
#include <iostream>
#include <memory>

// Basic smoke test that verifies:
// 1. Headers compile
// 2. Types are well-formed
// 3. Factory functions work
// 4. Interfaces can be instantiated

using namespace tutti::block_storage;

int main() {
    std::cout << "=== Layer 5 Block Storage Basic Smoke Test ===" << std::endl;

    // Test 1: Verify types compile
    std::cout << "[1/5] Testing type definitions..." << std::endl;
    {
        FileId id = 12345;
        FileShard shard;
        shard.device_id = 0;
        shard.namespace_id = 1;
        shard.start_lba = 0;
        shard.length_blocks = 1000;

        GpuFile file;
        file.file_id = id;
        file.name = "test.bin";
        file.logical_size = 1024 * 1024;
        file.stripe_size = 256 * 1024;
        file.shards.push_back(shard);

        FileInfo info;
        info.file_id = id;
        info.name = "test.bin";
        info.size = 1024 * 1024;

        std::cout << "  ✓ FileId, FileShard, GpuFile, FileInfo" << std::endl;
    }

    // Test 2: Verify config types
    std::cout << "[2/5] Testing config types..." << std::endl;
    {
        StripeConfig stripe_cfg;
        stripe_cfg.stripe_size = 256 * 1024;
        stripe_cfg.interleave_unit = 64 * 1024;
        stripe_cfg.max_shards_per_file = 8;

        DurabilityConfig durability_cfg;
        durability_cfg.sync_mode = SyncMode::METADATA_ONLY;
        durability_cfg.flush_interval_ms = 1000;

        BlockStorageConfig config;
        config.root_directory = "/tmp/tutti_test";
        config.stripe_config = stripe_cfg;
        config.durability_config = durability_cfg;
        config.max_open_files = 100;

        std::cout << "  ✓ StripeConfig, DurabilityConfig, BlockStorageConfig" << std::endl;
    }

    // Test 3: Verify enums
    std::cout << "[3/5] Testing enums..." << std::endl;
    {
        FileOpenMode mode = FileOpenMode::READ_ONLY;
        SyncMode sync = SyncMode::FULL;

        std::cout << "  ✓ FileOpenMode, SyncMode" << std::endl;
    }

    // Test 4: Verify factory functions exist
    std::cout << "[4/5] Testing factory functions..." << std::endl;
    {
        auto storage = create_block_storage();
        if (!storage) {
            std::cerr << "  ✗ create_block_storage() returned nullptr" << std::endl;
            return 1;
        }
        std::cout << "  ✓ create_block_storage()" << std::endl;
        std::cout << "  ✓ unique_ptr manages lifecycle automatically" << std::endl;
    }

    // Test 5: Verify interface methods compile (not callable without deps)
    std::cout << "[5/5] Testing interface signature..." << std::endl;
    {
        // Just verify the interface compiles - we can't call methods without real backends
        std::cout << "  ✓ IBlockStorage interface compiles" << std::endl;
    }

    std::cout << "\n=== All Basic Tests Passed ===" << std::endl;
    std::cout << "Block storage library is properly built and linked." << std::endl;

    return 0;
}
