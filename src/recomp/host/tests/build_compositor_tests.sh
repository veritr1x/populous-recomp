#!/bin/sh
# Standalone offscreen suite, also called by host/build_tests.sh. No GUI.
set -eu
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
SELF="$ROOT/src/recomp/host/tests/build_compositor_tests.sh"
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "compositor tests" "$SELF" "$@"
cd "$ROOT"
UI_DOUBLE=
if [ ! -f src/recomp/host/ui_layer.h ]; then
    UI_DOUBLE=-DPOPM_COMPOSITOR_TEST_UI_DOUBLE=1
fi
xcrun clang++ -std=c++20 -O1 -g -fobjc-arc -Wall -Wextra -Werror \
    $UI_DOUBLE -I"$ROOT" \
    src/recomp/host/compositor.mm src/recomp/host/tests/compositor_tests.mm \
    -framework Foundation -framework Metal -o build/recomp/compositor_tests
echo "built build/recomp/compositor_tests"
[ "${1:-}" = "--no-run" ] && exit 0
build/recomp/compositor_tests
