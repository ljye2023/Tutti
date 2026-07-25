# tutti/tests/device_manager/nvme/CMakeLists.txt -- Layer 2 NVMe real-hardware test
#
# nvme_real_hw_test is a GoogleTest that shells out to the prebuilt
# nvmeservice_client example binary to exercise the real NVMe path
# (daemon B3 owner + gRPC Connect + libnvm attach/create + GPU IO) against a
# physical controller (default 0000:b1:00.0).
#
# The test itself links only GoogleTest -- no libnvm / CUDA / gRPC -- because
# it drives everything through the client binary.  All cases GTEST_SKIP()
# unless TUTTI_NVME_REAL_HW=1 (and the destructive tier needs
# TUTTI_NVME_ALLOW_DESTRUCTIVE=1), so this is safe on hardware-less nodes.

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

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
add_executable(nvme_real_hw_test
    nvme_real_hw_test.cpp
)

set_target_properties(nvme_real_hw_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Bake in the default client-binary path so the test finds it without env
# overrides.  Prefer the exact built location of the client example target
# (a generator expression, resolved at build time) when that target exists in
# this configure; otherwise fall back to the conventional bin/ path (env
# TUTTI_NVME_CLIENT_BIN always overrides at run time regardless).
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

# Stage the b1-targeted config next to the binaries for the driver script.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sys_config.b1.yaml
               ${CMAKE_BINARY_DIR}/bin/sys_config.b1.yaml COPYONLY)

include(GoogleTest)
# Label so CI can exclude real-hardware / destructive cases by default:
#   ctest -L real_hw      (opt-in)     ctest -LE real_hw   (exclude)
gtest_discover_tests(nvme_real_hw_test
    PROPERTIES LABELS "real_hw;nvme;destructive")

message(STATUS "Configured nvme_real_hw_test (Layer 2 real-HW, shells out to nvmeservice_client)")
