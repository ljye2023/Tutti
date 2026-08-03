# tutti/cmake/accelerators/CUDA.cmake -- CUDA profile (NVIDIA-first)
#
# Included by the root CMakeLists.txt when TUTTI_ACCELERATOR=CUDA.
# Provides:
#   - TUTTI_BUILD_HARDWARE_STACK default ON
#   - tutti_configure_cuda_like() function

# CUDA profile defaults to full hardware stack
set(TUTTI_BUILD_HARDWARE_STACK ON CACHE BOOL
    "Build hardware stack (accel/device_manager/backends/io_engine)")

# ---------------------------------------------------------------------------
# tutti_configure_cuda_like(<target_name>)
#
# Configures the given INTERFACE target with CUDA usage requirements:
#   - TUTTI_USE_CUDA=1 definition
#   - tutti/include directory
#   - CUDA::cudart and CUDA::cuda_driver links
# ---------------------------------------------------------------------------
function(tutti_configure_cuda_like target_name)
    find_package(CUDAToolkit REQUIRED)

    target_compile_definitions(${target_name} INTERFACE TUTTI_USE_CUDA=1)

    target_include_directories(${target_name} INTERFACE
        "${PROJECT_SOURCE_DIR}/include"
    )

    target_link_libraries(${target_name} INTERFACE
        CUDA::cudart
        CUDA::cuda_driver
    )
endfunction()
