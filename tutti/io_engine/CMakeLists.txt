# tutti/io_engine/CMakeLists.txt
# Layer 4: IO Engine
#
# Provides:
#   - IIoEngine interface (backend-neutral IO orchestration)
#   - IO slice fan-out logic
#   - Batch submission coordination
#
# Dependencies: Layer 1 (tutti_accel), Layer 3 (backends)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_io_engine")
set(LAYER_NUMBER 4)
set(DEPENDS_ON "tutti_accel;tutti_backends")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/io_engine_impl.cpp
    src/local_nvme_io_engine.cpp
)

set(LAYER_HEADERS
    include/io_engine.h
    include/io_types.h
    include/local_nvme/launch_batch.h
    include/local_nvme/local_nvme_io_engine.h
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_io_engine"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Check if we have CUDA sources
list(FILTER LAYER_SOURCES INCLUDE REGEX "\\.cu$")
if(LAYER_SOURCES)
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
        tutti_backends
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
    DESTINATION include/tutti/io_engine
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.cuh"
)

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
