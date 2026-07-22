# Block Storage Layer (Layer 5)

Layer 5 provides named, striped, persistent GPU file containers with metadata management and durability guarantees.

## Architecture

### Core Components

1. **FileDirectory** - In-memory file registry with persistent metadata
   - Name-based file lookup
   - Thread-safe concurrent access
   - Atomic file operations

2. **StripeManager** - Multi-device striping and load balancing
   - Round-robin device allocation
   - Configurable stripe size
   - LBA range management via backend provider

3. **MetadataJournal** - Write-ahead logging for crash recovery
   - Append-only journal for operations (CREATE, DELETE, RESIZE)
   - Periodic checkpointing
   - Recovery on initialization

4. **BlockStorageImpl** - Main implementation of IBlockStorage interface
   - File lifecycle management
   - Handle tracking and resource limits
   - Backend and accelerator integration

## Public API

### IBlockStorage Interface

```cpp
// Initialize with configuration and dependencies
bool initialize(const BlockStorageConfig& config,
               backends::IBackendProvider* backend_provider,
               accel::IAccelerator* accelerator);

// File operations
GpuFileHandle* open_gpu_file(const std::string& name, FileOpenMode mode,
                             uint64_t stripe_size = 0, uint64_t initial_size = 0);
bool close_gpu_file(GpuFileHandle* handle);
bool delete_gpu_file(const std::string& name);

// Batch operations
std::vector<GpuFileHandle*> open_gpu_files_batch(
    const std::vector<std::string>& names,
    const std::vector<FileOpenMode>& modes, size_t count);

// Directory operations
std::vector<FileInfo> list_gpu_file_names();

// StorageTarget production for IO Engine
io_engine::StorageTarget acquire_device_handle(GpuFileHandle* handle, size_t shard_index);
bool release_device_handle(GpuFileHandle* handle, size_t shard_index);

// Durability operations
bool sync_file(GpuFileHandle* handle, void* stream);
bool flush_metadata();
```

## Types

### Core Types

- **FileId** - Unique file identifier (uint64_t)
- **FileShard** - Device slice descriptor with device_id, namespace_id, start_lba, length_blocks
- **GpuFile** - Named striped file with metadata and shard list
- **GpuFileHandle** - Runtime handle with backend target handles
- **FileInfo** - Directory listing entry

### Configuration

- **StripeConfig** - Stripe size, interleave unit, max shards per file
- **DurabilityConfig** - Sync mode (NONE, METADATA_ONLY, FULL), flush interval
- **BlockStorageConfig** - Root directory, stripe config, durability config, max open files

## Usage Example

```cpp
#include "block_storage.h"

// Create storage instance
auto storage = tutti::block_storage::create_block_storage();

// Configure
tutti::block_storage::BlockStorageConfig config;
config.root_directory = "/mnt/nvme/tutti_storage";
config.stripe_config.stripe_size = 256 * 1024;  // 256 KB
config.max_open_files = 1024;

// Initialize with dependencies
storage->initialize(config, backend_provider, accelerator);

// Create a new file
auto* handle = storage->open_gpu_file(
    "model_weights.dat",
    tutti::block_storage::FileOpenMode::CREATE_NEW,
    0,  // Use default stripe size
    1ULL * 1024 * 1024 * 1024  // 1 GB
);

// Acquire storage targets for IO
for (size_t i = 0; i < handle->target_handles.size(); ++i) {
    auto target = storage->acquire_device_handle(handle, i);
    // Use target with IO Engine...
    storage->release_device_handle(handle, i);
}

// Close and sync
storage->sync_file(handle, stream);
storage->close_gpu_file(handle);
```

## Striping Strategy

Files are striped across available devices using round-robin allocation with load balancing:

1. File size divided by stripe_size determines shard count
2. Shards allocated to devices with least current allocation
3. Each shard gets an LBA range from the backend provider
4. StorageTarget references map shards to IO operations

## Durability Modes

- **NONE** - No synchronization, fastest but no durability
- **METADATA_ONLY** - Persist file metadata only
- **FULL** - Stream sync + NVMe flush on all shards

## Testing

```bash
# Build tests
cmake --build build --target layer5_smoke_test layer5_integration_test

# Run smoke tests
./build/bin/layer5_smoke_test

# Run integration tests
./build/bin/layer5_integration_test
```

## Dependencies

- Layer 1: Accelerator interface (accel)
- Layer 3: Backend provider (backends)
- Layer 4: IO Engine types (io_engine)
- Standard: C++20, pthreads, filesystem

## Thread Safety

- FileDirectory uses shared_mutex for concurrent reads
- Handle operations protected by mutex
- Backend provider calls assumed thread-safe
- Metadata journal writes serialized
