# Built-in mods

Each immediate child directory is a mod with a `mod.toml`. Task 13 supplies
`display/` (`core.display`, `affects_simulation = false`); this packaging task
ships an empty root until then.

Every host build runs `tools/recomp/build_core.py` through the `core_mods` CMake
target. It copies this tree to staging, compiles each `<mod>/*.c` to a same-named
plugin with this platform's extension,
checks manifest plugin paths, and installs the tree only after successful
compilation. Build products stay out of the source tree. Plugin filenames must
match their C source basename; scripts and assets are copied recursively.

The app installs to `build/PopRecomp.app/Contents/Resources/mods/core` and
resolves it relative to its executable. Headless, smoke and parity install to
`build/recomp/mods/core`. `RECOMP_CORE_MODS_DIR` overrides discovery, while
`RECOMP_MODS_DIR` continues to select the user root (default `mods`).
`RECOMP_NO_MODS` disables both, including direct loader calls.

Discovery visits core before user, so the first core manifest wins duplicate
ids. Core mods load in dependency order before any user mod. Core cannot
require a user mod; users may require core mods. Conflicts favor the earlier
mod in this load order. A broken core mod is reported normally and unrelated
user mods still load. A missing core directory is an empty root.

Run `.venv/bin/python tools/build.py --target plugins` to install for headless
hosts; the app target installs into its own bundle.

Core plugins retain their required `LC_UUID` and use `ZERO_AR_DATE=1`,
`-Wl,-reproducible` when supported (checked by a link probe), `-g0`, `-Wl,-S`
to strip the debug map, and a stable `@rpath` install name. Deterministic
installed bytes keep the existing whole-directory payload hash stable. This
policy applies to every core plugin. Every host build installs core normally,
including the one in `mods_test.sh`.

`tools/recomp/tests/test_build_core.py` (a `nogame` CTest entry) proves the
installer: two real core builds in an isolated tree must have identical installed
bytes even after source mtimes change, a compile failure or a missing plugin
preserves the previous install, and the source tree stays clean. The mods suite
also loads the installed `display` plugin through the production loader,
requiring a successful `POP_OK` load status.
