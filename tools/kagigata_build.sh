#!/usr/bin/env bash
# Build a KagigataDisk example firmware with pico-sdk:
#   001  example/001/kagigatadisk       (USB drive)  -> $KAGIGATA_BUILD/kagigatadisk.uf2
#   002  example/002/kagigatadisk_fido  (FIDO2 key)  -> $KGFIDO_BUILD/kagigatadisk_fido.uf2
# Extra arguments go to cmake and stick in the build directory's cache until
# changed or --clean:
#   ./tools/kagigata_build.sh                     # 001: configure (if needed) + build
#   ./tools/kagigata_build.sh 002 --clean         # 002: wipe the build dir first
#   ./tools/kagigata_build.sh 002 -DKGFIDO_AAGUID=$(uuidgen | tr -d -)
set -euo pipefail
source "$(dirname "$0")/env.sh"

TARGET=001
if [[ "${1:-}" =~ ^[0-9]{3}$ ]]; then
  TARGET="$1"
  shift
fi
case "$TARGET" in
  001) SRC="$KAGIGATA_DIR" BUILD="$KAGIGATA_BUILD" NAME=kagigatadisk ;;
  002) SRC="$KGFIDO_DIR" BUILD="$KGFIDO_BUILD" NAME=kagigatadisk_fido ;;
  *) echo "unknown example: $TARGET (001 or 002)" >&2; exit 1 ;;
esac
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

if [ "${1:-}" = "--clean" ]; then
  shift
  rm -rf "$BUILD"
fi

# A cache configured from another source tree (e.g. a copied build dir) makes
# cmake refuse to run, so start that one over.
cache="$BUILD/CMakeCache.txt"
if [ -f "$cache" ]; then
  home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
  if [ "$(realpath -m "$home")" != "$(realpath "$SRC")" ]; then
    echo "build cache belongs to $home; recreating $BUILD" >&2
    rm -rf "$BUILD"
  fi
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@" >/dev/null
ninja -C "$BUILD"
echo "built: $BUILD/$NAME.uf2"
