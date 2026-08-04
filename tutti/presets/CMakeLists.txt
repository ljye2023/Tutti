# tutti/presets/CMakeLists.txt
#
# Preset assembly layer — public API factory functions that construct
# StorageRuntime with the appropriate DataPath + resolver injected.

# CUDA device linking — needed because the linked libraries
# (tutti_local_nvme_datapath etc.) contain .cu device code.
add_library(tutti_presets STATIC
    local_nvme_preset.cpp
    compute_kernels.cu
)

# Tell CMake to do CUDA device linking on this target.
set_target_properties(tutti_presets PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(tutti_presets PUBLIC
    tutti_local_nvme_datapath
    tutti_striped_local_nvme_datapath
    tutti_striped_file_resolver
)

target_link_libraries(tutti_presets PRIVATE
    ${TUTTI_ACCEL_RUNTIME_LIBS}
)

target_include_directories(tutti_presets PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."          # for private headers (tutti/data_paths/...)
)

# CCCL headers (cuda/atomic) — needed by storage_runtime.h includes.
# The root CMakeLists.txt adds these globally for the hardware stack,
# but we re-add here for safety.
if(EXISTS "/usr/local/cuda/targets/x86_64-linux/include/cccl")
    target_include_directories(tutti_presets PRIVATE
        /usr/local/cuda/targets/x86_64-linux/include/cccl)
endif()

set_target_properties(tutti_presets PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(tutti_presets PRIVATE
    -Wall -Wextra
)
