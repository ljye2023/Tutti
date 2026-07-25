#!/usr/bin/env bash
# reload_snvme_module.sh -- fresh (re)load of the snvme kernel module.
#
# Reloads the snvme kernel module FRESH: rmmod snvme snvme_core; insmod
# snvme-core.ko then snvme.ko (with io_queue_depth=<n>), using the .ko files
# produced by build_snvme_module.sh (default output dir:
# <repo>/tutti/build_snvme_module -- see build_snvme_module.sh's BUILD_DIR).
#
# WHY a fresh reload is needed: a fresh (re)load resets snvm_registered=0 so
# the daemon's first SNVM_DEVICE_BIND runs a real pci_register_driver() (whose
# auto-probe binds the controller). Without this, a module that has already
# served one bind no-ops register_driver() and every subsequent bind fails
# with -ENODEV.
#
# SAFETY: this script REFUSES to reload if snvme still owns a device (bound
# in /sys/bus/pci/drivers/snvme/), has fd holders on /dev/snvm*, or has a
# mounted snvme block device -- a botched reload can wedge the host.
#
# This logic is extracted from run_real_hw_test.sh's reload_module() (and its
# SUDO() wrapper) so it can be run standalone -- e.g. between test runs --
# without driving the whole daemon + GoogleTest flow.
#
# Usage:
#   ./reload_snvme_module.sh
#   ./reload_snvme_module.sh --ko-dir /path/to/build_snvme_module
#   ./reload_snvme_module.sh --io-queue-depth 32
#   ./reload_snvme_module.sh --passwd-file ""   # interactive sudo
#
# Env / options:
#   --ko-dir <dir>          override module dir (default: <repo>/tutti/build_snvme_module)
#   --passwd-file <f>       sudo password file (default: ~/.passwd/1; empty = interactive)
#   --io-queue-depth <n>    snvme io_queue_depth insmod param (default: 64)
#   -h, --help              show this help and exit

set -uo pipefail

# ── locate repo ────────────────────────────────────────────────────────────
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"          # tutti/tests/device_manager/nvme -> repo root

# ── defaults ───────────────────────────────────────────────────────────────
KO_DIR="${SNVME_KO_DIR:-$REPO/tutti/build_snvme_module}"
PASSWD_FILE="$HOME/.passwd/1"
IO_QUEUE_DEPTH="${SNVME_IO_QUEUE_DEPTH:-64}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ko-dir)           KO_DIR="$2"; shift 2 ;;
        --passwd-file)      PASSWD_FILE="$2"; shift 2 ;;
        --io-queue-depth)   IO_QUEUE_DEPTH="$2"; shift 2 ;;
        -h|--help)          sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

SNVME_CORE_KO="${SNVME_CORE_KO:-$KO_DIR/snvme-core.ko}"
SNVME_KO="${SNVME_KO:-$KO_DIR/snvme.ko}"

# ── sudo wrapper (non-interactive when a password file is given) ────────────
SUDO() {
    if [[ -n "$PASSWD_FILE" && -r "$PASSWD_FILE" ]]; then
        sudo -S -p '' "$@" < "$PASSWD_FILE"
    else
        sudo "$@"
    fi
}

# ── reload the snvme kernel module fresh ────────────────────────────────────
# A fresh (re)load resets snvm_registered=0 so the daemon's first
# SNVM_DEVICE_BIND runs a real pci_register_driver() (whose auto-probe binds
# the controller).  Without this, a module that has already served one bind
# no-ops register_driver() and every subsequent bind fails with -ENODEV.
# Safe only when snvme owns no devices, has no fd holders, and no snvme mount
# (checked here; refuse otherwise -- a botched reload can wedge the host).
reload_module() {
    echo "=== reloading snvme kernel module (fresh registration) ==="
    if [[ ! -r "$SNVME_CORE_KO" || ! -r "$SNVME_KO" ]]; then
        echo "  MISSING module .ko: $SNVME_CORE_KO / $SNVME_KO" >&2
        echo "  pass --ko-dir <dir> or rebuild via build_snvme_module.sh." >&2
        return 1
    fi
    # Safety gates before rmmod.
    local bound holders mounted
    bound="$(find /sys/bus/pci/drivers/snvme/ -maxdepth 1 -type l -name '????:??:??.?' 2>/dev/null || true)"
    if [[ -n "$bound" ]]; then
        echo "  refusing reload: devices still bound to snvme:" >&2
        echo "$bound" | sed 's/^/    /' >&2
        return 1
    fi
    holders="$(SUDO lsof /dev/snvm_control /dev/ssnvme* 2>/dev/null | tail -n +2 || true)"
    if [[ -n "$holders" ]]; then
        echo "  refusing reload: fd holders on /dev/snvm*:" >&2
        echo "$holders" | sed 's/^/    /' >&2
        return 1
    fi
    mounted="$(mount | grep -E '^/dev/snvme[0-9]+n[0-9]+ ' || true)"
    if [[ -n "$mounted" ]]; then
        echo "  refusing reload: snvme block device mounted:" >&2
        echo "$mounted" | sed 's/^/    /' >&2
        return 1
    fi
    # Unload both (order matters: snvme depends on snvme_core).  rmmod only
    # when actually loaded; verify each is gone before proceeding so a stale
    # module never collides with insmod ("File exists").
    #
    # IMPORTANT: grep the captured `lsmod` output via a here-string, never
    # `lsmod | grep -q`.  Under `set -o pipefail`, grep -q exits on first match
    # and SIGPIPEs lsmod (exit 141), which pipefail then reports as the
    # pipeline's status -- so `if lsmod | grep -q ...` reads as FALSE even when
    # the module IS loaded, silently skipping the rmmod and breaking the reload.
    local lsm
    lsm="$(lsmod)"
    if grep -qE '^snvme[[:space:]]' <<<"$lsm"; then
        SUDO rmmod snvme      || { echo "  rmmod snvme failed" >&2; return 1; }
    fi
    lsm="$(lsmod)"
    if grep -qE '^snvme_core[[:space:]]' <<<"$lsm"; then
        SUDO rmmod snvme_core || { echo "  rmmod snvme_core failed" >&2; return 1; }
    fi
    lsm="$(lsmod)"
    if grep -qE '^snvme(_core)?[[:space:]]' <<<"$lsm"; then
        echo "  refusing insmod: an snvme module is still loaded after rmmod:" >&2
        grep -E '^snvme' <<<"$lsm" | sed 's/^/    /' >&2
        return 1
    fi
    SUDO insmod "$SNVME_CORE_KO"                          || { echo "  insmod snvme_core failed" >&2; return 1; }
    SUDO insmod "$SNVME_KO" io_queue_depth="$IO_QUEUE_DEPTH" || { echo "  insmod snvme failed" >&2; return 1; }
    [[ -e /dev/snvm_control ]] || { echo "  /dev/snvm_control absent after reload" >&2; return 1; }
    echo "  module reloaded ($SNVME_KO); /dev/snvm_control present."
}

reload_module
exit $?
