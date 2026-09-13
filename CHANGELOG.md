# Changelog

## Unreleased

- Switches: the kit's environment switches are `RECOMP_<NAME>` now, and this
  repository's smoke scripts, mods, mod tests and docs use those names
  (`RECOMP_PIN_CLOCK`, `RECOMP_PROFILE_DIR`, `RECOMP_MODS_DIR`, ...). The
  `POPM_*` and `POP_*` spellings are no longer read once the `kit/` pin moves to
  a kit that carries the rename; until then the pinned kit still reads the old
  names.
- Thin repository: the runtime, translator, hosts and tools moved to
  [recomp-kit](https://github.com/veritr1x/recomp-kit), the git submodule at `kit/`.
  This repository keeps Populous's config (`game.toml`, `globals.toml`), game
  headers, mods, artwork, smoke scripts, release notes and docs, and drives the
  kit through `tools/*.py` wrappers. The previous single-repository tree is tag
  `legacy-macos-source`. `translation/` is no longer tracked; regenerate with
  `tools/build.py --regenerate`.
- iPad: Populous runs natively on an iPad by touch (kit milestone M1): fullscreen
  Metal, an on-screen key strip, taps that place the game's cursor, long-press
  right click, wheel-button drag, edge scrolling, lifecycle-driven suspend and
  persistent settings. `tools/build.py --target ios` builds, signs and installs.

- Build with CMake presets for macOS, Linux and Windows through the unchanged
  `tools/build.py` and `tools/test.py`; the xcrun shell scripts are gone.
- Add a platform layer (`src/recomp/platform/os.h`) so the runtime, adapters
  and mod foundation compile and pass their portable tests on Linux and Windows.
- Mod plugins resolve their file extension per platform; a manifest written on
  macOS loads its `.so` or `.dll` counterpart unchanged.
- CI compiles and tests the portable layers on macOS, Ubuntu and Windows.

## 2026-09-12 — Initial native source release

- Publish the macOS native runtime, static translator, Metal renderer and C/Lua
  mod API as a standalone contributor project.
- Provide verified local game setup, build/test commands, architecture and code guides.
- Include Enhanced rendering, widescreen projection, native FPS/frame-pacing
  overlay, live Options settings and one Graphics resolution selector through 4K.
- Preserve the fixes for animation timing, audio clock behavior, focus changes,
  pointer confinement, resolution cycling and settings persistence.
- Keep original game files, generated translations and local test artifacts out of Git.

Current validation boundaries and performance limits are recorded in [Testing](docs/testing.md).
