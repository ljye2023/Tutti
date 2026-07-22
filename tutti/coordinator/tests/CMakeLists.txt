# Coordinator Layer 6 Tests

# Simple smoke test - types and factory only
add_executable(layer6_smoke_test_simple
    layer6_smoke_test_simple.cpp
)

target_include_directories(layer6_smoke_test_simple PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer6_smoke_test_simple PRIVATE
    tutti_coordinator
    tutti_block_storage
    tutti_io_engine
    tutti_backends
    tutti_accel
)

set_target_properties(layer6_smoke_test_simple PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer6_smoke_test_simple COMMAND layer6_smoke_test_simple)

set_tests_properties(layer6_smoke_test_simple PROPERTIES
    LABELS "layer6;smoke;quick"
    TIMEOUT 10
)

# Basic smoke test - no mocks required
add_executable(layer6_basic_test
    layer6_basic_test.cpp
)

target_include_directories(layer6_basic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer6_basic_test PRIVATE
    tutti_coordinator
    tutti_block_storage
    tutti_io_engine
    tutti_backends
    tutti_accel
)

set_target_properties(layer6_basic_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer6_basic_test COMMAND layer6_basic_test)

set_tests_properties(layer6_basic_test PROPERTIES
    LABELS "layer6;basic"
    TIMEOUT 30
)

# Full smoke test with mocks (currently has interface issues)
add_executable(layer6_smoke_test
    layer6_smoke_test.cpp
)

target_include_directories(layer6_smoke_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer6_smoke_test PRIVATE
    tutti_coordinator
    tutti_block_storage
    tutti_io_engine
    tutti_backends
    tutti_accel
)

set_target_properties(layer6_smoke_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer6_smoke_test COMMAND layer6_smoke_test)

set_tests_properties(layer6_smoke_test PROPERTIES
    LABELS "layer6;smoke"
    TIMEOUT 60
)

# Integration test
add_executable(layer6_integration_test
    layer6_integration_test.cpp
)

target_include_directories(layer6_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer6_integration_test PRIVATE
    tutti_coordinator
    tutti_block_storage
    tutti_io_engine
    tutti_backends
    tutti_accel
)

set_target_properties(layer6_integration_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer6_integration_test COMMAND layer6_integration_test)

set_tests_properties(layer6_integration_test PROPERTIES
    LABELS "layer6;integration"
    TIMEOUT 120
)

# Hardware basic test - requires real NVMe + GPU
add_executable(layer6_hw_basic_test
    layer6_hw_basic_test.cpp
)

target_include_directories(layer6_hw_basic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer6_hw_basic_test PRIVATE
    tutti_coordinator
    tutti_block_storage
    tutti_io_engine
    tutti_backends
    tutti_device_manager
    tutti_accel
    libnvm
    ${CUDA_LIBRARIES}
)

set_target_properties(layer6_hw_basic_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Hardware test - not run by default in CI
add_test(NAME layer6_hw_basic_test COMMAND layer6_hw_basic_test)
set_tests_properties(layer6_hw_basic_test PROPERTIES
    LABELS "layer6;hardware;manual"
    TIMEOUT 120
)

install(TARGETS layer6_smoke_test layer6_integration_test layer6_hw_basic_test
    RUNTIME DESTINATION bin/tests
)
