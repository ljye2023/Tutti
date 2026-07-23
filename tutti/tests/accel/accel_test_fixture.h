// tutti/tests/accel/accel_test_fixture.h -- Shared GoogleTest fixture for Layer 1
//
// Provides a single CudaAccelerator instance to every IAccelerator test via a
// common base fixture. Tests derive from AccelTest and use accel_ / a_.
#pragma once

#include <gtest/gtest.h>

#include "iaccel.h"
#include "cuda_accelerator.h"

namespace tutti_test {

// Base fixture: constructs a CUDA-backed IAccelerator and ensures at least one
// device is present. Individual suites derive their own fixture from this so
// they can share SetUp/TearDown while keeping test output grouped per area.
class AccelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GT(a_->device_count(), 0) << "no accelerator devices present";
        ASSERT_TRUE(a_->set_device(0)) << "failed to select device 0";
    }

    tutti::CudaAccelerator accel_;
    tutti::IAccelerator* a_ = &accel_;
};

} // namespace tutti_test
