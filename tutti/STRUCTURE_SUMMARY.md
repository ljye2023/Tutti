# Tutti Refactor Project Structure Summary

## Directory Tree

```
tutti/
├── CMakeLists.txt                    # Root CMake build file
├── README.md                         # Project documentation
│
├── abstraction/                      # Layer 0: Vendor abstraction macros
│   └── accel.h                       # TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, etc.
│
├── types/                            # Layer 0: Shared value types
│   ├── storage_target.h              # StorageTarget union (NVME_FILE/RAW/RDMA)
│   └── io_types.h                    # IoRequest, SubSliceInfo, BufferDescriptor
│
├── accel/                            # Layer 1: Accelerator HAL
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── accel_types.h             # AccelStream, AccelEvent, IpcHandle
│   │   ├── iaccel.h                  # IAccelerator interface
│   │   ├── memory_kind.h             # MemoryKind enum
│   │   └── memory_region.h           # MemoryRegion struct
│   ├── cuda/                         # CUDA implementation (to be added)
│   └── src/cuda/                     # CUDA source files (to be added)
│
├── device_manager/                   # Layer 2: NVMe queue virtualization
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── vdevice.h                 # VDevice struct
│   │   └── virtual_nvme.h            # IVirtualNvme interface
│   └── src/                          # Implementation (to be added)
│
├── backends/                         # Layer 3: Backend providers
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── backend_provider.h        # IBackendProvider SPI
│   └── local_nvme/
│       ├── CMakeLists.txt
│       ├── include/                  # Headers (to be added)
│       └── src/                      # Implementation (to be added)
│
├── io_engine/                        # Layer 4: IO orchestration
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── io_engine.h               # IIoEngine interface
│   └── src/                          # Implementation (to be added)
│
├── block_storage/                    # Layer 5: Block storage interface
│   ├── CMakeLists.txt
│   ├── include/                      # Headers (to be added)
│   └── src/                          # Implementation (to be added)
│
├── raw_device/                       # Layer 5: Raw device interface
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── raw_device.h              # IRawDevice interface
│   └── src/                          # Implementation (to be added)
│
└── coordinator/                      # Layer 6: Top-level orchestrator
    ├── CMakeLists.txt
    ├── include/                      # Headers (to be added)
    └── src/                          # Implementation (to be added)
```

## Created Files

### Header Files (Interfaces & Types)
1. **abstraction/accel.h** - Vendor abstraction macros
2. **types/storage_target.h** - StorageTarget convergence type
3. **types/io_types.h** - IO request types
4. **accel/include/accel_types.h** - Opaque HAL types
5. **accel/include/iaccel.h** - IAccelerator interface
6. **accel/include/memory_kind.h** - Memory allocation kinds
7. **accel/include/memory_region.h** - Memory region metadata
8. **device_manager/include/vdevice.h** - VDevice struct
9. **device_manager/include/virtual_nvme.h** - IVirtualNvme interface
10. **backends/include/backend_provider.h** - IBackendProvider SPI
11. **io_engine/include/io_engine.h** - IIoEngine interface
12. **raw_device/include/raw_device.h** - IRawDevice interface

### CMake Files
1. **CMakeLists.txt** (root) - Main build configuration
2. **accel/CMakeLists.txt** - Accelerator HAL layer
3. **device_manager/CMakeLists.txt** - Device Manager layer
4. **backends/CMakeLists.txt** - Backends layer
5. **backends/local_nvme/CMakeLists.txt** - Local NVMe backend
6. **io_engine/CMakeLists.txt** - IO Engine layer
7. **block_storage/CMakeLists.txt** - Block Storage layer
8. **raw_device/CMakeLists.txt** - Raw Device layer
9. **coordinator/CMakeLists.txt** - Coordinator layer

### Documentation
1. **README.md** - Complete project documentation

## Build Targets

Each layer will build a shared library:
- `libtutti_accel.so`
- `libtutti_device_manager.so`
- `libtutti_backend_local_nvme.so`
- `libtutti_io_engine.so`
- `libtutti_block_storage.so`
- `libtutti_raw_device.so`
- `libtutti_coordinator.so`

## Implementation Status

✅ **Complete:**
- Directory structure
- CMake build system
- Phase 0 foundation types (headers only)
- Layer 1 HAL interfaces (headers only)
- Layer 2-6 interfaces (headers only)

🚧 **To be implemented:**
- All `.cpp` and `.cu` source files
- Unit tests
- Integration tests

## Next Steps

Follow the implementation sequence in `doc/refact_new/09-implementation-sequence.md`:
1. Phase 1: Implement Accelerator HAL (3-4 days)
2. Phase 2: Implement Device Manager (2-3 days)
3. Phase 3: Implement Backends (5-6 days)
4. Phase 4: Implement IO Engine (3-4 days)
5. Phase 5: Implement Storage Interfaces (2-3 days)
6. Phase 6: Integrate Coordinator (2-3 days)
7. Phase 7: Testing & Validation (3-4 days)

Total estimated: 21-27 days (4-5.5 weeks)
