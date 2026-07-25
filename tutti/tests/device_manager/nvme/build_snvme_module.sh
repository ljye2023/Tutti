#!/usr/bin/env bash
# build_snvme_module.sh -- build the snvme kernel module FROM THIS REPO's source.
#
# Source baseline: tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public
#
# blk_set_queue_dying(request_queue) was removed upstream in v5.17 and replaced
# by blk_mark_disk_dead(gendisk); the change was backported into some 5.15
# point-releases (e.g. the 5.15.0-185 HWE kernel).  The baseline source handles
# both: core.c / multipath.c select the call via the HAVE_BLK_MARK_DISK_DEAD
# macro, and the generated Makefile defines that macro by probing whether the
# target kernel exports blk_mark_disk_dead in its Module.symvers.  No source
# rewrite (sed shim) is needed anymore.
#
# This script copies the baseline into a build directory and builds
# snvme-core.ko + snvme.ko out-of-tree against the running kernel; the repo
# source is left untouched.
#
# Output: <build-dir>/snvme-core.ko and <build-dir>/snvme.ko
#   default build-dir: <repo>/tutti/build_snvme_module
#
# Usage:
#   ./build_snvme_module.sh                 # build into the default dir
#   ./build_snvme_module.sh --build-dir DIR
#   ./build_snvme_module.sh --baseline snvme-5.15.0-public
#
# This is intentionally standalone (not wired into the CMake `modules` target).

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"

BASELINE="snvme-5.15.0-public"
BUILD_DIR="$REPO/tutti/build_snvme_module"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --baseline)  BASELINE="$2";  shift 2 ;;
        -h|--help)   sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

SRC="$REPO/tutti/device_manager/nvme/kernel_modules/$BASELINE"
[[ -d "$SRC" ]] || { echo "baseline source not found: $SRC" >&2; exit 1; }

LIBNVM_INC="$REPO/tutti/device_manager/nvme/libnvm/include"
# newest nvidia driver source (for nv-p2p.h)
NV_INC="$(ls -d /usr/src/nvidia-*/nvidia 2>/dev/null | sort -V | tail -1 || true)"
[[ -n "$NV_INC" && -e "$NV_INC/nv-p2p.h" ]] || {
    echo "could not locate nvidia nv-p2p.h under /usr/src/nvidia-*/nvidia" >&2; exit 1; }

KERNEL_SRC="/lib/modules/$(uname -r)/build"
[[ -d "$KERNEL_SRC" ]] || { echo "kernel build dir missing: $KERNEL_SRC" >&2; exit 1; }

echo "=== snvme module build (from repo source) ==="
echo "  baseline : $SRC"
echo "  build dir: $BUILD_DIR"
echo "  libnvm   : $LIBNVM_INC"
echo "  nvidia   : $NV_INC"
echo "  kernel   : $KERNEL_SRC"

# Fresh copy of the baseline into the build dir (repo source untouched).
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cp -a "$SRC"/. "$BUILD_DIR"/

# NOTE: the baseline source no longer needs a build-dir sed shim.  core.c /
# multipath.c gate blk_set_queue_dying() vs blk_mark_disk_dead() on the
# HAVE_BLK_MARK_DISK_DEAD macro, which the baseline Makefile.in defines by
# probing the target kernel's Module.symvers.

# Generate the kbuild Makefile from the baseline Makefile.in, doing exactly the
# @...@ substitution that CMake's configure_file() would do.  This keeps the
# objs list, CONFIG gates and the symbol-probe logic in a SINGLE source of truth
# (Makefile.in); the script only fills in the three build-specific paths.
MAKEFILE_IN="$SRC/Makefile.in"
[[ -f "$MAKEFILE_IN" ]] || { echo "baseline Makefile.in not found: $MAKEFILE_IN" >&2; exit 1; }
sed -e "s#@module_root@#$BUILD_DIR#g" \
    -e "s#@module_output@#$BUILD_DIR#g" \
    -e "s#@module_ccflags@#-I$LIBNVM_INC -I$NV_INC#g" \
    "$MAKEFILE_IN" > "$BUILD_DIR/Makefile"

echo "=== compiling ==="
make -C "$KERNEL_SRC" M="$BUILD_DIR" modules 2>&1 | tail -20

echo "=== result ==="
ls -l "$BUILD_DIR"/snvme-core.ko "$BUILD_DIR"/snvme.ko
echo "srcversion(snvme)     = $(modinfo -F srcversion "$BUILD_DIR/snvme.ko" 2>/dev/null)"
echo "srcversion(snvme-core)= $(modinfo -F srcversion "$BUILD_DIR/snvme-core.ko" 2>/dev/null)"
echo "OK: built from repo baseline $BASELINE"
