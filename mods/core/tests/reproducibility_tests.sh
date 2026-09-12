#!/bin/sh
# Two real builds, no host/fixture execution and no changes to installed mods.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TEST=$(mktemp -d /tmp/pop-core-reproducibility.XXXXXX)
trap 'rm -rf "$TEST"' EXIT HUP INT TERM
mkdir -p "$TEST/mods" "$TEST/tools/recomp" "$TEST/src/recomp/mods"
cp -R "$ROOT/mods/core" "$TEST/mods/"
cp "$ROOT/tools/recomp/buildlock.sh" "$TEST/tools/recomp/"
cp "$ROOT/src/recomp/mods/pop_mod_api.h" "$TEST/src/recomp/mods/"
DEST="$TEST/build/recomp/mods/core"
# Use the parent suite's interpreter when available, including for the lock.
BUILDLOCK_PY=${BUILDLOCK_PY:-$ROOT/.venv/bin/python}
export BUILDLOCK_PY
sh "$TEST/mods/core/build_core.sh" "$DEST"
[ -s "$DEST/display/display.dylib" ]
cp -R "$DEST" "$TEST/first"
# Timestamp changes are not source changes. Move mtimes forward without a
# wall-clock delay, so this also catches debug/object timestamp regressions.
"$BUILDLOCK_PY" - "$TEST/mods/core" <<'PY'
import os, pathlib, sys
for path in pathlib.Path(sys.argv[1]).rglob('*.c'):
    st = path.stat()
    os.utime(path, (st.st_atime, st.st_mtime + 10))
PY
sh "$TEST/mods/core/build_core.sh" "$DEST"
# Compare ALL installed files, including any sidecars. A dylib-only comparison
# missed dSYM changes because the recorder hashes the entire mod directory.
diff -r "$TEST/first" "$DEST"
"$BUILDLOCK_PY" - "$TEST/first/display" "$DEST/display" <<'PY'
import pathlib, sys

def payload(root, h=1469598103934665603):
    # run_record.cpp hashes sorted entry names, recursing into directories,
    # then file contents (not mtimes or the absolute installation root).
    def add(data, h):
        for byte in data:
            h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
        return h
    for path in sorted(root.iterdir()):
        h = add(path.name.encode(), h)
        h = payload(path, h) if path.is_dir() else add(path.read_bytes(), h)
    return h

first, second = (payload(pathlib.Path(p)) for p in sys.argv[1:])
if first != second:
    sys.exit(f'FAIL: core.display payload changed: {first:016x} != {second:016x}')
print(f'PASS: two core builds have identical bytes; core.display payload {first:016x}')
PY
