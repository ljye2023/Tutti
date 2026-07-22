# Block Storage Layer 5 Tests

# Basic smoke test - no mocks required
add_executable(layer5_basic_test
    layer5_basic_test.cpp
)

target_include_directories(layer5_basic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer5_basic_test PRIVATE
    tutti_block_storage
    tutti_backends
    tutti_io_engine
    tutti_accel
)

set_target_properties(layer5_basic_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer5_basic_test COMMAND layer5_basic_test)

set_tests_properties(layer5_basic_test PROPERTIES
    LABELS "layer5;basic"
    TIMEOUT 30
)

# Full smoke test with mocks (currently has interface issues)
add_executable(layer5_smoke_test
    layer5_smoke_test.cpp
)

target_include_directories(layer5_smoke_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer5_smoke_test PRIVATE
    tutti_block_storage
    tutti_backends
    tutti_io_engine
    tutti_accel
)

set_target_properties(layer5_smoke_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer5_smoke_test COMMAND layer5_smoke_test)

set_tests_properties(layer5_smoke_test PROPERTIES
    LABELS "layer5;smoke"
    TIMEOUT 60
)

# Integration test
add_executable(layer5_integration_test
    layer5_integration_test.cpp
)

target_include_directories(layer5_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(layer5_integration_test PRIVATE
    tutti_block_storage
    tutti_backends
    tutti_io_engine
    tutti_accel
)

set_target_properties(layer5_integration_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME layer5_integration_test COMMAND layer5_integration_test)

set_tests_properties(layer5_integration_test PROPERTIES
    LABELS "layer5;integration"
    TIMEOUT 120
)

install(TARGETS layer5_smoke_test layer5_integration_test
    RUNTIME DESTINATION bin/tests
)
