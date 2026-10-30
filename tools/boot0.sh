#!/usr/bin/env bash
# Reboot the KagigataDisk into its UF2 bootloader, whichever firmware runs:
#   example/001 (USB drive): delete command/update-firmware on the drive and
#     flush, which the firmware takes as the request (it reboots 1 s later).
#     The drive is found by USB VID:PID and mounted through udisks if nothing
#     has mounted it yet.
#   example/002 (FIDO2 key): the KG_BOOTSEL vendor command (tools/kgfido.py).
#     The key erases its credentials first, after a finger is held on its
#     card contacts for 3 s (all LEDs lit).
#   ./tools/boot0.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

VID="$KAGIGATA_VID"
PID="$KAGIGATA_PID"

if dev="$(find_disk "$VID" "$PID")"; then
  mnt="$(mount_disk "$dev")" ||
    { echo "could not mount $dev (run tools/install.sh?)" >&2; exit 1; }

  rm -f "$mnt/command/update-firmware"
  sync
  # The board reboots 1 s after the deletion lands; unmount before it vanishes.
  udisksctl unmount -b "$dev" --no-user-interaction >/dev/null 2>&1 || true
  echo "deleted command/update-firmware on $dev ($mnt)"
elif hid="$(find_hidraw "$KGFIDO_VID" "$KGFIDO_PID")"; then
  python3 "$WS_ROOT/tools/kgfido.py" --device "$hid" bootsel
else
  echo "no $VID:$PID drive and no $KGFIDO_VID:$KGFIDO_PID FIDO key found" >&2
  exit 1
fi
