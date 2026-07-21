# tutti/device_manager/tests/CMakeLists.txt
# Layer 2 Smoke Tests

# =============================================================================
# Smoke Test Executable
# =============================================================================

add_executable(layer2_smoke_test
    layer2_smoke_test.cpp
)

target_include_directories(layer2_smoke_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../nvme/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../nvme/libnvm/include
)

target_link_libraries(layer2_smoke_test
    PRIVATE
        tutti_device_manager
)

set_target_properties(layer2_smoke_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# =============================================================================
# Add test to CTest
# =============================================================================

add_test(
    NAME layer2_smoke
    COMMAND layer2_smoke_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(layer2_smoke PROPERTIES
    LABELS "layer2;smoke"
    TIMEOUT 60
)

# =============================================================================
# Installation
# =============================================================================

install(TARGETS layer2_smoke_test
    RUNTIME DESTINATION bin/tests
)
