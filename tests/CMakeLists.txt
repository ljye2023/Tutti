# tests/CMakeLists.txt -- Layer 1 Smoke Tests Build Configuration

cmake_minimum_required(VERSION 3.18)

# Ensure CUDA is enabled
enable_language(CUDA)

# ---------------------------------------------------------------------------
# Layer 1 Smoke Test Executable
# ---------------------------------------------------------------------------

add_executable(layer1_smoke_test
    layer1_smoke_test.cu
)

# Set CUDA properties
set_target_properties(layer1_smoke_test PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
)

# Include directories
target_include_directories(layer1_smoke_test PRIVATE
    ${PROJECT_SOURCE_DIR}/tutti/accel/include/common
    ${PROJECT_SOURCE_DIR}/tutti/accel/include/cuda
    ${PROJECT_SOURCE_DIR}
)

# Link against CUDA runtime and accelerator implementation
target_link_libraries(layer1_smoke_test PRIVATE
    CUDA::cudart
    tutti_accel
)

# Set output directory
set_target_properties(layer1_smoke_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Add to default build target
add_custom_target(tests DEPENDS layer1_smoke_test)

message(STATUS "Configured layer1_smoke_test executable")
