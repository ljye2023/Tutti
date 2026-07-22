# StripeManager LBA Allocation Implementation Notes

## Changes Made

### 1. Device Enumeration
**Problem**: Hardcoded device_count=4
**Solution**: 
- Added `coordinator::IRawDevice*` parameter to `initialize()`
- Use `raw_device->list_namespaces()` to query actual available namespaces
- Store namespace IDs in `available_namespaces_` vector

### 2. LBA Allocation
**Problem**: Hardcoded placeholder LBA values (1000 + i * 10000)
**Solution**:
- Created `DeviceLbaAllocator` struct to track per-namespace allocation state
- Each allocator maintains:
  - `namespace_id`: The namespace being managed
  - `total_blocks`: Total capacity from NamespaceInfo
  - `next_free_lba`: Next available LBA for allocation
  - `allocated_blocks`: Total blocks currently allocated
- `allocate_shards()` now:
  - Selects namespace with least allocated_blocks (load balancing)
  - Allocates contiguous LBA range starting at `next_free_lba`
  - Updates `next_free_lba` and `allocated_blocks`
  - Checks for space exhaustion before allocation

### 3. LBA Deallocation
**Problem**: No actual deallocation, only tracking update
**Solution**:
- `deallocate_shards()` now decrements `allocated_blocks` for each namespace
- Note: Current implementation does NOT reclaim freed LBA ranges for reuse

## Current Limitations

### 1. No Free List Management
The allocator uses a simple bump-pointer strategy (`next_free_lba`). Once allocated, LBA ranges are never reused, even after deallocation. This will cause space exhaustion over time.

**Production Fix Needed**: Implement free list or bitmap to track which LBA ranges are available:
- Option A: Maintain per-namespace free list (extent tree)
- Option B: Bitmap allocation (good for fixed-size blocks)
- Option C: Buddy allocator (reduces fragmentation)

### 2. No Persistence
LBA allocations are in-memory only. After restart, all allocation state is lost.

**Production Fix Needed**: Persist allocation metadata:
- Store allocation map to stable storage (metadata journal)
- Reconstruct state from file metadata on restart
- Or scan namespace LBA space to detect used ranges

### 3. Backend Provider API Gap
The `IBackendProvider` interface lacks methods for:
- Device enumeration (`list_devices()`, `get_device_info()`)
- LBA space management (`allocate_lba_range()`, `deallocate_lba_range()`)

**Workaround**: Using `coordinator::IRawDevice` interface which provides:
- `list_namespaces()` - enumerate available namespaces
- `get_namespace_info(ns_id)` - get capacity and block size

**Recommended Backend Provider Extensions**:
```cpp
class IBackendProvider {
    // Device enumeration
    virtual std::vector<uint32_t> list_namespaces() = 0;
    virtual NamespaceInfo get_namespace_info(uint32_t ns_id) = 0;
    
    // LBA allocation (optional, for backends that manage LBA space)
    virtual bool allocate_lba_range(
        uint32_t ns_id,
        uint64_t length_blocks,
        uint64_t* out_start_lba) = 0;
    
    virtual bool deallocate_lba_range(
        uint32_t ns_id,
        uint64_t start_lba,
        uint64_t length_blocks) = 0;
};
```

### 4. No Fragmentation Handling
Allocator uses first-fit strategy. Over time, fragmentation will occur as files are created and deleted.

**Production Fix Needed**: 
- Implement best-fit or worst-fit allocation strategy
- Add defragmentation or compaction support
- Consider shard size alignment to reduce fragmentation

### 5. No Failure Recovery
If allocation fails partway through multi-shard allocation, already-allocated shards are not rolled back.

**Production Fix Needed**:
- Implement transactional allocation (all-or-nothing)
- Add rollback logic to `allocate_shards()` on partial failure

## Integration Requirements

Callers of `StripeManager::initialize()` must now provide:
- `backends::IBackendProvider*` backend_provider (existing)
- `coordinator::IRawDevice*` raw_device (NEW - for device enumeration)

Example:
```cpp
coordinator::ICoordinator* coordinator = create_coordinator();
coordinator->initialize(config);

backends::IBackendProvider* backend = /* get from coordinator */;
coordinator::IRawDevice* raw_device = coordinator->get_raw_device();

StripeManager stripe_mgr;
stripe_mgr.initialize(backend, raw_device, stripe_config);
```

## Testing Recommendations

1. **Multi-device allocation**: Verify shards are distributed across all available namespaces
2. **Load balancing**: Verify least-loaded namespace is selected for each allocation
3. **Space exhaustion**: Verify graceful handling when namespace runs out of space
4. **Deallocation**: Verify allocated_blocks counter is correctly decremented
5. **Edge cases**: 
   - File size < stripe size (single shard)
   - File size > all available space
   - Zero or empty namespace list
