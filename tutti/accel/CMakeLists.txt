# tutti/accel/CMakeLists.txt
# Layer 1: Accelerator HAL
#
# Provides:
#   - IAccelerator interface (include/common/iaccel.h)
#   - CudaAccelerator implementation (src/cuda/cuda_accelerator.cu)
#   - Shared types: AccelStream, AccelEvent, MemoryKind, MemoryRegion
#
# Dependencies: Layer 0 (abstraction - header-only)

cmake_minimum_required(VERSION 3.18)

# =============================================================================
# CUDA Toolkit
# =============================================================================

find_package(CUDAToolkit REQUIRED)
message(STATUS "Found CUDA Toolkit: ${CUDAToolkit_VERSION}")

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_accel")
set(LAYER_NUMBER 1)

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/cuda/cuda_accelerator.cu
)

set(LAYER_HEADERS
    include/common/accel_types.h
    include/common/memory_kind.h
    include/common/memory_region.h
    include/common/iaccel.h
    include/cuda/cuda_accelerator.h
)

# =============================================================================
# Library Target (libtutti_accel)
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

# Enable CUDA compilation for .cu files
set_source_files_properties(${LAYER_SOURCES} PROPERTIES LANGUAGE CUDA)

set_target_properties(${LAYER_NAME} PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_accel"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Define TUTTI_ACCEL_CUDA preprocessor macro
target_compile_definitions(${LAYER_NAME}
    PUBLIC
        TUTTI_ACCEL_CUDA
)

# =============================================================================
# Include Directories
# =============================================================================

target_include_directories(${LAYER_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/common>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/cuda>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# =============================================================================
# Dependencies
# =============================================================================

# Link CUDA runtime
target_link_libraries(${LAYER_NAME}
    PUBLIC
        CUDA::cudart
        tutti_types
)

# =============================================================================
# Layer Metadata (for validation)
# =============================================================================

set_target_properties(${LAYER_NAME} PROPERTIES
    TUTTI_LAYER_NUMBER ${LAYER_NUMBER}
)

# =============================================================================
# Installation
# =============================================================================

install(TARGETS ${LAYER_NAME}
    EXPORT tutti_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/common/
    DESTINATION include/tutti/accel
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY include/cuda/
    DESTINATION include/tutti/accel/cuda
    FILES_MATCHING PATTERN "*.h"
)

message(STATUS "Configured Layer 1: ${LAYER_NAME}")
