#!/bin/sh
# Builds build/recomp/pop_headless: the recompiled game booted from its real PE
# entry point with a file presenter instead of a screen.
#
#   tools/recomp/headless_build.sh              build (assumes librecomp_gen.a)
#   tools/recomp/headless_build.sh --translate  run tools/recomp/build.sh first
#
# Unlike tools/recomp/parity_build.sh this does NOT define RECOMP_NULL_HOST:
# the point of this binary is that src/recomp/host/headless_main.cpp provides
# strong host callbacks, so presented frames reach a file. It is still headless
# by construction - the only thing the host does with a frame is write a PPM.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Resolve this script's own path before the cd: the lock re-execs it by name,
# and a relative "$0" invocation ("./headless_build.sh") would no longer name
# anything once the working directory has moved.
SELF=$ROOT/tools/recomp/$(basename "$0")
cd "$ROOT"

RT=src/recomp/runtime
MODS=src/recomp/mods
DX=src/recomp/dx
HOST=src/recomp/host
GEN=build/recomp/gen
LIB=build/recomp/librecomp_gen.a
OUT=build/recomp

# build/recomp/gen, build/recomp/obj and librecomp_gen.a are shared with
# tools/recomp/build.sh, which regenerates all three. Take the same lock so a
# link never sees a tree that is half old and half new. This re-executes the
# script under the lock, so nothing below runs unlocked.
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "tools/recomp/headless_build.sh" "$SELF" "$@"
mkdir -p "$OUT"

if [ "${1:-}" = "--translate" ]; then
    # build.sh sees BUILDLOCK_HELD and runs inside the lock this script holds.
    "$ROOT/tools/recomp/build.sh"
fi

if [ ! -f "$LIB" ]; then
    echo "headless_build: $LIB is missing; run tools/recomp/build.sh first" >&2
    exit 1
fi

# The mod runtime ships in this binary. build_lua.sh takes the same build lock
# this script runs under, so the archive it writes cannot be half-written when
# the link below reads it.
"$ROOT/$MODS/lua/build_lua.sh"

"$ROOT/mods/core/build_core.sh" "$ROOT/build/recomp/mods/core"

xcrun clang++ -std=c++20 -O1 -g -fobjc-arc \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/$GEN" -I"$ROOT/third_party/lua" \
    -o "$OUT/pop_headless" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" \
    "$RT/snapshot.cpp" \
    "$HOST/headless_main.cpp" "$HOST/page_overlay.cpp" "$HOST/boot.cpp" "$HOST/report_lock.cpp" \
    "$HOST/audio.mm" "$HOST/audio_math.cpp" "$HOST/audio_capture.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$MODS"/*.cpp "$MODS"/lua/*.cpp build/recomp/liblua.a \
    -framework Foundation -framework AVFoundation -framework AudioToolbox \
    -Wl,-force_load,"$LIB"

echo "built $OUT/pop_headless"
