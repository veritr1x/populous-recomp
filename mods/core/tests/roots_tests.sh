#!/bin/sh
# Run only a tiny roots probe, including from a relocated app-shaped directory.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TEST=$(mktemp -d /tmp/pop-core-roots.XXXXXX)
trap 'rm -rf "$TEST"' EXIT HUP INT TERM
PROBE=${1:?usage: roots_tests.sh <path to roots_probe>}
cp "$PROBE" "$TEST/probe"
unset POPM_CORE_MODS_DIR POPM_MODS_DIR
"$TEST/probe" build/recomp/mods/core mods
POPM_CORE_MODS_DIR=/custom/core POPM_MODS_DIR=/custom/user \
    "$TEST/probe" /custom/core /custom/user
POPM_CORE_MODS_DIR= POPM_MODS_DIR= "$TEST/probe" build/recomp/mods/core mods
mkdir -p "$TEST/Relocated.app/Contents/MacOS"
cp "$TEST/probe" "$TEST/Relocated.app/Contents/MacOS/probe"
"$TEST/Relocated.app/Contents/MacOS/probe" \
    "$TEST/Relocated.app/Contents/MacOS/../Resources/mods/core" mods
POPM_CORE_MODS_DIR=/override "$TEST/Relocated.app/Contents/MacOS/probe" /override mods
if "$TEST/probe" /incorrect/core mods; then
    echo 'FAIL: roots probe accepted a wrong expected root' >&2; exit 1
fi
echo 'PASS: default, overrides, empty overrides, relocated app and negative control'
