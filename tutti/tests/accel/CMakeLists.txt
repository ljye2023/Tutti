# tutti/tests/accel/CMakeLists.txt -- Layer 1 IAccelerator smoke tests (GoogleTest)
#
# Builds the `accel_smoke_test` executable from the per-functional-area suite
# files. Exercises the IAccelerator abstraction against the CUDA backend using
# GoogleTest.

cmake_minimum_required(VERSION 3.18)

enable_language(CUDA)

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
# Prefer an installed GTest; otherwise fetch it. FetchContent is cached across
# configures, so the network is only hit on the first configure.
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    # Don't override the parent project's compiler/linker settings on Windows;
    # harmless on Linux but keeps the module boilerplate standard.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
add_executable(accel_smoke_test
    identity.cu
    memory.cu
    registry.cu
    stream_event.cu
    transfer.cu
    kernel.cu
    ipc.cu
)

set_target_properties(accel_smoke_test PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CXX_STANDARD 17
    CUDA_STANDARD 17
)

# accel/include/common and accel/include/cuda are exported PUBLICLY by the
# tutti_accel target, so they arrive transitively via target_link_libraries.
# Only our own test-source dir needs to be added explicitly.
target_include_directories(accel_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(accel_smoke_test PRIVATE
    CUDA::cudart
    tutti_accel
    GTest::gtest
    GTest::gtest_main
)

set_target_properties(accel_smoke_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Register with CTest for `ctest`-driven runs.
include(GoogleTest)
gtest_discover_tests(accel_smoke_test)

message(STATUS "Configured accel_smoke_test executable (GoogleTest)")
