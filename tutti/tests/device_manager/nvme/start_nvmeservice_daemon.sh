#!/usr/bin/env bash
# start_nvmeservice_daemon.sh -- start the NVMeService daemon (privileged bring-up).
#
# Extracted from run_real_hw_test.sh: this is the "start" half of what that
# script does BETWEEN the module reload and the GoogleTest run.  It brings up
# the daemon as the exclusive B3 owner of the target NVMe device (default
# 0000:b1:00.0) and waits for it to report ready, then exits -- leaving the
# daemon running in the background for a test/client to use.
#
# WARNING (destructive-adjacent): once ready, the daemon unbinds the target
# device from the in-tree nvme driver and takes exclusive ownership of it.
#
# Privilege model: the daemon needs root (PCI ownership + mount).  This script
# runs the privileged steps via sudo, reading the password from a file when
# one is provided (--passwd-file, default ~/.passwd/1) so the whole flow is
# non-interactive.
#
# Ordering with the sibling scripts:
#   reload_snvme_module.sh  ->  start_nvmeservice_daemon.sh  ->  ... tests ...  ->  stop_nvmeservice_daemon.sh
# By default this script does NOT reload the snvme kernel module -- it assumes
# the operator already ran reload_snvme_module.sh (a fresh module load is what
# resets snvm_registered=0 so the daemon's first SNVM_DEVICE_BIND actually
# probes).  Pass --reload-module to have this script invoke
# reload_snvme_module.sh itself first.
#
# Usage:
#   ./start_nvmeservice_daemon.sh
#   ./start_nvmeservice_daemon.sh --reload-module
#   ./start_nvmeservice_daemon.sh --pci 0000:b1:00.0 --config /path/sys_config.b1.yaml
#   PCI_ADDR=0000:b1:00.0 ./start_nvmeservice_daemon.sh
#
# Env / options:
#   --build-dir <dir>     cmake build dir (default: repo tutti/build)
#   --config <yaml>       daemon config (default: <build-dir>/bin/sys_config.b1.yaml)
#   --passwd-file <f>     sudo password file (default: ~/.passwd/1; empty = interactive)
#   --pci <bdf>           target PCI addr (default: 0000:b1:00.0 or $PCI_ADDR)
#   --reload-module       reload the snvme kernel module (via reload_snvme_module.sh) before preflight
#   -h, --help             show this help and exit
#
# On success: the daemon is left running; prints its pid + log path and how to
# stop it (stop_nvmeservice_daemon.sh), then exits 0.
# On any failure: exits non-zero.  (GoogleTest run, teardown, and the "re-run
# the client manually" epilogue are NOT part of this script -- see
# run_real_hw_test.sh / stop_nvmeservice_daemon.sh.)

set -uo pipefail

# ── locate repo ────────────────────────────────────────────────────────────
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"          # tutti/tests/device_manager/nvme -> repo root

# ── defaults ───────────────────────────────────────────────────────────────
BUILD_DIR="$REPO/tutti/build"
CONFIG=""
PASSWD_FILE="$HOME/.passwd/1"
PCI="${PCI_ADDR:-0000:b1:00.0}"
RELOAD_MODULE=0
ENDPOINT="127.0.0.1:50051"
DAEMON_LOG="/tmp/nvmeservice_daemon_b1.log"
DAEMON_PIDFILE="/tmp/nvmeservice_daemon_b1.pid"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)      BUILD_DIR="$2"; shift 2 ;;
        --config)         CONFIG="$2"; shift 2 ;;
        --passwd-file)    PASSWD_FILE="$2"; shift 2 ;;
        --pci)            PCI="$2"; shift 2 ;;
        --reload-module)  RELOAD_MODULE=1; shift ;;
        -h|--help)        sed -n '2,44p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -z "$CONFIG" ]] && CONFIG="$BUILD_DIR/bin/sys_config.b1.yaml"
DAEMON="$BUILD_DIR/device_manager/nvme/nvmeservice/examples/nvmeservice_daemon"

# ── sudo wrapper (non-interactive when a password file is given) ────────────
SUDO() {
    if [[ -n "$PASSWD_FILE" && -r "$PASSWD_FILE" ]]; then
        sudo -S -p '' "$@" < "$PASSWD_FILE"
    else
        sudo "$@"
    fi
}

# ── optional: reload the snvme kernel module fresh (composition) ───────────
# Delegates to reload_snvme_module.sh rather than reimplementing the reload
# here.  Default is NOT to reload -- assumes the operator already ran
# reload_snvme_module.sh (see header comment on ordering).
if [[ "$RELOAD_MODULE" == "1" ]]; then
    "$HERE/reload_snvme_module.sh" --passwd-file "$PASSWD_FILE" || {
        echo "module reload failed; aborting." >&2
        exit 1
    }
fi

# ── preflight ──────────────────────────────────────────────────────────────
echo "=== preflight ==="
fail=0
[[ -x "$DAEMON" ]]  || { echo "  MISSING daemon:  $DAEMON"; fail=1; }
[[ -f "$CONFIG" ]]  || { echo "  MISSING config:  $CONFIG"; fail=1; }
[[ -e "/dev/snvm_control" ]] || { echo "  MISSING /dev/snvm_control (snvme module not loaded)"; fail=1; }
[[ -d "/sys/bus/pci/devices/$PCI" ]] || { echo "  MISSING pci device $PCI"; fail=1; }
if [[ "$fail" == "1" ]]; then
    echo "preflight failed; build the targets and load the snvme module first." >&2
    exit 1
fi
echo "  daemon:  $DAEMON"
echo "  config:  $CONFIG"
echo "  pci:     $PCI"

# ── prime sudo (validates password file once, up front) ─────────────────────
if ! SUDO true; then
    echo "sudo failed -- check --passwd-file ($PASSWD_FILE) or run interactively." >&2
    exit 1
fi

# ── device driver preflight: clear driver_override, leave device on nvme ─────
# THE key precondition (root-caused against dmesg): the target PCI device must
# NOT have a driver_override pinning it to "nvme".  When
# /sys/bus/pci/devices/<bdf>/driver_override == "nvme", the PCI core's
# driver_match_device() rejects EVERY other driver -- so snvme never even
# reaches nvme_probe(), and SNVM_DEVICE_BIND fails with -ENODEV (errno 19),
# with NO probe line in dmesg.  Clearing the override (write an empty string)
# restores normal class-match behaviour.
#
# With the override cleared and a FRESH module (snvm_registered=0), the daemon's
# SNVM_DEVICE_BIND -> snvm_rebind_driver() releases stock nvme and binds snvme
# (dmesg: "disable device for bind new driver" -> "start to bind" ->
# "nvme_remap_bar" -> "snvme snvmeN: pci function ...").  The device may stay on
# stock nvme going in; the ioctl detaches it itself -- we do NOT pre-unbind.
echo "=== clearing driver_override on $PCI (was the -ENODEV bind blocker) ==="
ovr="$(cat "/sys/bus/pci/devices/$PCI/driver_override" 2>/dev/null || echo '(null)')"
echo "  driver_override before: '${ovr:-(empty)}'"
if [[ -n "$ovr" && "$ovr" != "(null)" ]]; then
    SUDO bash -c "echo '' > /sys/bus/pci/devices/$PCI/driver_override" || {
        echo "  failed to clear driver_override" >&2; exit 1; }
    echo "  driver_override after:  '$(cat "/sys/bus/pci/devices/$PCI/driver_override" 2>/dev/null || echo '(null)')'"
fi
# Ensure the device is claimable by stock nvme going in (proven baseline).
# NB: read the driver symlink directly -- `readlink -f` on a missing symlink
# prints the literal "driver", so guard on symlink existence and report "none".
if [[ -L "/sys/bus/pci/devices/$PCI/driver" ]]; then
    cur_drv="$(basename "$(readlink "/sys/bus/pci/devices/$PCI/driver")")"
else
    cur_drv="none"
fi
echo "=== current driver for $PCI: $cur_drv (daemon SNVM_DEVICE_BIND will claim it) ==="
if [[ "$cur_drv" == "none" ]]; then
    echo "  binding $PCI to stock nvme first (baseline for the daemon rebind)"
    SUDO bash "$REPO/scripts/bind_nvme_device.sh" "$PCI" >/dev/null 2>&1 || \
        echo "  (bind to nvme reported an issue; SNVM_DEVICE_BIND may still succeed)"
    sleep 1
fi

# ── start the daemon (background), wait for the listening marker ────────────
# NOTE: the daemon runs as root and writes the log/pidfile.  We create the log
# via sudo so it is root-owned -- otherwise fs.protected_regular (sysctl) blocks
# root from appending to a zfw-owned file in the sticky /tmp dir.
echo "=== starting NVMeService daemon (owner of $PCI) ==="
SUDO bash -c ": > '$DAEMON_LOG'"
SUDO bash -c "'$DAEMON' --config '$CONFIG' >> '$DAEMON_LOG' 2>&1 & echo \$! > '$DAEMON_PIDFILE'"
sleep 1
DPID="$(cat "$DAEMON_PIDFILE" 2>/dev/null || true)"
echo "  daemon pid=$DPID  log=$DAEMON_LOG"

ready=0
for _ in $(seq 1 30); do
    if grep -q "NVMeService daemon listening" "$DAEMON_LOG" 2>/dev/null; then ready=1; break; fi
    # Bail early if the daemon already died.
    if [[ -n "$DPID" ]] && ! SUDO kill -0 "$DPID" 2>/dev/null; then break; fi
    sleep 1
done
if [[ "$ready" != "1" ]]; then
    echo "daemon did not become ready; last log lines:" >&2
    tail -30 "$DAEMON_LOG" >&2
    exit 1
fi
echo "  daemon ready."
grep -E "device_id=|listening" "$DAEMON_LOG" | sed 's/^/    /'

echo
echo "Daemon started (pid=$DPID, log=$DAEMON_LOG)."
echo "Endpoint: $ENDPOINT"
echo "To stop the daemon and restore stock nvme:"
echo "  $HERE/stop_nvmeservice_daemon.sh --pci $PCI --passwd-file '$PASSWD_FILE'"

exit 0
