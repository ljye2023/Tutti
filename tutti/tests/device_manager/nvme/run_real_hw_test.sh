#!/usr/bin/env bash
# run_real_hw_test.sh -- run the nvme_real_hw_test GoogleTest against an
# ALREADY-RUNNING NVMeService daemon.
#
# This script does ONE job: run the gtest binary. It does NOT build binaries,
# manage the snvme kernel module, or start/stop the daemon. The gtest shells
# out to nvmeservice_client and connects over gRPC, so the daemon must already
# be running (see start_nvmeservice_daemon.sh).
#
# Sibling scripts (same directory) + typical flow:
#   tutti/build.sh                 # build binaries into tutti/build
#   reload_snvme_module.sh         # fresh snvme module (optional)
#   start_nvmeservice_daemon.sh    # bring the daemon up
#   ./run_real_hw_test.sh          # <-- THIS: run the gtest
#   stop_nvmeservice_daemon.sh     # stop daemon + restore stock nvme
#
# Tiers:
#   Tier 0  ListDevices           (non-destructive)
#   Tier 1  Connect + skip-io     (non-destructive)
#   Tier 2  GPU Write/Read/verify (DESTRUCTIVE -- writes to the namespace)
# Pass --no-destructive to skip Tier 2.
#
# Usage:
#   ./run_real_hw_test.sh
#   ./run_real_hw_test.sh --no-destructive
#   ./run_real_hw_test.sh --pci 0000:b1:00.0 --endpoint 127.0.0.1:50051
#
# Options:
#   --build-dir <dir>     cmake build dir (default: repo tutti/build)
#   --pci <bdf>           target PCI addr (default: 0000:b1:00.0 or $PCI_ADDR)
#   --endpoint <addr>     daemon gRPC endpoint (default: 127.0.0.1:50051)
#   --no-destructive      skip the Tier-2 GPU write/read/verify

set -uo pipefail

# ── locate repo ────────────────────────────────────────────────────────────
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"          # tutti/tests/device_manager/nvme -> repo root

# ── defaults ───────────────────────────────────────────────────────────────
BUILD_DIR="$REPO/tutti/build"
PCI="${PCI_ADDR:-0000:b1:00.0}"
ENDPOINT="127.0.0.1:50051"
DESTRUCTIVE=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)       BUILD_DIR="$2"; shift 2 ;;
        --pci)              PCI="$2"; shift 2 ;;
        --endpoint)         ENDPOINT="$2"; shift 2 ;;
        --no-destructive)   DESTRUCTIVE=0; shift ;;
        -h|--help)          sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

TEST_BIN="$BUILD_DIR/bin/nvme_real_hw_test"
CLIENT="$BUILD_DIR/device_manager/nvme/nvmeservice/examples/nvmeservice_client"

# ── preflight ──────────────────────────────────────────────────────────────
fail=0
[[ -x "$TEST_BIN" ]] || { echo "  MISSING test:   $TEST_BIN" >&2; fail=1; }
[[ -x "$CLIENT" ]]   || { echo "  MISSING client: $CLIENT" >&2; fail=1; }
if [[ "$fail" == "1" ]]; then
    echo "preflight failed; build the binaries first (tutti/build.sh) and start the daemon (start_nvmeservice_daemon.sh)." >&2
    exit 1
fi

# ── run the GoogleTest ──────────────────────────────────────────────────────
echo "=== running nvme_real_hw_test (pci=$PCI endpoint=$ENDPOINT destructive=$DESTRUCTIVE) ==="
export TUTTI_NVME_REAL_HW=1
export TUTTI_NVME_ENDPOINT="$ENDPOINT"
export TUTTI_NVME_PCI="$PCI"
export TUTTI_NVME_CLIENT_BIN="$CLIENT"
[[ "$DESTRUCTIVE" == "1" ]] && export TUTTI_NVME_ALLOW_DESTRUCTIVE=1

"$TEST_BIN" --gtest_color=yes
rc=$?
echo "=== nvme_real_hw_test exit rc=$rc ==="
exit $rc
