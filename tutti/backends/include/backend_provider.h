// tutti/backends/include/backend_provider.h
// Backend provider SPI interface (placeholder)
//
// This will be the updated IBackendProvider interface with:
// - initialize(VDevice* vdev) instead of raw queue access
// - acquire_target_handle(StorageTarget) / release_target_handle(void*)
// - AccelStream instead of cudaStream_t
//
// TODO: Define full interface based on doc/refact_new/05-layer3-backends-spi.md

#pragma once

namespace tutti {

// Forward declarations
class VDevice;
struct StorageTarget;
struct SubSliceInfo;
struct BufferDescriptor;
using AccelStream = void*;

class IBackendProvider {
public:
    virtual ~IBackendProvider() = default;

    // TODO: Add interface methods from design doc
};

}  // namespace tutti
