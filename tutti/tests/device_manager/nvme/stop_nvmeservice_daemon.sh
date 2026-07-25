#!/usr/bin/env bash
# stop_nvmeservice_daemon.sh -- standalone teardown for the Layer 2 NVMe
# real-hardware test daemon.
#
# This is exactly run_real_hw_test.sh's teardown() function (and its
# --teardown-only mode), extracted into its own script so it can be run
# on its own without going through the full test driver:
#
#   1. If a daemon pidfile exists and the pid is alive, SIGINT it (lets it
#      run its own chrdev/controller teardown), wait up to 10s for it to
#      exit, then remove the pidfile.
#   2. If the target device is still bound to the snvme driver, unbind it.
#   3. Rebind the device to the stock nvme driver via
#      scripts/bind_nvme_device.sh (warns, does not hard-fail, if that
#      reports an issue).
#
# Safe / idempotent to run when the daemon is already gone or the device is
# already back on stock nvme -- the same guards as run_real_hw_test.sh apply.
#
# Usage:
#   ./stop_nvmeservice_daemon.sh
#   ./stop_nvmeservice_daemon.sh --pci 0000:b1:00.0 --passwd-file ~/.passwd/1
#   PCI_ADDR=0000:b1:00.0 ./stop_nvmeservice_daemon.sh
#
# Options:
#   --passwd-file <f>     sudo password file (default: ~/.passwd/1; empty = interactive)
#   --pci <bdf>           target PCI addr (default: 0000:b1:00.0 or $PCI_ADDR)
#   -h, --help            show this help

set -uo pipefail

# ── locate repo ────────────────────────────────────────────────────────────
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"          # tutti/tests/device_manager/nvme -> repo root

# ── defaults ───────────────────────────────────────────────────────────────
PASSWD_FILE="$HOME/.passwd/1"
PCI="${PCI_ADDR:-0000:b1:00.0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --passwd-file)   PASSWD_FILE="$2"; shift 2 ;;
        --pci)           PCI="$2"; shift 2 ;;
        -h|--help)       sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# The daemon is stopped via its fixed pidfile (not a build-dir path), so this
# script needs no BUILD_DIR / daemon-binary location.
DAEMON_PIDFILE="/tmp/nvmeservice_daemon_b1.pid"

# ── sudo wrapper (non-interactive when a password file is given) ────────────
SUDO() {
    if [[ -n "$PASSWD_FILE" && -r "$PASSWD_FILE" ]]; then
        sudo -S -p '' "$@" < "$PASSWD_FILE"
    else
        sudo "$@"
    fi
}

# ── teardown: stop daemon, restore stock nvme ──────────────────────────────
teardown() {
    echo "=== teardown: stopping daemon + restoring stock nvme on $PCI ==="
    if [[ -f "$DAEMON_PIDFILE" ]]; then
        local pid; pid="$(cat "$DAEMON_PIDFILE" 2>/dev/null || true)"
        if [[ -n "$pid" ]] && SUDO kill -0 "$pid" 2>/dev/null; then
            echo "  SIGINT daemon pid=$pid (lets it run its own chrdev/controller teardown)"
            SUDO kill -INT "$pid" 2>/dev/null || true
            for _ in $(seq 1 10); do SUDO kill -0 "$pid" 2>/dev/null || break; sleep 1; done
        fi
        SUDO rm -f "$DAEMON_PIDFILE" 2>/dev/null || rm -f "$DAEMON_PIDFILE" 2>/dev/null || true
    fi
    # Unbind from snvme (if still bound) and hand back to stock nvme.
    if [[ -L "/sys/bus/pci/drivers/snvme/$PCI" ]]; then
        echo "  unbinding $PCI from snvme"
        SUDO bash -c "echo -n '$PCI' > /sys/bus/pci/drivers/snvme/unbind" 2>/dev/null || true
    fi
    echo "  rebinding $PCI to stock nvme"
    SUDO bash "$REPO/scripts/bind_nvme_device.sh" "$PCI" || \
        echo "  (bind_nvme_device.sh reported an issue; check 'lspci -k -s $PCI')"
    echo "teardown done."
}

teardown
exit 0
