#!/usr/bin/env bash
# Flash the KagigataDisk firmware over UF2 without sudo:
#   boot0 (delete command/update-firmware on the drive) -> wait for the
#   bootloader drive -> mount it with udisks -> copy the .uf2 -> wait for the
#   firmware's drive to return.
# Needs tools/install.sh to have been run once (not for a local desktop
# session, where udisks and uaccess already allow it).
#   ./tools/kagigata_flash.sh              # flash the last build
#   ./tools/kagigata_flash.sh --build      # run kagigata_build.sh first
#   ./tools/kagigata_flash.sh path/to.uf2  # flash this image
set -euo pipefail
here="$(dirname "$0")"
source "$here/env.sh"

UF2="$KAGIGATA_BUILD/kagigatadisk.uf2"
VID="$KAGIGATA_VID"
PID="$KAGIGATA_PID"
TIMEOUT="${FLASH_TIMEOUT:-15}"

if [ "${1:-}" = "--build" ]; then
  shift
  "$here/kagigata_build.sh"
fi
[ $# -ge 1 ] && UF2="$1"
[ -f "$UF2" ] || { echo "no image: $UF2 (build first)" >&2; exit 1; }

# The bootloader drive's partition, identified by its volume label.
find_boot_drive() {
  local d
  d="$(lsblk -rpno NAME,LABEL | awk '$2 == "RP2350" || $2 == "RPI-RP2" { print $1; exit }')"
  [ -n "$d" ] && echo "$d"
}

wait_for() {  # wait_for <description> <command...>
  local what="$1" i
  shift
  for ((i = 0; i < TIMEOUT * 5; i++)); do
    "$@" && return 0
    sleep 0.2
  done
  echo "timed out waiting for $what" >&2
  return 1
}

dev="$(find_boot_drive || true)"
if [ -z "$dev" ]; then
  "$here/boot0.sh"
  wait_for "the UF2 bootloader drive" find_boot_drive >/dev/null
  dev="$(find_boot_drive)"
fi

# A desktop session may auto-mount the drive by itself; use that mount if so,
# otherwise mount it ourselves.
sleep 0.5
mnt="$(mount_disk "$dev")" ||
  { echo "could not mount $dev (run tools/install.sh?)" >&2; exit 1; }

echo "copying $(basename "$UF2") to $mnt"
cp "$UF2" "$mnt/"
sync || true
# The bootloader reboots into the new image on its own once the copy lands,
# which takes the drive away; nothing left to unmount.

wait_for "the firmware's drive" find_disk "$VID" "$PID" >/dev/null
echo "flashed; drive back as $(find_disk "$VID" "$PID")"
