# tutti/bindings/memfs/CMakeLists.txt
#
# SAMPLE-ONLY binding package: memfs resolver + binding + DataPath.
#
# This is a community-extension sample (see doc/extending_tutti.md).
# It is NOT a production backend.  The entire sample is header-only and
# hardware-free: no CUDA, no libnvm, no kernel headers.
#
# Registration: a single `add_subdirectory(bindings/memfs)` line in
# tutti/CMakeLists.txt enables both the library target and (under
# BUILD_TESTING) the contract test.  No other core CMake change is
# required.

# ---------------------------------------------------------------------------
# Header-only INTERFACE library
# ---------------------------------------------------------------------------

add_library(tutti_memfs_binding INTERFACE)

# Workspace root so that <tutti/bindings/memfs/*.h> and
# <tutti/resolvers/memfs/*.h> resolve.
target_include_directories(tutti_memfs_binding INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/..>
)

target_link_libraries(tutti_memfs_binding INTERFACE
    tutti_spi
)

# ---------------------------------------------------------------------------
# Contract test (self-contained — no additional line in tutti/CMakeLists.txt)
# ---------------------------------------------------------------------------

if(BUILD_TESTING)
    add_subdirectory(
        "${PROJECT_SOURCE_DIR}/../tests/memfs_sample_contract"
        "${CMAKE_CURRENT_BINARY_DIR}/tests_memfs_sample_contract"
    )
    message(STATUS "Tutti: added tests/memfs_sample_contract (sample extension)")
endif()
