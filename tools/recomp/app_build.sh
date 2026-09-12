#!/bin/sh
# Builds build/PopRecomp.app: the recompiled game in an AppKit window, with
# the Metal presenter, the Metal Direct3D renderer, AVAudioEngine and macOS
# input.
#
#   tools/recomp/app_build.sh              build (assumes librecomp_gen.a)
#   tools/recomp/app_build.sh --translate  run tools/recomp/build.sh first
#
# This script only builds. It never launches anything: see docs/M1_ACCEPTANCE.md
# for the command that starts the app, which is the user's to run.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Resolve this script's own path before the cd: the lock re-execs it by name,
# and a relative "$0" invocation ("./app_build.sh") would no longer name
# anything once the working directory has moved.
SELF=$ROOT/tools/recomp/$(basename "$0")
cd "$ROOT"

RT=src/recomp/runtime
MODS=src/recomp/mods
DX=src/recomp/dx
HOST=src/recomp/host
GEN=build/recomp/gen
LIB=build/recomp/librecomp_gen.a
APP_NAME=${POP_RECOMP_APP_NAME:-PopRecomp}
case "$APP_NAME" in *[!A-Za-z0-9_-]*|'') echo 'Invalid POP_RECOMP_APP_NAME' >&2; exit 2;; esac
APP=build/$APP_NAME.app

# build/recomp/gen, build/recomp/obj and librecomp_gen.a are shared with
# tools/recomp/build.sh, which regenerates all three. Take the same lock so a
# link never sees a tree that is half old and half new. This re-executes the
# script under the lock, so nothing below runs unlocked.
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "tools/recomp/app_build.sh" "$SELF" "$@"

if [ "${1:-}" = "--translate" ]; then
    # build.sh sees BUILDLOCK_HELD and runs inside the lock this script holds.
    "$ROOT/tools/recomp/build.sh"
fi

if [ ! -f "$LIB" ]; then
    echo "app_build: $LIB is missing; run tools/recomp/build.sh first" >&2
    exit 1
fi

mkdir -p "$APP/Contents/MacOS"

# The mod runtime ships in this binary. build_lua.sh takes the same build lock
# this script runs under, so the archive it writes cannot be half-written when
# the link below reads it.
"$ROOT/$MODS/lua/build_lua.sh"

"$ROOT/mods/core/build_core.sh" "$ROOT/$APP/Contents/Resources/mods/core"

# Select the producer for guest-thread UI extraction.
UI_SOURCE=
UI_FLAGS=
if [ -f "$HOST/ui_layer.mm" ]; then
    UI_SOURCE="$HOST/ui_layer.mm"
    UI_FLAGS=-DPOPM_PRESENT_HAS_UI_LAYER=1
fi

xcrun clang++ -std=c++20 -O2 -g -fobjc-arc $UI_FLAGS \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/src" -I"$ROOT/$GEN" -I"$ROOT/third_party/lua" \
    -o "$APP/Contents/MacOS/$APP_NAME" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/snapshot.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$HOST/boot.cpp" "$HOST/report_lock.cpp" "$HOST/main.mm" "$HOST/present.mm" "$HOST/page_overlay.cpp" "$HOST/present_pixels.cpp" "$HOST/present_thread.mm" "$HOST/compositor.mm" $UI_SOURCE "$HOST/input.mm" "$HOST/input_gate.cpp" \
    "$HOST/audio.mm" "$HOST/audio_math.cpp" "$HOST/audio_capture.cpp" "$HOST/midi.mm" "$HOST/d3d_render.mm" \
    src/backends/cpu/indexed_frame.cpp \
    "$MODS"/*.cpp "$MODS"/lua/*.cpp build/recomp/liblua.a \
    -framework Cocoa -framework Metal -framework MetalKit \
    -framework AVFoundation -framework AudioToolbox -framework CoreText -framework CoreVideo -framework CoreGraphics -framework QuartzCore \
    -Wl,-force_load,"$LIB"

cp "$HOST/Info.plist" "$APP/Contents/Info.plist"
if [ "$APP_NAME" != PopRecomp ]; then
    python3 - "$APP/Contents/Info.plist" "$APP_NAME" <<'PYINFO'
import plistlib,sys
from pathlib import Path
p=Path(sys.argv[1]);d=plistlib.loads(p.read_bytes());name=sys.argv[2]
d.update(CFBundleName=name,CFBundleDisplayName=name,CFBundleExecutable=name,
         CFBundleIdentifier='io.github.veritr1x.populousrecomp.'+name.lower())
p.write_bytes(plistlib.dumps(d))
PYINFO
fi
# Package the committed probe list. The settings layer labels a missing list
# as a baseline fallback; packaging must never manufacture passing measurements.
if [ -f tools/recomp/baseline/classic-modes.json ]; then
    cp tools/recomp/baseline/classic-modes.json "$APP/Contents/Resources/classic-modes.json"
fi

PACK_SOURCE=${POPM_TEXTURE_PACK_DIR:-$ROOT/build/texture-pack}
if [ -f "$PACK_SOURCE/manifest.json" ]; then
    python3 "$ROOT/tools/recomp/package_texture_pack.py" "$PACK_SOURCE" "$ROOT/$APP/Contents/Resources/texture-pack"
fi

# Seal the assembled local bundle after copying its resources and plugins.
# The linker signature alone does not cover an app resource envelope.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

echo "built $APP"
