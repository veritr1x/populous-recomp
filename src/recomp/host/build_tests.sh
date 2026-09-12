#!/bin/sh
# Builds and (unless --no-run) runs the macOS host tests.
#
# Headless: no window is created, no application object exists and no audio
# device is opened. The Direct3D tests do use the real GPU, offscreen, into an
# MTLTexture the test reads back - that needs no window and is the only way to
# check that a command list becomes the right pixels.
#
# Must be run from the repository root.
set -e

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

HOST=src/recomp/host
DX=src/recomp/dx
RT=src/recomp/runtime
OUT=build/recomp
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "host tests" "$ROOT/src/recomp/host/build_tests.sh" "$@"
mkdir -p "$OUT"
sh "$HOST/tests/build_ui_layer_tests.sh" "${1:-}"

xcrun clang++ -std=c++20 -DPOPM_PRESENT_HAS_UI_LAYER=1 -O1 -g -fobjc-arc \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/src" \
    -o "$OUT/host_tests" \
    "$RT/memory.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/loader.cpp" "$RT/mods_seam.cpp" \
    "$HOST/report_lock.cpp" "$HOST/script.cpp" "$HOST/midi.mm" "$HOST/present.mm" "$HOST/page_overlay.cpp" "$HOST/present_pixels.cpp" "$HOST/present_thread.mm" "$HOST/ui_layer.mm" "$HOST/compositor.mm" "$HOST/input.mm" "$HOST/input_gate.cpp" "$HOST/audio.mm" "$HOST/audio_math.cpp" "$HOST/audio_capture.cpp" "$HOST/d3d_render.mm" \
    "$DX/com.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" "$DX/dinput.cpp" \
    src/backends/cpu/indexed_frame.cpp \
    "$HOST/tests/host_tests.mm" "$HOST/tests/test_frame_builder.mm" "$RT/tests/stub_recomp_call.cpp" \
    -framework Foundation -framework Metal -framework MetalKit \
    -framework AVFoundation -framework AudioToolbox -framework CoreText -framework CoreVideo -framework CoreGraphics -framework QuartzCore

echo "built $OUT/host_tests"
sh "$HOST/tests/build_compositor_tests.sh" "$@"
[ "${1:-}" = "--no-run" ] && exit 0
exec "$OUT/host_tests"
