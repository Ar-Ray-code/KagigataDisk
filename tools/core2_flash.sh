#!/usr/bin/env bash
# Build example/001/host for the M5Stack Core2 and flash it through the
# Core2's CP2104 UART bridge. The port is found by USB VID:PID unless given:
#   ./tools/core2_flash.sh                 # auto-detect CORE2_VID:CORE2_PID
#   ./tools/core2_flash.sh /dev/ttyUSB0    # use this port
#   ./tools/core2_flash.sh --build-only    # build, skip the upload
set -euo pipefail
source "$(dirname "$0")/env.sh"

PROJ="$CORE2_DIR"
ENV="$CORE2_ENV"
VID="$CORE2_VID"
PID="$CORE2_PID"

if [ "${1:-}" = "--build-only" ]; then
  exec "$PIO" run -d "$PROJ" -e "$ENV"
fi

port="${1:-}"
if [ -z "$port" ]; then
  port="$(find_tty "$VID" "$PID")" || { echo "no $VID:$PID tty found" >&2; exit 1; }
fi

echo "== building $ENV and flashing via $port ($VID:$PID) =="
"$PIO" run -d "$PROJ" -e "$ENV" -t upload --upload-port "$port"
