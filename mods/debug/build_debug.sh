#!/bin/sh
# Builds the diagnostic mods, in place beside their mod.toml.
#
# SEPARATE FROM mods/examples ON PURPOSE. tools/recomp/mods_test.sh asserts
# that exactly five example mods load and that the run record names five, and
# Gate B compares those five across runs. A sixth mod dropped into that
# directory fails both, and a diagnostic that breaks the gates is worse than no
# diagnostic. This directory is loaded only when POPM_MODS_DIR points at it.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
DBG=mods/debug

built=0
for src in "$DBG"/*/*.c; do
    [ -e "$src" ] || continue
    dir=$(dirname "$src")
    name=$(basename "$src" .c)
    xcrun clang -std=c11 -O1 -g -dynamiclib -fPIC \
        -Wall -Wextra -Wno-unused-parameter \
        -I"$ROOT/mods" \
        -Wl,-undefined,dynamic_lookup "$src" -o "$dir/$name.dylib"
    built=$((built + 1))
done

missing=""
for toml in "$DBG"/*/mod.toml; do
    dir=$(dirname "$toml")
    path=$(sed -n 's/^path = "\(.*\.dylib\)"/\1/p' "$toml" | head -1)
    [ -z "$path" ] && continue
    [ -f "$dir/$path" ] || missing="$missing $dir/$path"
done
if [ -n "$missing" ]; then
    echo "build_debug.sh: a manifest names a plugin that was not built:$missing" >&2
    exit 1
fi

echo "built $built diagnostic plugins in $DBG"
