#!/usr/bin/env bash
# Reboot the KagigataDisk into its UF2 bootloader: delete
# command/update-firmware on its USB drive and flush, which the firmware takes
# as the request (it reboots 1 s later). The drive is found by USB VID:PID and
# mounted through udisks if nothing has mounted it yet.
#   ./tools/boot0.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

VID="$KAGIGATA_VID"
PID="$KAGIGATA_PID"

dev="$(find_disk "$VID" "$PID")" || { echo "no $VID:$PID drive found" >&2; exit 1; }
mnt="$(mount_disk "$dev")" ||
  { echo "could not mount $dev (run tools/install.sh?)" >&2; exit 1; }

rm -f "$mnt/command/update-firmware"
sync
# The board reboots 1 s after the deletion lands; unmount before it vanishes.
udisksctl unmount -b "$dev" --no-user-interaction >/dev/null 2>&1 || true
echo "deleted command/update-firmware on $dev ($mnt)"
