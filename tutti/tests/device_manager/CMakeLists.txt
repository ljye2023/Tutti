# tutti/tests/device_manager/CMakeLists.txt -- Layer 2 vendor-neutral tests
#
# Exercises the vendor-neutral Device Manager API (IDeviceManager /
# DeviceManagerImpl, IDeviceDriver, IPhysicalDevice, IVirtualDevice,
# ILeaseManager) with MOCK drivers only.
#
# Deliberately compiles src/common/device_manager_impl.cpp directly into the
# test rather than linking tutti_device_manager. The point is to prove the
# common layer is vendor-neutral: if this target links with no NVMe / libnvm /
# CUDA symbols, the boundary holds.

cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# GoogleTest (installed, else fetched — cached across configures)
# ---------------------------------------------------------------------------
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
set(DM_LAYER_DIR "${PROJECT_SOURCE_DIR}/device_manager")

add_executable(device_manager_test
    device_manager_test.cpp
    physical_device_test.cpp
    lease_manager_test.cpp
    # Vendor-neutral implementation under test, compiled in directly.
    ${DM_LAYER_DIR}/src/common/device_manager_impl.cpp
    ${DM_LAYER_DIR}/mock/src/mock_device_driver.cpp
)

set_target_properties(device_manager_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Only the common headers are needed — no nvme/, no libnvm, no CUDA.
target_include_directories(device_manager_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # mock_device.h
    ${DM_LAYER_DIR}/include              # common/*.h
    ${DM_LAYER_DIR}/mock/include         # mock_*.h
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(device_manager_test PRIVATE
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(device_manager_test)

message(STATUS "Configured device_manager_test executable (GoogleTest, vendor-neutral)")
