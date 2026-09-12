#!/bin/sh
# Builds build/recomp/pop_smoke: the recompiled game driven by a script, with
# no window and no audio device. See src/recomp/host/smoke_main.mm.
#
#   tools/recomp/smoke_build.sh              build (assumes librecomp_gen.a)
#   tools/recomp/smoke_build.sh --translate  run tools/recomp/build.sh first
#   tools/recomp/smoke_build.sh --reuse-core  use the preceding host build's core mods
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Resolve this script's own path before the cd: the lock re-execs it by name,
# and a relative "$0" invocation ("./smoke_build.sh") would no longer name
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

BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "tools/recomp/smoke_build.sh" "$SELF" "$@"
mkdir -p "$OUT"

if [ "${1:-}" = "--translate" ]; then
    "$ROOT/tools/recomp/build.sh"
fi

if [ ! -f "$LIB" ]; then
    echo "smoke_build: $LIB is missing; run tools/recomp/build.sh first" >&2
    exit 1
fi

# present_pixels provides expansion and dumps; present_thread owns the offscreen
# compositor and synthetic link. No view or application object is created.
# smoke_main supplies the current host_present_mode, as present.mm does for the
# app/tests; present_thread also supplies a weak default for minimal hosts.
# The mod runtime ships in this binary. build_lua.sh takes the same build lock
# this script runs under, so the archive it writes cannot be half-written when
# the link below reads it.
"$ROOT/$MODS/lua/build_lua.sh"

if [ "${1:-}" = "--reuse-core" ]; then
    # Gate B already built these with the parity host under the same lock.
    [ -d "$ROOT/build/recomp/mods/core" ] || {
        echo "smoke_build: --reuse-core requires a preceding core build" >&2
        exit 1
    }
else
    "$ROOT/mods/core/build_core.sh" "$ROOT/build/recomp/mods/core"
fi

# Use the UI producer when available; minimal host builds retain the
# complete-surface fallback.
UI_SOURCE=
UI_FLAGS=
if [ -f "$HOST/ui_layer.mm" ]; then
    UI_SOURCE="$HOST/ui_layer.mm"
    UI_FLAGS=-DPOPM_PRESENT_HAS_UI_LAYER=1
fi

xcrun clang++ -std=c++20 -O1 -g -fobjc-arc $UI_FLAGS \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/src" -I"$ROOT/$GEN" -I"$ROOT/third_party/lua" \
    -o "$OUT/pop_smoke" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/snapshot.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$HOST/boot.cpp" "$HOST/report_lock.cpp" "$HOST/script.cpp" \
    "$HOST/input.mm" "$HOST/input_gate.cpp" "$HOST/page_overlay.cpp" "$HOST/smoke_main.mm" "$HOST/d3d_render.mm" \
    "$HOST/present_pixels.cpp" "$HOST/present_thread.mm" "$HOST/compositor.mm" $UI_SOURCE "$HOST/audio_math.cpp" \
    src/backends/cpu/indexed_frame.cpp \
    "$MODS"/*.cpp "$MODS"/lua/*.cpp build/recomp/liblua.a \
    -framework Foundation -framework Metal -framework MetalKit \
    -framework CoreText -framework CoreVideo -framework CoreGraphics -framework QuartzCore \
    -Wl,-force_load,"$LIB"

echo "built $OUT/pop_smoke"
