#!/bin/sh
# Builds the example mods' plugins, in place beside their mod.toml.
#
# In place, not into build/: a mod directory is what the loader discovers, and
# an example whose plugin lived somewhere else would not be an example of a mod
# anyone could copy. The build products are ignored by git.
#
# The conventions are the loader fixtures': an arm64 dylib per .c, compiled
# against src/recomp/mods for pop_mod_api.h, with undefined symbols resolved at
# load time because the API arrives as a pointer rather than by linking.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EX=mods/examples

built=0
for src in "$EX"/*/*.c; do
    [ -e "$src" ] || continue
    dir=$(dirname "$src")
    name=$(basename "$src" .c)
    xcrun clang -std=c11 -O1 -g -dynamiclib -fPIC \
        -Wall -Wextra -Wno-unused-parameter \
        -I"$ROOT/src/recomp/mods" \
        -Wl,-undefined,dynamic_lookup "$src" -o "$dir/$name.dylib"
    built=$((built + 1))
done

# Every plugin a manifest names has to exist, or the loader would reject the
# mod at run time with a message the caller of this script never sees.
missing=""
for toml in "$EX"/*/mod.toml; do
    dir=$(dirname "$toml")
    path=$(sed -n 's/^path = "\(.*\.dylib\)"/\1/p' "$toml" | head -1)
    [ -z "$path" ] && continue
    [ -f "$dir/$path" ] || missing="$missing $dir/$path"
done
if [ -n "$missing" ]; then
    echo "build_examples.sh: a manifest names a plugin that was not built:$missing" >&2
    exit 1
fi

echo "built $built example plugins in $EX"
