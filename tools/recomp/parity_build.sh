#!/bin/sh
# Builds build/recomp/pop_fixture: the headless parity fixture.
#
#   tools/recomp/parity_build.sh              build (assumes librecomp_gen.a)
#   tools/recomp/parity_build.sh --translate  run tools/recomp/build.sh first
#   tools/recomp/parity_build.sh --snapshot DIR
#       Copy generated sources into DIR under the build lock and verify that
#       their symbol table is internally consistent.
#   tools/recomp/parity_build.sh --trace DIR
#       Build pop_fixture_trace from caller-supplied wrappers and a matching
#       DIR/gen snapshot. This advanced path does not generate the wrappers;
#       it keeps their object files separate from the playable game's archive.
#
# Links the generated code, the runtime, the snapshot module, the fixture entry
# and the DirectX shims built with -DRECOMP_NULL_HOST, so every host callback
# is a strong no-op: no window, no device, no audio stream is ever opened.
# Must be run from anywhere; it resolves the repository root itself.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Resolve this script's own path before the cd: the lock re-execs it by name,
# and a relative "$0" invocation ("./parity_build.sh") would no longer name
# anything once the working directory has moved.
SELF=$ROOT/tools/recomp/$(basename "$0")
cd "$ROOT"

RT=src/recomp/runtime
MODS=src/recomp/mods
DX=src/recomp/dx
GEN=build/recomp/gen
LIB=build/recomp/librecomp_gen.a
OUT=build/recomp

# build/recomp/gen, build/recomp/obj and librecomp_gen.a are shared with
# tools/recomp/build.sh, which regenerates all three.  Take the same lock so a
# link never sees a tree that is half old and half new.
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "tools/recomp/parity_build.sh" "$SELF" "$@"
mkdir -p "$OUT"

# The fixture discovers bundled and user mods unless POPM_NO_MODS is set.
# Gate A disables both roots; Gate B overrides the user root explicitly.
"$ROOT/$MODS/lua/build_lua.sh"

"$ROOT/mods/core/build_core.sh" "$ROOT/build/recomp/mods/core"

if [ "${1:-}" = "--translate" ]; then
    # build.sh sees BUILDLOCK_HELD and runs inside the lock this script holds.
    "$ROOT/tools/recomp/build.sh"
fi

if [ "${1:-}" = "--snapshot" ]; then
    TSRC=${2:?parity_build.sh --snapshot needs a destination directory}
    # The translator rewrites build/recomp/gen in place, so a copy taken while
    # it is publishing can be internally inconsistent: chunks from one
    # generation beside a funcs.h from another. Copy, then check that every
    # prototype funcs.h declares has a definition in the copied chunks, and
    # retry if it does not.
    ok=0
    for attempt in 1 2 3 4 5; do
        rm -rf "$TSRC"
        mkdir -p "$TSRC"
        if ! cp "$GEN"/*.c "$GEN"/*.h "$TSRC"/ 2>/dev/null; then
            echo "parity_build: build/recomp/gen is being rewritten, retrying ($attempt)" >&2
            sleep 5
            continue
        fi
        if [ ! -f "$TSRC/table.c" ] || [ ! -f "$TSRC/funcs.h" ] || [ ! -f "$TSRC/x86.h" ]; then
            echo "parity_build: incomplete snapshot, retrying ($attempt)" >&2
            sleep 5
            continue
        fi
        protos=$(grep -ho '^void fn_[0-9a-f]\{8\}' "$TSRC/funcs.h" | sort -u | wc -l | tr -d ' ')
        defs=$(cat "$TSRC"/chunk_*.c | grep -ho '^void fn_[0-9a-f]\{8\}' | sort -u | wc -l | tr -d ' ')
        if [ "$protos" = "$defs" ] && [ "$protos" != 0 ]; then
            echo "snapshot: $protos functions in $TSRC"
            ok=1
            break
        fi
        echo "parity_build: torn snapshot ($protos prototypes, $defs definitions), retrying ($attempt)" >&2
        sleep 5
    done
    if [ "$ok" != 1 ]; then
        echo "parity_build: could not take a consistent snapshot of build/recomp/gen" >&2
        exit 1
    fi
    exit 0
fi

if [ "${1:-}" = "--trace" ]; then
    TDIR=${2:?parity_build.sh --trace needs the generated-trace directory}
    TOBJ="$OUT/parity/trace-obj"
    TSRC="$TDIR/gen"
    # The caller supplies wrappers generated from the same $TDIR/gen snapshot.
    # Do not read the live generated tree, which a later build may replace.
    if [ ! -f "$TSRC/table.c" ] || [ ! -f "$TSRC/funcs.h" ] || [ ! -f "$TSRC/x86.h" ]; then
        echo "parity_build: $TSRC is not a complete snapshot of build/recomp/gen" >&2
        exit 1
    fi
    rm -rf "$TOBJ"
    mkdir -p "$TOBJ"
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
    # The override header is force-included ahead of funcs.h, so every FN_<addr>
    # macro is already defined by the time funcs.h's defaults would set it.
    # This uses the native replacement indirection to record calls.
    cat > "$TOBJ/compile-one.sh" <<SH
#!/bin/sh
set -e
out="$TOBJ/\$(basename "\${1%.c}").o"
exec xcrun clang -O1 -g -std=c11 -Wall -Wno-unused \\
    -include "$TDIR/trace_override.h" \\
    -I"$TDIR" -I"$TSRC" -I"$ROOT" -I"$ROOT/$RT" \\
    -c "\$1" -o "\$out"
SH
    chmod +x "$TOBJ/compile-one.sh"
    echo "== compiling the traced generated code ($JOBS jobs) =="
    ls "$TSRC"/*.c "$TDIR"/trace_wrappers.c | xargs -P "$JOBS" -n 1 "$TOBJ/compile-one.sh"
    echo "== linking =="
    xcrun clang++ -std=c++20 -O1 -g -DRECOMP_NULL_HOST \
        -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
        -I"$ROOT" -I"$RT" -I"$TSRC" -I"$ROOT/third_party/lua" \
        -o "$OUT/pop_fixture_trace" \
        "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
        "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" \
        "$RT/snapshot.cpp" "$RT/fixture.cpp" \
        "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
        "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
        "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
        "$MODS"/*.cpp "$MODS"/lua/*.cpp build/recomp/liblua.a \
        "$TOBJ"/*.o
    echo "built $OUT/pop_fixture_trace"
    exit 0
fi

if [ ! -f "$LIB" ]; then
    echo "parity_build: $LIB is missing; run tools/recomp/build.sh first" >&2
    exit 1
fi

xcrun clang++ -std=c++20 -O1 -g -DRECOMP_NULL_HOST \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -I"$ROOT" -I"$RT" -I"$ROOT/$GEN" -I"$ROOT/third_party/lua" \
    -o "$OUT/pop_fixture" \
    "$RT/memory.cpp" "$RT/loader.cpp" "$RT/imports.cpp" "$RT/cpu.cpp" "$RT/profile.cpp" \
    "$RT/kernel32.cpp" "$RT/mods_seam.cpp" "$RT/user32.cpp" "$RT/misc.cpp" \
    "$RT/snapshot.cpp" "$RT/fixture.cpp" \
    "$DX/com.cpp" "$DX/dx.cpp" "$DX/host_api.cpp" "$DX/display_stubs.cpp" \
    "$DX/ddraw.cpp" "$DX/d3d.cpp" "$DX/dsound.cpp" "$DX/dinput.cpp" \
    "$DX/qmixer.cpp" "$DX/weanetr.cpp" \
    "$MODS"/*.cpp "$MODS"/lua/*.cpp build/recomp/liblua.a \
    -Wl,-force_load,"$LIB"

echo "built $OUT/pop_fixture"
