// tutti/tests/accel/memory.cu -- Layer 1 memory allocation tests
//
// Exercises allocate_host / allocate_device / free across every MemoryKind
// value, verifying data integrity, 64KB device alignment, invalid-kind
// rejection, null-free safety, and absence of device-memory leaks.

#include "accel_test_fixture.h"

#include <gtest/gtest.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace tutti;

namespace tutti_test {

class MemoryTest : public AccelTest {};

// ---------------------------------------------------------------------------
// HOST: plain malloc-backed buffer, host-visible read/write.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, HostAllocReadWrite) {
    constexpr size_t kBytes = 4096;
    auto* buf = static_cast<uint8_t*>(a_->allocate_host(kBytes, MemoryKind::HOST));
    ASSERT_NE(buf, nullptr) << "allocate_host(HOST) returned null";

    for (size_t i = 0; i < kBytes; ++i) {
        buf[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    for (size_t i = 0; i < kBytes; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i * 7 + 3)) << "mismatch at " << i;
    }

    a_->free(buf, MemoryKind::HOST);
}

// ---------------------------------------------------------------------------
// PINNED_HOST: cudaHostAlloc-backed buffer, host-visible read/write.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, PinnedHostAllocReadWrite) {
    constexpr size_t kBytes = 8192;
    auto* buf = static_cast<uint8_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(buf, nullptr) << "allocate_host(PINNED_HOST) returned null";

    for (size_t i = 0; i < kBytes; ++i) {
        buf[i] = static_cast<uint8_t>((i ^ 0x5A) & 0xFF);
    }
    for (size_t i = 0; i < kBytes; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>((i ^ 0x5A) & 0xFF)) << "mismatch at " << i;
    }

    a_->free(buf, MemoryKind::PINNED_HOST);
}

// ---------------------------------------------------------------------------
// DEVICE: allocator hands out a 64KB-aligned sub-pointer.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, DeviceAllocAligned) {
    constexpr size_t kBytes = 65536;
    void* p = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(p, nullptr) << "allocate_device(DEVICE) returned null";

    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 65536u, 0u)
        << "device pointer not 64KB-aligned";

    a_->free(p, MemoryKind::DEVICE);
}

// ---------------------------------------------------------------------------
// DEVICE: H->D->H round trip via memcpy_async, verify data survives.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, DeviceRoundTrip) {
    constexpr size_t kElems = 1024;
    constexpr size_t kBytes = kElems * sizeof(uint32_t);

    std::vector<uint32_t> src(kElems);
    std::vector<uint32_t> dst(kElems, 0);
    for (size_t i = 0; i < kElems; ++i) {
        src[i] = static_cast<uint32_t>(i * 2654435761u + 11u);
    }

    void* dptr = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(dptr, nullptr) << "allocate_device(DEVICE) returned null";

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid()) << "failed to create stream";

    EXPECT_TRUE(a_->memcpy_async(dptr, src.data(), kBytes, stream)) << "H->D copy failed";
    EXPECT_TRUE(a_->memcpy_async(dst.data(), dptr, kBytes, stream)) << "D->H copy failed";
    a_->synchronize_stream(stream);

    for (size_t i = 0; i < kElems; ++i) {
        EXPECT_EQ(dst[i], src[i]) << "round-trip mismatch at " << i;
    }

    a_->destroy_stream(stream);
    a_->free(dptr, MemoryKind::DEVICE);
}

// ---------------------------------------------------------------------------
// MANAGED: unified memory, host writes visible to device sync and back.
// Skips if managed memory is not supported on this box.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, ManagedAllocReadWrite) {
    constexpr size_t kElems = 512;
    constexpr size_t kBytes = kElems * sizeof(uint32_t);

    auto* buf = static_cast<uint32_t*>(a_->allocate_device(kBytes, MemoryKind::MANAGED, 0));
    if (buf == nullptr) {
        GTEST_SKIP() << "managed memory not supported on this device";
    }

    for (size_t i = 0; i < kElems; ++i) {
        buf[i] = static_cast<uint32_t>(i * 3 + 1);
    }
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess) << "cudaDeviceSynchronize failed";

    for (size_t i = 0; i < kElems; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint32_t>(i * 3 + 1)) << "managed mismatch at " << i;
    }

    a_->free(buf, MemoryKind::MANAGED);
}

// ---------------------------------------------------------------------------
// Kind validation: host allocator rejects device kinds and vice versa.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, InvalidKindRejected) {
    EXPECT_EQ(a_->allocate_host(4096, MemoryKind::DEVICE), nullptr)
        << "allocate_host(DEVICE) should reject";
    EXPECT_EQ(a_->allocate_device(4096, MemoryKind::HOST, 0), nullptr)
        << "allocate_device(HOST) should reject";
}

// ---------------------------------------------------------------------------
// free(nullptr, ...) is a documented safe no-op.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, FreeNullNoop) {
    a_->free(nullptr, MemoryKind::HOST);
    a_->free(nullptr, MemoryKind::PINNED_HOST);
    a_->free(nullptr, MemoryKind::DEVICE);
    a_->free(nullptr, MemoryKind::MANAGED);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Leak regression: repeated DEVICE alloc/free must not drift free memory,
// and every handed-out pointer must be 64KB-aligned.
// ---------------------------------------------------------------------------
TEST_F(MemoryTest, DeviceAllocLeakRegression) {
    constexpr size_t kBytes = 256 * 1024;  // 256 KB per allocation
    constexpr int kIters = 200;
    constexpr size_t kDriftLimit = 4u * 65536u;

    size_t free_before = 0, total_before = 0;
    ASSERT_EQ(cudaMemGetInfo(&free_before, &total_before), cudaSuccess)
        << "cudaMemGetInfo failed (before)";

    for (int i = 0; i < kIters; ++i) {
        void* p = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
        ASSERT_NE(p, nullptr) << "allocate_device failed at iter " << i;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 65536u, 0u)
            << "unaligned device pointer at iter " << i;
        a_->free(p, MemoryKind::DEVICE);
    }

    size_t free_after = 0, total_after = 0;
    ASSERT_EQ(cudaMemGetInfo(&free_after, &total_after), cudaSuccess)
        << "cudaMemGetInfo failed (after)";

    // free_after should be within a small slack of free_before if nothing leaked.
    size_t drift = (free_before > free_after) ? (free_before - free_after) : 0;
    EXPECT_LT(drift, kDriftLimit)
        << "device memory leaked: drift=" << drift << " bytes over " << kIters << " cycles";
}

} // namespace tutti_test
