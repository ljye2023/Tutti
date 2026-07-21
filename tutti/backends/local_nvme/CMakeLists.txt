# tutti/backends/local_nvme/CMakeLists.txt
# Layer 3: Local NVMe Backend
#
# Provides:
#   - LocalNvmeBackend (concrete IBackendProvider)
#   - PRP/SGL descriptor building
#   - GPU-stream batch submit kernel
#
# Dependencies: Layer 0, Layer 1 (tutti_accel), Layer 2 (tutti_device_manager)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_backend_local_nvme")
set(LAYER_NUMBER 3)
set(DEPENDS_ON "tutti_accel;tutti_device_manager")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/local_nvme_backend.cpp
    src/prp_builder.cu
    src/prp_page_cache.cu
    src/submit_kernel.cu
)

set(LAYER_HEADERS
    include/local_nvme_backend.h
    include/nvme_backend_device.cuh
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 20
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_backend_local_nvme"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

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
        ${PROJECT_SOURCE_DIR}/backends/local/nvme/libnvm/include
)

# =============================================================================
# Dependencies
# =============================================================================

target_link_libraries(${LAYER_NAME}
    PUBLIC
        tutti_types
        tutti_accel
        tutti_device_manager
        tutti_backend_interface
    PRIVATE
        libnvm
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
    DESTINATION include/tutti/backends/local_nvme
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.cuh"
)
