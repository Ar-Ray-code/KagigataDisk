# PlatformIO pre-build script: stops the build when lib/emu_storage's copy of
# the protocol definitions differs from protocol/emu_protocol_defs.h (the one
# the firmware uses). tools/sync_protocol.sh brings the copy up to date.
import filecmp
import os
import sys

Import("env")  # noqa: F821 - provided by PlatformIO

project = env.subst("$PROJECT_DIR")  # noqa: F821
original = os.path.normpath(os.path.join(project, "..", "..", "..", "protocol", "emu_protocol_defs.h"))
copy = os.path.join(project, "lib", "emu_storage", "src", "emu_protocol_defs.h")
if os.path.exists(original) and not filecmp.cmp(original, copy, shallow=False):
    sys.stderr.write("%s differs from %s: run tools/sync_protocol.sh\n" % (copy, original))
    env.Exit(1)  # noqa: F821
