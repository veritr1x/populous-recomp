#!/bin/sh
# Builds and (unless --no-run) runs the runtime tests.
# Called directly or from tools/recomp/build.sh runtime-tests.
# --profile builds/runs the real translated sampling fixture (requires build.sh).
# Must be run from the repository root: the tests open original/gog/.
set -e

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
SELF=$ROOT/src/recomp/runtime/build_tests.sh
BUILDLOCK_SH=$ROOT/tools/recomp/buildlock.sh
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "runtime tests" "$SELF" "$@"
if [ "${1:-}" = "--profile" ]; then
    "$ROOT/src/recomp/runtime/build_profile_tests.sh"
    exit $?
fi

RT=src/recomp/runtime
OUT=build/recomp
mkdir -p "$OUT"

xcrun clang++ -std=c++20 -O1 -g \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" \
    -o "$OUT/runtime_tests" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/mods_seam.cpp" \
    "$RT/tests/runtime_tests.cpp" "$RT/tests/stub_recomp_call.cpp"

echo "built $OUT/runtime_tests"
[ "$1" = "--no-run" ] && exit 0
exec "$OUT/runtime_tests"
