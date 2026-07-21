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

# ---------------------------------------------------------------------------
# Layer 3 Smoke Test Executable
# ---------------------------------------------------------------------------

add_executable(layer3_smoke_test
    layer3_smoke_test.cu
)

# Set CUDA properties
set_target_properties(layer3_smoke_test PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
)

# Include directories
target_include_directories(layer3_smoke_test PRIVATE
    ${PROJECT_SOURCE_DIR}/accel/include/common
    ${PROJECT_SOURCE_DIR}/accel/include/cuda
    ${PROJECT_SOURCE_DIR}/device_manager/include/common
    ${PROJECT_SOURCE_DIR}/device_manager/nvme/libnvm/include
    ${PROJECT_SOURCE_DIR}/backends/include
    ${PROJECT_SOURCE_DIR}
)

# Link against all required libraries
target_link_libraries(layer3_smoke_test PRIVATE
    CUDA::cudart
    tutti_accel
    tutti_device_manager
    tutti_backends
    -Wl,--whole-archive
    tutti_backends_nvme
    -Wl,--no-whole-archive
)

# Set output directory
set_target_properties(layer3_smoke_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "Configured layer3_smoke_test executable")

# ---------------------------------------------------------------------------
# Layer 3 Integration Test Executable (with real NVMe)
# ---------------------------------------------------------------------------

add_executable(layer3_integration_test
    layer3_integration_test.cu
)

# Set CUDA properties
set_target_properties(layer3_integration_test PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
)

# Include directories
target_include_directories(layer3_integration_test PRIVATE
    ${PROJECT_SOURCE_DIR}/accel/include/common
    ${PROJECT_SOURCE_DIR}/accel/include/cuda
    ${PROJECT_SOURCE_DIR}/device_manager/include/common
    ${PROJECT_SOURCE_DIR}/device_manager/nvme/libnvm/include
    ${PROJECT_SOURCE_DIR}/backends/include
    ${PROJECT_SOURCE_DIR}
)

# Link against all required libraries
target_link_libraries(layer3_integration_test PRIVATE
    CUDA::cudart
    tutti_accel
    tutti_device_manager
    tutti_backends
    -Wl,--whole-archive
    tutti_backends_nvme
    -Wl,--no-whole-archive
    libnvm
)

# Set output directory
set_target_properties(layer3_integration_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "Configured layer3_integration_test executable")

# ---------------------------------------------------------------------------
# Layer 3 Integration Test Executable (simplified, with real NVMe)
# ---------------------------------------------------------------------------

add_executable(layer3_integration_simple
    layer3_integration_simple.cu
)

# Set CUDA properties
set_target_properties(layer3_integration_simple PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
)

# Include directories
target_include_directories(layer3_integration_simple PRIVATE
    ${PROJECT_SOURCE_DIR}/accel/include/common
    ${PROJECT_SOURCE_DIR}/accel/include/cuda
    ${PROJECT_SOURCE_DIR}/device_manager/include/common
    ${PROJECT_SOURCE_DIR}/device_manager/nvme/libnvm/include
    ${PROJECT_SOURCE_DIR}/backends/include
    ${PROJECT_SOURCE_DIR}
)

# Link against all required libraries
target_link_libraries(layer3_integration_simple PRIVATE
    CUDA::cudart
    tutti_accel
    tutti_device_manager
    tutti_backends
    -Wl,--whole-archive
    tutti_backends_nvme
    -Wl,--no-whole-archive
    libnvm
)

# Set output directory
set_target_properties(layer3_integration_simple PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "Configured layer3_integration_simple executable")

