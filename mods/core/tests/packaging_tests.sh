#!/bin/sh
# Exercise the real installer in an isolated source tree, never the app.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "core packaging tests" "$ROOT/mods/core/tests/packaging_tests.sh" "$@"
TEST=$(mktemp -d /tmp/pop-core-packaging.XXXXXX)
trap 'rm -rf "$TEST"' EXIT HUP INT TERM
mkdir -p "$TEST/mods/core/probe/assets" "$TEST/tools/recomp" "$TEST/src/recomp/mods"
cp "$ROOT/mods/core/build_core.sh" "$TEST/mods/core/"
cp "$ROOT/tools/recomp/buildlock.sh" "$TEST/tools/recomp/"
cat > "$TEST/mods/core/probe/probe.c" <<'C'
int core_packaging_probe(void) { return 12; }
C
cat > "$TEST/mods/core/probe/mod.toml" <<'TOML'
id = "core.packaging.probe"
[plugin]
path = "probe.dylib"
TOML
printf 'asset payload\n' > "$TEST/mods/core/probe/assets/payload"
"$TEST/mods/core/build_core.sh"
DEST="$TEST/build/recomp/mods/core"
[ -s "$DEST/probe/probe.dylib" ]
cmp "$TEST/mods/core/probe/assets/payload" "$DEST/probe/assets/payload"
[ ! -e "$TEST/mods/core/probe/probe.dylib" ]
echo 'PASS: compiled plugin, copied nested assets, source tree clean'
"$TEST/mods/core/build_core.sh" "$TEST/build/PopRecomp.app/Contents/Resources/mods/core"
[ -s "$TEST/build/PopRecomp.app/Contents/Resources/mods/core/probe/probe.dylib" ]
echo 'PASS: app resource root installed without launching app'
# Failure must preserve the previous install, including its executable bytes.
cp "$DEST/probe/probe.dylib" "$TEST/previous.dylib"
printf 'not valid C\n' > "$TEST/mods/core/probe/probe.c"
if "$TEST/mods/core/build_core.sh" > "$TEST/failure.log" 2>&1; then
    echo 'FAIL: invalid source accepted' >&2; exit 1
fi
cmp "$TEST/previous.dylib" "$DEST/probe/probe.dylib"
echo 'PASS: compile failure is nonzero and preserves previous install'
rm "$TEST/mods/core/probe/probe.c"
if "$TEST/mods/core/build_core.sh" > "$TEST/missing.log" 2>&1; then
    echo 'FAIL: missing manifest plugin accepted' >&2; exit 1
fi
grep -q 'missing plugin' "$TEST/missing.log"
cmp "$TEST/previous.dylib" "$DEST/probe/probe.dylib"
echo 'PASS: missing plugin is nonzero and preserves previous install'
