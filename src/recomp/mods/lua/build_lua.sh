#!/bin/sh
# Builds the vendored Lua 5.4.6 into build/recomp/liblua.a.
#
#   src/recomp/mods/lua/build_lua.sh          build if any source is newer
#   FORCE=1 src/recomp/mods/lua/build_lua.sh  build regardless
#
# third_party/lua holds the library sources only - lua.c and luac.c, the two
# programs, are not vendored - so everything here is compiled and archived.
# LUA_USE_MACOSX gives Lua the POSIX pieces it can use on this platform; it
# does NOT enable dynamic loading of Lua C modules for scripts, because
# `package` never reaches a mod's interpreter (see lua/bindings.cpp).
#
# Headless, and it writes one file: build/recomp/liblua.a. It takes the shared
# build lock anyway, because that file lives beside the generated archive and
# src/recomp/mods/build_tests.sh links both under the same lock.
set -e

ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
# Absolute, resolved before the cd: the lock re-execs this script by name.
SELF=$ROOT/src/recomp/mods/lua/$(basename "$0")
cd "$ROOT"

SRC=third_party/lua
OUT=build/recomp
OBJ=$OUT/lua-obj
LIB=$OUT/liblua.a
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
. "$BUILDLOCK_SH"
buildlock_acquire "$ROOT" "src/recomp/mods/lua/build_lua.sh" "$SELF" "$@"

[ -d "$SRC" ] || { echo "build_lua.sh: $SRC is missing" >&2; exit 1; }
mkdir -p "$OBJ"

# Skip the work when the archive is newer than every source and header.
if [ "${FORCE:-0}" != "1" ] && [ -f "$LIB" ]; then
    newer=$(find "$SRC" -name '*.[ch]' -newer "$LIB" -print -quit)
    if [ -z "$newer" ]; then
        echo "liblua.a is up to date"
        exit 0
    fi
fi

CFLAGS="-O2 -g -std=c99 -DLUA_USE_MACOSX -Wall -Wextra -Wno-unused-parameter"

compile_one() {
    o="$OBJ/$(basename "${1%.c}").o"
    xcrun clang $CFLAGS -c "$1" -o "$o"
}
export -f compile_one 2>/dev/null || true   # bash; ignored by a plain sh
export OBJ CFLAGS

# Built under a temporary name and renamed: a reader either gets the whole
# previous archive or the whole new one, never a partial `ar` run.
rm -f "$OBJ"/*.o
ls "$SRC"/*.c | xargs -P "$JOBS" -I{} sh -c 'o="$OBJ/$(basename "${1%.c}").o"; xcrun clang $CFLAGS -c "$1" -o "$o"' _ {}
rm -f "$LIB.new"
xcrun ar rcs "$LIB.new" "$OBJ"/*.o
xcrun ranlib "$LIB.new" 2>/dev/null || true
mv "$LIB.new" "$LIB"

echo "built $LIB ($(ls "$OBJ"/*.o | wc -l | tr -d ' ') objects, $(ls -lh "$LIB" | awk '{print $5}'))"
