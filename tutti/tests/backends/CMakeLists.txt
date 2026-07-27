# tutti/tests/backends/CMakeLists.txt -- Layer 3 device-agnostic backend tests
#
# Exercises the vendor-neutral IBackend contract (IBackend / MockBackend +
# BackendFactory) driven by Layer 2's mock Device Manager stack.
#
# Like tests/device_manager, this compiles the implementation sources directly
# rather than linking tutti_backends / tutti_device_manager. The point is to
# prove the Layer 3 device-agnostic boundary holds: if this target links with no
# NVMe / libnvm / CUDA symbols, MockBackend truly depends only on the neutral
# interfaces.

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
set(BACKENDS_LAYER_DIR "${PROJECT_SOURCE_DIR}/backends")
set(DM_LAYER_DIR       "${PROJECT_SOURCE_DIR}/device_manager")

add_executable(backend_test
    backend_test.cpp
    # Device-agnostic implementation under test, compiled in directly.
    ${BACKENDS_LAYER_DIR}/mock/src/mock_backend.cpp
    ${BACKENDS_LAYER_DIR}/src/backend_factory.cpp
    # Layer 2 mock stack that supplies the vdevices.
    ${DM_LAYER_DIR}/src/common/device_manager_impl.cpp
    ${DM_LAYER_DIR}/mock/src/mock_device_driver.cpp
)

set_target_properties(backend_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Only neutral headers -- no nvme/, no libnvm, no CUDA.
target_include_directories(backend_test PRIVATE
    ${PROJECT_SOURCE_DIR}                    # "backends/include/..." style includes
    ${BACKENDS_LAYER_DIR}/include            # backend*.h
    ${BACKENDS_LAYER_DIR}/mock/include       # mock_backend.h
    ${DM_LAYER_DIR}/include                  # common/*.h
    ${DM_LAYER_DIR}/mock/include             # mock_*.h
)

target_link_libraries(backend_test PRIVATE
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(backend_test)

message(STATUS "Configured backend_test executable (GoogleTest, device-agnostic)")
