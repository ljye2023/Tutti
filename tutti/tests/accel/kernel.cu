// tutti/tests/accel/kernel.cu -- Layer 1 accel smoke tests: kernel launch
//
// Exercises IAccelerator::launch() by running a vector_add kernel through the
// CUDA backend and verifying the computed data end to end.

#include "accel_test_fixture.h"

#include <gtest/gtest.h>
#include <cuda_runtime.h>

using namespace tutti;

// Kernel must live at file scope (outside any C++ namespace) so nvcc registers
// it as an entry point usable with cudaLaunchKernel via a host function pointer.
namespace {

__global__ void vector_add(const float* a, const float* b, float* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

}  // anonymous namespace

namespace tutti_test {

class KernelTest : public AccelTest {};

TEST_F(KernelTest, VectorAddSingleBlock) {
    // n kept as a local whose address stays valid across launch + synchronize.
    int n = 256;
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);

    // Pinned host buffers.
    float* a = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    float* b = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    float* c = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // Device buffers.
    float* d_a = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    float* d_b = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    float* d_c = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    ASSERT_NE(d_a, nullptr);
    ASSERT_NE(d_b, nullptr);
    ASSERT_NE(d_c, nullptr);

    for (int i = 0; i < n; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = static_cast<float>(i * 2);
        c[i] = -1.0f;
    }

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid());

    // Host -> Device for the inputs.
    EXPECT_TRUE(a_->memcpy_async(d_a, a, bytes, stream));
    EXPECT_TRUE(a_->memcpy_async(d_b, b, bytes, stream));

    Dim3 grid(1);
    Dim3 block(256);
    void* args[] = {&d_a, &d_b, &d_c, &n};
    // Clear any sticky CUDA error left by earlier tests (error-path tests in
    // other suites share this process-wide context) so the post-launch check
    // below reflects only this launch.
    cudaGetLastError();
    a_->launch(reinterpret_cast<void*>(vector_add), grid, block, 0, stream, args);

    // Device -> Host for the result, then wait for the whole stream.
    EXPECT_TRUE(a_->memcpy_async(c, d_c, bytes, stream));
    a_->synchronize_stream(stream);

    // Surface any launch/runtime error explicitly.
    EXPECT_EQ(cudaGetLastError(), cudaSuccess);

    for (int i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(c[i], static_cast<float>(3 * i)) << "mismatch at index " << i;
    }

    a_->destroy_stream(stream);
    a_->free(d_a, MemoryKind::DEVICE);
    a_->free(d_b, MemoryKind::DEVICE);
    a_->free(d_c, MemoryKind::DEVICE);
    a_->free(a, MemoryKind::PINNED_HOST);
    a_->free(b, MemoryKind::PINNED_HOST);
    a_->free(c, MemoryKind::PINNED_HOST);
}

TEST_F(KernelTest, VectorAddMultiBlock) {
    int n = 1000;
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);

    float* a = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    float* b = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    float* c = static_cast<float*>(a_->allocate_host(bytes, MemoryKind::PINNED_HOST));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    float* d_a = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    float* d_b = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    float* d_c = static_cast<float*>(a_->allocate_device(bytes, MemoryKind::DEVICE, 0));
    ASSERT_NE(d_a, nullptr);
    ASSERT_NE(d_b, nullptr);
    ASSERT_NE(d_c, nullptr);

    for (int i = 0; i < n; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = static_cast<float>(i * 2);
        c[i] = -1.0f;
    }

    AccelStream stream = a_->create_stream();
    ASSERT_TRUE(stream.is_valid());

    EXPECT_TRUE(a_->memcpy_async(d_a, a, bytes, stream));
    EXPECT_TRUE(a_->memcpy_async(d_b, b, bytes, stream));

    const uint32_t block_x = 256;
    const uint32_t grid_x = (static_cast<uint32_t>(n) + block_x - 1) / block_x;
    Dim3 grid(grid_x);
    Dim3 block(block_x);
    void* args[] = {&d_a, &d_b, &d_c, &n};
    // Clear any sticky CUDA error before launching (see note in single-block test).
    cudaGetLastError();
    a_->launch(reinterpret_cast<void*>(vector_add), grid, block, 0, stream, args);

    EXPECT_TRUE(a_->memcpy_async(c, d_c, bytes, stream));
    a_->synchronize_stream(stream);

    EXPECT_EQ(cudaGetLastError(), cudaSuccess);

    for (int i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(c[i], static_cast<float>(3 * i)) << "mismatch at index " << i;
    }

    a_->destroy_stream(stream);
    a_->free(d_a, MemoryKind::DEVICE);
    a_->free(d_b, MemoryKind::DEVICE);
    a_->free(d_c, MemoryKind::DEVICE);
    a_->free(a, MemoryKind::PINNED_HOST);
    a_->free(b, MemoryKind::PINNED_HOST);
    a_->free(c, MemoryKind::PINNED_HOST);
}

}  // namespace tutti_test
