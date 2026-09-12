# Populous Recomp

[Build & contribute](CONTRIBUTING.md) · [How it works](docs/architecture.md) ·
[Display settings](docs/DISPLAY.md) · [Modding](docs/MODDING.md) · [Changelog](CHANGELOG.md)

A native macOS recompilation of **Populous: The Beginning**, with Metal rendering,
native audio and input, persistent game settings, and a C/Lua mod API. Original
game instructions are translated to C ahead of time and compiled with the native host.

**You need your own copy of the game.** This repository contains the runtime,
translator, tools and documentation. Game executables, original artwork, sound,
levels, generated game code and replacement packs are prepared locally and are
not included. See [NOTICE](NOTICE) for ownership and dependency credits.

## Play on macOS

The current port targets Apple Silicon Macs. Intel macOS, Windows, Linux and iOS
are not validated game ports. The runtime, adapter and mod layers compile and
test on Linux and Windows; a playable host for them is future work.

```sh
git clone https://github.com/veritr1x/populous-recomp.git
cd populous-recomp
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
```

Follow the [one-time game setup](CONTRIBUTING.md#prepare-your-game-installation), then:

```sh
.venv/bin/python tools/build.py
open build/PopRecomp.app
```

The app stays inside the checkout so it can find your local game-data link.
F10 and **Populous → Settings…** open the game's Options screen; Command-Q exits.
Resolution has one control under **Options → Graphics → Screen Resolution**.
Enhanced, window mode, frame limit, filtering and overlay settings apply during play.

## Make your first change

You can improve docs and run tooling tests without owning game files or installing
Ghidra. Start with:

```sh
.venv/bin/python tools/test.py
```

For native changes, build the app and use the [testing guide](docs/testing.md).
The [code guide](docs/code-guide.md) maps common tasks to the relevant functions
and explains the thread and memory rules. Handwritten source is formatted with
the checked-in `.clang-format`; generated code remains a local build product.

## Find your way around

| Path | Purpose |
| --- | --- |
| `src/recomp/host/` | SDL3 window, the GPU interface and its Metal backend, audio, input and presentation |
| `src/recomp/runtime/` | Guest memory, executable loading, imports and cooperative scheduling |
| `src/recomp/dx/` | Original graphics, input and sound interfaces adapted to the native host |
| `src/recomp/mods/` | Mod loading, hooks, settings, native Options controls and C/Lua API |
| `src/recomp/native/` | Capture/replay helpers for validating native function replacements |
| `tools/recomp/` | Translator, instruction helpers, builders and gameplay smoke scripts |
| `mods/examples/` | Small C, Lua and asset-overlay examples |
| `assets/terrain/` | Project-created material-detail artwork and its provenance |
| `third_party/lua/` | Unmodified Lua source and upstream license |
| `third_party/volk/` | volk 1.4.304, the Vulkan meta-loader, unmodified |
| `third_party/vulkan-headers/` | Vulkan-Headers v1.4.304, the subset the backend includes, unmodified |
| `CMakeLists.txt`, `cmake/` | The build: targets per directory, presets for macOS, Linux and Windows |
| `build/recomp/gen/` | Locally generated game functions; edit the translator, not these files |

## Status

Native menu navigation, level entry, unit selection and movement, resolution
cycling through 4K, settings persistence, audio playback and clean exit have been
exercised locally. A frame limit of 120 FPS is available; sustained **4K at 120 FPS**
is an optimization target, not a guaranteed performance result.

Long campaign completion and multiplayer still need validation. Linux and Windows
hosts do not exist yet; their portable layers are compiled and tested in CI. The HD path supports full-color replacements and material detail;
enlarging original art does not create a complete newly painted remaster.

GitHub Actions checks contributor tooling, formatting and native host compilation
without game files. Full translation and gameplay checks use a contributor's local
installation; CI does not claim to have run the original game.
