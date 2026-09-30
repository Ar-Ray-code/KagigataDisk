#!/usr/bin/env bash
# protocol/emu_protocol_defs.h is the one definition of the Kagigata protocol.
# The host library carries a copy so it builds on its own (e.g. `pio ci`, or
# copied into another project); both builds refuse to run while the copy
# differs. After editing the original, run:
#   ./tools/sync_protocol.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cp "$root/protocol/emu_protocol_defs.h" "$root/example/001/host/lib/emu_storage/src/emu_protocol_defs.h"
echo "copied protocol/emu_protocol_defs.h into lib/emu_storage/src/"
