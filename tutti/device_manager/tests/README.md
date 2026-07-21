# Layer 2 Smoke Test

## Overview

The Layer 2 smoke test verifies the Device Manager implementation without requiring actual NVMe hardware. It uses mock implementations to test interfaces and allocation logic.

## Test Structure

```
tutti/device_manager/tests/
├── layer2_smoke_test.cpp    # Main test file (400+ lines)
├── mock_device.h.in          # Mock Device type for testing
└── CMakeLists.txt            # Build configuration
```

## Tests Included

### 1. Mock Registry Tests
- **test_mock_registry**: Verifies MockDeviceRegistry operations
  - Open/Close
  - device_at() indexing
  - find_by_id() lookup
  - list() enumeration

### 2. Data Structure Tests
- **test_vdevice_struct**: Validates VDevice struct fields
  - Field assignments
  - Capability bits
  - Namespace metadata

### 3. Allocation Tests
- **test_allocation_basic**: Basic vDevice allocation/deallocation
  - Initial queue availability (16 queues)
  - Single allocation (4 queues)
  - Queue availability after allocation (12 queues)
  - Deallocation and recovery (16 queues)

- **test_allocation_multiple**: Multiple concurrent allocations
  - Allocate 4 vDevices × 4 queues = 16 total
  - Pool exhaustion (0 queues available)
  - Failed allocation when exhausted
  - Partial deallocation and reallocation

- **test_allocation_failures**: Error handling
  - Zero quota rejection
  - Invalid device ID rejection
  - Excessive quota rejection
  - Correct error messages

### 4. Multi-Device Tests
- **test_multi_device**: Multiple physical devices
  - Independent allocation from device 0 and 1
  - Separate queue pools per device
  - Different pointer ranges validation

### 5. Advanced Tests
- **test_capabilities**: Capability queries
  - GPUDIRECT capability detection
  - Invalid device returns 0 caps

- **test_null_handling**: Null pointer safety
  - close_vdevice(nullptr) is no-op

## Mock Implementations

### MockDeviceRegistry
Simulates a physical device registry:
- Creates 2 mock NVMe devices
- Each device has 16 queue pairs
- Implements full IDeviceRegistry interface

### MockNvmeQueueGroup
Simulates GPU-resident queue pool:
- Allocates array of MockQueuePair structs
- Returns device pointer via d_qps()
- No actual GPU memory (host memory only)

### MockQueuePair
Minimal queue pair structure:
```cpp
struct MockQueuePair {
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t ns_id;
};
```

## Building

### Standalone Build
```bash
cd tutti/device_manager/tests
mkdir build && cd build
cmake ..
make
```

### Full Project Build
```bash
# Add to tutti/device_manager/CMakeLists.txt:
add_subdirectory(tests)

# Then build normally
mkdir build && cd build
cmake ..
make layer2_smoke_test
```

## Running

### Direct Execution
```bash
./layer2_smoke_test
```

### Expected Output
```
========================================
Layer 2 (Device Manager) Smoke Test
========================================

[TEST] test_mock_registry
✅ PASS: test_mock_registry

[TEST] test_vdevice_struct
✅ PASS: test_vdevice_struct

[TEST] test_allocation_basic
✅ PASS: test_allocation_basic

[TEST] test_allocation_multiple
✅ PASS: test_allocation_multiple

[TEST] test_allocation_failures
✅ PASS: test_allocation_failures

[TEST] test_multi_device
✅ PASS: test_multi_device

[TEST] test_capabilities
✅ PASS: test_capabilities

[TEST] test_null_handling
✅ PASS: test_null_handling

========================================
Test Results
========================================
Passed: 8
Failed: 0
Total:  8

✅ All tests passed!
```

### Via CTest
```bash
ctest -R layer2_smoke -V
```

## Test Coverage

### Interfaces Tested
- ✅ IDeviceRegistry (all methods)
- ✅ IVirtualNvme (all methods)
- ✅ VDevice (all fields)
- ✅ LocalNvmeVirtualRegistry (complete)

### Allocation Algorithm Tested
- ✅ Contiguous-first-fit strategy
- ✅ Bitmap allocation tracking
- ✅ Pointer arithmetic (d_qps slicing)
- ✅ Thread-safe mutex protection (implicit)
- ✅ Error handling and rollback

### Edge Cases Tested
- ✅ Zero quota
- ✅ Excessive quota
- ✅ Pool exhaustion
- ✅ Null pointer handling
- ✅ Invalid device ID
- ✅ Multi-device independence

## Limitations

### Not Tested (Requires Hardware)
- ❌ Actual NVMe controller interaction
- ❌ Real GPU memory allocation
- ❌ Device-side queue helpers (CUDA kernels)
- ❌ snvme kernel module ioctls
- ❌ libnvm controller operations
- ❌ NVMeService daemon interaction
- ❌ Cross-process arbitration

### Not Tested (Out of Scope)
- ❌ Fragmentation with non-contiguous allocation
- ❌ Concurrent multi-threaded allocation
- ❌ Memory leak detection (requires sanitizers)
- ❌ Performance benchmarks
- ❌ Integration with backends

## Extending Tests

### Adding a New Test
```cpp
bool test_my_feature() {
    tutti::MockDeviceRegistry registry;
    TEST_ASSERT(registry.Open(), "Registry should open");

    tutti::LocalNvmeVirtualRegistry allocator(&registry);

    // Your test logic here
    std::string error;
    tutti::VDevice* vdev = allocator.open_vdevice(0, 4, &error);
    TEST_ASSERT(vdev != nullptr, "Should allocate");

    allocator.close_vdevice(vdev);
    registry.Close();
    return true;
}

// In main():
RUN_TEST(test_my_feature);
```

### Adding Mock Functionality
```cpp
class ExtendedMockRegistry : public MockDeviceRegistry {
public:
    // Add custom mock behavior
    void simulate_device_failure(int device_id) {
        // Implementation
    }
};
```

## Debugging

### Enable Verbose Output
Add debug prints in LocalNvmeVirtualRegistry:
```cpp
std::fprintf(stderr, "[DEBUG] Allocating %u queues from device %d\n", 
             quota, phys_id);
```

### Run with Sanitizers
```bash
# Address sanitizer
CXXFLAGS="-fsanitize=address" cmake ..
make layer2_smoke_test
./layer2_smoke_test

# Thread sanitizer (for concurrency issues)
CXXFLAGS="-fsanitize=thread" cmake ..
```

### GDB Debugging
```bash
gdb ./layer2_smoke_test
(gdb) break test_allocation_basic
(gdb) run
```

## Success Criteria

The test suite passes if:
- ✅ All 8 tests return true
- ✅ No segmentation faults or crashes
- ✅ No memory leaks (when run with sanitizers)
- ✅ Exit code is 0

## Integration with CI/CD

```yaml
# Example CI configuration
test:
  stage: test
  script:
    - cd tutti/device_manager/tests
    - mkdir build && cd build
    - cmake ..
    - make
    - ./layer2_smoke_test
  artifacts:
    when: on_failure
    paths:
      - tutti/device_manager/tests/build/Testing/
```

## Known Issues

None currently. If tests fail:
1. Check mock implementations match current interfaces
2. Verify pointer arithmetic assumptions
3. Ensure contiguous-first-fit logic is correct
4. Check for unintended state mutations

## Future Enhancements

- [ ] Add fragmentation stress test
- [ ] Add concurrent allocation test (multi-threaded)
- [ ] Add memory leak detection test
- [ ] Add performance benchmark test
- [ ] Mock NVMeService for Level-1 testing
- [ ] Integration test with real libnvm (requires hardware)

---

**Status**: Ready for use  
**Last Updated**: 2026-07-21  
**Maintainer**: Layer 2 implementation team
