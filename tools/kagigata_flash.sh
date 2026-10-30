#!/usr/bin/env bash
# Flash a KagigataDisk example firmware over UF2 without sudo, from whatever
# runs on the board now (example/001's drive, example/002's FIDO key, or the
# bootloader):
#   boot0 -> wait for the bootloader drive -> mount it with udisks -> copy the
#   .uf2 -> wait for the new firmware's USB device to return.
# Over a running FIDO key (002): hold a finger on its card contacts for 3 s
# (all LEDs lit); it erases every credential and the PIN first.
# Needs tools/install.sh to have been run once (not for a local desktop
# session, where udisks and uaccess already allow it).
#   ./tools/kagigata_flash.sh                  # 001 (USB drive), last build
#   ./tools/kagigata_flash.sh 002 --build      # 002 (FIDO2 key), build first
#   ./tools/kagigata_flash.sh 002 path/to.uf2  # flash this image as 002
#   ./tools/kagigata_flash.sh 002 -DKGFIDO_AAGUID=...  # -D...: build with these
#                                              # cmake variables first (kept)
set -euo pipefail
here="$(dirname "$0")"
source "$here/env.sh"

TARGET=001
if [[ "${1:-}" =~ ^[0-9]{3}$ ]]; then
  TARGET="$1"
  shift
fi

# What each example builds and how it shows up once running.
case "$TARGET" in
  001)
    UF2="$KAGIGATA_BUILD/kagigatadisk.uf2"
    find_target() { find_disk "$KAGIGATA_VID" "$KAGIGATA_PID"; }
    WHAT="the firmware's drive"
    ;;
  002)
    UF2="$KGFIDO_BUILD/kagigatadisk_fido.uf2"
    find_target() { find_hidraw "$KGFIDO_VID" "$KGFIDO_PID"; }
    WHAT="the FIDO key"
    ;;
  *) echo "unknown example: $TARGET (001 or 002)" >&2; exit 1 ;;
esac
TIMEOUT="${FLASH_TIMEOUT:-15}"

build=false
build_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --build) build=true ;;
    -D*) build=true; build_args+=("$1") ;;
    *) UF2="$1" ;;
  esac
  shift
done
if $build; then
  "$here/kagigata_build.sh" "$TARGET" "${build_args[@]}"
fi
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
  # With neither firmware attached the board is (most likely) on its way into
  # the bootloader already, e.g. right after `kgfido.py bootsel`: just wait.
  if find_disk "$KAGIGATA_VID" "$KAGIGATA_PID" >/dev/null ||
     find_hidraw "$KGFIDO_VID" "$KGFIDO_PID" >/dev/null; then
    "$here/boot0.sh"
  fi
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

wait_for "$WHAT" find_target >/dev/null
echo "flashed $TARGET; back as $(find_target)"
