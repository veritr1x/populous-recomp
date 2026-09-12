# Translator and low-level tools

The contributor entry points are [setup.py](../setup.py), [build.py](../build.py)
and [test.py](../test.py). They check prerequisites and select the commands below.
Use those wrappers for an ordinary first build.

| Tool | Responsibility |
| --- | --- |
| `translate.py` | Decode instruction listings, resolve dispatch entries and emit C plus symbols |
| `runtime/x86.h` | Register/flag/x87 state and instruction semantics used by generated code |
| `symbols/` | Reviewed metadata for stable hooks and guest globals |
| `build.sh`, `buildlock.sh` | Generate/compile one consistent archive under a shared process lock |
| `app_build.sh`, `smoke_build.sh`, `headless_build.sh` | Link the same translated archive with the chosen host |
| `mode_probe.py`, `mode_probe.sh` | Exercise candidate modes with isolated settings and explicit evidence |
| `smoke/` | Scripted menu, graphics and gameplay scenarios |
| `texture_pack.py`, `terrain_detail.py`, `package_texture_pack.py` | Prepare and package local texture inputs |
| `presentation_smoke.py`, `performance_run.py` | Bounded presentation measurements and diagnostics |
| `oracle.py`, `parity_build.sh` | Low-level deterministic capture helpers for original/translated comparisons |
| `tests/` | Differential instruction tests and tooling regressions |

Generated functions preserve addresses, symbol names and instruction comments.
They are a mechanical translation, so register operations are expected. Add
meaningful names in reviewed symbol metadata and comments in the translator;
do not hand-edit generated chunks or publish them.

A normal translation needs the exact executable and listings prepared by setup.
`build.sh` publishes its generated directory and archive together. Tools that
read both during a run must hold the same build lock; this prevents combining
functions from one generation with a dispatch table from another.

The deterministic capture helpers are diagnostic building blocks. Their pinned
clocks and manually constructed level startup do not replace the native gameplay
suite or establish real-time performance. See [Testing](../../docs/testing.md).
