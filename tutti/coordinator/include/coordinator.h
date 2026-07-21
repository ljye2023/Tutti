// tutti/coordinator/include/coordinator.h
// Layer 6: Coordinator - Top-level Orchestrator
//
// Main entry point for the Tutti storage runtime

#pragma once

#include "tutti/accel/include/accel_types.h"
#include "tutti/accel/include/iaccel.h"
#include "tutti/device_manager/include/vdevice.h"
#include "tutti/block_storage/include/block_storage.h"
#include "tutti/raw_device/include/raw_device.h"
#include <memory>
#include <string>

namespace tutti {

// Coordinator configuration
struct CoordinatorConfig {
    std::string device_path;
    uint32_t num_queues;
    uint32_t queue_depth;
    bool enable_p2p;
};

// Top-level coordinator class
class Coordinator {
public:
    Coordinator();
    ~Coordinator();

    // Initialization
    int initialize(const CoordinatorConfig& config);
    void shutdown();

    // Accelerator access
    IAccelerator* get_accelerator();

    // Storage interfaces
    IBlockStorage* get_block_storage();
    IRawDevice* get_raw_device();

    // Device access
    VDevice get_vdevice() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tutti
