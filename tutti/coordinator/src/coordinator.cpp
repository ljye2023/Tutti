// tutti/coordinator/src/coordinator.cpp
// Layer 6: Coordinator - Implementation

#include "tutti/coordinator/include/coordinator.h"
#include "tutti/coordinator/include/device.h"
#include "tutti/accel/src/cuda/cuda_accelerator.h"
#include "tutti/device_manager/src/local_nvme_virtual.h"
#include "tutti/backends/local_nvme/include/local_nvme_backend.h"
#include "tutti/io_engine/src/io_engine_impl.h"
#include "tutti/block_storage/src/block_storage_impl.h"
#include "tutti/raw_device/src/raw_device_impl.h"

namespace tutti {

struct Coordinator::Impl {
    std::unique_ptr<CudaAccelerator> accelerator;
    std::unique_ptr<LocalNvmeVirtualRegistry> virt_registry;
    std::unique_ptr<IBackendProvider> backend;
    std::unique_ptr<IoEngineImpl> io_engine;
    std::unique_ptr<BlockStorageImpl> block_storage;
    std::unique_ptr<RawDeviceImpl> raw_device;
    Device device;
    bool initialized;

    Impl() : initialized(false) {}
};

Coordinator::Coordinator()
    : impl_(std::make_unique<Impl>()) {
}

Coordinator::~Coordinator() {
    if (impl_->initialized) {
        shutdown();
    }
}

int Coordinator::initialize(const CoordinatorConfig& config) {
    if (impl_->initialized) {
        return 0;
    }

    // Layer 1: Initialize accelerator
    impl_->accelerator = std::make_unique<CudaAccelerator>();
    if (impl_->accelerator->initialize() != 0) {
        return -1;
    }

    // Layer 2: Initialize device manager
    impl_->virt_registry = std::make_unique<LocalNvmeVirtualRegistry>();
    if (impl_->virt_registry->initialize() != 0) {
        return -1;
    }

    // Allocate VDevice
    impl_->device.vdev = impl_->virt_registry->allocate_vdevice(config.num_queues);
    impl_->device.device_path = config.device_path;
    impl_->device.is_initialized = true;

    // Layer 3: Initialize backend (placeholder - needs actual implementation)
    // impl_->backend = std::make_unique<LocalNvmeBackend>();

    // Layer 4: Initialize IO engine
    impl_->io_engine = std::make_unique<IoEngineImpl>(impl_->backend.get());

    // Layer 5: Initialize storage interfaces
    impl_->block_storage = std::make_unique<BlockStorageImpl>(impl_->io_engine.get());
    impl_->raw_device = std::make_unique<RawDeviceImpl>(impl_->io_engine.get());

    impl_->initialized = true;
    return 0;
}

void Coordinator::shutdown() {
    if (!impl_->initialized) {
        return;
    }

    // Shutdown in reverse order (top-down)
    impl_->raw_device.reset();
    impl_->block_storage.reset();
    impl_->io_engine.reset();
    impl_->backend.reset();

    if (impl_->device.is_initialized) {
        impl_->virt_registry->free_vdevice(impl_->device.vdev);
        impl_->device.is_initialized = false;
    }

    impl_->virt_registry.reset();
    impl_->accelerator.reset();

    impl_->initialized = false;
}

IAccelerator* Coordinator::get_accelerator() {
    return impl_->accelerator.get();
}

IBlockStorage* Coordinator::get_block_storage() {
    return impl_->block_storage.get();
}

IRawDevice* Coordinator::get_raw_device() {
    return impl_->raw_device.get();
}

VDevice Coordinator::get_vdevice() const {
    return impl_->device.vdev;
}

} // namespace tutti
