# Release pipeline: prebuilt binaries with the translation embedded (sub-project 4)

Date: 2026-09-12. Follows `2026-09-12-vulkan-and-platform-hosts-design.md`
(sub-project 3, merged). Last of the four sub-projects decided on 2026-09-12.

## Goal

A player downloads one archive for their platform from GitHub Releases,
unpacks it anywhere, launches it, points it once at their own GOG
`D3DPopTB.exe`, and plays. The compiled translation ships inside the binary;
the game's own files are never distributed. Every push to `main` refreshes a
rolling `latest` pre-release; `v*` tags make stable releases.

## Decisions

| Decision | Choice |
| --- | --- |
| Where the generated code lives | Tracked in the repository under `translation/` (about 74 MB of C, 6.5 MB compressed, no LFS). Policy change recorded in NOTICE and README. |
| How the player names the game | A native file dialog on first run, the path saved per user, the exe hash verified against the loader's pinned digest. `--exe` and `POP_RECOMP_EXE` override. |
| Release cadence | Every `main` push updates the `latest` pre-release; `v*` tags create stable releases. Three archives: macOS arm64 `.app` zip, Windows x64 zip, Linux x64 tar.gz. |
| Layout | Resources beside the executable (`resources/`) on Windows and Linux, `Contents/Resources` on macOS, the checkout as the developer fallback; profile in the OS per-user data directory outside a checkout. |
| Signing | macOS ad-hoc signature only (players right-click Open once); no notarization, no Windows signing. |

## Non-goals

- Installers (NSIS, `.pkg`, AppImage), auto-update, signing certificates or
  notarization.
- Intel macOS, more than one supported exe build, any game data in the
  archive.
- The pre-existing integration fixture failures.

## 1. The tracked translation

`translation/` holds what `tools/build.py --regenerate` produces: `chunk_*.c`,
`table.c`, `funcs.h`, `x86.h` and `symbols.json` (which records the source
exe's SHA-256). The loader already refuses any exe whose digest differs from
its pinned constant, so the shipped binary can only run the GOG build the
translation came from.

`cmake/Translate.cmake` sets `POP_GEN_DIR` to the first of
`build/recomp/gen` (a developer's fresh regeneration overrides) and
`translation/` that contains `table.c`; `POP_TRANSLATE=OFF` still disables the
game targets. `tools/build.py --regenerate --publish-tracked` copies the newly
published `build/recomp/gen` into `translation/` (replacing it wholesale) for
the commit that updates the translation; the existing staging-by-rename is
untouched.

A `nogame` test, `tools/recomp/tests/test_translation.py`, checks that
`translation/symbols.json` names the same digest as `LOADER_EXPECTED_SHA256`
in `src/recomp/runtime/loader.cpp`, and that every `chunk_*.c` referenced by
`table.c` exists.

`checks.yml` drops `-DPOP_TRANSLATE=OFF`: every job builds `PopRecomp`,
`pop_smoke` and `pop_headless` as well. Suites labelled `game`, `mods` and
`device` still need the game files and stay local.

Policy text (NOTICE, README "Status" and the third-party table): the
repository includes a translation of the one supported GOG build of
`D3DPopTB.exe`, generated from that executable by this project's tools; the
game's data, executable, artwork, audio and levels are still required from the
player's own copy and are never included; the translation is distributed under
the same terms as the rest of the handwritten code with no claim over the
original work's behaviour or ownership.

## 2. One install layout

`src/recomp/mods/layout.h` (the mods layer, below the host, because roots, overlay, settings and run-record need it):

```cpp
struct HostLayout {
    std::string resources_dir; // core mods, texture pack, classic-modes.json
    std::string profile_dir;   // settings, saves, game-path.txt
    std::string checkout_root; // empty outside a checkout
    bool developer;            // checkout_root is set
};
const HostLayout &host_layout();          // computed once from os_exe_path()
std::string host_resource(const char *rel); // resources_dir + "/" + rel
```

Resolution, first hit wins:

- `resources_dir`: `<bundle>/Contents/Resources` when the executable is at
  `<bundle>.app/Contents/MacOS/`; else `<exe dir>/resources` when that
  directory exists; else the checkout when an ancestor of the executable
  contains `tools/recomp/baseline/classic-modes.json` (then core mods are
  `build/recomp/mods/core`, the texture pack `build/texture-pack`, and the
  table `tools/recomp/baseline/classic-modes.json`, so `host_resource()`
  maps the three names for this case).
- `profile_dir`: `POPM_PROFILE_DIR` if set; `<checkout>/build/recomp/profile`
  in developer mode; else `os_user_data_dir("PopRecomp")`, created on first
  use.
- `developer`: the checkout was found.

`os.h` gains `int os_user_data_dir(const char *app, char *buf, size_t cap)`:
`~/Library/Application Support/<app>` on macOS, `%APPDATA%\<app>` on Windows,
`$XDG_DATA_HOME/<app>` or `~/.local/share/<app>` on Linux.

Consumers change from hard-coded paths to the layout: `mods/roots.cpp` (core
mods), `d3d_render.cpp` (texture pack), `sdl/main.cpp` (classic modes table),
`mods/overlay.cpp` and `settings.cpp` (profile). Each keeps the environment
override it honours today. `pop_headless` and `pop_smoke` use the same unit.

Test: a `nogame` `layout_tests` executable points the layout at a temporary
`resources/` tree and a temporary `X.app/Contents/MacOS/` tree through a test
seam `host_layout_set_exe_path_for_test(const char *)` and checks each
resolution and the developer fallback.

Archives, produced by `tools/recomp/package.py --preset <p> --out <dir>` from
a finished build:

- macOS: `PopRecomp-<version>-macos-arm64.zip` containing `PopRecomp.app` as
  `finish_bundle.py` assembles it (resources, core mods, texture pack, ad-hoc
  signature) plus `LICENSE`, `NOTICE`, `README.txt`.
- Windows: `PopRecomp-<version>-windows-x64.zip` with `PopRecomp.exe`,
  `resources/mods/core/*.dll`, `resources/texture-pack/`,
  `resources/classic-modes.json`, `LICENSE`, `NOTICE`, `README.txt`.
- Linux: `PopRecomp-<version>-linux-x64.tar.gz`, same tree with `.so`
  plugins.

`README.txt` carries the first-run steps (unsigned-app prompt, the picker, the
supported build, the `--exe` override, where the profile lives).

## 3. Finding the game

Resolution order in `sdl/main.cpp`, first hit wins:

1. `--exe <path>`, then `POP_RECOMP_EXE`.
2. Developer mode: `original/gog/D3DPopTB.exe` above the executable, as today.
3. `<profile_dir>/game-path.txt` when the file it names exists and hashes to
   the pinned digest.
4. The picker.

The picker: the window is created hidden first (it already is), then
`SDL_ShowOpenFileDialog` with a filter for `D3DPopTB.exe`; the host pumps
events until the callback delivers a path or a cancel. A chosen file is hashed
with the loader's SHA-256 (`loader_hash_file(path)` exposed for this) and
compared with the pinned digest before load. Match: the path is written to
`game-path.txt` and boot continues. Mismatch: `SDL_ShowMessageBox` "This is
not the supported GOG build of D3DPopTB.exe" with the expected digest and the
buttons Choose again and Quit. Cancel: one line on stderr, exit status 2. When
SDL reports no dialog backend (Linux without the desktop portal or zenity),
the message box explains `--exe` and exits 2.

The runtime takes the game's data directory from the exe's location, so
nothing else changes. The host must not depend on the current directory: the
release smoke launches the packaged binary from an unrelated directory.

New flags: `--version` prints the version string and the GPU backend and
exits 0; `--probe-layout` prints the resolved `resources_dir`, `profile_dir`
and `developer` and exits 0. The version comes from `git describe --tags
--always` at configure time (`POP_RECOMP_VERSION`, also written into
`Info.plist`'s `CFBundleShortVersionString`).

## 4. The release workflow

`.github/workflows/release.yml`, on `push` to `main` and on tags `v*`;
`checks.yml` keeps gating pull requests.

- Build jobs: `macos-15` (arm64), `windows-2025`, `ubuntu-22.04` (older glibc
  than the checks runner, for wider compatibility). Each: install the same
  dependencies `checks.yml` installs, configure the preset (tracked
  translation, `POP_RECOMP_VERSION` from `git describe`), build `PopRecomp`
  and `core_mods`, build the terrain texture pack the way `tools/build.py`'s
  `texture_pack()` does (`tools/recomp/terrain_detail.py --source
  assets/terrain/materials-v1.png --output build/texture-pack`, NumPy and
  Pillow from `tools/recomp/texture-pack-requirements.txt`), run `package.py`, upload the archive as an
  artifact.
- Smoke without the game: from an unrelated directory, run the packaged
  executable with `--version` and `--probe-layout`; both must exit 0 and the
  probe must report the archive's `resources` directory.
- Publish job (`contents: write`): download the artifacts. On `main`, move the
  `latest` tag to the commit and replace the `latest` pre-release's assets
  (`gh release delete-asset`/`upload --clobber`), with notes listing commits
  since the newest `v*` tag. On a `v*` tag, `gh release create` with generated
  notes. Concurrency group `release-main`, cancel in progress.

## 5. Verification

- `nogame` checks green on all three platforms in `checks.yml`, hosts now
  built with the tracked translation.
- `release.yml` on `main` publishes `latest` with three archives; each
  platform's smoke passes.
- On this Mac, outside the checkout: unzip the macOS archive, launch, pick the
  GOG exe, play; relaunch without a dialog; pick a wrong file and get the
  mismatch box with Choose again working; the same launch with
  `POP_GPU_BACKEND=vulkan`.
- Developer flow unchanged: `tools/build.py` and every existing suite behave
  as before, profile under `build/`, no dialog.
- Windows and Linux archives verified by hand when hardware is available; not
  a gate.

## Risks

- The tracked translation makes the repository 74 MB larger and each
  regeneration a large diff; acceptable, it changes rarely.
- SDL's file dialog on Linux depends on the desktop portal or zenity; the
  `--exe` fallback covers headless or minimal desktops.
- Moving `latest` rewrites a tag on every `main` push; anyone who pinned it
  gets a moving target, which is what a rolling pre-release means.
