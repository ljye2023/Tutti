# tutti/raw_device/CMakeLists.txt
# Layer 5: Raw Device Interface
#
# Provides:
#   - IRawDevice interface
#   - Namespace + LBA → StorageTarget conversion
#
# Dependencies: Layer 1 (tutti_accel), Layer 3 (backends), Layer 4 (tutti_io_engine)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_raw_device")
set(LAYER_NUMBER 5)
set(DEPENDS_ON "tutti_accel;tutti_backend_interface;tutti_io_engine")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/raw_device_impl.cpp
)

set(LAYER_HEADERS
    include/raw_device.h
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_raw_device"
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
        tutti_backend_interface
        tutti_io_engine
        tutti_device_manager
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
    DESTINATION include/tutti/raw_device
    FILES_MATCHING PATTERN "*.h"
)
