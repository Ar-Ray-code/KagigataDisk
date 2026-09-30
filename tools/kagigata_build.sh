#!/usr/bin/env bash
# Build the KagigataDisk firmware (example/001/kagigatadisk) with pico-sdk.
#   ./tools/kagigata_build.sh                     # configure (if needed) + build
#   ./tools/kagigata_build.sh --clean             # wipe the build dir first
#   ./tools/kagigata_build.sh -DPICO_BOARD=pico2_w  # extra args go to cmake
# Outputs: $KAGIGATA_BUILD/kagigatadisk.{elf,uf2,bin,hex}
set -euo pipefail
source "$(dirname "$0")/env.sh"

SRC="$KAGIGATA_DIR"
BUILD="$KAGIGATA_BUILD"
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
echo "built: $BUILD/kagigatadisk.uf2"
