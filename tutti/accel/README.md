# Layer 1: Accelerator HAL (Hardware Abstraction Layer)

## Overview

Layer 1 provides a vendor-neutral hardware abstraction layer for GPU/accelerator operations. It wraps vendor-specific APIs (CUDA, ROCm, SYCL) behind a unified `IAccelerator` interface.

## Directory Structure

```
accel/
├── include/
│   ├── common/           # Public API headers
│   │   ├── iaccel.h      # Main HAL interface
│   │   ├── accel_types.h # Stream, Event, IPC handle types
│   │   ├── memory_kind.h # Memory type enumeration
│   │   └── memory_region.h # Memory region metadata
│   └── cuda/
│       └── cuda_accelerator.h # CUDA implementation header
├── src/
│   └── cuda/
│       └── cuda_accelerator.cu # CUDA implementation
├── CMakeLists.txt
└── README.md (this file)
```

## Features

### Implemented ✅
- Device management (enumerate, set active device)
- Memory allocation (host, device, pinned, managed)
- Memory registration and tracking
- Stream lifecycle (create, destroy, synchronize, query)
- Event lifecycle (create, destroy, record, wait, query)
- Async memory operations (memcpy, memset)
- Kernel launch via runtime API
- IPC memory handles (export/import)
- Host ↔ Device pointer translation

### Design Philosophy

**Layer 1 focuses on GPU abstraction only.** Advanced features like DMA mapping for PCIe P2P transfers are intentionally left to upper layers (NVMe backend, P2P services) where they can:

- Access physical addresses via kernel modules
- Integrate with specific hardware (NVMe controllers, RDMA)
- Use backend-specific libraries (libnvm, etc.)
- Manage end-to-end data paths

This keeps the Accel layer clean, portable, and testable without hardware dependencies.

## Building

This layer is built as part of the Tutti project:

```bash
mkdir build_tutti
cd build_tutti
cmake ../tutti -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
```

Output: `libtutti_accel.a`

## Testing

Layer 1 includes comprehensive smoke tests:

```bash
cd build_tutti
./bin/layer1_smoke_test
```

Tests cover:
1. ✅ Device management
2. ✅ Host memory allocation
3. ✅ Device memory allocation  
4. ✅ Pinned host memory
5. ✅ Memory registration
6. ✅ Stream lifecycle
7. ✅ Event lifecycle
8. ✅ Async memcpy (H↔D, D↔D)
9. ✅ Kernel launch

**Current Status:** 10/10 tests passing

## API Example

```cpp
#include "iaccel.h"
#include "cuda_accelerator.h"

using namespace tutti;

// Create accelerator
IAccelerator* accel = new CudaAccelerator();

// Allocate memory
void* host_ptr = accel->allocate_host(4096, MemoryKind::PINNED_HOST);
void* device_ptr = accel->allocate_device(4096, MemoryKind::DEVICE, 0);

// Register memory
MemoryRegion* region = accel->register_device(device_ptr, 4096, 0);

// Create stream
AccelStream stream = accel->create_stream();

// Async copy
accel->memcpy_async(device_ptr, host_ptr, 4096, stream);
accel->synchronize_stream(stream);

// Launch kernel
Dim3 grid(1, 1, 1);
Dim3 block(256, 1, 1);
void* args[] = {&device_ptr};
accel->launch(kernel_func, grid, block, 0, stream, args);

// Cleanup
accel->destroy_stream(stream);
accel->unregister(region);
accel->free(device_ptr, MemoryKind::DEVICE);
accel->free(host_ptr, MemoryKind::PINNED_HOST);
delete accel;
```

## Dependencies

- **Layer 0:** `tutti_types` (header-only abstraction layer)
- **External:** CUDA Toolkit 11.0+
- **No dependency on:** libnvm, kernel modules, or other Tutti layers

## Design Principles

### Separation of Concerns

1. **Accel Layer (this layer):**
   - GPU memory lifecycle
   - Vendor API abstraction (CUDA/ROCm/SYCL)
   - Stream and event management
   - Basic memory operations

2. **Upper Layers:**
   - DMA and PCIe P2P
   - Physical address resolution
   - Hardware-specific integrations
   - End-to-end data paths

### Thread Safety

All public methods are thread-safe through internal mutex protection on the registry.

### Portability

By keeping hardware-specific features out of this layer, the same interface can support:
- NVIDIA GPUs (CUDA)
- AMD GPUs (ROCm/HIP)
- Intel GPUs (SYCL/oneAPI)
- Other accelerators

## Next Steps

1. **ROCm backend** (HipAccelerator)
2. **SYCL backend** (SyclAccelerator)  
3. **Performance profiling** and optimization
4. **Extended IPC support** for multi-GPU scenarios
