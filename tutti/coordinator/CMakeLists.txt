# tutti/coordinator/CMakeLists.txt
# Layer 6: Coordinator
#
# Provides:
#   - Coordinator class (top-level orchestrator)
#   - Bootstrap + device initialization
#   - High-level API for all storage interfaces
#
# Dependencies: All lower layers (1-5)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_coordinator")
set(LAYER_NUMBER 6)
set(DEPENDS_ON "tutti_accel;tutti_device_manager;tutti_backend_interface;tutti_io_engine;tutti_block_storage;tutti_raw_device")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/coordinator.cpp
)

set(LAYER_HEADERS
    include/coordinator.h
    include/device.h
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_coordinator"
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
        tutti_io_engine
        tutti_block_storage
        tutti_raw_device
    PRIVATE
        tutti_backend_local_nvme
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
    DESTINATION include/tutti/coordinator
    FILES_MATCHING PATTERN "*.h"
)
