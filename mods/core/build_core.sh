#!/bin/sh
# Build bundled plugins in a staging tree, then replace only the core root.
# Usage: mods/core/build_core.sh [destination]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SELF="$ROOT/mods/core/build_core.sh"
BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "build and install core mods" "$SELF" "$@"
DEST=${1:-$ROOT/build/recomp/mods/core}
case "$DEST" in
    "$ROOT/build/recomp/mods/core"|"$ROOT/build/PopRecomp.app/Contents/Resources/mods/core"|"$ROOT/build/PopPresentationReview.app/Contents/Resources/mods/core") ;;
    *) echo "build_core: unsupported install root: $DEST" >&2; exit 1 ;;
esac
mkdir -p "$(dirname "$DEST")"
STAGE=$(mktemp -d "${DEST}.stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
cp -R "$ROOT/mods/core/." "$STAGE/"
# Keep LC_UUID: dyld requires it. Ask supporting linkers for deterministic
# output, and suppress archive/object timestamps and debug maps as well.
ZERO_AR_DATE=1
export ZERO_AR_DATE
REPRODUCIBLE=""
if xcrun clang -dynamiclib -x c /dev/null -Wl,-reproducible \
        -o "$STAGE/.link-probe.dylib" >"$STAGE/.link-probe.log" 2>&1; then
    REPRODUCIBLE="-Wl,-reproducible"
fi
rm -f "$STAGE/.link-probe.dylib" "$STAGE/.link-probe.log"
built=0
for src in "$STAGE"/*/*.c; do
    [ -f "$src" ] || continue
    # Payload identity covers every installed byte, including debug bundles.
    # Retain a content-derived UUID, omit temporary object/debug paths, and
    # never use the random staging directory as LC_ID_DYLIB.
    name=$(basename "${src%.c}.dylib")
    xcrun clang -std=c11 -O1 -g0 -dynamiclib -fPIC \
        -Wall -Wextra -Wno-unused-parameter -I"$ROOT/src/recomp/mods" \
        -Wl,-undefined,dynamic_lookup $REPRODUCIBLE -Wl,-S \
        -Wl,-install_name,"@rpath/$name" "$src" -o "${src%.c}.dylib"
    built=$((built + 1))
done
for toml in "$STAGE"/*/mod.toml; do
    [ -f "$toml" ] || continue
    plugin=$(awk '
        /^\[plugin\]/ { plugin=1; next }
        /^\[/ { plugin=0 }
        plugin && /^[[:space:]]*path[[:space:]]*=/ {
            sub(/^[^"]*"/, ""); sub(/".*$/, ""); print
        }' "$toml")
    [ -z "$plugin" ] || [ -f "$(dirname "$toml")/$plugin" ] || {
        echo "build_core: missing plugin $toml: $plugin" >&2; exit 1;
    }
done
rm -rf "$DEST"
mv "$STAGE" "$DEST"
echo "core mods: installed $DEST ($built plugins)"
