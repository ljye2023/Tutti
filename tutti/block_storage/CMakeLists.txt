# tutti/block_storage/CMakeLists.txt
# Layer 5: Block Storage Interface
#
# Provides:
#   - IBlockStorage interface
#   - GpuFile abstraction
#   - Shard resolution helpers
#
# Dependencies: Layer 1 (tutti_accel), Layer 3 (backends), Layer 4 (tutti_io_engine)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_block_storage")
set(LAYER_NUMBER 5)
set(DEPENDS_ON "tutti_accel;tutti_backend_interface;tutti_io_engine")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/block_storage_impl.cpp
    src/host_fs_backed_block_storage.cpp
    src/gpu_file_kernel.cu
)

set(LAYER_HEADERS
    include/block_storage.h
    include/gpu_file.h
    include/gpu_file_resolve.cuh
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_block_storage"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Check if we have CUDA sources
set(_cuda_sources ${LAYER_SOURCES})
list(FILTER _cuda_sources INCLUDE REGEX "\\.cu$")
if(_cuda_sources)
    set_target_properties(${LAYER_NAME} PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_STANDARD 20
    )
endif()

# =============================================================================
# Include Directories
# =============================================================================

target_include_directories(${LAYER_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# =============================================================================
# Dependencies
# =============================================================================

target_link_libraries(${LAYER_NAME}
    PUBLIC
        tutti_types
        tutti_accel
        tutti_backend_interface
        tutti_io_engine
    PRIVATE
        CUDA::cudart
)

# =============================================================================
# Layer Metadata (for validation)
# =============================================================================

set_target_properties(${LAYER_NAME} PROPERTIES
    TUTTI_LAYER_NUMBER ${LAYER_NUMBER}
    TUTTI_LAYER_DEPENDENCIES "${DEPENDS_ON}"
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

install(DIRECTORY include/
    DESTINATION include/tutti/block_storage
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.cuh"
)
