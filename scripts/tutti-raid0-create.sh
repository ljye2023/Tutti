#!/usr/bin/env bash
# One-time creation of an md RAID0 over Tutti-owned NVMe namespaces.
#
# DESTROYS all data on the member devices. Run while tutti_daemon is up (the
# /dev/snvme*n1 block devices exist only after bring-up) and before it is
# configured with raid0_arrays -- or with the array configured: the daemon
# then just warns that it cannot assemble yet. Restart the daemon afterwards;
# it assembles and mounts the array itself from then on.
#
#   scripts/tutti-raid0-create.sh NAME MOUNT_PATH MEMBER... [-- CHUNK_KIB]
#   scripts/tutti-raid0-create.sh tutti0 /mnt/tutti_md0 /dev/snvme{0,1,2,3}n1
#
# ext4 is created directly on the md device (no partition) with stride /
# stripe_width hints, so large files are allocated chunk-aligned -- the
# object store relies on that to rotate slots across members.
set -euo pipefail

if [[ $# -lt 4 ]]; then
    sed -n '2,15p' "$0"
    exit 2
fi
NAME=$1; MOUNT=$2; shift 2
CHUNK_KIB=${CHUNK_KIB:-128}   # = 段起点对齐（槽位前缀 128 KiB），IO 不跨条带
MEMBERS=("$@")

for dev in "${MEMBERS[@]}"; do
    [[ -b $dev ]] || { echo "not a block device: $dev" >&2; exit 1; }
    if findmnt -rn -S "$dev" >/dev/null; then
        echo "$dev is mounted; refusing" >&2; exit 1
    fi
done

echo "About to DESTROY all data on: ${MEMBERS[*]}"
read -r -p "Type the array name ($NAME) to continue: " answer
[[ $answer == "$NAME" ]] || { echo "aborted"; exit 1; }

wipefs -a "${MEMBERS[@]}"
mdadm --create "/dev/md/$NAME" --level=0 --raid-devices=${#MEMBERS[@]} \
      --chunk="$CHUNK_KIB" --metadata=1.2 --name="$NAME" --run "${MEMBERS[@]}"
udevadm settle --timeout=10

STRIDE=$((CHUNK_KIB / 4))                      # 4 KiB filesystem blocks
mkfs.ext4 -F -q -T largefile4 -L "$NAME" \
    -E stride=$STRIDE,stripe_width=$((STRIDE * ${#MEMBERS[@]})),lazy_itable_init=0,lazy_journal_init=0 \
    "/dev/md/$NAME"

# Leave the array stopped: the daemon assembles and mounts it on its next start.
mdadm --stop "/dev/md/$NAME"
echo "created /dev/md/$NAME; restart tutti_daemon with raid0_arrays mounting it at $MOUNT"
