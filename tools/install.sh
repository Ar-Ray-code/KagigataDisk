#!/usr/bin/env bash
# One-time setup so tools/kagigata_flash.sh (boot0 -> UF2 copy) runs without
# sudo. Installs a udev rule and a polkit rule and makes sure the invoking
# user is in plugdev. Both rules are persistent files, so they keep
# working across re-plugs and reboots. Safe to re-run.
#   ./tools/install.sh              # asks for sudo once
#   ./tools/install.sh --uninstall
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

UDEV_RULE=/etc/udev/rules.d/60-kagigatadisk.rules
POLKIT_RULE=/etc/polkit-1/rules.d/50-kagigatadisk.rules
TARGET_USER="${SUDO_USER:-$USER}"

if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

reload_udev() {
  udevadm control --reload-rules
  udevadm trigger --subsystem-match=block --subsystem-match=usb
}

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$UDEV_RULE" "$POLKIT_RULE"
  reload_udev
  echo "removed $UDEV_RULE and $POLKIT_RULE"
  exit 0
fi

install -D -m 0644 "$here/system/60-kagigatadisk.rules" "$UDEV_RULE"
# polkitd watches rules.d and picks the new file up without a restart.
install -D -m 0644 "$here/system/50-kagigatadisk.rules" "$POLKIT_RULE"
reload_udev

relogin=0
for g in plugdev; do
  if ! id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$g"; then
    usermod -aG "$g" "$TARGET_USER"
    relogin=1
  fi
done

echo "installed $UDEV_RULE"
echo "installed $POLKIT_RULE"
if [ "$relogin" -eq 1 ]; then
  echo "added $TARGET_USER to plugdev: log out and back in once" \
       "(a local desktop session already has access via uaccess)."
fi
