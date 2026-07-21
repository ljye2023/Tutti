# Tutti Refactor

> Clean-slate implementation of the Tutti layered architecture  
> **Target**: Linux 5.15 kernel + CUDA 12.6+ for v0.1  
> **Status**: Phase 0 - Foundation types and CMake structure complete

## Overview

This directory contains the refactored Tutti stack with strict layered architecture and vendor abstraction. The design eliminates CUDA leaks above the HAL boundary, inverts Device Manager to sit below backends, and provides a unified `StorageTarget` convergence type for both Block Storage and Raw Device paths.

## Architecture

```
Layer 6: coordinator/        — Top-level orchestrator
         ├─┐
Layer 5: │ block_storage/    — IBlockStorage (GPUFile → StorageTarget)
         │ raw_device/        — IRawDevice (ns + LBA → StorageTarget)
         └─┘
Layer 4: io_engine/          — IIoEngine (backend-neutral IO orchestration)
         │
Layer 3: backends/           — IBackendProvider implementations (local_nvme, ...)
         │
Layer 2: device_manager/     — IVirtualNvme (Level-2 QP allocator) + VDevice
         │
Layer 1: accel/              — IAccelerator HAL (vendor-neutral memory + stream + launch)
         │
Layer 0: abstraction/        — Macros (TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, ...)
         types/              — Shared value types (StorageTarget, IoRequest, ...)
```

## Key Design Principles

1. **Strict bottom-up dependencies** — Each layer depends only on layers below + shared types
2. **HAL boundary is sacred** — No `cuda_runtime.h` above the HAL layer
3. **Backend-neutral IO Engine** — Uses `StorageTarget`, not NVMe-specific types
4. **Device Manager has no hot path** — Only called during bootstrap for `VDevice` allocation
5. **Convergence over duplication** — `StorageTarget` is the common language for both storage paths

## Directory Structure

```
tutti/
├── CMakeLists.txt                      # Root build file
├── README.md                           # This file
│
├── abstraction/                        # Layer 0: Vendor abstraction macros
│   └── accel.h                         # TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, ...
│
├── types/                              # Layer 0: Shared value types
│   ├── storage_target.h                # StorageTarget union
│   └── io_types.h                      # IoRequest, SubSliceInfo, BufferDescriptor
│
├── accel/                              # Layer 1: Accelerator HAL
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── accel_types.h               # AccelStream, AccelEvent, IpcHandle
│   │   ├── memory_kind.h               # MemoryKind enum
│   │   ├── memory_region.h             # MemoryRegion struct
│   │   └── iaccel.h                    # IAccelerator interface
│   └── cuda/
│       ├── cuda_accelerator.h          # CUDA implementation
│       └── cuda_accelerator.cu         # CUDA implementation (all cuda_runtime.h here)
│
├── device_manager/                     # Layer 2: NVMe queue virtualization
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── vdevice.h                   # VDevice struct
│   │   └── virtual_nvme.h              # IVirtualNvme interface
│   └── src/
│       └── local_nvme_virtual_registry.cpp  # Level-2 allocator
│
├── backends/                           # Layer 3: Backend providers
│   ├── CMakeLists.txt
│   └── local_nvme/
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── local_nvme_backend.h    # LocalNvmeBackend class
│       └── src/
│           ├── local_nvme_backend.cpp  # Backend implementation
│           ├── prp_builder.cu          # PRP/SGL descriptor building
│           └── submit_kernel.cu        # GPU-stream batch submit
│
├── io_engine/                          # Layer 4: IO orchestration
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── io_engine.h                 # IIoEngine interface
│   └── src/
│       └── io_engine_impl.cpp          # Fan-out logic (moved from memory/)
│
├── block_storage/                      # Layer 5: Block storage interface
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── block_storage_adapter.h     # Updated IBlockStorage
│   └── src/
│       └── block_storage_adapter.cpp   # GPUFile → StorageTarget
│
├── raw_device/                         # Layer 5: Raw device interface
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── raw_device.h                # IRawDevice interface (NEW)
│   └── src/
│       └── raw_device_impl.cpp         # ns + LBA → StorageTarget
│
└── coordinator/                        # Layer 6: Top-level orchestrator
    ├── CMakeLists.txt
    ├── include/
    │   └── coordinator_adapter.h       # Updated Coordinator
    └── src/
        └── coordinator_impl.cpp        # Bootstrap with VDevice allocation
```

## Building

### Prerequisites

- CMake 3.18+
- CUDA Toolkit 12.6+
- Linux 5.15 kernel (for NVMe kernel module support)
- libnvm (from parent project)

### Build Steps

```bash
# From the parent Tutti project root
mkdir -p build
cd build
cmake ..
make tutti_coordinator  # Builds all layers via dependencies
```

Or build from this directory standalone (for development):

```bash
cd tutti/
mkdir build
cd build
cmake ..
make -j$(nproc)
```

### Build Targets

Each layer builds a shared library:

- `libtutti_accel.so` — Accelerator HAL
- `libtutti_device_manager.so` — Device Manager
- `libtutti_backend_local_nvme.so` — Local NVMe backend
- `libtutti_io_engine.so` — IO Engine
- `libtutti_block_storage.so` — Block Storage adapter
- `libtutti_raw_device.so` — Raw Device interface
- `libtutti_coordinator.so` — Coordinator

## Implementation Status

### Phase 0: Foundation Types ✅ COMPLETE

- [x] `abstraction/accel.h` — Vendor macros
- [x] `types/storage_target.h` — StorageTarget union
- [x] `types/io_types.h` — IoRequest, SubSliceInfo
- [x] `accel/include/accel_types.h` — AccelStream, AccelEvent
- [x] `accel/include/memory_kind.h` — MemoryKind enum
- [x] `accel/include/memory_region.h` — MemoryRegion struct
- [x] CMake build system with dependency ordering

### Phase 1: Accelerator HAL 🚧 IN PROGRESS

- [x] `accel/include/iaccel.h` — IAccelerator interface
- [x] `accel/cuda/cuda_accelerator.h` — CUDA implementation header
- [x] `accel/cuda/cuda_accelerator.cu` — CUDA implementation (skeleton)
- [ ] Unit tests: alloc → register → dma_map → free
- [ ] Unit tests: stream create → memcpy_async → sync

### Phase 2: Device Manager 📋 TODO

- [x] `device_manager/include/vdevice.h` — VDevice struct
- [x] `device_manager/include/virtual_nvme.h` — IVirtualNvme interface
- [ ] `device_manager/src/local_nvme_virtual_registry.cpp` — Implementation
- [ ] Unit tests: open_vdevice → close_vdevice
- [ ] Unit tests: queue pool exhaustion

### Phase 3-7: TODO

See `doc/refact_new/09-implementation-sequence.md` for detailed implementation plan.

## Integration with Parent Project

This refactored stack is built **alongside** the v0.1 codebase, not **in place of** it. The Coordinator will temporarily hold both old and new implementations during Phase 6 integration, allowing gradual migration and rollback if needed.

### Dependencies on Parent Project

- `libnvm` — NVMe user-space library (for DMA mapping)
- `device_manager/` (old) — IDeviceRegistry, NvmeQueueGroup
- `nvmeservice` — gRPC-based NVMe service client
- Kernel module: `snvme-5.15.0-*` — NVMe kernel driver

## Design Documentation

Comprehensive design docs are in `doc/refact_new/`:

- `00-overview.md` — Refactor goals and architecture overview
- `01-missing-types.md` — All new types that must be created
- `02-layer0-abstraction.md` — Macro layer specification
- `03-layer1-accelerator-hal.md` — IAccelerator design
- `04-layer2-device-manager.md` — IVirtualNvme + VDevice design
- `05-layer3-backends-spi.md` — IBackendProvider changes
- `06-layer4-io-engine.md` — IIoEngine changes
- `07-layer5-storage-interfaces.md` — IBlockStorage + IRawDevice
- `08-validation.md` — Dependency rules + end-to-end validation
- `09-implementation-sequence.md` — Build order (this roadmap)
- `10-open-questions.md` — Unresolved design decisions

## Testing Strategy

### Unit Tests (per-layer)

Each layer has unit tests that validate its interface contract without dependencies on higher layers:

- **Layer 1**: Memory allocation, DMA mapping, stream operations
- **Layer 2**: VDevice allocation, queue pool management
- **Layer 3**: Target handle acquisition, descriptor preparation
- **Layer 4**: IO slice fan-out, batch submission
- **Layer 5**: StorageTarget generation

### Integration Tests

End-to-end flows through the full stack:

1. Bootstrap → open_gpu_file → register_device → submit_batch → verify data
2. Bootstrap → acquire_raw_target → register_device → submit_batch → verify data

### Performance Validation

No regression from v0.1 baselines:

- Single 1 GiB GPU-stream read: ≤ baseline + 5%
- Batch of 100×10 MiB reads: ≤ baseline + 5%
- Queue acquire latency (device-side): ≤ baseline + 10%

## Contributing

When implementing new layers:

1. Read the corresponding `doc/refact_new/0X-layerN-*.md` design doc
2. Implement interfaces before concrete classes
3. Write unit tests before integration
4. Validate dependency rules with `08-validation.md`
5. Update this README's "Implementation Status" section

## License

Same as parent Tutti project.

---

**Created**: 2026-07-21  
**Target Release**: v0.1 (Linux 5.15 + CUDA)  
**Estimated Timeline**: 4-5.5 weeks (single engineer)
