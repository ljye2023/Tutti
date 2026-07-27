# tutti/backends/CMakeLists.txt
# Layer 3: Backends - Transport Adapters
#
# Provides:
#   - Common: IBackendProvider interface, BackendFactory
#   - NVMe: LocalNvmeBackend implementation
#
# Dependencies: Layer 0 (abstraction/types), Layer 1 (accel), Layer 2 (device_manager)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_backends")
set(LAYER_NUMBER 3)
set(DEPENDS_ON "tutti_abstraction;tutti_types;tutti_accel;tutti_device_manager")

# =============================================================================
# Source Collection
# =============================================================================

# Common sources (transport-agnostic interfaces and factory)
set(COMMON_HEADERS
    include/backend.h
    include/backend_factory.h
    include/backend_types.h
)

set(COMMON_SOURCES
    src/backend_factory.cpp
)

# Mock backend: device-agnostic IBackend impl for testing (no NVMe/CUDA).
set(MOCK_SOURCES
    mock/src/mock_backend.cpp
)

set(LAYER_SOURCES
    ${COMMON_SOURCES}
    ${MOCK_SOURCES}
)

set(LAYER_HEADERS
    ${COMMON_HEADERS}
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_backends"
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
        ${CMAKE_CURRENT_SOURCE_DIR}/mock/include
)

# =============================================================================
# Dependencies
# =============================================================================

target_link_libraries(${LAYER_NAME}
    PUBLIC
        tutti_device_manager
        tutti_accel
        tutti_types
)

# =============================================================================
# Backend Implementations
# =============================================================================

# NVMe backend -- migrated to IBackend + NvmeVirtualDevice.
add_subdirectory(nvme)

# Future backends:
# add_subdirectory(rdma)
# add_subdirectory(gds)

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

# Install common headers
install(DIRECTORY include/
    DESTINATION include/tutti/backends
    FILES_MATCHING PATTERN "*.h"
)

# =============================================================================
# Documentation
# =============================================================================

# Layer 3 (Backends) owns:
#
# 1. Backend Service Provider Interface (SPI)
#    - IBackendProvider interface (include/backend_provider.h)
#    - BackendFactory for runtime selection (include/backend_factory.h)
#    - BackendType enum (include/backend_types.h)
#
# 2. Transport Adapters
#    - Convert ioaddrs + StorageTarget to device-issuable commands
#    - NVMe: PRP/SGL descriptor construction
#    - RDMA: RDMA descriptor with rkeys (future)
#    - GDS: GPUDirect Storage (future)
#
# 3. Target Handle Management
#    - acquire_target_handle(): Build GPU-resident handle from StorageTarget
#    - release_target_handle(): Free GPU-resident handle
#    - Handles are opaque void* typed by backend
#
# 4. Submission Modes
#    - launch_batch_gpu_stream(): GPU kernel submission (primary path)
#    - submit_batch_cpu_sync(): CPU synchronous submission
#    - submit_batch_cpu_async(): CPU async submission (optional)
#
# Directory Structure:
#
# include/                  - Common backend SPI
#   backend_provider.h     - IBackendProvider interface (core SPI)
#   backend_factory.h      - Runtime backend selection
#   backend_types.h        - Backend-agnostic types
# src/                      - Common implementation
#   backend_factory.cpp    - Factory implementation
# nvme/                     - NVMe backend implementation
#   include/               - NVMe backend public API
#   src/                   - Host-side implementation
#   device/                - Device-side kernels
