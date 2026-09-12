#!/bin/sh
# Standalone CPU-only host test. Safe to run with no Metal device or guest.
set -eu
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
SELF="$ROOT/src/recomp/host/tests/build_ui_layer_tests.sh"
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "UI layer tests" sh "$SELF" "$@"
cd "$ROOT"
OUT=build/recomp/ui_layer_tests
xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
    src/recomp/host/ui_layer.mm src/recomp/host/tests/test_frame_builder.mm \
    src/recomp/host/tests/ui_layer_tests.mm -o "$OUT"
echo "built $OUT"
[ "${1:-}" = "--no-run" ] && exit 0
"$OUT"
