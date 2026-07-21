# tutti/CMakeLists.txt -- Tutti refactor root CMake
#
# Layered architecture with strict bottom-up dependencies:
#
#   Layer 0: abstraction/        (macros only, no library)
#   Layer 0: types/              (value types, header-only)
#   Layer 1: accel/              (IAccelerator HAL + CUDA impl)
#   Layer 2: device_manager/     (IVirtualNvme + VDevice)
#   Layer 3: backends/           (IBackendProvider impls)
#   Layer 4: io_engine/          (IIoEngine)
#   Layer 5: block_storage/      (IBlockStorage)
#   Layer 5: raw_device/         (IRawDevice)
#   Layer 6: coordinator/        (top-level orchestrator)
#
# Target: Linux 5.15 kernel + CUDA 12.6+ for v0.1
#
# NOTE: This CMakeLists.txt currently only builds Layer 0 and Layer 1

cmake_minimum_required(VERSION 3.18)

project(Tutti LANGUAGES C CXX CUDA)

# ---------------------------------------------------------------------------
# Global settings (inherited from parent if included as subdirectory)
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CUDA architecture (default to Ada/Hopper, override with -DCMAKE_CUDA_ARCHITECTURES)
if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 90)
endif()

# ---------------------------------------------------------------------------
# Find dependencies
# ---------------------------------------------------------------------------

find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

# yaml-cpp: used by Layer 2 (device_manager/nvme/nvmeservice) config parser.
# Normalize the target name across packagings:
#   vcpkg / new upstream : yaml-cpp::yaml-cpp
#   old system pkg (0.5.x): bare "yaml-cpp" target, or only ${YAML_CPP_LIBRARIES}
find_package(yaml-cpp REQUIRED)
if(TARGET yaml-cpp::yaml-cpp)
    set(TUTTI_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(TUTTI_YAML_CPP_TARGET yaml-cpp)
else()
    set(TUTTI_YAML_CPP_TARGET ${YAML_CPP_LIBRARIES})
endif()
message(STATUS "Tutti: yaml-cpp target: ${TUTTI_YAML_CPP_TARGET}")

message(STATUS "Tutti: Using CUDA ${CUDAToolkit_VERSION}")
message(STATUS "Tutti: Target architectures: ${CMAKE_CUDA_ARCHITECTURES}")

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

# C++ flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# CUDA flags
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -lineinfo --expt-relaxed-constexpr --expt-extended-lambda -rdc=true")
set(CMAKE_CUDA_FLAGS_DEBUG "${CMAKE_CUDA_FLAGS_DEBUG} -G -DDEBUG")
set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} -O2")

# Generate code for specified architectures
foreach(arch ${CMAKE_CUDA_ARCHITECTURES})
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -gencode arch=compute_${arch},code=sm_${arch}")
endforeach()

# ---------------------------------------------------------------------------
# Global include directories
# ---------------------------------------------------------------------------

# Make tutti/ headers available to all targets
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Layer 0: Abstraction (header-only, no target)
# ---------------------------------------------------------------------------

# abstraction/ provides macros (TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, etc.)
# No library target needed - headers included directly

# ---------------------------------------------------------------------------
# Layer 0: Types (header-only, interface library)
# ---------------------------------------------------------------------------

add_library(tutti_types INTERFACE)
target_include_directories(tutti_types INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Layer 1: Accelerator HAL (libtutti_accel)
# ---------------------------------------------------------------------------

add_subdirectory(accel)

# ---------------------------------------------------------------------------
# Future layers (commented out, not built yet)
# ---------------------------------------------------------------------------

add_subdirectory(device_manager)   # Layer 2 ✅ ENABLED
add_subdirectory(backends)         # Layer 3 ✅ ENABLED
# add_subdirectory(io_engine)        # Layer 4
# add_subdirectory(block_storage)    # Layer 5
# add_subdirectory(raw_device)       # Layer 5
# add_subdirectory(coordinator)      # Layer 6

# ---------------------------------------------------------------------------
# Tests (Layer 1 smoke tests)
# ---------------------------------------------------------------------------

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/CMakeLists.txt")
    add_subdirectory(../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
    message(STATUS "Added tests subdirectory for Layer 1 smoke tests")
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

# Install all public headers
install(DIRECTORY abstraction/ DESTINATION include/tutti/abstraction
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY types/ DESTINATION include/tutti/types
        FILES_MATCHING PATTERN "*.h")

# Layer-specific headers installed by their own CMakeLists.txt
