#include "accel_test_fixture.h"
#include <gtest/gtest.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <vector>

using namespace tutti;

namespace tutti_test {

// Suite-specific fixture so transfer tests group together in the output.
class TransferTest : public AccelTest {};

// Pinned host -> device -> host round trip with byte-exact verification.
TEST_F(TransferTest, RoundTripH2DtoD2H) {
    constexpr size_t kCount = 4096;
    constexpr size_t kBytes = kCount * sizeof(uint32_t);

    auto* host_src = static_cast<uint32_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(host_src, nullptr) << "failed to allocate pinned host source";
    auto* host_dst = static_cast<uint32_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(host_dst, nullptr) << "failed to allocate pinned host destination";

    void* dev = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev, nullptr) << "failed to allocate device buffer";

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid()) << "failed to create stream";

    // Seed the source pattern and a distinct destination to catch a no-op copy.
    for (size_t i = 0; i < kCount; ++i) {
        host_src[i] = static_cast<uint32_t>(i * 7);
        host_dst[i] = 0xDEADBEEFu;
    }

    EXPECT_TRUE(a_->memcpy_async(dev, host_src, kBytes, stream)) << "H2D copy failed";
    EXPECT_TRUE(a_->memcpy_async(host_dst, dev, kBytes, stream)) << "D2H copy failed";
    a_->synchronize_stream(stream);

    for (size_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(host_dst[i], static_cast<uint32_t>(i * 7))
            << "round trip mismatch at index " << i;
    }

    a_->destroy_stream(stream);
    a_->free(dev, MemoryKind::DEVICE);
    a_->free(host_dst, MemoryKind::PINNED_HOST);
    a_->free(host_src, MemoryKind::PINNED_HOST);
}

// Device-to-device copy: fill device_src via H->D, copy D->D, read back and verify.
TEST_F(TransferTest, DeviceToDevice) {
    constexpr size_t kCount = 2048;
    constexpr size_t kBytes = kCount * sizeof(uint32_t);

    auto* host_src = static_cast<uint32_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(host_src, nullptr) << "failed to allocate pinned host source";
    auto* host_dst = static_cast<uint32_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(host_dst, nullptr) << "failed to allocate pinned host destination";

    void* dev_src = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev_src, nullptr) << "failed to allocate device source";
    void* dev_dst = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev_dst, nullptr) << "failed to allocate device destination";

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid()) << "failed to create stream";

    for (size_t i = 0; i < kCount; ++i) {
        host_src[i] = static_cast<uint32_t>(i * 3 + 1);
        host_dst[i] = 0u;
    }

    EXPECT_TRUE(a_->memcpy_async(dev_src, host_src, kBytes, stream)) << "H2D copy failed";
    EXPECT_TRUE(a_->memcpy_async(dev_dst, dev_src, kBytes, stream)) << "D2D copy failed";
    EXPECT_TRUE(a_->memcpy_async(host_dst, dev_dst, kBytes, stream)) << "D2H copy failed";
    a_->synchronize_stream(stream);

    for (size_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(host_dst[i], static_cast<uint32_t>(i * 3 + 1))
            << "device-to-device mismatch at index " << i;
    }

    a_->destroy_stream(stream);
    a_->free(dev_dst, MemoryKind::DEVICE);
    a_->free(dev_src, MemoryKind::DEVICE);
    a_->free(host_dst, MemoryKind::PINNED_HOST);
    a_->free(host_src, MemoryKind::PINNED_HOST);
}

// A zero-size memcpy must succeed and leave both buffers untouched.
TEST_F(TransferTest, ZeroSizeNoop) {
    constexpr size_t kCount = 16;
    constexpr size_t kBytes = kCount * sizeof(uint32_t);

    auto* host_buf = static_cast<uint32_t*>(a_->allocate_host(kBytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(host_buf, nullptr) << "failed to allocate pinned host buffer";

    void* dev = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(dev, nullptr) << "failed to allocate device buffer";

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid()) << "failed to create stream";

    std::vector<uint32_t> expected(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        host_buf[i] = static_cast<uint32_t>(i * 11 + 5);
        expected[i] = host_buf[i];
    }

    // Zero-length transfers in both directions should report success.
    EXPECT_TRUE(a_->memcpy_async(dev, host_buf, 0, stream)) << "zero-size H2D should succeed";
    EXPECT_TRUE(a_->memcpy_async(host_buf, dev, 0, stream)) << "zero-size D2H should succeed";
    a_->synchronize_stream(stream);

    for (size_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(host_buf[i], expected[i])
            << "zero-size copy must not alter host data at index " << i;
    }

    a_->destroy_stream(stream);
    a_->free(dev, MemoryKind::DEVICE);
    a_->free(host_buf, MemoryKind::PINNED_HOST);
}

} // namespace tutti_test
