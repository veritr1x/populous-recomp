# Multi-platform port: progress tracker

Read this first if you are picking the work up cold. It says what is done, what
is in flight, where the documents are, and how to verify. Keep it current: update
the "Now" section at every milestone and commit it with the work.

## Roadmap (decided 2026-09-12)

| # | Sub-project | Spec | Plan | State |
| --- | --- | --- | --- | --- |
| 1 | Portable build system + `os.h` platform layer | `specs/2026-09-12-portable-build-system-design.md` | `plans/2026-09-12-portable-build-system.md` | **Done.** Merged to `main` (commits 3fa44f4..920de70), CI green on macOS, Ubuntu, Windows. |
| 2 | Host abstraction: GPU device interface (Metal first), SDL3 window/input on macOS, portable audio mixer + TinySoundFont MIDI | `specs/2026-09-12-host-abstraction-design.md` | `plans/2026-09-12-host-abstraction.md` | **Done.** Merged to `main` (commits 1b35ca9..0e862a5), CI green on macOS, Ubuntu, Windows. The manual window checklist is still for a person to run. |
| 3 | Vulkan backend + Windows/Linux hosts (presets, CI labels) | `specs/2026-09-12-vulkan-and-platform-hosts-design.md` | `plans/2026-09-12-vulkan-and-platform-hosts.md` | **Done.** Merged to `main` (commits a8ff5e9..44cee8d). CI green on macOS (Metal), Ubuntu (lavapipe runs the GPU suites) and Windows (ported suites) at 4a73426 (run 34701915809). Game-backed suites pass over Vulkan on this Mac (MoltenVK). Manual runs on real Windows/Linux hardware still to do; not a merge gate. |
| 4 | Release pipeline with the translation embedded; player points at their own D3DPopTB.exe | `specs/2026-09-12-release-pipeline-design.md` | `plans/2026-09-12-release-pipeline.md` | **Done.** Merged to `main` with the Vulkan async-upload fix. `translation/` tracked; CI builds and packages the hosts on all three platforms (run 34706772249); the Windows archive runs the game under CrossOver on this Mac; the macOS archive runs from outside the checkout. The `latest` pre-release is published by the merge. |

Decisions that must not be reopened without the user: clang only (no MSVC); SDL3 on
every platform including macOS; one software mixer everywhere (AVAudioEngine and the
Apple DLS synth retired); TinySoundFont vendored for MIDI; Metal on Apple, Vulkan on
Windows/Linux/Android, WebGPU if web is ever attempted; device-level GPU interface
(renderer, compositor, presenter and overlay are portable C++ over it).

## Now

- Sub-project 4 merged to `main` together with the Vulkan async-upload fix
  (branch `vulkan-async-uploads`): uploads and texture creation no longer wait
  on the GPU, which removed ~1000 queue drains per second and is the fix for
  the 5-10 fps report on real Windows. The merge is what first runs
  `release.yml` and publishes the rolling `latest` pre-release.
- Verified: `checks.yml` green on macOS, Ubuntu and Windows with the hosts built
  from `translation/` and packaged as artifacts; the macOS archive unzipped in
  /tmp boots with `--exe`, with a saved path (no dialog) and waits in the picker
  with a stale one; the Windows archive from CI runs the game under CrossOver
  (bottle `PopRecompTest`, win10_64) to the front end with mods, music and sound.
- What a person still has to do: run the `latest` Windows archive on the real
  Windows machine with `POP_GPU_TRACE=1` and read the per-second
  `gpu/vulkan:` lines (one-shots should be 0; `wait()` is the renderer's own
  readback); click through the picker and the mismatch box once on macOS; run
  the Linux archive on Linux hardware; consider tagging `v0.1.0`.
- All four sub-projects of the multi-platform port are then complete.

## Environment gotcha found during Task 4

- `build/recomp/profile/` is the default profile every run without
  `POPM_PROFILE_DIR` shares, and the game writes its own options into
  `POP3.CD/SAVE/CONFIG00.DAT` there. A run that changed the resolution left
  1024x768 behind, and the pinned `level1.script` then clicked into a message
  box instead of a brave ("EXPECT watch_selected FAILED ... got 0"). It looks
  like a presenter/input regression; it is not. Delete `build/recomp/profile`
  (or run with a fresh `POPM_PROFILE_DIR`) before trusting the pinned
  integration checks. Verified: `main` and this branch both pass with a fresh
  profile, both fail with the drifted one.
- `mods_tests` needs the snapshot `tools/test.py --mods` generates; a plain
  `ctest -L mods` fails in `game_view_tests` on `load_snapshot()`.
- The fixture checks in `integration_tests.sh` fail because
  `tools/recomp/parity.py` is not in the repo: pre-existing.

## How to resume

1. `git log --oneline` on `main`; sub-project 4 starts with brainstorming.
2. Read the spec, then the plan; the plan has checkboxes per step. Find the first
   unchecked step. Each task ends with a commit, so `git log` shows how far it got.
3. Build and test: `.venv/bin/python tools/build.py` (needs the game, see below),
   `.venv/bin/python tools/test.py --compile-only`, `.venv/bin/ctest --preset macos -L "nogame|gpu|device"`.
4. Push the branch and run CI with `gh workflow run checks.yml --ref <branch>`;
   confirm the run's `headSha` matches `HEAD` before trusting it.

## Environment notes

- Release archives: `python3 tools/recomp/package.py --preset macos --version $(git describe --tags --always)`
  writes `dist/`. `PopRecomp --version`, `--probe-layout`, `--exe <path>`; the
  profile is `~/Library/Application Support/PopRecomp` outside a checkout and
  `build/recomp/profile` inside one (`POPM_PROFILE_DIR` overrides).
- Windows archive under CrossOver on this Mac: download the `PopRecomp-windows`
  artifact of a checks run, unzip, then
  `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxstart --bottle PopRecompTest --wait-children "$PWD/PopRecomp.exe" --exe 'Z:\Users\...\original\gog\D3DPopTB.exe'`
  (the bottle must be a 64-bit template: `cxbottle --bottle PopRecompTest --create --template win10_64`).
  `--version` and `--probe-layout` work the same way. The Windows artifact also
  carries `PopRecomp.map`; the Windows fault handler prints crash RVAs against it.
- `tools/build.py --publish-tracked` regenerates and copies the translation into
  `translation/` (needs the game and Ghidra listings); plain builds need neither.

- Vulkan on this Mac: `brew install molten-vk vulkan-loader vulkan-headers shaderc`
  (and `vulkan-validationlayers` for validation). The Homebrew loader is not on
  the dynamic linker's path; the backend finds `/opt/homebrew/lib/libvulkan.1.dylib`
  itself (`POP_VULKAN_LIBRARY` overrides). Validation runs need
  `DYLD_LIBRARY_PATH=/opt/homebrew/lib VK_LAYER_PATH=$(brew --prefix vulkan-validationlayers)/share/vulkan/explicit_layer.d POP_GPU_VALIDATE=1`.
- `POP_GPU_BACKEND=metal|vulkan` picks the backend; `POP_GPU_TRACE=1` prints one
  line per commit from the Vulkan backend. `python3 tools/recomp/shaders.py compile`
  regenerates `gpu/vulkan/shaders_spv.h` after editing GLSL (glslc from shaderc);
  `shader_drift_check` compares only when the same glslc version is installed.
- `check_binaries` builds the test binaries only; rebuild `pop_smoke`, `PopRecomp`
  and `pop_headless` explicitly (`cmake --build --preset macos --target pop_smoke`)
  before a game run, or the run uses a stale host.
- Metal vs Vulkan frame comparison: `tools/recomp/compare_frames.py a.ppm b.ppm`
  over dumps from `POP_RECOMP_SCRIPT=tools/recomp/smoke/flyby.script`.

- This checkout links the game and listings from sibling directories:
  `original/gog -> ../pop-metal/original/gog`, `analysis -> ../populous-recomp/analysis`.
  The `analysis` symlink shows as untracked; never `git add` it.
- Game-backed suites: `tools/test.py --native`, `--mods`, `--gameplay`,
  `sh src/recomp/host/tests/integration_tests.sh`, `sh tools/recomp/mods_test.sh`,
  `.venv/bin/python tools/recomp/tests/test_translate.py -n 50`.
- Pre-existing failures on the parent commit 51b945f, verified 2026-09-12, not
  regressions: integration_tests.sh's parity block (calls a missing
  `tools/recomp/parity.py`), its empty-mods F10 check, its pinned-clock repeatability
  check (0..450 differing QMixer calls per pair on this machine, both builds);
  mods_test.sh's "five mods" check (six load, including core.display), "run record
  names every loaded mod", "every api call the documentation names exists".
- Windows and Linux are verified only in CI (plus Linux in Docker); macOS is the only
  playable platform until sub-project 3.

## Log

- 2026-09-12: Windows 5-10 fps report traced to synchronous Vulkan uploads
  (~1000 fence waits/s measured on the Mac flyby); fixed with a pending
  transfer buffer; CrossOver run at the front end shows zero blocking calls.

- 2026-09-12: sub-project 4 implemented on `release-pipeline`; CI green with the
  tracked translation; Windows build verified under CrossOver.

- 2026-09-12: sub-project 3 implemented on `vulkan-hosts` (spec, plan, nine tasks);
  CI green on all three platforms; Vulkan game runs match Metal on this Mac.

- 2026-09-12: sub-project 1 designed, implemented, merged, pushed; CI green. Game-backed
  verification done locally (see the plan's execution notes). Sub-project 2 spec approved.
- 2026-09-12: sub-project 2 implemented (commits 1b35ca9..edb9bff + Task 10); every host file outside `gpu/metal/` is C++; suites green except the pre-existing fixture/pinned-A/B checks.
