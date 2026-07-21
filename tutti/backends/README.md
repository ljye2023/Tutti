# Tutti Backends Layer (Layer 3)

Thin transport adapters that turn ioaddrs + StorageTarget into device-issuable IO commands.

## Architecture

```
Layer 3: Backends
├── include/                    # Common backend SPI
│   ├── backend_provider.h      # IBackendProvider interface (core SPI)
│   ├── backend_factory.h       # Runtime backend selection
│   └── backend_types.h         # Backend-agnostic types
├── src/
│   └── backend_factory.cpp     # Factory implementation
├── local_nvme/                 # NVMe backend implementation
│   ├── include/                # NVMe backend public API
│   ├── src/                    # Host-side implementation
│   └── device/                 # Device-side kernels
└── CMakeLists.txt
```

## Core Interface: IBackendProvider

The `IBackendProvider` interface defines the backend SPI consumed by IO Engine and Block/Raw Storage.

### Lifecycle

1. **Construction** via `BackendFactory::create_backend(BackendType)`
2. **Initialization** via `initialize(VDevice*)` - receive queue slice from Device Manager
3. **Steady-state operations** - descriptor prep, target handles, submission
4. **Cleanup** via `cleanup()` - return resources to Device Manager

### Key Responsibilities

#### Descriptor Build
- `prepare_descriptors()` - Convert ioaddrs (DMA bus addresses) to transport commands
- For NVMe: PRP/SGL descriptor construction
- For RDMA: RDMA descriptor with rkeys
- Called once per tensor registration by Memory Layer

#### Target Handle Management
- `acquire_target_handle()` - Build GPU-resident handle from StorageTarget
- `release_target_handle()` - Free GPU-resident handle
- For NVMe: NvmeFileDeviceHandle (extents + vdev reference)
- Handles are opaque void* typed by backend

#### Submission Modes

**REQUIRED Paths:**
- `launch_batch_gpu_stream()` - GPU kernel submission (primary production path)
- `submit_batch_cpu_sync()` - CPU synchronous submission (bootstrap, metadata, tests)

**OPTIONAL Paths:**
- `submit_batch_cpu_async()` - CPU asynchronous submission (returns IOFuture)
- `setup_coop_channel()` - Cooperative kernel mode setup

#### Metadata
- `backend_type()` - BackendType enum
- `backend_name()` - Human-readable name
- `max_io_size()` - Maximum single IO size (MDTS for NVMe, etc.)
- `metadata()` - Full backend metadata with capabilities

## Backend Types

```cpp
enum class BackendType {
    LOCAL_NVME = 0,  // Local NVMe via Device Manager VDevice
    RDMA = 1,        // RDMA-capable remote storage
    GDS = 2,         // NVIDIA GPUDirect Storage
    UNKNOWN = 255
};
```

## Backend Factory

Runtime backend selection and instantiation:

```cpp
// Backend self-registration (in backend implementation file)
REGISTER_BACKEND(BackendType::LOCAL_NVME, []() {
    return new LocalNvmeBackend();
});

// Backend instantiation (in IO Engine)
auto backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);
if (backend && backend->initialize(vdev)) {
    // Use backend...
}
```

## Key Design Principles

### Layer Independence
- **No cuda_runtime.h in backend_provider.h** - Uses `AccelStream` (opaque void*)
- **Backends receive VDevice once** at `initialize()` - No hot-path DM involvement
- **StorageTarget convergence** - Namespace producers emit it, backends consume it

### Pluggability
- IO Engine holds `IBackendProvider*`, never NVMe-specific types
- Each transfer mode written once in IO Engine, not per backend
- Backends stay fully pluggable via factory pattern

### Device Manager Integration
- NVMe backends consume VDevice (queue slice + namespace view + caps)
- Non-NVMe backends (GDS, RDMA) receive `vdev == nullptr`, ignore and use own resources
- Steady-state IO never calls DM - all queue access via device-side helpers

## Thread Safety

Implementations must be thread-safe for:
- Concurrent descriptor preparation from multiple threads
- Concurrent submission from multiple IO Engine threads
- Internal resource management (PRP cache, target handle map)

## Dependencies

**Consumes (downward):**
- Device Manager: VDevice (queues + namespace view + caps) at initialize()
- Accelerator HAL: AccelStream, dma_map, allocation wrappers
- Abstraction Layer: TUTTI_DEVICE, TUTTI_GLOBAL, TUTTI_LAUNCH_KERNEL macros

**Consumed By (upward):**
- IO Engine: descriptor prep, batch submission
- Block Storage: target handle lifecycle
- Memory Layer: descriptor preparation during tensor registration

## Implementation Status

- ✅ Common interfaces (backend_provider.h, backend_factory.h, backend_types.h)
- ✅ Factory implementation (backend_factory.cpp)
- 🚧 Local NVMe backend (in progress)
- ⏳ RDMA backend (future)
- ⏳ GDS backend (future)
