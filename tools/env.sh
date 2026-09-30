#!/usr/bin/env bash
# Shared settings for the tools/ scripts. Everything is overridable from the
# environment, e.g.
#   KAGIGATA_BUILD=/tmp/kgd ./tools/kagigata_build.sh

WS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export WS_ROOT

# --- KagigataDisk (RP2354A device side, pico-sdk) -------------------------
export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico/pico-sdk}"
export KAGIGATA_DIR="${KAGIGATA_DIR:-$WS_ROOT/example/001/kagigatadisk}"
export KAGIGATA_BUILD="${KAGIGATA_BUILD:-$KAGIGATA_DIR/build}"
# USB mass-storage example VID:PID
export KAGIGATA_VID="${KAGIGATA_VID:-cafe}"
export KAGIGATA_PID="${KAGIGATA_PID:-4002}"

# --- M5Stack Core2 (ESP32 host side, PlatformIO) --------------------------
export CORE2_DIR="${CORE2_DIR:-$WS_ROOT/example/001/host}"
export CORE2_ENV="${CORE2_ENV:-core2}"
# On-board CP2104 UART bridge.
export CORE2_VID="${CORE2_VID:-10c4}"
export CORE2_PID="${CORE2_PID:-ea60}"
export PIO="${PIO:-pio}"

# Print the /dev path of the first tty whose USB device matches VID:PID
# (lower-case hex, e.g. find_tty 10c4 ea60 for the Core2). Returns 1 when
# none is attached.
find_tty() {
  local vid="$1" pid="$2" t dev
  for t in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
    [ -e "$t/device" ] || continue
    # Walk up to the USB device node; ttyUSB sits deeper than ttyACM.
    dev="$(readlink -f "$t/device")"
    while [ "$dev" != "/" ] && [ ! -e "$dev/idVendor" ]; do
      dev="$(dirname "$dev")"
    done
    if [ "$(cat "$dev/idVendor" 2>/dev/null)" = "$vid" ] &&
       [ "$(cat "$dev/idProduct" 2>/dev/null)" = "$pid" ]; then
      echo "/dev/$(basename "$t")"
      return 0
    fi
  done
  return 1
}

# Print the /dev path of the first whole disk whose USB device matches VID:PID
# (e.g. find_disk cafe 4002). Returns 1 when none is attached.
find_disk() {
  local vid="$1" pid="$2" b dev
  for b in /sys/block/*; do
    [ -e "$b/device" ] || continue
    dev="$(readlink -f "$b/device")"
    while [ "$dev" != "/" ] && [ ! -e "$dev/idVendor" ]; do
      dev="$(dirname "$dev")"
    done
    if [ "$(cat "$dev/idVendor" 2>/dev/null)" = "$vid" ] &&
       [ "$(cat "$dev/idProduct" 2>/dev/null)" = "$pid" ]; then
      echo "/dev/$(basename "$b")"
      return 0
    fi
  done
  return 1
}

# Print where block device $1 is mounted, mounting it through udisks first if
# nothing (e.g. the desktop) has. Returns 1 if it cannot be mounted.
mount_disk() {
  local dev="$1" mnt
  mnt="$(findmnt -no TARGET "$dev" | head -1 || true)"
  if [ -z "$mnt" ]; then
    udisksctl mount -b "$dev" --no-user-interaction >/dev/null 2>&1 || true
    mnt="$(findmnt -no TARGET "$dev" | head -1 || true)"
  fi
  [ -n "$mnt" ] && echo "$mnt"
}
