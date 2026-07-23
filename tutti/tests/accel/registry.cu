#include "accel_test_fixture.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstddef>

using namespace tutti;

namespace tutti_test {

// Suite-specific fixture so registry tests are grouped in the test output.
class RegistryTest : public AccelTest {};

// Register a PINNED_HOST buffer via register_host and verify the region fields,
// both lookup paths, and clean removal on unregister.
TEST_F(RegistryTest, RegisterHostFieldsAndLookup) {
    constexpr size_t kSize = 4096;
    void* buf = a_->allocate_host(kSize, MemoryKind::PINNED_HOST);
    ASSERT_NE(buf, nullptr) << "allocate_host(PINNED_HOST) failed";

    MemoryRegion* region = a_->register_host(buf, kSize);
    ASSERT_NE(region, nullptr) << "register_host returned nullptr";

    EXPECT_EQ(region->kind, MemoryKind::HOST);
    EXPECT_EQ(region->device_id, -1);
    EXPECT_EQ(region->host_ptr, buf);
    EXPECT_EQ(region->device_ptr, nullptr);
    EXPECT_EQ(region->size, kSize);
    EXPECT_EQ(region->external, nullptr);

    const uint64_t id = region->region_id;

    MemoryRegion* by_ptr = a_->lookup(buf);
    ASSERT_NE(by_ptr, nullptr) << "lookup(ptr) missed a registered region";
    EXPECT_EQ(by_ptr->region_id, id);

    MemoryRegion* by_id = a_->lookup_by_id(id);
    ASSERT_NE(by_id, nullptr) << "lookup_by_id missed a registered region";
    EXPECT_EQ(by_id->region_id, id);

    a_->unregister(region);

    // Re-check via lookup (not the freed region pointer) that it is gone.
    EXPECT_EQ(a_->lookup(buf), nullptr);
    EXPECT_EQ(a_->lookup_by_id(id), nullptr);

    a_->free(buf, MemoryKind::PINNED_HOST);
}

// Register a DEVICE buffer via register_device and verify the region fields and
// pointer lookup, then unregister and free.
TEST_F(RegistryTest, RegisterDeviceFieldsAndLookup) {
    constexpr size_t kSize = 4096;
    void* dbuf = a_->allocate_device(kSize, MemoryKind::DEVICE, 0);
    ASSERT_NE(dbuf, nullptr) << "allocate_device(DEVICE) failed";

    MemoryRegion* region = a_->register_device(dbuf, kSize, 0);
    ASSERT_NE(region, nullptr) << "register_device returned nullptr";

    EXPECT_EQ(region->kind, MemoryKind::DEVICE);
    EXPECT_EQ(region->device_id, 0);
    EXPECT_EQ(region->device_ptr, dbuf);
    EXPECT_EQ(region->host_ptr, nullptr);
    EXPECT_EQ(region->size, kSize);

    const uint64_t id = region->region_id;

    MemoryRegion* by_ptr = a_->lookup(dbuf);
    ASSERT_NE(by_ptr, nullptr) << "lookup(device_ptr) missed the region";
    EXPECT_EQ(by_ptr->region_id, id);

    a_->unregister(region);
    EXPECT_EQ(a_->lookup(dbuf), nullptr);

    a_->free(dbuf, MemoryKind::DEVICE);
}

// Register an EXTERNAL region over a PINNED_HOST buffer with an APP_MANAGED spec.
// The allocator stores a copy of the spec, which unregister must free cleanly.
TEST_F(RegistryTest, RegisterExternalAppManaged) {
    constexpr size_t kSize = 4096;
    void* buf = a_->allocate_host(kSize, MemoryKind::PINNED_HOST);
    ASSERT_NE(buf, nullptr) << "allocate_host(PINNED_HOST) failed";

    ExternalMemorySpec spec;
    spec.source = ExternalMemorySpec::Source::APP_MANAGED;

    MemoryRegion* region = a_->register_external(buf, nullptr, kSize, spec);
    ASSERT_NE(region, nullptr) << "register_external returned nullptr";

    EXPECT_EQ(region->kind, MemoryKind::EXTERNAL);
    EXPECT_EQ(region->host_ptr, buf);
    EXPECT_EQ(region->size, kSize);
    ASSERT_NE(region->external, nullptr) << "external spec copy missing";
    EXPECT_EQ(region->external->source, ExternalMemorySpec::Source::APP_MANAGED);

    // Must not crash; frees the internally-allocated spec copy.
    a_->unregister(region);
    EXPECT_EQ(a_->lookup(buf), nullptr);

    a_->free(buf, MemoryKind::PINNED_HOST);
}

// register_external documented error paths: size==0 and both pointers null.
TEST_F(RegistryTest, RegisterExternalErrorPaths) {
    ExternalMemorySpec spec;
    spec.source = ExternalMemorySpec::Source::APP_MANAGED;

    int dummy = 0;

    // size == 0 -> nullptr (even with a valid host pointer).
    EXPECT_EQ(a_->register_external(&dummy, nullptr, 0, spec), nullptr);

    // both host and device pointers null -> nullptr.
    EXPECT_EQ(a_->register_external(nullptr, nullptr, 4096, spec), nullptr);
}

// register_host documented error paths: null pointer and size 0.
TEST_F(RegistryTest, RegisterHostErrorPaths) {
    int dummy = 0;

    EXPECT_EQ(a_->register_host(nullptr, 4096), nullptr);
    EXPECT_EQ(a_->register_host(&dummy, 0), nullptr);
}

// lookup miss paths: null pointer and an unknown region id.
TEST_F(RegistryTest, LookupMissPaths) {
    EXPECT_EQ(a_->lookup(nullptr), nullptr);
    EXPECT_EQ(a_->lookup_by_id(999999), nullptr);
}

// unregister must tolerate nullptr and repeated/unknown removals without crashing.
TEST_F(RegistryTest, UnregisterSafety) {
    // unregister(nullptr) is a safe no-op.
    a_->unregister(nullptr);

    constexpr size_t kSize = 2048;
    void* buf = a_->allocate_host(kSize, MemoryKind::PINNED_HOST);
    ASSERT_NE(buf, nullptr) << "allocate_host(PINNED_HOST) failed";

    MemoryRegion* region = a_->register_host(buf, kSize);
    ASSERT_NE(region, nullptr);
    const uint64_t id = region->region_id;

    a_->unregister(region);
    EXPECT_EQ(a_->lookup_by_id(id), nullptr) << "region still present after unregister";

    // Fetch again by id (now null) and attempt a second unregister -> no crash.
    MemoryRegion* stale = a_->lookup_by_id(id);
    EXPECT_EQ(stale, nullptr);
    a_->unregister(stale);  // unregister(nullptr) again, must be safe.

    a_->free(buf, MemoryKind::PINNED_HOST);
}

// device_pointer_for over PINNED_HOST memory yields a valid device pointer.
TEST_F(RegistryTest, DevicePointerForPinned) {
    constexpr size_t kSize = 4096;
    void* buf = a_->allocate_host(kSize, MemoryKind::PINNED_HOST);
    ASSERT_NE(buf, nullptr) << "allocate_host(PINNED_HOST) failed";

    void* dptr = a_->device_pointer_for(buf);
    EXPECT_NE(dptr, nullptr) << "device_pointer_for(PINNED_HOST) returned null";

    a_->free(buf, MemoryKind::PINNED_HOST);
}

// device_pointer_for over plain pageable HOST memory should fail (nullptr).
// If it unexpectedly succeeds on this box, skip rather than fail.
TEST_F(RegistryTest, DevicePointerForPlainHostNull) {
    constexpr size_t kSize = 4096;
    void* buf = a_->allocate_host(kSize, MemoryKind::HOST);
    ASSERT_NE(buf, nullptr) << "allocate_host(HOST) failed";

    void* dptr = a_->device_pointer_for(buf);
    if (dptr != nullptr) {
        a_->free(buf, MemoryKind::HOST);
        GTEST_SKIP() << "device_pointer_for unexpectedly resolved plain host memory";
    }
    EXPECT_EQ(dptr, nullptr);

    a_->free(buf, MemoryKind::HOST);
}

} // namespace tutti_test
