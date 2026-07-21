# tutti/backends/CMakeLists.txt
# Layer 3: Backends SPI
#
# Provides:
#   - IBackendProvider interface (backend_provider.h)
#   - Backend implementations (local_nvme, future: rdma_remote, cxl_memory)
#
# Dependencies: Layer 0, Layer 1, Layer 2

# =============================================================================
# Backend Provider Interface (header-only)
# =============================================================================

add_library(tutti_backend_interface INTERFACE)
target_include_directories(tutti_backend_interface INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# =============================================================================
# Backend Implementations
# =============================================================================

# Local NVMe backend
add_subdirectory(local_nvme)

# Future backends:
# add_subdirectory(rdma_remote)
# add_subdirectory(cxl_memory)

# =============================================================================
# Installation
# =============================================================================

install(DIRECTORY include/
    DESTINATION include/tutti/backends
    FILES_MATCHING PATTERN "*.h"
)
