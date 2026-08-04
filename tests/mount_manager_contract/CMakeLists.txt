# tests/mount_manager_contract/CMakeLists.txt
# Round 17 S1: host-side unit tests for MountManager.
#
# These tests do NOT touch the running daemon or real NVMe devices.
# They test:
#   1. config parsing (auto_mount field, unmount_retry block)
#   2. /proc parsing helpers (scan_holders against the test process itself)
#   3. is_mounted() against /proc/self/mountinfo
#   4. mount_one/unmount_all lifecycle using a loopback tmpfs (no real NVMe)

add_executable(tutti_mount_manager_contract_test
    mount_manager_contract_test.cpp
)

target_link_libraries(tutti_mount_manager_contract_test PRIVATE
    nvmeservice
    ${TUTTI_YAML_CPP_TARGET}
    Threads::Threads
)

set_target_properties(tutti_mount_manager_contract_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME tutti_mount_manager_contract_test
         COMMAND tutti_mount_manager_contract_test)
set_tests_properties(tutti_mount_manager_contract_test PROPERTIES
    LABELS "host;mount_manager")
