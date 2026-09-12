# Multi-platform port: progress tracker

Read this first if you are picking the work up cold. It says what is done, what
is in flight, where the documents are, and how to verify. Keep it current: update
the "Now" section at every milestone and commit it with the work.

## Roadmap (decided 2026-09-12)

| # | Sub-project | Spec | Plan | State |
| --- | --- | --- | --- | --- |
| 1 | Portable build system + `os.h` platform layer | `specs/2026-09-12-portable-build-system-design.md` | `plans/2026-09-12-portable-build-system.md` | **Done.** Merged to `main` (commits 3fa44f4..920de70), CI green on macOS, Ubuntu, Windows. |
| 2 | Host abstraction: GPU device interface (Metal first), SDL3 window/input on macOS, portable audio mixer + TinySoundFont MIDI | `specs/2026-09-12-host-abstraction-design.md` | `plans/2026-09-12-host-abstraction.md` | **Done.** Merged to `main` (commits 1b35ca9..0e862a5), CI green on macOS, Ubuntu, Windows. The manual window checklist is still for a person to run. |
| 3 | Vulkan backend + Windows/Linux hosts (presets, CI labels) | not yet | not yet | Not started. |
| 4 | Release pipeline with the translation embedded; player points at their own D3DPopTB.exe | not yet | not yet | Not started. |

Decisions that must not be reopened without the user: clang only (no MSVC); SDL3 on
every platform including macOS; one software mixer everywhere (AVAudioEngine and the
Apple DLS synth retired); TinySoundFont vendored for MIDI; Metal on Apple, Vulkan on
Windows/Linux/Android, WebGPU if web is ever attempted; device-level GPU interface
(renderer, compositor, presenter and overlay are portable C++ over it).

## Now

- Sub-project 2 merged to `main` at 0e862a5; branch deleted locally and on origin.
- Plan: `plans/2026-09-12-host-abstraction.md`, all ten tasks executed inline;
  execution notes at the end of the plan record every deviation.
- What a person still has to do: launch `build/PopRecomp.app` and run the
  window checklist (front end renders; mouse and keyboard reach the game;
  Escape releases capture; window modes 0/1/2 from the settings page; resize;
  focus loss; Cmd-Q quits with the close report).
- CI: green on macOS, Ubuntu and Windows at 39bb72d (run 34692907811). The Linux job now installs SDL3's documented build dependencies.
- Next sub-project: 3 (Vulkan backend behind `gpu/gpu.h`, Windows/Linux hosts
  over the same SDL host, presets and CI labels). Start with brainstorming.

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

1. `git checkout host-abstraction && git log --oneline main..HEAD`.
2. Read the spec, then the plan; the plan has checkboxes per step. Find the first
   unchecked step. Each task ends with a commit, so `git log` shows how far it got.
3. Build and test: `.venv/bin/python tools/build.py` (needs the game, see below),
   `.venv/bin/python tools/test.py --compile-only`, `.venv/bin/ctest --preset macos -L "nogame|gpu|device"`.
4. Push the branch and run CI with `gh workflow run checks.yml --ref host-abstraction`;
   confirm the run's `headSha` matches `HEAD` before trusting it.

## Environment notes

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

- 2026-09-12: sub-project 1 designed, implemented, merged, pushed; CI green. Game-backed
  verification done locally (see the plan's execution notes). Sub-project 2 spec approved.
- 2026-09-12: sub-project 2 implemented (commits 1b35ca9..edb9bff + Task 10); every host file outside `gpu/metal/` is C++; suites green except the pre-existing fixture/pinned-A/B checks.
