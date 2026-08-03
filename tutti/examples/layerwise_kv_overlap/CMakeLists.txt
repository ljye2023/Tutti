# tutti/examples/layerwise_kv_overlap/CMakeLists.txt
#
# Layerwise KV-cache overlap simulator — StorageRuntime port.
#
# HY3-shaped 128K-context request (80 layers, 512×256-token chunks,
# 90% prefix hit) with 3-stream layerwise pipeline:
#   read(L+1) ∥ SGEMM compute(L) ∥ write(L-1)
#
# All data-plane operations use ONLY the public StorageRuntime API.
# The private LocalNvmeDataPath + LocalFileResolver are constructed
# only as injected RuntimeComponents fixtures.
#
# Hardware test: needs GPU + snvme + libnvm + NVMe mount.
# Gated by TUTTI_BUILD_HARDWARE_TESTS + TUTTI_FEATURE_LOCAL_NVME.

cmake_minimum_required(VERSION 3.18)

add_executable(tutti_layerwise_kv_overlap
    layerwise_kv_overlap.cu
)

target_link_libraries(tutti_layerwise_kv_overlap PRIVATE
    tutti_local_nvme_datapath
    CUDA::cudart
)

set_target_properties(tutti_layerwise_kv_overlap PROPERTIES
    CUDA_STANDARD 17
    CXX_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_compile_options(tutti_layerwise_kv_overlap PRIVATE
    -Wall -Wextra
)

add_test(NAME tutti_layerwise_kv_overlap
         COMMAND tutti_layerwise_kv_overlap)

set_tests_properties(tutti_layerwise_kv_overlap PROPERTIES
    LABELS "hardware;local_nvme;layerwise_overlap"
)

message(STATUS "Configured tutti_layerwise_kv_overlap (hardware;local_nvme;layerwise_overlap)")
