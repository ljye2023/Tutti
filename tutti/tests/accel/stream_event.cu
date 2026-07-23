// tutti/tests/accel/stream_event.cu -- Layer 1 stream + event tests
//
// Exercises stream/event lifecycle and cross-stream ordering guarantees of the
// CUDA-backed IAccelerator, using the shared AccelTest fixture.
#include "accel_test_fixture.h"
#include <gtest/gtest.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace tutti;

namespace tutti_test {

class StreamEventTest : public AccelTest {};

// create_stream must yield a valid handle; synchronize on an empty stream is a
// no-op that must return cleanly; destroy_stream frees it; destroying a null
// stream handle is safe.
TEST_F(StreamEventTest, StreamLifecycle) {
    AccelStream s = a_->create_stream();
    ASSERT_TRUE(s.is_valid()) << "create_stream returned an invalid handle";

    // Synchronizing an empty (idle) stream should just return.
    a_->synchronize_stream(s);

    a_->destroy_stream(s);

    // Null-safe: destroying a default-constructed (null) stream must not crash.
    a_->destroy_stream(AccelStream());
}

// Full event happy path: enqueue an H->D copy, record an event on the stream,
// synchronize, and the event must then report completion.
TEST_F(StreamEventTest, EventLifecycle) {
    constexpr size_t kCount = 1024;
    constexpr size_t kBytes = kCount * sizeof(uint32_t);

    AccelStream s = a_->create_stream();
    ASSERT_TRUE(s.is_valid()) << "create_stream failed";

    AccelEvent e = a_->create_event();
    ASSERT_TRUE(e.is_valid()) << "create_event failed";

    void* h_src = a_->allocate_host(kBytes, MemoryKind::PINNED_HOST);
    ASSERT_NE(h_src, nullptr) << "pinned host allocation failed";
    void* d_buf = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(d_buf, nullptr) << "device allocation failed";

    uint32_t* src = static_cast<uint32_t*>(h_src);
    for (size_t i = 0; i < kCount; ++i) {
        src[i] = static_cast<uint32_t>(i * 2654435761u + 7u);
    }

    EXPECT_TRUE(a_->memcpy_async(d_buf, h_src, kBytes, s)) << "H->D memcpy_async failed";
    a_->record_event(e, s);
    a_->synchronize_stream(s);

    // After synchronize, all prior work on the stream (including the recorded
    // event) has completed, so query must report ready.
    EXPECT_TRUE(a_->query_event(e)) << "event not ready after synchronize_stream";

    a_->free(d_buf, MemoryKind::DEVICE);
    a_->free(h_src, MemoryKind::PINNED_HOST);
    a_->destroy_event(e);
    a_->destroy_stream(s);
}

// Documented guarantees for null handles: querying a null event returns true,
// and destroying a null event is a safe no-op.
TEST_F(StreamEventTest, QueryNullEventSafe) {
    EXPECT_TRUE(a_->query_event(AccelEvent())) << "query_event(null) must return true";
    a_->destroy_event(AccelEvent());
}

// Cross-stream ordering: work enqueued on streamB after wait_event(streamB, E)
// must not begin until E (recorded on streamA) completes. We copy a known
// pattern H->D on streamA, record E, have streamB wait on E, then copy D->H on
// streamB. The final host buffer must observe the pattern.
TEST_F(StreamEventTest, CrossStreamWaitEvent) {
    constexpr size_t kCount = 4096;
    constexpr size_t kBytes = kCount * sizeof(uint32_t);

    AccelStream streamA = a_->create_stream();
    ASSERT_TRUE(streamA.is_valid()) << "create_stream (A) failed";
    AccelStream streamB = a_->create_stream();
    ASSERT_TRUE(streamB.is_valid()) << "create_stream (B) failed";

    AccelEvent eventE = a_->create_event();
    ASSERT_TRUE(eventE.is_valid()) << "create_event failed";

    void* h_src = a_->allocate_host(kBytes, MemoryKind::PINNED_HOST);
    ASSERT_NE(h_src, nullptr) << "pinned host src allocation failed";
    void* h_dst = a_->allocate_host(kBytes, MemoryKind::PINNED_HOST);
    ASSERT_NE(h_dst, nullptr) << "pinned host dst allocation failed";
    void* d_buf = a_->allocate_device(kBytes, MemoryKind::DEVICE, 0);
    ASSERT_NE(d_buf, nullptr) << "device allocation failed";

    uint32_t* src = static_cast<uint32_t*>(h_src);
    uint32_t* dst = static_cast<uint32_t*>(h_dst);
    for (size_t i = 0; i < kCount; ++i) {
        src[i] = static_cast<uint32_t>(0xA5A50000u ^ (i * 40503u));
    }
    std::memset(dst, 0, kBytes);

    // streamA: upload the known pattern, then record the completion event.
    EXPECT_TRUE(a_->memcpy_async(d_buf, h_src, kBytes, streamA))
        << "H->D memcpy_async on streamA failed";
    a_->record_event(eventE, streamA);

    // streamB: block until the upload on streamA finishes, then read back.
    a_->wait_event(streamB, eventE);
    EXPECT_TRUE(a_->memcpy_async(h_dst, d_buf, kBytes, streamB))
        << "D->H memcpy_async on streamB failed";

    a_->synchronize_stream(streamB);

    // The ordering dependency must have ensured the upload landed before the
    // readback, so host_dst holds the original pattern byte-for-byte.
    EXPECT_EQ(std::memcmp(src, dst, kBytes), 0)
        << "cross-stream wait_event did not enforce ordering; data mismatch";

    a_->free(d_buf, MemoryKind::DEVICE);
    a_->free(h_dst, MemoryKind::PINNED_HOST);
    a_->free(h_src, MemoryKind::PINNED_HOST);
    a_->destroy_event(eventE);
    a_->destroy_stream(streamB);
    a_->destroy_stream(streamA);
}

} // namespace tutti_test
