# Testing

Run checks appropriate to your change. Every suite's output belongs under ignored
`build/`; requested tests must report failure rather than silently skip prerequisites.
Every command is a wrapper around the kit's `kit/tools/test.py` with this
repository as the game directory.

| Command | What it checks | Needs game files? |
| --- | --- | --- |
| `tools/test.py` | The kit's portable Python suites (setup, config, texture and display-mode tooling) | No |
| `python -m pytest -q tests` | This repository's `game.toml` renders the hooks and globals the kit expects | No |
| `tools/test.py --compile-only` | Every native test binary this platform has compiles | No |
| `tools/test.py --native` | Runtime, adapters, offscreen Metal and UI tests; the `game`-labelled suites load the image | Yes for the `game` label |
| `tools/test.py --mods` | Real loader/hooks/settings/native Options and replay contracts | Yes, plus translated archive |
| `tools/test.py --gameplay` | Menu navigation, mode cycling, selection, movement and clean exit (`smoke/native-options.script`) | Yes, plus translated archive |
| `tools/test.py --integration` | The host integration script: roots, plugins, headless, smoke and fixture runs | Yes, plus translated archive |

Native suites are CTest entries with labels: `nogame` runs everywhere and in the
kit's CI, `game` needs your installation, `gpu` needs a Metal device, `mods`
needs the translated archive and the entity snapshot `tools/test.py --mods`
captures. Run one directly with `.venv/bin/ctest --test-dir build/cmake/macos -L nogame`
or `-R dx_tests`.

The mod suite first builds and runs a deterministic 32-frame entity capture
from your own game in an isolated directory under `build/tests`; no saved
capture from another checkout is needed. The gameplay run keeps its
diagnostics under `build/gameplay`.

## On the iPad

`tools/build.py --target ios --console` installs and streams the console;
`RECOMP_TRACE_POINTER=1` in the launch environment (see the kit's
`tools/build.py --help`) traces pointer placement. `tools/ios_logs.py` pulls
the app's Documents.


## iPad runtime update, 2026-09-21

Kit `9e97515` includes the merged Metal Fatigue runtime and iPad input/display
fixes. Five game configuration tests passed. The translated smoke host and
signed iOS app built, and the updated app was installed in place on the iPad;
a screenshot confirmed its main menu and on-screen controls.

The full options/gameplay script did not pass: unit selection, movement and
state changes were observed, but the terrain-detail counter stayed at zero and
the resolution sequence stayed at 640x480. Initializing the smoke host's native
settings handler restored scripted F10 access, without fixing those remaining
checks. Do not treat this run as a full gameplay/options pass.

Ignored evidence: `build/ipad-update-config-tests.log`,
`build/ipad-update-gameplay.log`, `build/ipad-update-gameplay-fixed.log`,
`build/gameplay/options-4mjvx6xh/`, `build/ipad-update-ios-final-build.log`,
`build/ipad-update-install.json`, and `build/ipad-update-device.png`.


The final pin is `bbdc3b0`, which refreshes exclusive DirectDraw windows on movie
mode switches. All five configuration tests passed. Native smoke and signed iOS
builds succeeded, and the iPad app was updated in place again. A fresh isolated
profile running the integration script at 640x480 reached the level, rendered
terrain and units, opened settings and exited successfully. The composite level
image was visually inspected. This does not resolve the separate full gameplay
suite's terrain-detail and resolution assertions above.
Evidence: `build/display-mode-smoke/host.log`, `build/display-mode-smoke/level.png`,
`build/display-mode-ios-build.log` and `build/display-mode-install.json`.
