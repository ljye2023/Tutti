// device_manager_test.cpp -- DeviceManagerImpl dispatcher (vendor-neutral)
//
// Exercises the generic IDeviceManager facade against mock drivers: lifecycle,
// registry queries, driver dispatch, vdevice allocation, and teardown ordering.

#include "mock_physical_device.h"
#include "mock_virtual_device.h"
#include "mock_lease_manager.h"
#include "mock_device_driver.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/device_manager_impl.h"

using namespace tutti;

namespace {

// Build a manager owning a single LOCAL_NVME mock driver: 2 devices, 16 QPs each.
std::unique_ptr<IDeviceManager> make_single_driver_manager(MockDeviceDriver** out = nullptr) {
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    auto driver = std::make_unique<MockDeviceDriver>(
        DeviceType::LOCAL_NVME, /*base_id=*/0, /*count=*/2, /*grant=*/16, /*caps=*/0x1);
    if (out) *out = driver.get();
    drivers.push_back(std::move(driver));
    return create_device_manager(std::move(drivers));
}

}  // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

TEST(DeviceManagerLifecycle, OpenEnumeratesAllDrivers) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_single_driver_manager(&drv);

    EXPECT_EQ(mgr->device_count(), 0) << "no devices before Open()";
    ASSERT_TRUE(mgr->Open());
    EXPECT_EQ(drv->enumerate_count(), 1);
    EXPECT_EQ(mgr->device_count(), 2);
}

TEST(DeviceManagerLifecycle, OpenIsIdempotent) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_single_driver_manager(&drv);

    ASSERT_TRUE(mgr->Open());
    ASSERT_TRUE(mgr->Open());  // second Open is a no-op
    EXPECT_EQ(drv->enumerate_count(), 1) << "enumerate must not run twice";
    EXPECT_EQ(mgr->device_count(), 2);
}

TEST(DeviceManagerLifecycle, CloseCallsDriverShutdown) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_single_driver_manager(&drv);

    ASSERT_TRUE(mgr->Open());
    mgr->Close();
    EXPECT_EQ(drv->shutdown_count(), 1);
    EXPECT_EQ(mgr->device_count(), 0) << "registry cleared after Close()";
}

TEST(DeviceManagerLifecycle, OpenFailsWhenDriverEnumerateFails) {
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    auto driver = std::make_unique<MockDeviceDriver>(
        DeviceType::LOCAL_NVME, 0, 2, 16, 0x1);
    driver->set_fail_enumerate(true);
    drivers.push_back(std::move(driver));
    auto mgr = create_device_manager(std::move(drivers));

    EXPECT_FALSE(mgr->Open());
    EXPECT_EQ(mgr->device_count(), 0);
}

// ── Registry queries ──────────────────────────────────────────────────────────

TEST(DeviceManagerRegistry, DeviceAtAndFindById) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* d0 = mgr->device_at(0);
    IPhysicalDevice* d1 = mgr->device_at(1);
    ASSERT_NE(d0, nullptr);
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(d0->id(), 0);
    EXPECT_EQ(d1->id(), 1);

    EXPECT_EQ(mgr->find_by_id(1), d1);
    EXPECT_EQ(mgr->find_by_id(0), d0);
}

TEST(DeviceManagerRegistry, OutOfRangeAndUnknownReturnNull) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());

    EXPECT_EQ(mgr->device_at(-1), nullptr);
    EXPECT_EQ(mgr->device_at(2), nullptr);
    EXPECT_EQ(mgr->find_by_id(999), nullptr);
}

TEST(DeviceManagerRegistry, ListReturnsAllDevices) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());

    auto all = mgr->list();
    EXPECT_EQ(all.size(), 2u);
}

TEST(DeviceManagerRegistry, FindByTypeWithOrdinal) {
    // Two drivers of different types; NVMe ids [0,1], RDMA ids [100].
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    drivers.push_back(std::make_unique<MockDeviceDriver>(
        DeviceType::LOCAL_NVME, /*base_id=*/0, /*count=*/2, /*grant=*/16, /*caps=*/0x1));
    drivers.push_back(std::make_unique<MockDeviceDriver>(
        DeviceType::RDMA, /*base_id=*/100, /*count=*/1, /*grant=*/8, /*caps=*/0x0));
    auto mgr = create_device_manager(std::move(drivers));
    ASSERT_TRUE(mgr->Open());

    EXPECT_EQ(mgr->device_count(), 3);

    IPhysicalDevice* nvme0 = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    IPhysicalDevice* nvme1 = mgr->find_by_type(DeviceType::LOCAL_NVME, 1);
    IPhysicalDevice* rdma0 = mgr->find_by_type(DeviceType::RDMA, 0);
    ASSERT_NE(nvme0, nullptr);
    ASSERT_NE(nvme1, nullptr);
    ASSERT_NE(rdma0, nullptr);
    EXPECT_EQ(nvme0->id(), 0);
    EXPECT_EQ(nvme1->id(), 1);
    EXPECT_EQ(rdma0->id(), 100);
    EXPECT_EQ(rdma0->type(), DeviceType::RDMA);

    // Out-of-range ordinal and absent type.
    EXPECT_EQ(mgr->find_by_type(DeviceType::LOCAL_NVME, 2), nullptr);
    EXPECT_EQ(mgr->find_by_type(DeviceType::GDS, 0), nullptr);
}

// ── Virtual device allocation / dispatch ──────────────────────────────────────

TEST(DeviceManagerAlloc, OpenVdeviceDispatchesToDriver) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());

    std::string err;
    IVirtualDevice* vdev = mgr->open_vdevice(0, 4, &err);
    ASSERT_NE(vdev, nullptr) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(vdev->phys_id(), 0);
    EXPECT_EQ(vdev->resource_count(), 4u);
    EXPECT_EQ(vdev->type(), DeviceType::LOCAL_NVME);

    // Grant accounting flows through IPhysicalDevice.
    EXPECT_EQ(mgr->available_resources(0), 12u);

    mgr->close_vdevice(vdev);
    EXPECT_EQ(mgr->available_resources(0), 16u);
}

TEST(DeviceManagerAlloc, RejectsZeroQuotaUnknownIdAndExhaustion) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());
    std::string err;

    EXPECT_EQ(mgr->open_vdevice(0, 0, &err), nullptr);
    EXPECT_FALSE(err.empty());

    err.clear();
    EXPECT_EQ(mgr->open_vdevice(999, 4, &err), nullptr);
    EXPECT_FALSE(err.empty());

    err.clear();
    EXPECT_EQ(mgr->open_vdevice(0, 100, &err), nullptr) << "quota exceeds grant";
    EXPECT_FALSE(err.empty());
}

TEST(DeviceManagerAlloc, MultipleVdevicesShareOneDeviceGrant) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());
    std::string err;

    IVirtualDevice* a = mgr->open_vdevice(0, 8, &err);
    IVirtualDevice* b = mgr->open_vdevice(0, 8, &err);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(mgr->available_resources(0), 0u);

    // Pool exhausted -> next alloc fails.
    EXPECT_EQ(mgr->open_vdevice(0, 1, &err), nullptr);

    // Freeing one returns its slice.
    mgr->close_vdevice(a);
    EXPECT_EQ(mgr->available_resources(0), 8u);
    mgr->close_vdevice(b);
    EXPECT_EQ(mgr->available_resources(0), 16u);
}

TEST(DeviceManagerAlloc, CloseNullptrIsNoop) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());
    mgr->close_vdevice(nullptr);  // must not crash
    SUCCEED();
}

TEST(DeviceManagerAlloc, CapsQuery) {
    auto mgr = make_single_driver_manager();
    ASSERT_TRUE(mgr->Open());
    EXPECT_EQ(mgr->caps(0), 0x1u);
    EXPECT_EQ(mgr->caps(999), 0u) << "unknown id -> 0";
}

// ── Teardown safety ───────────────────────────────────────────────────────────

TEST(DeviceManagerTeardown, CloseFreesOutstandingVdevices) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_single_driver_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    std::string err;
    ASSERT_NE(mgr->open_vdevice(0, 4, &err), nullptr);
    ASSERT_NE(mgr->open_vdevice(1, 4, &err), nullptr);
    EXPECT_EQ(drv->live_vdevice_count(), 2u);

    mgr->Close();
    EXPECT_EQ(drv->live_vdevice_count(), 0u) << "manager frees live vdevices on Close()";
    EXPECT_EQ(drv->shutdown_count(), 1);
}

TEST(DeviceManagerTeardown, DestructorClosesWhenOpen) {
    // Observe teardown through a lease manager that OUTLIVES the driver, since
    // the driver itself is owned by (and destroyed with) the manager. Reading
    // the driver pointer after the manager dies would be use-after-free.
    MockLeaseManager lease;
    {
        std::vector<std::unique_ptr<IDeviceDriver>> drivers;
        drivers.push_back(std::make_unique<MockDeviceDriver>(
            DeviceType::LOCAL_NVME, /*base_id=*/0, /*count=*/1, /*grant=*/16,
            /*caps=*/0x1, &lease));
        auto mgr = create_device_manager(std::move(drivers));
        ASSERT_TRUE(mgr->Open());
        EXPECT_EQ(lease.heartbeats(), 1) << "driver beats once at enumerate()";
        std::string err;
        ASSERT_NE(mgr->open_vdevice(0, 4, &err), nullptr);
        // ~DeviceManagerImpl() runs here -> Close() -> driver->shutdown()
    }
    EXPECT_EQ(lease.releases(), 1) << "~DeviceManagerImpl() must Close() -> shutdown() -> release_lease()";
}
