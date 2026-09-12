#!/bin/sh
# Builds and (unless --no-run) runs the DirectX/audio/input shim tests.
# Headless: no window, no device, no audio stream is ever opened.
# Must be run from the repository root.
set -e

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

DX=src/recomp/dx
RT=src/recomp/runtime
OUT=build/recomp
mkdir -p "$OUT"

# host_api.h is a C header as well as a C++ one, and it is also consumed by
# code built to an older C++ standard than this suite uses. Neither is checked
# by compiling it as C++20 alongside everything else, so both are checked here,
# on their own, before the suite is built. A syntax error in either is a
# failure of this script rather than something found later by whoever imports
# the header next.
echo "== host_api.h as C11 and C++17 =="
xcrun clang   -std=c11   -O1 -Wall -Wextra -I"$ROOT" \
    -c "$DX/tests/host_api_header_test.c" -o "$OUT/host_api_header_test.o"
xcrun clang++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$ROOT" \
    -fsyntax-only -x c++ "$DX/host_api.h"

xcrun clang++ -std=c++20 -O1 -g \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" \
    -o "$OUT/dx_tests" \
    "$RT/memory.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/user32.cpp" "$RT/misc.cpp" "$RT/loader.cpp" "$RT/mods_seam.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$DX/tests/dx_tests.cpp" "$OUT/host_api_header_test.o" "$RT/tests/stub_recomp_call.cpp"

echo "built $OUT/dx_tests"
[ "$1" = "--no-run" ] && exit 0
exec "$OUT/dx_tests"
