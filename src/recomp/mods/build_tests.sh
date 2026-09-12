#!/bin/sh
# Builds and (unless --no-run) runs the mod-foundation tests.
#
# Headless: no window, no audio device, no application object. The tests map
# the real guest image and link the generated archive, so the repository root
# is the working directory and tools/recomp/build.sh must have run once.
#
# Source globs include new modules automatically. The standard app build
# prepares Lua; minimal runtime-only builds can omit its archive.
set -e

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
# Resolved before the cd: the build lock re-execs this script by name.
SELF=$ROOT/src/recomp/mods/$(basename "$0")
cd "$ROOT"

MODS=src/recomp/mods
RT=src/recomp/runtime
GEN=build/recomp/gen
LIB=build/recomp/librecomp_gen.a
OUT=build/recomp
mkdir -p "$OUT"

# build/recomp/gen and librecomp_gen.a are rewritten by tools/recomp/build.sh.
# Link against them under the same lock, so a test binary is never half of one
# generation and half of another.
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "src/recomp/mods/build_tests.sh" "$SELF" "$@"

if [ ! -f "$LIB" ]; then
    echo "mods/build_tests.sh: $LIB is missing; run tools/recomp/build.sh first" >&2
    exit 1
fi

# Fixtures the loader tests dlopen have to exist before the tests run.
[ -x "$MODS/tests/fixtures/build_fixtures.sh" ] && "$MODS/tests/fixtures/build_fixtures.sh"
# Exercise the installed core artifact, including its production link flags.
sh "$ROOT/mods/core/build_core.sh" "$ROOT/build/recomp/mods/core"

# Include the runtime seam and built Lua services when available.
SEAM=""
[ -f "$RT/mods_seam.cpp" ] && SEAM="$RT/mods_seam.cpp"

LUA_INC=""
LUA_LIB=""
LUA_SRC=""
if [ -f build/recomp/liblua.a ]; then
    LUA_INC="-I$ROOT/third_party/lua"
    LUA_LIB="build/recomp/liblua.a"
    LUA_SRC=$(ls "$MODS"/lua/*.cpp 2>/dev/null || true)
fi

xcrun clang -std=c11 -O1 -g -DPOPM_TESTING=1 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/$GEN" \
    -c "$MODS/tests/api_header_test.c" -o "$OUT/api_header_test.o"

# POPM_TESTING compiles the test-only entry points: the loader's reset, the
# owner-counter seam and the scheduler's guest-entry latch reset. None of them
# exists in a real build, because each one undoes something a real run relies
# on never being undone.
xcrun clang++ -std=c++20 -O1 -g -DPOPM_TESTING=1 \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/src" -I"$ROOT/$GEN" $LUA_INC \
    -o "$OUT/mods_tests" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/snapshot.cpp" \
    $SEAM \
    $(ls "$MODS"/*.cpp 2>/dev/null || true) $LUA_SRC \
    $(ls "$MODS"/tests/*.cpp 2>/dev/null || true) \
    "$OUT/api_header_test.o" $LUA_LIB \
    -Wl,-force_load,"$LIB"

echo "built $OUT/mods_tests"
if [ "${1:-}" != "--no-run" ]; then "$OUT/mods_tests"; fi
if [ -n "$LUA_LIB" ]; then
    sh "$ROOT/$MODS/tests/build_present_events_tests.sh" "$@"
fi
if [ "${1:-}" != "--no-run" ]; then
    sh "$ROOT/mods/core/tests/reproducibility_tests.sh"
fi
