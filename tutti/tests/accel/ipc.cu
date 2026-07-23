// tutti/tests/accel/ipc.cu -- Layer 1 IPC export/import tests
//
// Exercises IAccelerator::ipc_export / ipc_import on the CUDA backend. IPC is
// frequently unavailable (containers without --ipc=host, boxes lacking P2P,
// or in-process self-import which CUDA generally rejects), so the roundtrip
// case skips defensively when the primitive signals lack of support. The two
// argument-validation cases are documented, backend-independent guarantees and
// therefore assert hard.
#include "accel_test_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "accel_types.h"
#include "memory_kind.h"
#include "memory_region.h"

using namespace tutti;

namespace tutti_test {

class IpcTest : public AccelTest {};

namespace {

// True if every byte of the IPC handle payload is zero.
bool handle_all_zero(const IpcHandle& h) {
    for (size_t i = 0; i < IpcHandle::MAX_HANDLE_SIZE; ++i) {
        if (h.data[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

// A host-only region has device_ptr == nullptr, so export must fail regardless
// of platform IPC support. This is a documented guarantee -> hard failure.
TEST_F(IpcTest, ExportNullDevicePtrFails) {
    constexpr size_t kSize = 4096;

    void* host_ptr = a_->allocate_host(kSize, MemoryKind::HOST);
    ASSERT_NE(host_ptr, nullptr) << "allocate_host(HOST) returned null";

    MemoryRegion* region = a_->register_host(host_ptr, kSize);
    ASSERT_NE(region, nullptr) << "register_host returned null";
    ASSERT_EQ(region->device_ptr, nullptr)
        << "host region unexpectedly has a device pointer";

    IpcHandle handle;
    EXPECT_FALSE(a_->ipc_export(region, &handle))
        << "ipc_export must fail for a region with a null device pointer";

    a_->unregister(region);
    a_->free(host_ptr, MemoryKind::HOST);
}

// A null out_handle is invalid input; export must reject it even when the
// region is otherwise valid. Documented guarantee -> hard failure.
TEST_F(IpcTest, ExportNullHandleFails) {
    constexpr size_t kSize = 4096;

    void* dev_ptr = a_->allocate_device(kSize, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev_ptr, nullptr) << "allocate_device(DEVICE) returned null";

    MemoryRegion* region = a_->register_device(dev_ptr, kSize, 0);
    ASSERT_NE(region, nullptr) << "register_device returned null";

    EXPECT_FALSE(a_->ipc_export(region, nullptr))
        << "ipc_export must fail when out_handle is null";

    a_->unregister(region);
    a_->free(dev_ptr, MemoryKind::DEVICE);
}

// Full export -> import path. Any step that signals unavailability (export
// unsupported, or in-process self-import rejected) results in a skip rather
// than a failure. When import does succeed, the resulting region must carry
// the documented EXTERNAL / DEVICE_IPC metadata.
TEST_F(IpcTest, ExportImportRoundtrip) {
    constexpr size_t kSize = 64 * 1024;

    void* dev_ptr = a_->allocate_device(kSize, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev_ptr, nullptr) << "allocate_device(DEVICE) returned null";

    MemoryRegion* region = a_->register_device(dev_ptr, kSize, 0);
    ASSERT_NE(region, nullptr) << "register_device returned null";

    IpcHandle handle;
    if (!a_->ipc_export(region, &handle)) {
        a_->unregister(region);
        a_->free(dev_ptr, MemoryKind::DEVICE);
        GTEST_SKIP() << "IPC unsupported (ipc_export failed on this platform)";
    }

    // A successful export must have written a non-trivial handle payload.
    EXPECT_FALSE(handle_all_zero(handle))
        << "ipc_export succeeded but produced an all-zero handle";

    MemoryRegion* imported = a_->ipc_import(handle, 0);
    if (imported == nullptr) {
        a_->unregister(region);
        a_->free(dev_ptr, MemoryKind::DEVICE);
        GTEST_SKIP() << "self-import unsupported in-process";
    }

    // Import succeeded: verify the documented EXTERNAL / DEVICE_IPC metadata.
    EXPECT_EQ(imported->kind, MemoryKind::EXTERNAL)
        << "imported region should be EXTERNAL";
    ASSERT_NE(imported->external, nullptr)
        << "imported EXTERNAL region must carry an ExternalMemorySpec";
    EXPECT_EQ(imported->external->source,
              ExternalMemorySpec::Source::DEVICE_IPC)
        << "imported region source should be DEVICE_IPC";

    a_->unregister(imported);

    a_->unregister(region);
    a_->free(dev_ptr, MemoryKind::DEVICE);
}

} // namespace tutti_test
