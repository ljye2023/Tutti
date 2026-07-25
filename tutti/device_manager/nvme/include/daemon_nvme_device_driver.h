#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

#include "common/idevice_driver.h"
#include "common/ilease_manager.h"
#include "nvme_physical_device.h"
#include "daemon_nvme_queue_alloc.h"  // DaemonQueueSet

namespace tutti {

class IAccelerator;

/**
 * DaemonNvmeDeviceDriver -- IDeviceDriver for daemon NVMe (multi-process mode).
 *
 * enumerate():
 *   1. Connects to the nvmeservice daemon at daemon_addr_ via gRPC.
 *   2. Calls NvmeServiceClient::list_devices() to discover available NVMe
 *      controllers managed by the daemon.
 *   3. For each device calls NvmeServiceClient::connect() to obtain a Session
 *      (allocation_id, snvme_dev_path, bar0_size, granted_queues, namespace
 *      metadata, heartbeat parameters).
 *   4. Calls nvm_ctrl_attach_client() + nvm_create_group() to prepare the
 *      per-fd libnvm state.  ctrl and group_id are stored in PhysContext.
 *
 * alloc_vdevice():
 *   Calls daemon_nvme_alloc_queues() (CUDA helper) to allocate GPU SQ/CQ
 *   rings, register them with the kernel via NVM_ADD_USER_QUEUE, and return
 *   a device-accessible QueuePair[] typed as nvm_queue_t* (NvmeVirtualDevice::d_qps).
 *
 * free_vdevice():
 *   Calls daemon_nvme_free_queues() to release GPU buffers and DMA handles.
 *
 * Heartbeat:
 *   NvmeServiceClient's internal gRPC bidi-stream heartbeat keeps the daemon
 *   lease alive.  The DaemonNvmeDeviceDriver also runs its own heartbeat_loop()
 *   calling lease_mgr_->heartbeat() for higher-level resource tracking.
 *
 * NvmeClientState is an opaque struct (defined in daemon_nvme_device_driver.cpp)
 * that holds the NvmeServiceClient and per-device Session objects.  This keeps
 * gRPC / protobuf headers out of the public include graph.
 */

// Opaque forward declaration -- full definition in daemon_nvme_device_driver.cpp.
struct NvmeClientState;

class DaemonNvmeDeviceDriver : public IDeviceDriver {
public:
    // mock_mode: when true, enumerate() uses the mock-grant path regardless of
    // whether TUTTI_NVMESERVICE_ENABLED was defined at build time.  Use in unit
    // tests that must not depend on a live daemon.
    DaemonNvmeDeviceDriver(IAccelerator* accel, ILeaseManager* lease_mgr,
                            std::string daemon_addr,
                            bool mock_mode = false);
    ~DaemonNvmeDeviceDriver() override;

    DeviceType type() const override { return DeviceType::LOCAL_NVME; }
    int enumerate(std::vector<IPhysicalDevice*>& out_devices) override;
    IVirtualDevice* alloc_vdevice(IPhysicalDevice* dev, uint32_t resource_quota,
                                    std::string* error) override;
    void free_vdevice(IVirtualDevice* vdev) override;
    void shutdown() override;

private:
    void heartbeat_loop();

    // Per-physical-device libnvm state obtained after gRPC Connect.
    struct PhysContext {
        nvm_ctrl_t* ctrl     = nullptr;
        uint32_t    group_id = 0;
        int         cuda_dev = 0;   // CUDA device the session was opened for
    };

    IAccelerator*  accel_;
    ILeaseManager* lease_mgr_;
    std::string    daemon_addr_;
    bool           mock_mode_;
    std::string    lease_id_;

    // gRPC client + per-device Sessions (opaque, see daemon_nvme_device_driver.cpp).
    std::unique_ptr<NvmeClientState> client_state_;

    // libnvm per-physical-device state (parallel to phys_devices_).
    std::vector<PhysContext> phys_ctxs_;

    std::vector<std::unique_ptr<NvmePhysicalDevice>> phys_devices_;
    std::vector<std::unique_ptr<IVirtualDevice>>     vdevices_;

    // Queue sets allocated per virtual device -- used for free_vdevice cleanup.
    std::unordered_map<IVirtualDevice*, DaemonQueueSet> vdev_queue_sets_;

    std::thread          heartbeat_thread_;
    std::atomic<bool>    shutdown_requested_{false};
};

} // namespace tutti
