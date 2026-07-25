// lease_manager_test.cpp -- ILeaseManager contract + NullLeaseManager
//
// The vendor-neutral lease interface. NullLeaseManager is the direct-mode
// (single-process) no-op shipped in the common layer; MockLeaseManager is
// a test double proving the per-driver injection contract (Decision 4).

#include "mock_physical_device.h"
#include "mock_virtual_device.h"
#include "mock_lease_manager.h"
#include "mock_device_driver.h"

#include <gtest/gtest.h>

#include "common/null_lease_manager.h"

using namespace tutti;

// ── NullLeaseManager (direct mode) ───────────────────────────────────────────

TEST(NullLeaseManager, AllOperationsSucceedAsNoop) {
    NullLeaseManager lease;
    EXPECT_TRUE(lease.heartbeat("any-id"));
    EXPECT_TRUE(lease.release_lease("any-id"));
    EXPECT_TRUE(lease.has_lease("any-id"));
    // Empty id is also fine in direct mode.
    EXPECT_TRUE(lease.heartbeat(""));
    EXPECT_TRUE(lease.has_lease(""));
}

TEST(NullLeaseManager, UsableThroughInterfacePointer) {
    NullLeaseManager concrete;
    ILeaseManager* lease = &concrete;
    EXPECT_TRUE(lease->heartbeat("id"));
    EXPECT_TRUE(lease->release_lease("id"));
}

// ── Per-driver injection (Decision 4) ────────────────────────────────────────

TEST(LeaseInjection, DriverBeatsOnEnumerateAndReleasesOnShutdown) {
    MockLeaseManager lease;
    MockDeviceDriver driver(DeviceType::LOCAL_NVME, /*base_id=*/0, /*count=*/1,
                            /*grant=*/16, /*caps=*/0x1, &lease);

    std::vector<IPhysicalDevice*> devices;
    ASSERT_EQ(driver.enumerate(devices), 1);
    EXPECT_EQ(lease.heartbeats(), 1) << "grant acquired at enumerate()";
    EXPECT_EQ(lease.releases(), 0);

    driver.shutdown();
    EXPECT_EQ(lease.releases(), 1) << "grant released at shutdown()";
}

TEST(LeaseInjection, EachDriverOwnsItsOwnLease) {
    // Two drivers, two independent lease managers -> per-driver scoping.
    MockLeaseManager lease_a;
    MockLeaseManager lease_b;
    MockDeviceDriver a(DeviceType::LOCAL_NVME, 0, 1, 16, 0x1, &lease_a);
    MockDeviceDriver b(DeviceType::RDMA, 100, 1, 8, 0x0, &lease_b);

    std::vector<IPhysicalDevice*> da, db;
    a.enumerate(da);
    b.enumerate(db);
    EXPECT_EQ(lease_a.heartbeats(), 1);
    EXPECT_EQ(lease_b.heartbeats(), 1);

    a.shutdown();
    EXPECT_EQ(lease_a.releases(), 1);
    EXPECT_EQ(lease_b.releases(), 0) << "shutting down A must not touch B's lease";

    b.shutdown();
    EXPECT_EQ(lease_b.releases(), 1);
}
