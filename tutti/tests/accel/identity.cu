// tutti/tests/accel/identity.cu -- Layer 1 identity API tests
//
// Exercises the vendor/identity surface of IAccelerator:
//   vendor_name(), device_count(), get_device(), set_device().
#include "accel_test_fixture.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace tutti;

namespace tutti_test {

// Suite-specific fixture so identity tests are grouped in the output.
class IdentityTest : public AccelTest {};

// vendor_name() must return a usable, non-empty string.
TEST_F(IdentityTest, VendorNameNonEmpty) {
    const char* name = a_->vendor_name();
    ASSERT_NE(name, nullptr) << "vendor_name() returned null";
    EXPECT_GT(std::strlen(name), 0u) << "vendor_name() returned an empty string";
}

// A CUDA-backed accelerator must report at least one device.
TEST_F(IdentityTest, DeviceCountPositive) {
    EXPECT_GT(a_->device_count(), 0) << "device_count() must be positive";
}

// The currently selected device must be a valid index.
TEST_F(IdentityTest, GetDeviceInRange) {
    const int count = a_->device_count();
    ASSERT_GT(count, 0);
    const int dev = a_->get_device();
    EXPECT_GE(dev, 0) << "get_device() returned a negative index";
    EXPECT_LT(dev, count) << "get_device() returned an out-of-range index";
}

// Selecting device 0 must succeed and be observable via get_device().
TEST_F(IdentityTest, SetDeviceZero) {
    EXPECT_TRUE(a_->set_device(0)) << "set_device(0) failed";
    EXPECT_EQ(a_->get_device(), 0) << "get_device() did not reflect set_device(0)";
}

// On a multi-GPU box, selecting device 1 must work; otherwise skip.
TEST_F(IdentityTest, SetDeviceMultiGpu) {
    const int count = a_->device_count();
    if (count <= 1) {
        GTEST_SKIP() << "only " << count << " device(s) present; multi-GPU test skipped";
    }

    EXPECT_TRUE(a_->set_device(1)) << "set_device(1) failed on multi-GPU box";
    EXPECT_EQ(a_->get_device(), 1) << "get_device() did not reflect set_device(1)";

    // Restore device 0 for subsequent tests / fixture invariant.
    EXPECT_TRUE(a_->set_device(0)) << "failed to restore device 0";
    EXPECT_EQ(a_->get_device(), 0);
}

// Selecting a nonexistent device index must fail cleanly.
TEST_F(IdentityTest, SetDeviceInvalidFails) {
    EXPECT_FALSE(a_->set_device(99999)) << "set_device(99999) should fail";

    // The failed selection must not corrupt device state; device 0 stays valid.
    EXPECT_TRUE(a_->set_device(0)) << "failed to reselect device 0 after invalid set";
    EXPECT_EQ(a_->get_device(), 0);
}

} // namespace tutti_test
