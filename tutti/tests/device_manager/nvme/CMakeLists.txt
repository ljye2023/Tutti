# tutti/tests/device_manager/nvme/CMakeLists.txt -- Layer 2 NVMe tests
#
# Two test executables:
#
#   nvme_real_hw_test     -- Integration test that shells out to the prebuilt
#                            nvmeservice_client binary (no libnvm/CUDA link).
#                            Gated behind TUTTI_NVME_REAL_HW=1.
#
#   daemon_driver_test    -- Unit + integration tests for DaemonNvmeDeviceDriver.
#                            Unit cases (DaemonDriverUnit.*) work with no hardware
#                            by using the mock-grant fallback built into the driver.
#                            Real-HW cases (DaemonDriverRealHw.*) gate on
#                            TUTTI_NVME_REAL_HW=1 + a live daemon.

cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# GoogleTest (installed, else fetched -- cached across configures)
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

set(DM_LAYER_DIR "${PROJECT_SOURCE_DIR}/device_manager")

# ---------------------------------------------------------------------------
# nvme_real_hw_test -- shells out to nvmeservice_client, no driver link
# ---------------------------------------------------------------------------
add_executable(nvme_real_hw_test
    nvme_real_hw_test.cpp
)

set_target_properties(nvme_real_hw_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(TARGET nvmeservice_client_example)
    target_compile_definitions(nvme_real_hw_test PRIVATE
        TUTTI_NVME_CLIENT_BIN="$<TARGET_FILE:nvmeservice_client_example>")
else()
    target_compile_definitions(nvme_real_hw_test PRIVATE
        TUTTI_NVME_CLIENT_BIN="${CMAKE_BINARY_DIR}/bin/nvmeservice_client")
endif()

target_link_libraries(nvme_real_hw_test PRIVATE
    GTest::gtest
    GTest::gtest_main
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sys_config.b1.yaml
               ${CMAKE_BINARY_DIR}/bin/sys_config.b1.yaml COPYONLY)

# ---------------------------------------------------------------------------
# daemon_driver_test -- DaemonNvmeDeviceDriver unit + integration
# ---------------------------------------------------------------------------
add_executable(daemon_driver_test
    daemon_driver_test.cpp
)

set_target_properties(daemon_driver_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Include paths:
#   - nvme/include    : daemon_nvme_device_driver.h, nvme_virtual_device.h, etc.
#   - device_manager/include : common/*.h (IDeviceDriver, ILeaseManager, …)
#   - mock/include    : MockLeaseManager
#   - libnvm/include  : nvm_types.h (needed by nvme_virtual_device.h)
target_include_directories(daemon_driver_test PRIVATE
    ${DM_LAYER_DIR}/nvme/include
    ${DM_LAYER_DIR}/include
    ${DM_LAYER_DIR}/mock/include
    ${DM_LAYER_DIR}/nvme/libnvm/include
    ${PROJECT_SOURCE_DIR}
)

# When nvmeservice is available the test is compiled with the same flag so the
# real-HW fixture can #ifdef-check for it.
if(gRPC_FOUND)
    target_compile_definitions(daemon_driver_test PRIVATE
        TUTTI_NVMESERVICE_ENABLED=1)
endif()

target_link_libraries(daemon_driver_test PRIVATE
    tutti_device_manager   # provides DaemonNvmeDeviceDriver (mock-grant or gRPC path)
    GTest::gtest
    GTest::gtest_main
)

# ---------------------------------------------------------------------------
# CTest registration
# ---------------------------------------------------------------------------
include(GoogleTest)

gtest_discover_tests(nvme_real_hw_test
    PROPERTIES LABELS "real_hw;nvme;destructive")

# Unit cases always run; real-HW cases carry the real_hw label so they can be
# excluded on hardware-less nodes with: ctest -LE real_hw
gtest_discover_tests(daemon_driver_test
    PROPERTIES LABELS "nvme")

message(STATUS "Configured nvme_real_hw_test and daemon_driver_test (Layer 2 NVMe)")
