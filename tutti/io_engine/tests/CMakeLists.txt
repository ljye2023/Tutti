# tutti/io_engine/tests/CMakeLists.txt
# Layer 4: IO Engine Tests

add_executable(layer4_smoke_test
    layer4_smoke_test.cpp
    ../src/io_engine_impl.cpp
)

target_include_directories(layer4_smoke_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tutti
)

target_link_libraries(layer4_smoke_test PRIVATE
    tutti_backends
    tutti_accel
)

# Register test with CTest
add_test(NAME layer4_smoke_test COMMAND layer4_smoke_test)
