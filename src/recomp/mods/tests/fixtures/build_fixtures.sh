#!/bin/sh
# Builds the plugin fixtures the loader tests dlopen.
#
# Every .c in this directory becomes a dylib of the same name, rather than a
# fixed list, so a task that adds a fixture does not have to edit this script
# and two tasks adding one at the same time cannot collide in it.
#
# old_cpu.c is the one exception: it is built with tests/fixtures/old_header
# AHEAD of the real include path, so its sizeof(pop_cpu_v1) really is the older
# one. Writing a smaller number into its ABI record by hand would prove
# nothing, because that is not what an old plugin does.
set -e
ROOT=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$ROOT"
FIX=src/recomp/mods/tests/fixtures
OUT=build/recomp/mods-fixtures
mkdir -p "$OUT"

for src in "$FIX"/*.c; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .c)
    if [ "$name" = "old_cpu" ]; then
        xcrun clang -std=c11 -O1 -g -dynamiclib -fPIC \
            -I"$ROOT/$FIX/old_header" -I"$ROOT/src/recomp/mods" \
            -Wl,-undefined,dynamic_lookup "$src" -o "$OUT/$name.dylib"
    else
        xcrun clang -std=c11 -O1 -g -dynamiclib -fPIC -I"$ROOT/src/recomp/mods" \
            -Wl,-undefined,dynamic_lookup "$src" -o "$OUT/$name.dylib"
    fi
done
echo "built $(ls "$OUT"/*.dylib 2>/dev/null | wc -l | tr -d ' ') fixtures in $OUT"
