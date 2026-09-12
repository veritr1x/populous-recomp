#!/usr/bin/env bash
# Translate D3DPopTB.exe to C and build build/recomp/librecomp_gen.a.
#
#   tools/recomp/build.sh                   full run (translate + compile + archive)
#   tools/recomp/build.sh --no-translate    reuse build/recomp/gen as-is
#   tools/recomp/build.sh runtime-tests     build and run the runtime tests
#                                           (src/recomp/runtime/build_tests.sh)
#   tools/recomp/build.sh clean             remove ONLY this task's outputs
#
# Nothing here ever removes build/recomp itself: other tasks keep their own
# artifacts under it (build/recomp/review, build/recomp/trace, ...).
#
# Generated sources land in build/recomp/gen/ and are never committed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Absolute, because the lock re-execs this script by name and a relative "$0"
# stops naming it the moment anything changes directory.
SELF="$ROOT/tools/recomp/$(basename "${BASH_SOURCE[0]}")"
GEN="$ROOT/build/recomp/gen"
OBJ="$ROOT/build/recomp/obj"
LIB="$ROOT/build/recomp/librecomp_gen.a"
# The hookable-symbol index, generated beside the sources and published with
# them: it describes one particular generation of gen/, so it must not appear
# before that generation does.
SYM="$ROOT/build/recomp/symbols.json"
PY="${PY:-$ROOT/.venv/bin/python}"
CFLAGS=${CFLAGS:--O2 -g -std=c11 -Wall -Wextra -Wno-unused}
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

if [ "${1:-}" = "clean" ]; then
    # Take the lock before removing anything: deleting the outputs from under
    # a running build is exactly what the lock exists to prevent, and refusing
    # to wait would just move the race to whoever retries.
    BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
    . "$BUILDLOCK_SH"
    buildlock_acquire "$ROOT" "tools/recomp/build.sh clean" "$SELF" "$@"
    # Only this task's own outputs.  Never `rm -rf $ROOT/build/recomp`, and
    # never the lock itself: buildlock_release drops that on exit.
    rm -rf "$ROOT/build/recomp/gen" "$ROOT/build/recomp/obj" \
           "$ROOT/build/recomp"/gen.new.* "$ROOT/build/recomp/gen.old" \
           "$ROOT/build/recomp"/obj.new.* "$ROOT/build/recomp/obj.old"
    rm -f "$ROOT/build/recomp/librecomp_gen.a" \
          "$ROOT/build/recomp"/librecomp_gen.a.new* \
          "$ROOT/build/recomp/librecomp_test.dylib" \
          "$ROOT/build/recomp/synth.c" \
          "$ROOT/build/recomp/libdispatch_probe.dylib" \
          "$ROOT/build/recomp/dispatch_probe.c" \
          "$ROOT/build/recomp/dispatch_probe.h" \
          "$ROOT/build/recomp/translate-report.json" \
          "$ROOT/build/recomp/symbols.json" \
          "$ROOT/build/recomp"/symbols.json.new.*
    rm -rf "$ROOT/build/recomp/librecomp_test.dylib.dSYM" \
           "$ROOT/build/recomp/libdispatch_probe.dylib.dSYM"
    echo "cleaned build/recomp/gen, build/recomp/obj and this task's artifacts"
    exit 0
fi

if [ "${1:-}" = "runtime-tests" ]; then
    shift
    exec "$ROOT/src/recomp/runtime/build_tests.sh" "$@"
fi

BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "tools/recomp/build.sh" "$SELF" "$@"

start=$(date +%s)

# Everything is built in a scratch directory and published at the end, all of
# it or none of it.  gen/ and librecomp_gen.a have to agree with EACH OTHER:
# the headers say what the archive is supposed to contain, and a link that
# takes the new funcs.h against the old archive fails, or worse, resolves a
# name that has since changed meaning.  Publishing gen/ before the compile had
# succeeded left exactly that pair behind whenever the compile failed, and the
# next thing to link - app_build.sh, parity_build.sh - was the one to find out.
# Nothing here is published until ar has run.
STAGE_GEN=""
STAGE_OBJ="$OBJ.new.$$"
STAGE_LIB="$LIB.new.$$"
cleanup() {
    rm -rf ${STAGE_GEN:+"$STAGE_GEN"} "$STAGE_OBJ"
    rm -f "$STAGE_LIB" "$SYM.new.$$"
}
trap cleanup EXIT

if [ "${1:-}" != "--no-translate" ]; then
    echo "== translating =="
    STAGE_GEN="$GEN.new.$$"
    rm -rf "$STAGE_GEN"
    mkdir -p "$STAGE_GEN"
    "$PY" "$ROOT/tools/recomp/translate.py" --out "$STAGE_GEN" \
        --report "$ROOT/build/recomp/translate-report.json"
    # x86.h sits beside the generated sources so #include "x86.h" resolves.
    cp "$ROOT/tools/recomp/runtime/x86.h" "$STAGE_GEN/x86.h"
    SRC="$STAGE_GEN"
else
    # Reusing what is already published; only the archive is republished.
    SRC="$GEN"
    mkdir -p "$GEN"
    [ -f "$GEN/x86.h" ] || cp "$ROOT/tools/recomp/runtime/x86.h" "$GEN/x86.h"
fi

# -I$ROOT/src/recomp/runtime for intrinsics.h; -I$ROOT so it can find the
# canonical tools/recomp/runtime/x86.h rather than the copy beside the sources.
# -I$SRC, not -I$GEN: the compile reads the staged tree, not the published one.
INCLUDES="-I$SRC -I$ROOT -I$ROOT/src/recomp/runtime"
rm -rf "$STAGE_OBJ"
mkdir -p "$STAGE_OBJ"

echo "== compiling ($JOBS jobs) =="
compile_one() {
    src="$1"
    out="$STAGE_OBJ/$(basename "${src%.c}").o"
    xcrun clang $CFLAGS $INCLUDES -c "$src" -o "$out"
}
export -f compile_one
export STAGE_OBJ CFLAGS INCLUDES

printf '%s\n' "$SRC"/*.c | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

echo "== archiving =="
xcrun ar rcs "$STAGE_LIB" "$STAGE_OBJ"/*.o
xcrun ranlib "$STAGE_LIB" 2>/dev/null || true

# Publish.  Renames only, no writes: each one replaces a whole artifact and
# none of them can half-finish.  Every reader holds the lock this script holds,
# so nothing observes the moment between them; what matters is that a failure
# anywhere above reaches none of this and leaves the previous gen/ and archive
# together, still a matched pair.
rm -rf "$GEN.old" "$OBJ.old"
if [ -n "$STAGE_GEN" ]; then
    [ -d "$GEN" ] && mv "$GEN" "$GEN.old"
    mv "$STAGE_GEN" "$GEN"
    STAGE_GEN=""
fi
[ -d "$OBJ" ] && mv "$OBJ" "$OBJ.old"
mv "$STAGE_OBJ" "$OBJ"
mv "$STAGE_LIB" "$LIB"
[ -f "$GEN/symbols.json" ] && cp "$GEN/symbols.json" "$SYM.new.$$" \
    && mv "$SYM.new.$$" "$SYM"
rm -rf "$GEN.old" "$OBJ.old"

end=$(date +%s)
echo "librecomp_gen.a: $(ls -lh "$LIB" | awk '{print $5}')  objects: $(ls "$OBJ"/*.o | wc -l | tr -d ' ')"
echo "total time: $((end - start))s"
