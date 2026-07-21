# tutti/backends/nvme/CMakeLists.txt
# Layer 3: NVMe Backend - Local NVMe Transport Adapter
#
# Provides:
#   - LocalNvmeBackend: IBackendProvider implementation for local NVMe
#   - NVMe command building and submission
#   - PRP page cache management
#   - Device-side batch submission kernels
#
# Dependencies: Layer 2 (device_manager), CUDA

# =============================================================================
# Source Collection
# =============================================================================

# NVMe-specific headers
set(NVME_HEADERS
    include/nvme_backend.h
    include/nvme_command_builder.h
    include/nvme_target_handle.h
    include/prp_page_cache.h
)

# Device-side headers
set(NVME_DEVICE_HEADERS
    device/nvme_device_helpers.cuh
)

# NVMe-specific sources
set(NVME_SOURCES
    src/nvme_backend.cpp
    src/nvme_target_handle.cpp
    src/nvme_submission.cpp
    src/prp_page_cache.cpp
    src/nvme_command_builder.cpp
    src/nvme_backend_registration.cpp
)

# Device-side CUDA sources
set(NVME_CUDA_SOURCES
    device/submit_batch_kernel.cu
)

# =============================================================================
# Library Target
# =============================================================================

add_library(tutti_backends_nvme
    ${NVME_SOURCES}
    ${NVME_CUDA_SOURCES}
)

set_target_properties(tutti_backends_nvme PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_backends_nvme"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# =============================================================================
# Include Directories
# =============================================================================

target_include_directories(tutti_backends_nvme
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/device
        ${PROJECT_SOURCE_DIR}/device_manager/nvme/libnvm/include
        ${PROJECT_SOURCE_DIR}/device_manager/nvme/libnvm/src
)

# =============================================================================
# Dependencies
# =============================================================================

target_link_libraries(tutti_backends_nvme
    PUBLIC
        tutti_backends              # Backend SPI interface
    PRIVATE
        tutti_device_manager        # VDevice, queue helpers
        CUDA::cudart
)

# =============================================================================
# Compiler Options
# =============================================================================

target_compile_options(tutti_backends_nvme PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>
    $<$<COMPILE_LANGUAGE:CUDA>:
        --expt-relaxed-constexpr
        --expt-extended-lambda
        -Xcompiler=-Wall,-Wextra
    >
)

# =============================================================================
# Installation
# =============================================================================

install(TARGETS tutti_backends_nvme
    EXPORT tutti_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install NVMe backend headers
install(DIRECTORY include/
    DESTINATION include/tutti/backends/nvme
    FILES_MATCHING PATTERN "*.h"
)

# Install device-side headers
install(DIRECTORY device/
    DESTINATION include/tutti/backends/nvme/device
    FILES_MATCHING PATTERN "*.cuh"
)

# =============================================================================
# Documentation
# =============================================================================

# NVMe Backend Implementation:
#
# 1. Core Backend (src/)
#    - nvme_backend.cpp: LocalNvmeBackend class, IBackendProvider impl
#    - nvme_backend_registration.cpp: Factory registration
#    - nvme_target_handle.cpp: Target handle management
#    - nvme_submission.cpp: Submission path implementations
#
# 2. Command Building (src/)
#    - nvme_command_builder.cpp: NVMe command construction
#    - prp_page_cache.cpp: PRP descriptor page cache
#
# 3. Device-Side Kernels (device/)
#    - submit_batch_kernel.cu: GPU-driven batch submission
#    - nvme_device_helpers.cuh: Device-side helper functions
#
# Responsibilities:
#    - Convert ioaddrs to NVMe PRP/SGL descriptors
#    - Build NVMe commands from StorageTarget + LbaExtent
#    - Submit via VDevice queue slice from Device Manager
#    - Manage PRP page cache for scatter-gather IO
#    - Provide both CPU and GPU submission paths
