# Populous Recomp

[Build & contribute](CONTRIBUTING.md) · [Display settings](docs/DISPLAY.md) ·
[HD textures](docs/HD_TEXTURES.md) · [Modding](docs/MODDING.md) ·
[Testing](docs/testing.md) · [Changelog](CHANGELOG.md)

A native macOS and iPad recompilation of **Populous: The Beginning**: Metal
rendering, native audio and input, touch play, persistent game settings and a
C/Lua mod API. Original game instructions are translated to C ahead of time
and compiled with the native host.

The runtime, translator, hosts and mod foundation are
[recomp-kit](https://github.com/veritr1x/recomp-kit), pulled in as the git
submodule `kit/`. This repository holds what is Populous's: `game.toml` and
`globals.toml` (identity, addresses, curated symbols), `core/` and `tests/`
(game-specific headers), `mods/` (core plugins, examples, the smoke probe),
`assets/` (artwork the texture pack is compiled from), `smoke/` (scripted
runs), release notes and docs.

**You need your own copy of the game.** Game executables, artwork, sound,
levels, generated game code and replacement packs are prepared locally and are
not included. See [NOTICE](NOTICE) for ownership and dependency credits.

## Build on macOS

```sh
git clone --recurse-submodules https://github.com/veritr1x/populous-recomp.git
cd populous-recomp
python3 -m venv .venv
.venv/bin/python -m pip install -r kit/requirements-dev.txt
.venv/bin/python tools/setup.py --install "/path/to/your/Populous" --ghidra-home /path/to/ghidra_12.1.3_PUBLIC
.venv/bin/python tools/build.py --regenerate
open build/PopRecomp.app
```

`tools/*.py` are four-line wrappers around the kit's tools; every option is
the kit's (`--help` lists them). Outputs (the translation, the texture pack,
the apps, the logs) live under ignored `build/`; your installation is linked
at ignored `original/gog` and the Ghidra listings live in ignored
`analysis/`.

## Play on an iPad

Requires Xcode with the iOS SDK, an Apple developer team signed in to Xcode,
a paired iPad with developer mode on, and a macOS build already regenerated.

```sh
export RECOMP_IOS_TEAM=<your team id>       # security find-identity -v -p codesigning
.venv/bin/python tools/build.py --target ios --console
```

The build stages your game directory into the app (see `[bundle].exclude` in
`game.toml`), signs it, installs it with `devicectl` and streams the console.
Touch: tap = left click, long press then lift = right click (unselect), long
press then drag = wheel-button drag (scroll; rotate when it starts near the
bottom), a hold on a screen edge scrolls, drag = left drag, two-finger drag
pans, two-finger tap = Escape, three-finger tap = F10 (Options), four-finger
tap toggles the system keyboard. An on-screen split keyboard sits in the
bottom corners: HIDE/KEYS tabs per half, Shift/Ctrl/Alt hold to chord, tap
to latch, double tap to lock; size and visibility are on the F10 page. `tools/ios_logs.py --device <id>` pulls the app's Documents (saves)
back to the Mac.

## Check a change

```sh
.venv/bin/python tools/test.py              # the kit's portable suites
.venv/bin/python -m pytest -q tests         # this game's config
.venv/bin/python tools/test.py --native     # runtime, adapters, Metal and UI suites against the game
.venv/bin/python tools/test.py --mods       # real game-backed mod tests (build the app first)
.venv/bin/python tools/test.py --gameplay   # scripted Options and gameplay run
.venv/bin/python kit/tools/format.py        # native code style, in the kit
```

Changes to the runtime, hosts or tools belong in the kit's repository; bump
the submodule here once they land.
