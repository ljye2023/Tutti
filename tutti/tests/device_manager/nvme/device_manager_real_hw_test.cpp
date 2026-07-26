// device_manager_real_hw_test.cpp -- IDeviceManager upper-layer facade tests.
//
// These tests exercise the *upper-layer* resource-acquisition path that a
// real consumer (e.g. tutti/backends/nvme) uses:
//
//     create_device_manager({DaemonNvmeDeviceDriver}) -> IDeviceManager
//         dm->Open()
//         IVirtualDevice* v = dm->open_vdevice(phys_id, quota, &err)
//         assert v->type() == LOCAL_NVME
//         auto* nvme = static_cast<NvmeVirtualDevice*>(v)   // downcast
//         read nvme->d_qps / queue_quota / namespace_id / blk_size / ...
//         dm->close_vdevice(v)
//         dm->Close()
//
// This is distinct from daemon_driver_test.cpp, which drives the low-level
// DaemonNvmeDeviceDriver directly and bypasses IDeviceManager entirely.
//
// Unit tier (always runs, no hardware or daemon):
//   DeviceManagerFacadeUnit.*
//   Uses the driver's mock-grant mode; d_qps is null in mock mode so only
//   metadata + grant accounting through the facade are asserted.
//
// Integration tier (real hardware + live daemon, TUTTI_NVME_REAL_HW=1):
//   DeviceManagerFacadeRealHw.*
//   Goes through the real gRPC path and asserts open_vdevice() yields a
//   downcastable NvmeVirtualDevice with live GPU queues (d_qps non-null) and
//   populated namespace metadata.
//
// Env knobs (integration tier):
//   TUTTI_NVME_REAL_HW=1        required to run any real-HW case
//   TUTTI_NVME_ENDPOINT         daemon gRPC endpoint  (default 127.0.0.1:50051)

#include "common/device_manager_impl.h"   // create_device_manager, IDeviceManager
#include "common/idevice_driver.h"
#include "daemon_nvme_device_driver.h"
#include "nvme_virtual_device.h"
#include "mock_lease_manager.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace tutti;

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

bool env_flag(const char* key) {
    const char* v = std::getenv(key);
    return v && std::string(v) == "1";
}

// Build an IDeviceManager owning a single DaemonNvmeDeviceDriver.
//   mock_mode=true  -> no daemon required (unit tier)
//   mock_mode=false -> real gRPC path (real-HW tier)
std::unique_ptr<IDeviceManager> make_manager(ILeaseManager* lease,
                                             const std::string& addr,
                                             bool mock_mode) {
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    drivers.push_back(std::make_unique<DaemonNvmeDeviceDriver>(
        /*accel=*/nullptr, lease, addr, mock_mode));
    return create_device_manager(std::move(drivers));
}

}  // namespace

// ===========================================================================
// Unit tier -- mock-grant mode, no hardware / daemon.
// ===========================================================================

TEST(DeviceManagerFacadeUnit, OpenEnumeratesThroughFacade) {
    MockLeaseManager lease;
    auto dm = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);

    ASSERT_TRUE(dm->Open());
    EXPECT_GT(dm->device_count(), 0);

    IPhysicalDevice* phys = dm->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr) << "no LOCAL_NVME device surfaced through facade";
    EXPECT_GT(phys->process_grant(), 0u);

    dm->Close();
}

TEST(DeviceManagerFacadeUnit, OpenVdeviceReturnsDowncastableNvmeVirtual) {
    MockLeaseManager lease;
    auto dm = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(dm->Open());

    IPhysicalDevice* phys = dm->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    const int32_t  phys_id = phys->id();
    const uint32_t before  = dm->available_resources(phys_id);
    ASSERT_GE(before, 2u) << "mock grant too small for this test";

    std::string err;
    IVirtualDevice* v = dm->open_vdevice(phys_id, 2, &err);
    ASSERT_NE(v, nullptr) << "open_vdevice failed: " << err;

    // The facade returns a vendor-neutral handle; type() gates the downcast.
    ASSERT_EQ(v->type(), DeviceType::LOCAL_NVME);
    EXPECT_EQ(v->phys_id(), phys_id);
    EXPECT_EQ(v->resource_count(), 2u);

    auto* nvme = static_cast<NvmeVirtualDevice*>(v);
    EXPECT_EQ(nvme->queue_quota, 2u);
    // Mock mode: ctrl is null so d_qps stays null -- documented, not a failure.

    // Grant accounting is visible through the facade.
    EXPECT_EQ(dm->available_resources(phys_id), before - 2);

    dm->close_vdevice(v);
    EXPECT_EQ(dm->available_resources(phys_id), before)
        << "close_vdevice did not restore the pool";

    dm->Close();
}

TEST(DeviceManagerFacadeUnit, OpenVdeviceRejectsBadArgs) {
    MockLeaseManager lease;
    auto dm = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(dm->Open());

    std::string err;
    EXPECT_EQ(dm->open_vdevice(/*phys_id=*/99999, 1, &err), nullptr);
    EXPECT_FALSE(err.empty());

    IPhysicalDevice* phys = dm->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    err.clear();
    EXPECT_EQ(dm->open_vdevice(phys->id(), 0, &err), nullptr) << "quota 0 must fail";
    EXPECT_FALSE(err.empty());

    dm->Close();
}

// ===========================================================================
// Integration tier -- real hardware + live daemon (TUTTI_NVME_REAL_HW=1).
//
// Labelled "real_hw" so hardware-less nodes can opt out:  ctest -LE real_hw
// ===========================================================================

class DeviceManagerFacadeRealHw : public ::testing::Test {
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
    }

    std::string      endpoint_{"127.0.0.1:50051"};
    MockLeaseManager lease_;
};

// The full upper-layer flow: open the manager, open a vdevice through the
// facade, downcast to NvmeVirtualDevice, and read every NVMe resource field a
// backend needs (d_qps, queue_quota, namespace_id, blk_size, blk_size_log,
// max_data_size).  This is exactly how tutti/backends/nvme acquires resources.
TEST_F(DeviceManagerFacadeRealHw, OpenVdeviceExposesLiveNvmeResources) {
    auto dm = make_manager(&lease_, endpoint_, /*mock_mode=*/false);

    ASSERT_TRUE(dm->Open()) << "Open() failed -- is the daemon running at "
                            << endpoint_ << "?";
    ASSERT_GT(dm->device_count(), 0)
        << "facade surfaced 0 devices -- daemon unreachable?";

    IPhysicalDevice* phys = dm->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    const int32_t phys_id = phys->id();
    ASSERT_GT(dm->available_resources(phys_id), 0u)
        << "device granted zero queues";

    std::string err;
    IVirtualDevice* v = dm->open_vdevice(phys_id, 1, &err);
    ASSERT_NE(v, nullptr) << "open_vdevice failed: " << err;

    // Vendor-neutral gate, then downcast -- the pattern a backend must follow.
    ASSERT_EQ(v->type(), DeviceType::LOCAL_NVME);
    auto* nvme = static_cast<NvmeVirtualDevice*>(v);

    // Live GPU queue slice must be wired up on real hardware.
    EXPECT_NE(nvme->d_qps, nullptr)
        << "d_qps is null -- queue allocation failed or ctrl was null";
    EXPECT_EQ(nvme->queue_quota, 1u);

    // Namespace view -- populated from daemon session metadata.
    EXPECT_GT(nvme->namespace_id, 0u) << "namespace_id not set from session";
    EXPECT_GT(nvme->blk_size, 0u)     << "blk_size not set from session";
    EXPECT_GT(nvme->blk_size_log, 0u) << "blk_size_log not set from session";
    // max_data_size is MDTS in bytes; MDTS==0 legitimately means "no limit",
    // so we only report it rather than asserting a lower bound.
    RecordProperty("max_data_size", static_cast<int>(nvme->max_data_size));

    dm->close_vdevice(v);
    EXPECT_EQ(dm->available_resources(phys_id), phys->process_grant())
        << "grant not fully restored after close_vdevice";

    dm->Close();
}

// Two concurrent vdevices through the facade share one physical device's pool
// and both carry independent live queue slices.
TEST_F(DeviceManagerFacadeRealHw, TwoVdevicesShareOnePhysicalPool) {
    auto dm = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(dm->Open());

    IPhysicalDevice* phys = dm->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    const int32_t  phys_id = phys->id();
    const uint32_t total   = dm->available_resources(phys_id);
    ASSERT_GE(total, 2u) << "device granted < 2 queues; test needs >= 2";

    std::string err;
    IVirtualDevice* a = dm->open_vdevice(phys_id, 1, &err);
    ASSERT_NE(a, nullptr) << err;
    IVirtualDevice* b = dm->open_vdevice(phys_id, 1, &err);
    ASSERT_NE(b, nullptr) << err;

    EXPECT_EQ(dm->available_resources(phys_id), total - 2);
    EXPECT_NE(a->vdev_id(), b->vdev_id()) << "vdev ids must be distinct";

    auto* na = static_cast<NvmeVirtualDevice*>(a);
    auto* nb = static_cast<NvmeVirtualDevice*>(b);
    EXPECT_NE(na->d_qps, nullptr);
    EXPECT_NE(nb->d_qps, nullptr);
    EXPECT_NE(na->d_qps, nb->d_qps) << "two vdevices must own distinct queue slices";

    dm->close_vdevice(a);
    dm->close_vdevice(b);
    EXPECT_EQ(dm->available_resources(phys_id), total);

    dm->Close();
}
