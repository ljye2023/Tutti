# tutti/block_storage/CMakeLists.txt
# Layer 5: Block Storage - Named, striped, persistent GPU file containers
#
# Provides:
#   - IBlockStorage interface for file directory operations
#   - GpuFile management with multi-device striping
#   - StorageTarget production for IO Engine consumption
#   - Metadata persistence and durability operations
#
# Dependencies: Layer 1 (accel), Layer 3 (backends), Layer 4 (io_engine)

# =============================================================================
# Layer Configuration
# =============================================================================

set(LAYER_NAME "tutti_block_storage")
set(LAYER_NUMBER 5)
set(DEPENDS_ON "tutti_accel;tutti_backends;tutti_io_engine")

# =============================================================================
# Source Collection
# =============================================================================

set(LAYER_SOURCES
    src/block_storage_impl.cpp
    src/file_directory.cpp
    src/stripe_manager.cpp
    src/metadata_journal.cpp
)

set(LAYER_HEADERS
    include/block_storage.h
    include/block_storage_types.h
    include/storage_config.h
)

# =============================================================================
# Library Target
# =============================================================================

add_library(${LAYER_NAME} ${LAYER_SOURCES})

set_target_properties(${LAYER_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "tutti_block_storage"
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
    DESTINATION include/tutti/block_storage
    FILES_MATCHING PATTERN "*.h"
)

# =============================================================================
# Tests
# =============================================================================

# Layer-specific tests are excluded; all tests live under tutti/tests/

# =============================================================================
# Documentation
# =============================================================================

# Layer 5 (Block Storage) responsibilities:
#
# 1. Named File Management
#    - Persistent directory of GpuFile entries (name -> metadata)
#    - File lifecycle: create, open, close, delete
#    - Metadata journal for crash recovery
#
# 2. Multi-Device Striping
#    - Stripe files across N device shards with configurable stripe_size
#    - Round-robin device allocation for load balancing
#    - Tensor-size interleaving for GPU-efficient access patterns
#
# 3. StorageTarget Production
#    - acquire_device_handle(): Map FileShard to StorageTarget (LBA extents)
#    - Backend integration: delegate to IBackendProvider for target handle creation
#    - release_device_handle(): Cleanup backend handles
#
# 4. Durability Operations
#    - sync_file(): Stream-sync + NVMe flush per shard
#    - flush_metadata(): Persist directory to journal
#    - Configurable sync_mode (NONE, METADATA_ONLY, FULL)
#
# 5. Batch Operations
#    - open_gpu_files_batch(): Bulk file open with deferred shard allocation
#    - Amortize metadata lookups and backend calls across batch
