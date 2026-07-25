// daemon_driver_test.cpp -- DaemonNvmeDeviceDriver unit + integration tests.
//
// Unit tier (always runs, no hardware or gRPC required):
//   DaemonDriverUnit.*
//   Drives the driver in mock-grant mode (TUTTI_NVMESERVICE_ENABLED not
//   defined at test-build time).  Covers enumerate/alloc/free lifecycle,
//   quota accounting, error paths, shutdown ordering, and lease release.
//
// Integration tier (real hardware + live daemon, TUTTI_NVME_REAL_HW=1):
//   DaemonDriverRealHw.*
//   Instantiates DaemonNvmeDeviceDriver against an actual nvmeservice daemon
//   and asserts that enumerate() populates real device metadata and
//   alloc_vdevice() returns a live vdevice (d_qps non-null) tied to GPU queues.
//
// Env knobs (integration tier):
//   TUTTI_NVME_REAL_HW=1        required to run any real-HW case
//   TUTTI_NVME_ENDPOINT         daemon gRPC endpoint  (default 127.0.0.1:50051)
//   TUTTI_NVME_DEVICE_ID        daemon device_id      (default 0)
//   TUTTI_NVME_CUDA             CUDA device index     (default 0)

#include "daemon_nvme_device_driver.h"
#include "nvme_virtual_device.h"
#include "nvme_physical_device.h"
#include "mock_lease_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace tutti;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

bool env_flag(const char* key) {
    const char* v = std::getenv(key);
    return v && std::string(v) == "1";
}

// Build a DaemonNvmeDeviceDriver in mock mode (no daemon required).
// mock_mode=true bypasses gRPC even when TUTTI_NVMESERVICE_ENABLED is defined,
// so unit tests never need a live daemon.
std::unique_ptr<DaemonNvmeDeviceDriver> make_driver(
        MockLeaseManager* lease = nullptr,
        const std::string& addr = "127.0.0.1:59999") {
    return std::make_unique<DaemonNvmeDeviceDriver>(
        /*accel=*/nullptr, lease, addr, /*mock_mode=*/true);
}

// Enumerate + collect physical device pointers.
int do_enumerate(DaemonNvmeDeviceDriver& drv,
                 std::vector<IPhysicalDevice*>& out) {
    return drv.enumerate(out);
}

}  // namespace

// ===========================================================================
// Unit tier -- exercises mock-grant path (no daemon, no CUDA required)
// ===========================================================================

// ── Lifecycle ───────────────────────────────────────────────────────────────

TEST(DaemonDriverUnit, EnumerateReturnsOneDevice) {
    MockLeaseManager lease;
    auto drv = make_driver(&lease);

    std::vector<IPhysicalDevice*> devs;
    int n = do_enumerate(*drv, devs);

    EXPECT_GE(n, 1) << "enumerate() should return at least one device";
    EXPECT_EQ(devs.size(), static_cast<size_t>(n));
    for (auto* d : devs) {
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->type(), DeviceType::LOCAL_NVME);
        EXPECT_GT(d->process_grant(), 0u);
    }
}

TEST(DaemonDriverUnit, EnumerateDeviceGrantPositive) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);

    ASSERT_FALSE(devs.empty());
    IPhysicalDevice* d = devs[0];
    // grant == available_grant when nothing is allocated yet
    EXPECT_EQ(d->available_grant(), d->process_grant());
    EXPECT_GT(d->available_grant(), 0u);
}

TEST(DaemonDriverUnit, ShutdownReleasesLease) {
    MockLeaseManager lease;
    auto drv = make_driver(&lease);

    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    drv->shutdown();
    EXPECT_EQ(lease.releases(), 1)
        << "shutdown() must call lease_mgr_->release_lease()";
}

TEST(DaemonDriverUnit, DestructorReleasesLease) {
    MockLeaseManager lease;
    {
        auto drv = make_driver(&lease);
        std::vector<IPhysicalDevice*> devs;
        drv->enumerate(devs);
    }  // destructor calls shutdown()
    EXPECT_EQ(lease.releases(), 1);
}

TEST(DaemonDriverUnit, DoubleShutdownIsSafe) {
    MockLeaseManager lease;
    auto drv = make_driver(&lease);
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);

    drv->shutdown();
    drv->shutdown();  // must not crash or double-release
    EXPECT_EQ(lease.releases(), 1) << "release_lease called exactly once";
}

// ── alloc_vdevice() -- valid paths ───────────────────────────────────────────

TEST(DaemonDriverUnit, AllocVdeviceReturnsValidDescriptor) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    std::string err;
    IVirtualDevice* vdev = drv->alloc_vdevice(devs[0], 4, &err);
    ASSERT_NE(vdev, nullptr) << "alloc_vdevice failed: " << err;
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(vdev->phys_id(), devs[0]->id());
    EXPECT_EQ(vdev->resource_count(), 4u);
    EXPECT_EQ(vdev->type(), DeviceType::LOCAL_NVME);
}

TEST(DaemonDriverUnit, AllocVdeviceNvmeFields) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    std::string err;
    IVirtualDevice* vdev = drv->alloc_vdevice(devs[0], 2, &err);
    ASSERT_NE(vdev, nullptr) << err;

    auto* nvme = static_cast<NvmeVirtualDevice*>(vdev);
    EXPECT_EQ(nvme->queue_quota, 2u);
    // In mock mode ctrl is null so d_qps is null; that's expected.
    // namespace_id / blk_size can be zero (mock defaults); we only check
    // that the cast works and queue_quota matches the requested count.
}

TEST(DaemonDriverUnit, AllocVdeviceDecreasesAvailableGrant) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    const uint32_t before = devs[0]->available_grant();
    std::string err;
    ASSERT_NE(drv->alloc_vdevice(devs[0], 4, &err), nullptr) << err;
    EXPECT_EQ(devs[0]->available_grant(), before - 4);
}

TEST(DaemonDriverUnit, MultipleVdevicesShareOneGrant) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    const uint32_t total = devs[0]->available_grant();
    ASSERT_GE(total, 8u) << "mock grant must be >= 8 for this test";

    std::string err;
    IVirtualDevice* a = drv->alloc_vdevice(devs[0], 4, &err);
    IVirtualDevice* b = drv->alloc_vdevice(devs[0], 4, &err);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    EXPECT_EQ(devs[0]->available_grant(), total - 8);

    drv->free_vdevice(a);
    EXPECT_EQ(devs[0]->available_grant(), total - 4);
    drv->free_vdevice(b);
    EXPECT_EQ(devs[0]->available_grant(), total);
}

// ── alloc_vdevice() -- error paths ──────────────────────────────────────────

TEST(DaemonDriverUnit, AllocNullDevReturnsNullptr) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);

    std::string err;
    EXPECT_EQ(drv->alloc_vdevice(nullptr, 4, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(DaemonDriverUnit, AllocZeroQuotaReturnsNullptr) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    std::string err;
    EXPECT_EQ(drv->alloc_vdevice(devs[0], 0, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(DaemonDriverUnit, AllocExceedingGrantReturnsNullptr) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    const uint32_t too_many = devs[0]->available_grant() + 1;
    std::string err;
    EXPECT_EQ(drv->alloc_vdevice(devs[0], too_many, &err), nullptr);
    EXPECT_FALSE(err.empty());
    // Grant must be unchanged after the failed alloc.
    EXPECT_EQ(devs[0]->available_grant(), devs[0]->process_grant());
}

TEST(DaemonDriverUnit, AllocWithForeignDevReturnsNullptr) {
    // Two independent drivers; pass driver-A's device to driver-B.
    auto drvA = make_driver(nullptr, "127.0.0.1:59991");
    auto drvB = make_driver(nullptr, "127.0.0.1:59992");

    std::vector<IPhysicalDevice*> devsA, devsB;
    drvA->enumerate(devsA);
    drvB->enumerate(devsB);
    ASSERT_FALSE(devsA.empty());

    std::string err;
    EXPECT_EQ(drvB->alloc_vdevice(devsA[0], 1, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

// ── Exhaustion + re-alloc ────────────────────────────────────────────────────

TEST(DaemonDriverUnit, GrantExhaustion) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    const uint32_t total = devs[0]->available_grant();
    ASSERT_GT(total, 0u);

    std::string err;
    IVirtualDevice* vdev = drv->alloc_vdevice(devs[0], total, &err);
    ASSERT_NE(vdev, nullptr) << err;
    EXPECT_EQ(devs[0]->available_grant(), 0u);

    // Pool is now exhausted -- any further alloc must fail.
    EXPECT_EQ(drv->alloc_vdevice(devs[0], 1, &err), nullptr);

    drv->free_vdevice(vdev);
    EXPECT_EQ(devs[0]->available_grant(), total) << "grant not restored after free";
}

TEST(DaemonDriverUnit, FreeAndReallocSucceeds) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    std::string err;
    IVirtualDevice* first = drv->alloc_vdevice(devs[0], 2, &err);
    ASSERT_NE(first, nullptr);
    drv->free_vdevice(first);

    IVirtualDevice* second = drv->alloc_vdevice(devs[0], 2, &err);
    ASSERT_NE(second, nullptr) << "re-alloc after free must succeed: " << err;
    drv->free_vdevice(second);
}

// ── free_vdevice() -- error paths ────────────────────────────────────────────

TEST(DaemonDriverUnit, FreeNullptrIsNoop) {
    auto drv = make_driver();
    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    drv->free_vdevice(nullptr);  // must not crash
    SUCCEED();
}

TEST(DaemonDriverUnit, FreeForeignVdevIsNoop) {
    // Two drivers; free driver-A's vdevice through driver-B.
    auto drvA = make_driver(nullptr, "127.0.0.1:59993");
    auto drvB = make_driver(nullptr, "127.0.0.1:59994");

    std::vector<IPhysicalDevice*> devsA, devsB;
    drvA->enumerate(devsA); drvB->enumerate(devsB);
    ASSERT_FALSE(devsA.empty());

    std::string err;
    IVirtualDevice* vdev = drvA->alloc_vdevice(devsA[0], 1, &err);
    ASSERT_NE(vdev, nullptr);

    const uint32_t grantA_before = devsA[0]->available_grant();
    drvB->free_vdevice(vdev);  // wrong driver -- must log+return, not crash
    // Grant on driver-A's device must be unchanged.
    EXPECT_EQ(devsA[0]->available_grant(), grantA_before);

    drvA->free_vdevice(vdev);  // proper cleanup
}

// ── Shutdown ordering ────────────────────────────────────────────────────────

TEST(DaemonDriverUnit, ShutdownFreesOutstandingVdevices) {
    MockLeaseManager lease;
    auto drv = make_driver(&lease);

    std::vector<IPhysicalDevice*> devs;
    drv->enumerate(devs);
    ASSERT_FALSE(devs.empty());

    std::string err;
    drv->alloc_vdevice(devs[0], 2, &err);
    drv->alloc_vdevice(devs[0], 2, &err);

    // Shutdown must free outstanding vdevices (no leaked GPU resources).
    // After shutdown the device list and vdevice list are cleared; we can't
    // deref devs[0] any more.  We only verify shutdown completes without crashing.
    drv->shutdown();
    SUCCEED();
}

TEST(DaemonDriverUnit, ShutdownBeforeEnumerateIsSafe) {
    MockLeaseManager lease;
    auto drv = make_driver(&lease);
    drv->shutdown();  // shutdown without prior enumerate -- must not crash
    EXPECT_EQ(lease.releases(), 0) << "no lease to release if enumerate never ran";
}

// ── Driver type ──────────────────────────────────────────────────────────────

TEST(DaemonDriverUnit, TypeIsLocalNvme) {
    auto drv = make_driver();
    EXPECT_EQ(drv->type(), DeviceType::LOCAL_NVME);
}

// ===========================================================================
// Integration tier -- requires TUTTI_NVME_REAL_HW=1 + live daemon
// ===========================================================================
//
// These tests instantiate DaemonNvmeDeviceDriver directly (not via the client
// binary) so they require TUTTI_NVMESERVICE_ENABLED to be set at build time
// (i.e. gRPC must be found) and a running nvmeservice daemon.
//
// They are labelled "real_hw" so the CI matrix can opt them out:
//   ctest -LE real_hw   (exclude)
//   ctest -L  real_hw   (opt-in)

class DaemonDriverRealHw : public ::testing::Test {
protected:
    void SetUp() override {
        if (!env_flag("TUTTI_NVME_REAL_HW")) {
            GTEST_SKIP() << "TUTTI_NVME_REAL_HW != 1";
        }
#ifndef TUTTI_NVMESERVICE_ENABLED
        GTEST_SKIP() << "built without TUTTI_NVMESERVICE_ENABLED "
                        "(gRPC not found at configure time)";
#endif
        endpoint_ = env_or("TUTTI_NVME_ENDPOINT", "127.0.0.1:50051");
        cuda_dev_ = std::stoi(env_or("TUTTI_NVME_CUDA", "0"));
    }

    std::string endpoint_{"127.0.0.1:50051"};
    int         cuda_dev_{0};
    MockLeaseManager lease_;
};

TEST_F(DaemonDriverRealHw, EnumerateConnectsAndFindsDevices) {
    auto drv = std::make_unique<DaemonNvmeDeviceDriver>(
        nullptr, &lease_, endpoint_);

    std::vector<IPhysicalDevice*> devs;
    int n = drv->enumerate(devs);

    ASSERT_GT(n, 0) << "enumerate() returned 0 devices -- is the daemon running at "
                    << endpoint_ << "?";
    EXPECT_EQ(devs.size(), static_cast<size_t>(n));

    for (auto* d : devs) {
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->type(), DeviceType::LOCAL_NVME);
        EXPECT_GT(d->available_grant(), 0u)
            << "daemon returned a device with zero granted queues";

        auto* np = static_cast<NvmePhysicalDevice*>(d);
        // With a live daemon, ctrl must be non-null (nvm_ctrl_attach_client ran).
        EXPECT_NE(np->ctrl, nullptr)
            << "ctrl is null -- nvm_ctrl_attach_client may have failed";
        // Namespace metadata is populated from the session.
        EXPECT_GT(np->blk_size, 0u) << "blk_size not set from session metadata";
    }
}

TEST_F(DaemonDriverRealHw, AllocVdevicePopulatesQueuePointer) {
    auto drv = std::make_unique<DaemonNvmeDeviceDriver>(
        nullptr, &lease_, endpoint_);

    std::vector<IPhysicalDevice*> devs;
    ASSERT_GT(drv->enumerate(devs), 0) << "no devices, daemon unreachable?";

    std::string err;
    IVirtualDevice* vdev = drv->alloc_vdevice(devs[0], 1, &err);
    ASSERT_NE(vdev, nullptr) << "alloc_vdevice failed: " << err;

    auto* nvme = static_cast<NvmeVirtualDevice*>(vdev);
    EXPECT_NE(nvme->d_qps, nullptr)
        << "d_qps is null -- daemon_nvme_alloc_queues failed or ctrl was null";
    EXPECT_EQ(nvme->queue_quota, 1u);
    EXPECT_GT(nvme->blk_size, 0u);

    drv->free_vdevice(vdev);
    // After free, available grant must be restored.
    EXPECT_EQ(devs[0]->available_grant(), devs[0]->process_grant());
}

TEST_F(DaemonDriverRealHw, MultipleAllocsAndFreeRestoresGrant) {
    auto drv = std::make_unique<DaemonNvmeDeviceDriver>(
        nullptr, &lease_, endpoint_);

    std::vector<IPhysicalDevice*> devs;
    ASSERT_GT(drv->enumerate(devs), 0);

    const uint32_t total = devs[0]->available_grant();
    ASSERT_GE(total, 2u) << "daemon granted fewer than 2 queues; test needs >= 2";

    std::string err;
    IVirtualDevice* a = drv->alloc_vdevice(devs[0], 1, &err);
    IVirtualDevice* b = drv->alloc_vdevice(devs[0], 1, &err);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    EXPECT_EQ(devs[0]->available_grant(), total - 2);

    drv->free_vdevice(a);
    EXPECT_EQ(devs[0]->available_grant(), total - 1);
    drv->free_vdevice(b);
    EXPECT_EQ(devs[0]->available_grant(), total);
}

TEST_F(DaemonDriverRealHw, ShutdownDisconnectsFromDaemon) {
    auto drv = std::make_unique<DaemonNvmeDeviceDriver>(
        nullptr, &lease_, endpoint_);

    std::vector<IPhysicalDevice*> devs;
    ASSERT_GT(drv->enumerate(devs), 0);

    std::string err;
    drv->alloc_vdevice(devs[0], 1, &err);  // leave a vdevice open

    drv->shutdown();  // must free the vdevice + nvm_destroy_group + Disconnect RPC
    EXPECT_EQ(lease_.releases(), 1)
        << "shutdown() did not call release_lease()";
}
