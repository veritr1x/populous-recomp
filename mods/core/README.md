# Built-in mods

Each immediate child directory is a mod with a `mod.toml`. Task 13 supplies
`display/` (`core.display`, `affects_simulation = false`); this packaging task
ships an empty root until then.

Every host build invokes `build_core.sh`. It copies this tree to staging,
compiles each `<mod>/*.c` to a same-named dylib using the example mod toolchain,
checks manifest plugin paths, and installs the tree only after successful
compilation. Build products stay out of the source tree. Plugin filenames must
match their C source basename; scripts and assets are copied recursively.

The app installs to `build/PopRecomp.app/Contents/Resources/mods/core` and
resolves it relative to its executable. Headless, smoke and parity install to
`build/recomp/mods/core`. `POPM_CORE_MODS_DIR` overrides discovery, while
`POPM_MODS_DIR` continues to select the user root (default `mods`).
`POPM_NO_MODS` disables both, including direct loader calls.

Discovery visits core before user, so the first core manifest wins duplicate
ids. Core mods load in dependency order before any user mod. Core cannot
require a user mod; users may require core mods. Conflicts favor the earlier
mod in this load order. A broken core mod is reported normally and unrelated
user mods still load. A missing core directory is an empty root.

Run `mods/core/build_core.sh` to install for headless hosts, or pass the
absolute app core destination above. The script acquires the shared build
lock itself; callers already holding it reuse that lock.

Core plugins retain their required `LC_UUID` and use `ZERO_AR_DATE=1`,
`-Wl,-reproducible` when supported (checked by a link probe), `-g0`, `-Wl,-S`
to strip the debug map, and a stable `@rpath` install name. Deterministic
installed bytes keep the existing whole-directory payload hash stable. This
policy applies to every core plugin. Gate B builds core with the parity host,
passes `--reuse-core` to the smoke host build, and runs fixtures with
`--no-build`; Gate C reuses that installation too. Standalone host builds,
including the one in `mods_test.sh`, still install core normally.

`src/recomp/mods/build_tests.sh` also runs `tests/reproducibility_tests.sh`:
two real core builds in an isolated tree must have identical installed bytes
and the same `core.display` payload hash, even after source mtimes change.
The suite also builds core once and loads the installed `display/display.dylib`
through the production loader, requiring a successful `POP_OK` load status.
