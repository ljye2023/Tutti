# tutti/tests/backends/nvme/CMakeLists.txt -- NvmeBackend Layer 3 tests
#
# Two tiers:
#   Unit (NvmeBackendUnit.*):    DaemonNvmeDeviceDriver in mock_mode; no hardware.
#   Real-HW (NvmeBackendRealHw.*): live daemon + GPU; TUTTI_NVME_REAL_HW=1.
#
# Labels:
#   nvme        -- all NVMe-related tests (daemon unit + real-HW)
#   real_hw     -- real-HW tier only; exclude with: ctest -LE real_hw

cmake_minimum_required(VERSION 3.18)

find_package(CUDAToolkit REQUIRED)

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
# Executable
# ---------------------------------------------------------------------------

set(BACKENDS_DIR   "${PROJECT_SOURCE_DIR}/backends")
set(DM_DIR         "${PROJECT_SOURCE_DIR}/device_manager")

add_executable(nvme_backend_test
    nvme_backend_test.cpp
    # Compile the registration TU directly so its static initializer runs.
    # The linker strips it from libtutti_backends_nvme because nothing
    # references its symbols; adding it here is the same trick backend_test
    # uses for mock_backend.cpp.
    ${BACKENDS_DIR}/nvme/src/nvme_backend_registration.cpp
)

set_target_properties(nvme_backend_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(nvme_backend_test PRIVATE
    # Project root: resolves "backends/...", "device_manager/..." style includes
    ${PROJECT_SOURCE_DIR}
    # NVMe backend private headers (nvme_io_types.h etc.)
    ${BACKENDS_DIR}/nvme/include
    # Device Manager: NvmeVirtualDevice, DaemonNvmeDeviceDriver
    ${DM_DIR}/nvme/include
    ${DM_DIR}/include
    ${DM_DIR}/mock/include
    ${DM_DIR}/nvme/libnvm/include   # nvm_types.h (needed by nvme_virtual_device.h)
)

# gRPC: when found, compile with TUTTI_NVMESERVICE_ENABLED so the real-HW
# fixture can connect to the daemon.
if(gRPC_FOUND)
    target_compile_definitions(nvme_backend_test PRIVATE
        TUTTI_NVMESERVICE_ENABLED=1)
endif()

target_link_libraries(nvme_backend_test PRIVATE
    tutti_backends_nvme       # NvmeBackend + BackendFactory registration
    tutti_device_manager      # create_device_manager, DaemonNvmeDeviceDriver
    CUDA::cudart
    GTest::gtest
    GTest::gtest_main
)

# ---------------------------------------------------------------------------
# CTest registration
# ---------------------------------------------------------------------------
include(GoogleTest)

# Unit cases always run (no label guard).
# Real-HW cases carry "real_hw" so hardware-less nodes can opt out.
gtest_discover_tests(nvme_backend_test
    PROPERTIES LABELS "nvme")

message(STATUS "Configured nvme_backend_test (Unit + Real-HW NVMe backend tests)")
