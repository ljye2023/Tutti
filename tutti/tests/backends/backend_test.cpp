// backend_test.cpp -- IBackend device-agnostic contract (vendor-neutral)
//
// Drives MockBackend through the Layer 2 mock Device Manager stack: lifecycle,
// vdevice roster, handle validity, initialize() rollback on failure, shutdown
// idempotency + return-to-DM, and the BackendFactory path for BackendType::MOCK.

#include "mock_backend.h"

#include "mock_physical_device.h"
#include "mock_virtual_device.h"
#include "mock_lease_manager.h"
#include "mock_device_driver.h"

#include "backends/include/backend_factory.h"
#include "common/device_manager_impl.h"
#include "common/idevice_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace tutti;
using namespace tutti::backends;

namespace {

// Build a manager owning a single LOCAL_NVME mock driver: 2 devices, 16 QPs each.
// phys_id 0 and 1 each expose 16 resource units.
std::unique_ptr<IDeviceManager> make_manager(MockDeviceDriver** out = nullptr) {
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    auto driver = std::make_unique<MockDeviceDriver>(
        DeviceType::LOCAL_NVME, /*base_id=*/0, /*count=*/2, /*grant=*/16, /*caps=*/0x1);
    if (out) *out = driver.get();
    drivers.push_back(std::move(driver));
    return create_device_manager(std::move(drivers));
}

BackendConfig cfg(int32_t phys_id, uint32_t vdev_count, uint32_t quota) {
    BackendConfig c;
    c.phys_id = phys_id;
    c.vdevice_count = vdev_count;
    c.quota_per_vdevice = quota;
    return c;
}

}  // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────────

TEST(MockBackend, InitializeOpensRosterAndReportsInitialized) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    EXPECT_FALSE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 0u);

    ASSERT_TRUE(be.initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/3, /*quota=*/4)));
    EXPECT_TRUE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 3u);
    EXPECT_EQ(be.initialize_count(), 1);
    EXPECT_EQ(drv->live_vdevice_count(), 3u);
}

TEST(MockBackend, MetadataIdentity) {
    MockBackend be;
    EXPECT_EQ(be.backend_type(), BackendType::MOCK);
    EXPECT_STREQ(be.backend_name(), "mock");

    BackendMetadata m = be.metadata();
    EXPECT_STREQ(m.name, "mock");
    EXPECT_EQ(m.type, BackendType::MOCK);
    EXPECT_EQ(m.capabilities, 0u);  // no transport
}

// ── Roster + handle validity ─────────────────────────────────────────────────

TEST(MockBackend, RosterAccessorsAndHandles) {
    auto mgr = make_manager();
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/2, /*quota=*/4)));

    // In-range: non-null vdevice, valid dense-index handle.
    for (uint32_t i = 0; i < 2; ++i) {
        EXPECT_NE(be.vdevice_at(i), nullptr);
        VDeviceHandle h = be.vdevice_handle_at(i);
        EXPECT_TRUE(h.is_valid());
        EXPECT_EQ(h.index, i);
    }

    // Out-of-range: null vdevice, invalid handle.
    EXPECT_EQ(be.vdevice_at(2), nullptr);
    EXPECT_FALSE(be.vdevice_handle_at(2).is_valid());
    EXPECT_EQ(be.vdevice_handle_at(VDeviceHandle::INVALID).index, VDeviceHandle::INVALID);
}

// ── initialize() guards ──────────────────────────────────────────────────────

TEST(MockBackend, InitializeRejectsBadArguments) {
    auto mgr = make_manager();
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    EXPECT_FALSE(be.initialize(nullptr, cfg(0, 1, 1)));            // null manager
    EXPECT_FALSE(be.initialize(mgr.get(), cfg(0, /*count=*/0, 1)));// zero vdevices
    EXPECT_FALSE(be.initialize(mgr.get(), cfg(0, 1, /*quota=*/0)));// zero quota
    EXPECT_FALSE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 0u);
}

// ── initialize() rollback ────────────────────────────────────────────────────

TEST(MockBackend, InitializeRollsBackOnUnknownPhysId) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    // phys_id 999 does not exist -> the very first open_vdevice fails.
    EXPECT_FALSE(be.initialize(mgr.get(), cfg(/*phys=*/999, /*count=*/2, /*quota=*/4)));
    EXPECT_FALSE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 0u);
    EXPECT_EQ(drv->live_vdevice_count(), 0u);  // nothing leaked
}

TEST(MockBackend, InitializeRollsBackOnQuotaExhaustion) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    // phys 0 has 16 units. Ask for 3 vdevices x 8 = 24 -> the 3rd open fails
    // after the first two succeed. Rollback must return those two to the DM.
    MockBackend be;
    EXPECT_FALSE(be.initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/3, /*quota=*/8)));
    EXPECT_FALSE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 0u);
    EXPECT_EQ(drv->live_vdevice_count(), 0u);          // partial roster rolled back
    EXPECT_EQ(mgr->available_resources(0), 16u);       // quota fully restored
}

// ── shutdown() ───────────────────────────────────────────────────────────────

TEST(MockBackend, ShutdownReturnsVdevicesAndIsIdempotent) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/3, /*quota=*/4)));
    ASSERT_EQ(drv->live_vdevice_count(), 3u);

    be.shutdown();
    EXPECT_FALSE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 0u);
    EXPECT_EQ(drv->live_vdevice_count(), 0u);      // all returned to DM
    EXPECT_EQ(mgr->available_resources(0), 16u);
    EXPECT_EQ(be.shutdown_count(), 1);

    be.shutdown();                                 // idempotent: no-op, no counter bump
    EXPECT_EQ(be.shutdown_count(), 1);
}

TEST(MockBackend, ReinitializeAfterShutdown) {
    MockDeviceDriver* drv = nullptr;
    auto mgr = make_manager(&drv);
    ASSERT_TRUE(mgr->Open());

    MockBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/2, /*quota=*/4)));
    be.shutdown();
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(/*phys=*/1, /*count=*/1, /*quota=*/4)));

    EXPECT_TRUE(be.is_initialized());
    EXPECT_EQ(be.vdevice_count(), 1u);
    EXPECT_EQ(be.initialize_count(), 2);
    EXPECT_EQ(drv->live_vdevice_count(), 1u);
}

// ── Factory path (scheme B) ──────────────────────────────────────────────────

TEST(BackendFactory, MockIsRegistered) {
    EXPECT_TRUE(BackendFactory::is_registered(BackendType::MOCK));

    auto types = BackendFactory::available_backends();
    EXPECT_NE(std::find(types.begin(), types.end(), BackendType::MOCK), types.end());
}

TEST(BackendFactory, CreatesMockBackendUsableThroughInterface) {
    auto mgr = make_manager();
    ASSERT_TRUE(mgr->Open());

    std::unique_ptr<IBackend> be = BackendFactory::create_backend(BackendType::MOCK);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(be->backend_type(), BackendType::MOCK);

    ASSERT_TRUE(be->initialize(mgr.get(), cfg(/*phys=*/0, /*count=*/2, /*quota=*/4)));
    EXPECT_EQ(be->vdevice_count(), 2u);
    EXPECT_TRUE(be->vdevice_handle_at(0).is_valid());
    be->shutdown();
}

TEST(BackendFactory, UnknownTypeReturnsNull) {
    EXPECT_EQ(BackendFactory::create_backend(BackendType::UNKNOWN), nullptr);
}
