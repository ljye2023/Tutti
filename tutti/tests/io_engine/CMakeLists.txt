# tutti/tests/io_engine/CMakeLists.txt -- Layer 4 IO Engine tests
#
# Standalone unit test for io_engine::StripeManager, the backend-agnostic
# IO-time logical->physical stripe mapper. Like the other layer tests, this
# compiles the implementation source directly rather than linking the (still
# disabled) tutti_io_engine library. The point is to prove the mapper is truly
# self-contained: if this target links with no backend / HAL / CUDA symbols,
# the stripe manager really is pure math over its own StripeLayout input.

cmake_minimum_required(VERSION 3.18)

set(IO_ENGINE_LAYER_DIR "${PROJECT_SOURCE_DIR}/io_engine")

add_executable(stripe_manager_test
    stripe_manager_test.cpp
    # Implementation under test, compiled in directly (no library link).
    ${IO_ENGINE_LAYER_DIR}/src/stripe_manager.cpp
)

set_target_properties(stripe_manager_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# This is an assert()-driven test. RelWithDebInfo/Release define NDEBUG, which
# turns every assert() into a no-op and makes the test pass vacuously. Undefine
# NDEBUG for this target only (appended after the build-type -DNDEBUG, so it
# wins) so the checks always run regardless of CMAKE_BUILD_TYPE.
target_compile_options(stripe_manager_test PRIVATE -UNDEBUG)

# Only PROJECT_SOURCE_DIR is needed: both files use the
# "io_engine/include/stripe_manager.h" include style. No backend/HAL/CUDA dirs.
target_include_directories(stripe_manager_test PRIVATE
    ${PROJECT_SOURCE_DIR}
)

add_test(NAME stripe_manager_test COMMAND stripe_manager_test)

message(STATUS "Configured stripe_manager_test executable (standalone, pure-math)")

# ---------------------------------------------------------------------------
# layer4_smoke_test -- IO Engine end-to-end against a mock IBatchSubmitter.
#
# Phase 0 proof that IoEngineImpl compiles and runs submit_batch through the
# narrow nvme::IBatchSubmitter SPI. Because the engine binds to that interface
# (not concrete NvmeBackend), this links with NO backend / CUDA / libnvm
# symbols -- the mock submitter and mock accelerator supply everything. It is a
# non-assert test (if/return + prints), so NDEBUG does not affect it.
# ---------------------------------------------------------------------------
add_executable(layer4_smoke_test
    ${IO_ENGINE_LAYER_DIR}/tests/layer4_smoke_test.cpp
    ${IO_ENGINE_LAYER_DIR}/src/io_engine_impl.cpp
)

set_target_properties(layer4_smoke_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(layer4_smoke_test PRIVATE
    ${PROJECT_SOURCE_DIR}
)

add_test(NAME layer4_smoke_test COMMAND layer4_smoke_test)

message(STATUS "Configured layer4_smoke_test executable (mock IBatchSubmitter, no backend link)")

# ---------------------------------------------------------------------------
# submit_one_test -- Unit test for IoEngineImpl::submit_one.
#
# Tests the full submit_one path (acquire handle -> MDTS fan-out ->
# prepare_descriptors -> launch) against a MockBatchSubmitter that tracks
# acquire_count and prepare_count, with no CUDA / libnvm / real vdevice symbols.
# Requires SingleShardIoRequest in io_types.h and acquire_target_handle on
# IBatchSubmitter (both added in the same refact branch).
# ---------------------------------------------------------------------------
add_executable(submit_one_test
    submit_one_test.cpp
    ${IO_ENGINE_LAYER_DIR}/src/io_engine_impl.cpp
    ${IO_ENGINE_LAYER_DIR}/src/stripe_manager.cpp
)

set_target_properties(submit_one_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Assert-driven test: undefine NDEBUG so asserts always fire regardless of
# CMAKE_BUILD_TYPE (same rationale as stripe_manager_test).
target_compile_options(submit_one_test PRIVATE -UNDEBUG)

target_include_directories(submit_one_test PRIVATE
    ${PROJECT_SOURCE_DIR}
)

add_test(NAME submit_one_test COMMAND submit_one_test)

message(STATUS "Configured submit_one_test executable (mock IBatchSubmitter, assert-driven)")
