#!/bin/sh
# Real mod loader/hooks/stdout and DirectDraw seal/service, with fake GPU acks.
# Called under mods/build_tests.sh's build lock. Never creates an app or device.
set -e
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT"
RT="$ROOT/src/recomp/runtime"
DX="$ROOT/src/recomp/dx"
HOST="$ROOT/src/recomp/host"
MODS="$ROOT/src/recomp/mods"
OUT="$ROOT/build/recomp/present_events_tests"
xcrun clang++ -std=c++20 -O1 -g -fobjc-arc -DPOPM_TESTING=1 -DPOPM_PRESENT_HAS_UI_LAYER=1 \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$ROOT/src" -I"$RT" -I"$ROOT/build/recomp/gen" -I"$ROOT/third_party/lua" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/snapshot.cpp" "$RT/mods_seam.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$HOST/present_thread.mm" "$HOST/present_pixels.cpp" "$HOST/page_overlay.cpp" "$HOST/d3d_render.mm" \
    "$HOST/compositor.mm" "$HOST/ui_layer.mm" "$HOST/input.mm" "$HOST/input_gate.cpp" \
    "$ROOT/src/backends/cpu/indexed_frame.cpp" \
    "$MODS"/*.cpp "$MODS"/lua/*.cpp "$ROOT/build/recomp/liblua.a" \
    "$MODS/tests/present_events_tests.mm" \
    -framework Foundation -framework Metal -framework MetalKit -framework CoreText -framework CoreVideo \
    -framework CoreGraphics -framework QuartzCore \
    -Wl,-force_load,"$ROOT/build/recomp/librecomp_gen.a" -o "$OUT"
echo "built $OUT"
[ "${1:-}" = "--no-run" ] && exit 0
exec "$OUT" "$ROOT"
