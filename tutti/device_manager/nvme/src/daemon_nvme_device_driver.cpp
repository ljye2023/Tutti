// daemon_nvme_device_driver.cpp -- DaemonNvmeDeviceDriver implementation
//
// Multi-process mode: the nvmeservice daemon owns the hardware and arbitrates
// queue access.  This driver:
//
//   enumerate()     -- gRPC list_devices + connect, then nvm_ctrl_attach_client
//                      + nvm_create_group per device.
//   alloc_vdevice() -- reserves quota + calls daemon_nvme_alloc_queues (CUDA).
//   free_vdevice()  -- calls daemon_nvme_free_queues + releases quota.
//   shutdown()      -- nvm_destroy_group + nvm_ctrl_free_client per device,
//                      then Session dtors send Disconnect RPCs.
//
// When TUTTI_NVMESERVICE_ENABLED is not defined (gRPC not found at build time),
// enumerate() falls back to the original mock grant so the target still links
// and tests that don't require a real daemon continue to work.
//
// Threading:
//   enumerate() is called once before alloc/free.
//   heartbeat_loop() only reads lease_id_ (set before thread start).
//   alloc_vdevice / free_vdevice called from manager thread (no concurrent
//   access with the heartbeat thread on the mutable vectors).

#include "daemon_nvme_device_driver.h"
#include "nvme_physical_device.h"
#include "nvme_virtual_device.h"
#include "daemon_nvme_queue_alloc.h"

#ifdef TUTTI_NVMESERVICE_ENABLED
// gRPC client -- included here (not in the public header) to keep gRPC/protobuf
// headers out of the public include graph.
#  include "nvmeservice_client.h"
#endif

// libnvm
#include <nvm_ctrl.h>   // nvm_ctrl_attach_client / free_client / create_group / destroy_group

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

namespace tutti {

// ---------------------------------------------------------------------------
// NvmeClientState -- opaque gRPC state (forward-declared in the header)
// ---------------------------------------------------------------------------

struct NvmeClientState {
#ifdef TUTTI_NVMESERVICE_ENABLED
    std::unique_ptr<nvmeservice::NvmeServiceClient> client;
    // One session per physical device; session dtor sends Disconnect RPC.
    std::vector<std::unique_ptr<nvmeservice::NvmeServiceClient::Session>> sessions;
#endif
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DaemonNvmeDeviceDriver::DaemonNvmeDeviceDriver(
        IAccelerator*  accel,
        ILeaseManager* lease_mgr,
        std::string    daemon_addr,
        bool           mock_mode)
    : accel_(accel)
    , lease_mgr_(lease_mgr)
    , daemon_addr_(std::move(daemon_addr))
    , mock_mode_(mock_mode)
{}

DaemonNvmeDeviceDriver::~DaemonNvmeDeviceDriver() {
    shutdown();
}

// ---------------------------------------------------------------------------
// enumerate()
// ---------------------------------------------------------------------------

int DaemonNvmeDeviceDriver::enumerate(std::vector<IPhysicalDevice*>& out_devices) {
    auto state = std::make_unique<NvmeClientState>();

#ifdef TUTTI_NVMESERVICE_ENABLED
    // -----------------------------------------------------------------------
    // gRPC path: connect to the running nvmeservice daemon.
    // Skipped when mock_mode_ == true (unit-test-only bypass).
    // -----------------------------------------------------------------------
    if (!mock_mode_) {
    state->client = std::make_unique<nvmeservice::NvmeServiceClient>(daemon_addr_);

    auto device_list = state->client->list_devices();
    if (device_list.empty()) {
        std::fprintf(stderr,
            "[DaemonNvmeDeviceDriver] list_devices returned empty list from '%s'\n",
            daemon_addr_.c_str());
        return 0;
    }

    const int cuda_device = 0;
    const int num_queues  = 0;  // 0 = daemon default (capped by config)

    for (const auto& dev_info : device_list) {
        auto sess = state->client->connect(
            dev_info.device_id, cuda_device, num_queues);
        if (!sess) {
            std::fprintf(stderr,
                "[DaemonNvmeDeviceDriver] connect failed for device_id=%d\n",
                dev_info.device_id);
            continue;
        }

        nvm_ctrl_t* ctrl = nullptr;
        int rc = nvm_ctrl_attach_client(
            &ctrl,
            sess->snvme_dev_path.c_str(),
            static_cast<uint32_t>(sess->bar0_size));
        if (rc != 0) {
            std::fprintf(stderr,
                "[DaemonNvmeDeviceDriver] nvm_ctrl_attach_client(%s) failed: errno=%d\n",
                sess->snvme_dev_path.c_str(), rc);
            continue;
        }
        // nvm_ctrl_attach_client() does not call NVM_GET_DEV_INFO (that is
        // the owner's job).  The daemon already has device info and returns
        // q_depth in the Connect RPC response; copy it onto the ctrl handle
        // so QueuePair(B3) can validate ring sizes.
        if (sess->queue_depth > 0) {
            ctrl->q_depth = static_cast<uint16_t>(sess->queue_depth);
        }

        uint32_t group_id = 0, max_q = 0;
        rc = nvm_create_group(ctrl, &group_id, &max_q);
        if (rc != 0) {
            std::fprintf(stderr,
                "[DaemonNvmeDeviceDriver] nvm_create_group failed: errno=%d\n", rc);
            nvm_ctrl_free_client(ctrl);
            continue;
        }

        const uint32_t caps = 1u;  // GPUDIRECT_CAPABLE

        auto phys = std::make_unique<NvmePhysicalDevice>(
            dev_info.device_id,
            dev_info.pci_addr,
            "NVMe(daemon)@" + daemon_addr_,
            caps,
            static_cast<uint32_t>(sess->granted_queues));

        phys->ctrl           = ctrl;
        phys->namespace_id   = sess->namespace_id;
        phys->blk_size       = sess->blk_size;
        phys->blk_size_log   = sess->blk_size_log;
        phys->max_data_size  = sess->max_data_size;

        if (lease_id_.empty()) {
            lease_id_ = sess->allocation_id;
        }

        PhysContext ctx;
        ctx.ctrl     = ctrl;
        ctx.group_id = group_id;
        ctx.cuda_dev = cuda_device;

        out_devices.push_back(phys.get());
        phys_devices_.push_back(std::move(phys));
        phys_ctxs_.push_back(ctx);
        state->sessions.push_back(std::move(sess));
    }

    if (phys_devices_.empty()) {
        std::fprintf(stderr,
            "[DaemonNvmeDeviceDriver] no devices successfully attached\n");
        return 0;
    }
    } else {
        // mock_mode_ == true: bypass gRPC even though it was compiled in.
        // Same single-device mock used by the #else path below.
        lease_id_ = "lease-" + daemon_addr_ + "-" +
                    std::to_string(static_cast<unsigned long>(::getpid()));
        auto phys = std::make_unique<NvmePhysicalDevice>(
            0, "0000:01:00.0", "NVMe(daemon-mock)@" + daemon_addr_,
            /*caps=*/0u, /*queue_pairs=*/16u);
        out_devices.push_back(phys.get());
        phys_devices_.push_back(std::move(phys));
        phys_ctxs_.push_back(PhysContext{});
    }

#else   // !TUTTI_NVMESERVICE_ENABLED
    // -----------------------------------------------------------------------
    // Fallback mock grant -- used when gRPC was not found at build time.
    // Preserves the original single-device mock so unit tests continue to work.
    // -----------------------------------------------------------------------
    struct MockGrant {
        int32_t     device_id    = 0;
        std::string pci_addr     = "0000:01:00.0";
        std::string display_name;
        uint32_t    caps         = 0;
        uint32_t    queue_pairs  = 16;
        std::string lease_id;
    };

    MockGrant grant;
    grant.display_name = "NVMe(daemon-mock)@" + daemon_addr_;
    grant.lease_id     = "lease-" + daemon_addr_ + "-" +
                         std::to_string(static_cast<unsigned long>(::getpid()));
    lease_id_ = grant.lease_id;

    auto phys = std::make_unique<NvmePhysicalDevice>(
        grant.device_id, grant.pci_addr, grant.display_name,
        grant.caps, grant.queue_pairs);
    // ctrl / queue_group remain null in mock mode.

    out_devices.push_back(phys.get());
    phys_devices_.push_back(std::move(phys));
    // Add a placeholder PhysContext so index arithmetic in alloc_vdevice holds.
    phys_ctxs_.push_back(PhysContext{});
#endif  // TUTTI_NVMESERVICE_ENABLED

    client_state_ = std::move(state);

    // Start the higher-level lease heartbeat after lease_id_ is set.
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });

    return static_cast<int>(phys_devices_.size());
}

// ---------------------------------------------------------------------------
// heartbeat_loop()
// ---------------------------------------------------------------------------

void DaemonNvmeDeviceDriver::heartbeat_loop() {
    constexpr int kIntervalMs    = 5000;
    constexpr int kSliceMs       = 100;
    constexpr int kSlicesPerBeat = kIntervalMs / kSliceMs;

    while (!shutdown_requested_.load(std::memory_order_relaxed)) {
        for (int i = 0; i < kSlicesPerBeat; ++i) {
            if (shutdown_requested_.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        }
        if (shutdown_requested_.load(std::memory_order_relaxed)) return;

        if (lease_mgr_ && !lease_id_.empty()) {
            if (!lease_mgr_->heartbeat(lease_id_)) {
                std::fprintf(stderr,
                    "[DaemonNvmeDeviceDriver] heartbeat rejected: lease='%s'\n",
                    lease_id_.c_str());
            }
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

    auto pit = std::find_if(phys_devices_.begin(), phys_devices_.end(),
        [dev](const std::unique_ptr<NvmePhysicalDevice>& p) {
            return p.get() == dev;
        });
    if (pit == phys_devices_.end()) {
        if (error) *error = "dev does not belong to this driver";
        return nullptr;
    }

    const std::size_t idx = static_cast<std::size_t>(
        std::distance(phys_devices_.begin(), pit));
    NvmePhysicalDevice* nvme_phys = pit->get();
    const PhysContext&  ctx       = phys_ctxs_[idx];

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
        nvme_phys->id(), new_vdev_id, nvme_phys->caps());

    vdev->queue_quota   = resource_quota;
    vdev->namespace_id  = nvme_phys->namespace_id;
    vdev->blk_size      = nvme_phys->blk_size;
    vdev->blk_size_log  = nvme_phys->blk_size_log;
    vdev->max_data_size = nvme_phys->max_data_size;

    // Allocate GPU queue pairs when a real libnvm ctrl handle is available.
    if (ctx.ctrl != nullptr) {
        DaemonQueueAllocArgs alloc_args{};
        alloc_args.ctrl         = ctx.ctrl;
        alloc_args.group_id     = ctx.group_id;
        alloc_args.n_queues     = resource_quota;
        alloc_args.queue_depth  = (ctx.ctrl->q_depth > 0)
                                  ? static_cast<uint32_t>(ctx.ctrl->q_depth)
                                  : 64u;
        alloc_args.namespace_id = nvme_phys->namespace_id;
        alloc_args.cuda_device  = ctx.cuda_dev;
        alloc_args.blk_size     = nvme_phys->blk_size;
        alloc_args.blk_size_log = nvme_phys->blk_size_log;
        alloc_args.page_size    = static_cast<uint32_t>(ctx.ctrl->page_size);

        DaemonQueueSet qs{};
        int rc = daemon_nvme_alloc_queues(&alloc_args, &qs);
        if (rc != 0) {
            nvme_phys->release(resource_quota);  // undo reserve on failure
            if (error) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "daemon_nvme_alloc_queues failed: errno=%d", rc);
                *error = buf;
            }
            return nullptr;
        }

        vdev->d_qps = qs.d_qps;
        vdev->ctrl  = ctx.ctrl;          // expose ctrl for backend DMA mapping
        vdev_queue_sets_[vdev.get()] = qs;
    }
    // If ctx.ctrl is null (mock/fallback mode), d_qps stays null --
    // backends must check before dereferencing.

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

    auto qs_it = vdev_queue_sets_.find(vdev);
    if (qs_it != vdev_queue_sets_.end()) {
        daemon_nvme_free_queues(&qs_it->second);
        vdev_queue_sets_.erase(qs_it);
    }

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
    if (shutdown_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    if (lease_mgr_ && !lease_id_.empty()) {
        lease_mgr_->release_lease(lease_id_);
    }

    // Free all virtual devices (releases GPU queue sets).
    std::vector<IVirtualDevice*> to_free;
    to_free.reserve(vdevices_.size());
    for (const auto& v : vdevices_) to_free.push_back(v.get());
    for (IVirtualDevice* v : to_free) free_vdevice(v);

    // Tear down per-device libnvm state before the sessions are destroyed.
    for (auto& ctx : phys_ctxs_) {
        if (ctx.ctrl) {
            if (ctx.group_id != 0) {
                int rc = nvm_destroy_group(ctx.ctrl, ctx.group_id);
                if (rc != 0) {
                    std::fprintf(stderr,
                        "[DaemonNvmeDeviceDriver] nvm_destroy_group(%u) failed: errno=%d\n",
                        ctx.group_id, rc);
                }
            }
            nvm_ctrl_free_client(ctx.ctrl);
            ctx.ctrl     = nullptr;
            ctx.group_id = 0;
        }
    }
    phys_ctxs_.clear();
    phys_devices_.clear();

    // Session dtors send Disconnect RPCs; NvmeServiceClient dtor joins its
    // internal heartbeat thread.
    client_state_.reset();
}

} // namespace tutti
