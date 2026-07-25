// daemon_nvme_device_driver.cpp -- DaemonNvmeDeviceDriver implementation
//
// Connects to the DeviceService daemon via gRPC to obtain a cross-process
// NVMe grant, then owns a heartbeat thread for the duration of the lease.
//
// Threading model:
//   enumerate() is called once before any alloc/free calls.
//   heartbeat_loop() runs on heartbeat_thread_ and only reads lease_id_
//   (set by enumerate before thread start) and shutdown_requested_.
//   alloc_vdevice()/free_vdevice() are called from the manager thread.
//   No concurrent access exists between the heartbeat thread and the
//   vectors, so no additional mutex is needed.

#include "daemon_nvme_device_driver.h"
#include "nvme_physical_device.h"
#include "nvme_virtual_device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace tutti {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DaemonNvmeDeviceDriver::DaemonNvmeDeviceDriver(
        IAccelerator*  accel,
        ILeaseManager* lease_mgr,
        std::string    daemon_addr)
    : accel_(accel)
    , lease_mgr_(lease_mgr)
    , daemon_addr_(std::move(daemon_addr))
{}

DaemonNvmeDeviceDriver::~DaemonNvmeDeviceDriver() {
    shutdown();
}

// ---------------------------------------------------------------------------
// enumerate()
// ---------------------------------------------------------------------------

int DaemonNvmeDeviceDriver::enumerate(std::vector<IPhysicalDevice*>& out_devices) {
    // TODO: gRPC call — connect to daemon at daemon_addr_, send
    //   AllocateGrantRequest{ process_id, requested_qps }
    //   and receive GrantResponse{ lease_id, device_id, pci_addr,
    //                              display_name, caps, queue_pairs }
    //
    // Mock grant used until gRPC proto is wired in.

    struct MockGrant {
        int32_t     device_id    = 0;
        std::string pci_addr     = "0000:01:00.0";
        std::string display_name;
        uint32_t    caps         = 0;     // bit 0: GPUDIRECT_CAPABLE
        uint32_t    queue_pairs  = 16;    // QPs granted to this process
        std::string lease_id;
    };

    MockGrant grant;
    grant.display_name = "NVMe(daemon)@" + daemon_addr_;
    grant.lease_id     = "lease-" + daemon_addr_ + "-" +
                         std::to_string(static_cast<unsigned long>(::getpid()));

    // Persist the lease_id before starting the heartbeat thread.
    lease_id_ = grant.lease_id;

    auto phys = std::make_unique<NvmePhysicalDevice>(
        grant.device_id,
        grant.pci_addr,
        grant.display_name,
        grant.caps,
        grant.queue_pairs);
    // ctrl, queue_group remain null — daemon owns the hardware.

    out_devices.push_back(phys.get());
    phys_devices_.push_back(std::move(phys));

    // Start heartbeat thread after lease_id_ is set and devices are recorded.
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });

    return static_cast<int>(phys_devices_.size());
}

// ---------------------------------------------------------------------------
// heartbeat_loop()
// ---------------------------------------------------------------------------

void DaemonNvmeDeviceDriver::heartbeat_loop() {
    // Beat every 5 s, but check shutdown_requested_ every 100 ms so
    // that shutdown() returns promptly instead of waiting a full interval.
    constexpr int kIntervalMs    = 5000;
    constexpr int kSliceMs       = 100;
    constexpr int kSlicesPerBeat = kIntervalMs / kSliceMs;

    while (!shutdown_requested_.load(std::memory_order_relaxed)) {
        // Interruptible sleep: kSliceMs ticks up to kIntervalMs.
        for (int i = 0; i < kSlicesPerBeat; ++i) {
            if (shutdown_requested_.load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        }

        if (shutdown_requested_.load(std::memory_order_relaxed)) {
            return;
        }

        if (!lease_mgr_->heartbeat(lease_id_)) {
            std::fprintf(stderr,
                "[DaemonNvmeDeviceDriver] heartbeat rejected: lease='%s' addr='%s'\n",
                lease_id_.c_str(), daemon_addr_.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// alloc_vdevice()
// ---------------------------------------------------------------------------

IVirtualDevice* DaemonNvmeDeviceDriver::alloc_vdevice(
        IPhysicalDevice* dev,
        uint32_t         resource_quota,
        std::string*     error) {
    if (!dev) {
        if (error) *error = "dev is null";
        return nullptr;
    }
    if (resource_quota == 0) {
        if (error) *error = "resource_quota is zero";
        return nullptr;
    }

    // Verify dev belongs to this driver.
    auto pit = std::find_if(phys_devices_.begin(), phys_devices_.end(),
        [dev](const std::unique_ptr<NvmePhysicalDevice>& p) {
            return p.get() == dev;
        });
    if (pit == phys_devices_.end()) {
        if (error) *error = "dev does not belong to this driver";
        return nullptr;
    }

    NvmePhysicalDevice* nvme_phys = pit->get();

    if (nvme_phys->available_grant() < resource_quota) {
        if (error) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "insufficient grant: requested=%u available=%u",
                resource_quota, nvme_phys->available_grant());
            *error = buf;
        }
        return nullptr;
    }

    nvme_phys->reserve(resource_quota);

    const uint32_t new_vdev_id = static_cast<uint32_t>(vdevices_.size());

    auto vdev = std::make_unique<NvmeVirtualDevice>(
        nvme_phys->id(),
        new_vdev_id,
        nvme_phys->caps());

    // Populate NVMe-specific metadata from the physical device view.
    // d_qps is null in daemon mode: the daemon owns queue memory.
    vdev->queue_quota   = resource_quota;
    vdev->namespace_id  = nvme_phys->namespace_id;
    vdev->blk_size      = nvme_phys->blk_size;
    vdev->blk_size_log  = nvme_phys->blk_size_log;
    vdev->max_data_size = nvme_phys->max_data_size;

    IVirtualDevice* raw = vdev.get();
    vdevices_.push_back(std::move(vdev));
    return raw;
}

// ---------------------------------------------------------------------------
// free_vdevice()
// ---------------------------------------------------------------------------

void DaemonNvmeDeviceDriver::free_vdevice(IVirtualDevice* vdev) {
    if (!vdev) return;

    auto it = std::find_if(vdevices_.begin(), vdevices_.end(),
        [vdev](const std::unique_ptr<IVirtualDevice>& v) {
            return v.get() == vdev;
        });

    if (it == vdevices_.end()) {
        std::fprintf(stderr,
            "[DaemonNvmeDeviceDriver] free_vdevice: vdev not owned by this driver\n");
        return;
    }

    // Release quota back to the physical device before destroying the vdev.
    const int32_t  phys_id = vdev->phys_id();
    const uint32_t count   = vdev->resource_count();

    auto pit = std::find_if(phys_devices_.begin(), phys_devices_.end(),
        [phys_id](const std::unique_ptr<NvmePhysicalDevice>& p) {
            return p->id() == phys_id;
        });
    if (pit != phys_devices_.end()) {
        (*pit)->release(count);
    }

    vdevices_.erase(it);
}

// ---------------------------------------------------------------------------
// shutdown()
// ---------------------------------------------------------------------------

void DaemonNvmeDeviceDriver::shutdown() {
    // exchange returns previous value; if already true, skip double-shutdown.
    if (shutdown_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    if (lease_mgr_ && !lease_id_.empty()) {
        lease_mgr_->release_lease(lease_id_);
    }

    // Destroy virtual devices before physical devices (order matters for
    // any future resource-tracking extensions).
    vdevices_.clear();
    phys_devices_.clear();
}

} // namespace tutti
