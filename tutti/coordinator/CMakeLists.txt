# tutti/coordinator/CMakeLists.txt
# Layer 6: Coordinator - Top-level orchestrator bridging applications to storage layers
#
# Provides:
#   - ICoordinator interface for application entry point
#   - IRawDevice interface for fileless namespace + LBA-range access
#   - Buffer registration orchestration via HAL
#   - Batch submission workflows to IO Engine
#   - Dual data paths: Block Storage (GpuFile) and Raw Device (namespace/LBA)
#
# Dependencies: Layer 1 (accel), Layer 3 (backends), Layer 4 (io_engine), Layer 5 (block_storage)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_coordinator")
set(LAYER_NUMBER 6)
set(DEPENDS_ON "tutti_accel;tutti_backends;tutti_io_engine;tutti_block_storage")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/coordinator_impl.cpp
    src/raw_device_impl.cpp
    src/buffer_registry.cpp
)

set(LAYER_HEADERS
    include/coordinator.h
    include/coordinator_types.h
    include/raw_device.h
    include/gpu_file.h
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
        tutti_backends
        tutti_io_engine
        tutti_block_storage
    PRIVATE
        Threads::Threads
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

# =============================================================================
# Tests
# =============================================================================

# Layer-specific tests are excluded; all tests live under tutti/tests/
