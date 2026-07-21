# tutti/device_manager/CMakeLists.txt
# Layer 2: Device Manager
#
# Provides:
#   - IVirtualNvme interface (Level-2 queue allocator)
#   - VDevice struct (queue slice + caps)
#   - Device-side queue helpers
#
# Dependencies: Layer 0, Layer 1 (tutti_accel)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_device_manager")
set(LAYER_NUMBER 2)
set(DEPENDS_ON "tutti_accel")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/local_nvme_virtual.cpp
)

set(LAYER_HEADERS
    include/vdevice.h
    include/virtual_nvme.h
    include/device_registry.h
    include/local_nvme_device.h
    include/nvme_queue_group.h
    include/lease_manager.h
    include/queue_acquire_helper.cuh
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_device_manager"
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
    PRIVATE
        libnvm
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
    DESTINATION include/tutti/device_manager
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.cuh"
)
