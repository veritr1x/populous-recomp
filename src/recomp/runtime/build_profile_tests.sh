#!/bin/sh
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SELF=$ROOT/src/recomp/runtime/build_profile_tests.sh
BUILDLOCK_SH=$ROOT/tools/recomp/buildlock.sh
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "runtime profile tests" "$SELF" "$@"
cd "$ROOT"
RT=src/recomp/runtime
DX=src/recomp/dx
xcrun clang++ -std=c++20 -O1 -g -DRECOMP_NULL_HOST -I. -I"$RT" -Ibuild/recomp/gen \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" "$RT/tests/profile_tests.cpp" \
    -Wl,-force_load,build/recomp/librecomp_gen.a -o build/recomp/profile_tests
POPM_PROFILE=0 build/recomp/profile_tests disabled
POPM_PROFILE=1 build/recomp/profile_tests enabled
