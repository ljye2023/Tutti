#!/usr/bin/env bash
# Tutti unified build script
#
# Builds the entire Tutti project - business/service binaries AND test
# binaries - into a single build directory: tutti/build
#
# BUILD_TESTING=ON is passed to CMake so test targets are configured, and
# the build step builds ALL targets (no --target is passed to
# `cmake --build`), so business binaries and test binaries land side by
# side in the same build directory.
#
# Safe to re-run: builds are incremental. Use --reconfigure to force a
# fresh CMake configure step (existing CMakeCache.txt is reused otherwise).
#
# Options:
#   --reconfigure       Force a fresh CMake configure step
#   --build-type <t>    CMake build type (default: RelWithDebInfo)
#   --cuda-arch <n>     CUDA architecture (default: 89)
#   -j <N>              Parallel build jobs (default: nproc)
#   -h, --help          Show this help
#
# Environment:
#   TUTTI_ACCELERATOR   Accelerator profile: CUDA (default) or HOST
#
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/tutti (cmake source dir)
REPO="$(cd "$HERE/.." && pwd)"                                # <repo>
BUILD_DIR="$HERE/build"                                       # <repo>/tutti/build (single output dir)

BUILD_TYPE="RelWithDebInfo"
CUDA_ARCH="89"
JOBS="$(nproc)"
RECONFIGURE=0
ACCELERATOR="${TUTTI_ACCELERATOR:-CUDA}"
ACCELERATOR="${ACCELERATOR^^}"
if [[ "$ACCELERATOR" != "CUDA" && "$ACCELERATOR" != "HOST" ]]; then
  echo "ERROR: TUTTI_ACCELERATOR='$ACCELERATOR' is not supported. Supported: CUDA, HOST" >&2
  exit 2
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reconfigure)
      RECONFIGURE=1
      shift
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --cuda-arch)
      CUDA_ARCH="$2"
      shift 2
      ;;
    -j)
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,20p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

# A fresh configure is REQUIRED when the cache was not created with the vcpkg
# toolchain: CMAKE_TOOLCHAIN_FILE is only honored on the FIRST configure of a
# build dir, so re-running cmake over a stale cache silently keeps gRPC/protobuf
# unfound (=> NVMeService daemon/client get gated out). --reconfigure therefore
# removes the existing cache so the toolchain is applied from scratch.
if [[ "$RECONFIGURE" -eq 1 && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "Removing existing CMake cache in $BUILD_DIR for a fresh configure"
  rm -f "$BUILD_DIR/CMakeCache.txt"
  rm -rf "$BUILD_DIR/CMakeFiles"
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "Configuring Tutti (build type=$BUILD_TYPE, CUDA arch=$CUDA_ARCH, accelerator=$ACCELERATOR) in $BUILD_DIR"
  cmake -S "$HERE" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/third_pkgs/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=ON -DCMAKE_CUDA_ARCHITECTURES="$CUDA_ARCH" \
    -DTUTTI_ACCELERATOR="$ACCELERATOR"
else
  echo "Reusing existing CMake configuration in $BUILD_DIR (use --reconfigure to force)"
fi

cmake --build "$BUILD_DIR" -j "$JOBS"

echo "Build complete. Binaries in $BUILD_DIR (bin/ and per-component dirs)."
