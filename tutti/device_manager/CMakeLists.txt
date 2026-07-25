# tutti/device_manager/CMakeLists.txt
# Layer 2: Device Manager - Complete NVMe Virtualization Stack
#
# Provides:
#   - Common: IVirtualNvme, IDeviceRegistry, ILeaseManager, VDevice
#   - NVMe: LocalNvmeVirtualRegistry, queue helpers, registries
#   - libnvm: User-space NVMe queue library
#   - NVMeService: Cross-process arbitration daemon
#   - snvme: Kernel module for NVMe user queues
#
# Dependencies: Layer 0 (abstraction), Layer 1 (accel)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_device_manager")
set(LAYER_NUMBER 2)
set(DEPENDS_ON "tutti_abstraction;tutti_accel")

# =============================================================================
# libnvm Library
# =============================================================================

# libnvm is the user-space NVMe queue library that wraps snvme kernel module
add_subdirectory(nvme/libnvm)

# =============================================================================
# NVMeService Daemon
# =============================================================================

# NVMeService provides cross-process arbitration (Level-1 allocator)
# NVMeService requires gRPC - make it optional
find_package(gRPC CONFIG QUIET)
if(gRPC_FOUND)
    add_subdirectory(nvme/nvmeservice)
    message(STATUS "NVMeService enabled (gRPC found)")
else()
    message(STATUS "NVMeService disabled (gRPC not found)")
endif()

# =============================================================================
# Source Collection
# =============================================================================

# Common sources (transport-agnostic interfaces)
set(COMMON_HEADERS
    include/common/device.h
    include/common/vdevice.h
    include/common/virtual_nvme.h
    include/common/device_registry.h
    include/common/lease_manager.h
)

# NVMe-specific sources
set(NVME_HEADERS
    nvme/include/local_nvme_device.h
    nvme/include/nvme_queue_group.h
    nvme/include/local_nvme_virtual.h
    nvme/include/queue_acquire_helper.cuh
)

set(NVME_SOURCES
    nvme/src/local_nvme_virtual.cpp
    nvme/src/nvme_physical_device.cpp
)

# New Layer 2 common sources (DeviceManagerImpl, etc.)
set(COMMON_SOURCES
    src/common/device_manager_impl.cpp
)

# New NVMe driver sources (DirectNvmeDeviceDriver, DaemonNvmeDeviceDriver)
set(NVME_DRIVER_SOURCES
    nvme/src/direct_nvme_device_driver.cpp
    nvme/src/daemon_nvme_device_driver.cpp
)

# Mock backend sources (in-memory IDeviceDriver, peer to nvme)
set(MOCK_HEADERS
    mock/include/mock_physical_device.h
    mock/include/mock_virtual_device.h
    mock/include/mock_lease_manager.h
    mock/include/mock_device_driver.h
)

set(MOCK_SOURCES
    mock/src/mock_device_driver.cpp
)

set(NVME_CUDA_HEADERS
    nvme/src/queue_acquire_helper_impl.cuh
)

set(LAYER_SOURCES
    ${COMMON_SOURCES}
    ${NVME_SOURCES}
    ${NVME_DRIVER_SOURCES}
    ${MOCK_SOURCES}
)

set(LAYER_HEADERS
    ${COMMON_HEADERS}
    ${NVME_HEADERS}
    ${NVME_CUDA_HEADERS}
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/nvme/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mock/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/nvme/src
        ${CMAKE_CURRENT_SOURCE_DIR}/nvme/libnvm/include
)

# =============================================================================
# Dependencies
# =============================================================================

target_link_libraries(${LAYER_NAME}
    PUBLIC
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

# Install common headers
install(DIRECTORY include/common/
    DESTINATION include/tutti/device_manager/common
    FILES_MATCHING PATTERN "*.h"
)

# Install nvme headers
install(DIRECTORY nvme/include/
    DESTINATION include/tutti/device_manager/nvme
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.cuh"
)

install(DIRECTORY mock/include/
    DESTINATION include/tutti/device_manager/mock
    FILES_MATCHING PATTERN "*.h"
)

# =============================================================================
# Kernel Module Reference
# =============================================================================

# kernel_modules/ contains snvme kernel module sources
# Build instructions in kernel_modules/PORTING.md
# Supports:
#   - Linux 5.15.0 (snvme-5.15.0-public)
#   - Linux 5.4.241 (snvme-5.4.241-1-tlinux4-0017)

# =============================================================================
# Documentation
# =============================================================================

# Layer 2 (Device Manager) owns:
#
# 1. Physical Device Management
#    - snvme kernel module (kernel_modules/)
#    - libnvm user-space library (libnvm/)
#    - Controller bring-up (init_b3, attach_client)
#
# 2. Cross-Process Arbitration (Level-1)
#    - NVMeService daemon (nvmeservice/)
#    - Lease management and heartbeat
#    - Dead-process queue reclamation
#
# 3. In-Process Virtualization (Level-2)
#    - IVirtualNvme interface (include/common/)
#    - LocalNvmeVirtualRegistry (nvme/src/)
#    - Per-backend vDevice allocation
#
# 4. Device-Side Queue Mechanics
#    - Queue helpers (nvme/include/queue_acquire_helper.cuh)
#    - acquire_queue, issue_nvme_cmd, poll
#
# Directory Structure:
#
# include/common/           - Transport-agnostic interfaces
#   device.h               - Device struct + BackendType enum
#   vdevice.h              - VDevice struct
#   virtual_nvme.h         - IVirtualNvme interface
#   device_registry.h      - IDeviceRegistry interface
#   lease_manager.h        - ILeaseManager interface
#
# nvme/                     - All NVMe-specific code
#   include/               - NVMe-specific types
#     local_nvme_device.h  - LocalNvmeDevice struct
#     nvme_queue_group.h   - NvmeQueueGroup class
#     local_nvme_virtual.h - LocalNvmeVirtualRegistry class
#     queue_acquire_helper.cuh - Device-side queue helpers
#   src/                   - NVMe implementations
#     local_nvme_virtual.cpp - LocalNvmeVirtualRegistry implementation
#     queue_acquire_helper_impl.cuh - Device-side helper implementations
#   libnvm/                - User-space NVMe library
#     include/             - libnvm public headers
#     src/                 - libnvm implementation
#   nvmeservice/           - Cross-process daemon
#     src/                 - NVMeService implementation
#     NVMeService.md       - Documentation
#   kernel_modules/        - snvme kernel module
#     snvme-5.15.0-public/ - Linux 5.15.0 version
#     snvme-5.4.241-1-tlinux4-0017/ - Linux 5.4.241 version
#     PORTING.md           - Kernel porting guide

# =============================================================================
# Tests
# =============================================================================

# Layer-specific tests are excluded; all tests live under tutti/tests/
