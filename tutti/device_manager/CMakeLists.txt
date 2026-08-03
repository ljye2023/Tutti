# tutti/device_manager/CMakeLists.txt
#
# Layer 2 container — shrunk by T-041 (Round 10 Session 2).
#
# The control-plane driver/common/mock sources that used to live here were
# deleted in 2026-08 dead-code cleanup (the production DataPath talks to
# libnvm directly; the driver abstraction layer had no consumer). What
# remains in this directory is the single-source-of-truth for libnvm and
# the optional gRPC nvmeservice daemon, plus the snvme kernel_modules tree
# (built out-of-tree, not by CMake).
#
# Per Session 1: libnvm and kernel_modules stay at this location. nvmeservice
# stays here too (optional gRPC daemon); its consumer attribution is documented
# in chat/round10/result2.md.

# =============================================================================
# libnvm — user-space NVMe queue library (wraps snvme kernel module)
# =============================================================================
add_subdirectory(nvme/libnvm)

# =============================================================================
# nvmeservice — cross-process arbitration daemon (gRPC, optional)
# =============================================================================
find_package(gRPC CONFIG QUIET)
if(gRPC_FOUND)
    add_subdirectory(nvme/nvmeservice)
    message(STATUS "NVMeService enabled (gRPC found)")
else()
    message(STATUS "NVMeService disabled (gRPC not found)")
endif()

# kernel_modules/ contains the snvme kernel module sources; built out-of-tree
# by the deployment system (see kernel_modules/PORTING.md), not by CMake.
