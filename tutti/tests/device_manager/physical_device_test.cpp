// physical_device_test.cpp -- IPhysicalDevice / IVirtualDevice contract
//
// Verifies the grant-accounting semantics (Decision 1) and the generic
// virtual-device downcast contract, using vendor-neutral mocks only.

#include "mock_physical_device.h"
#include "mock_virtual_device.h"
#include "mock_lease_manager.h"
#include "mock_device_driver.h"

#include <gtest/gtest.h>

using namespace tutti;

// ── IPhysicalDevice grant semantics (Decision 1) ─────────────────────────────

TEST(PhysicalDeviceGrant, ProcessGrantIsPerProcessView) {
    // process_grant() is what THIS process was granted, not the hardware total.
    MockPhysicalDevice dev(/*id=*/0, DeviceType::LOCAL_NVME, "00:00.0",
                           "Mock", /*caps=*/0x1, /*grant=*/16);
    EXPECT_EQ(dev.process_grant(), 16u);
    EXPECT_EQ(dev.available_grant(), 16u) << "nothing allocated yet";
}

TEST(PhysicalDeviceGrant, AvailableGrantTracksReserveAndRelease) {
    MockPhysicalDevice dev(0, DeviceType::LOCAL_NVME, "00:00.0", "Mock", 0x1, 16);

    dev.reserve(4);
    EXPECT_EQ(dev.available_grant(), 12u);
    EXPECT_EQ(dev.process_grant(), 16u) << "grant is constant; only available changes";

    dev.reserve(12);
    EXPECT_EQ(dev.available_grant(), 0u);

    dev.release(16);
    EXPECT_EQ(dev.available_grant(), 16u);
}

TEST(PhysicalDeviceGrant, IdentityFields) {
    MockPhysicalDevice dev(7, DeviceType::RDMA, "0000:af:00.0", "NIC-A", 0x0, 8);
    EXPECT_EQ(dev.id(), 7);
    EXPECT_EQ(dev.type(), DeviceType::RDMA);
    EXPECT_EQ(dev.pci_addr(), "0000:af:00.0");
    EXPECT_EQ(dev.display_name(), "NIC-A");
    EXPECT_EQ(dev.caps(), 0x0u);
}

// ── IVirtualDevice generic contract + downcast ───────────────────────────────

TEST(VirtualDeviceContract, GenericFields) {
    MockVirtualDevice vdev(/*phys_id=*/3, /*vdev_id=*/1, DeviceType::LOCAL_NVME,
                           /*resource_count=*/8, /*caps=*/0x1);
    EXPECT_EQ(vdev.phys_id(), 3);
    EXPECT_EQ(vdev.vdev_id(), 1u);
    EXPECT_EQ(vdev.type(), DeviceType::LOCAL_NVME);
    EXPECT_EQ(vdev.resource_count(), 8u);
    EXPECT_EQ(vdev.caps(), 0x1u);
}

TEST(VirtualDeviceContract, TypeGuardedDowncast) {
    // The documented pattern: check type() before static_cast to a concrete type.
    MockVirtualDevice concrete(3, 1, DeviceType::LOCAL_NVME, 8, 0x1);
    IVirtualDevice* base = &concrete;

    ASSERT_EQ(base->type(), DeviceType::LOCAL_NVME);
    auto* down = static_cast<MockVirtualDevice*>(base);
    EXPECT_EQ(down->resource_count(), 8u);
    EXPECT_EQ(down, &concrete);
}
