# Contributing

This repository holds Populous: The Beginning's configuration, mods, artwork,
smoke scripts and docs on top of [recomp-kit](https://github.com/veritr1x/recomp-kit),
the submodule at `kit/`. Runtime, translator, host and tooling changes go to
the kit; open an issue there before a large architecture change. Here, useful
areas are mods and mod examples, the smoke scripts, curated symbols in
`globals.toml`, artwork and documentation.

## Prerequisites

- Python 3.9 or later; create `.venv` and install `kit/requirements-dev.txt`.
- Native builds on macOS: Apple Silicon, Xcode Command Line Tools and Git. CMake
  and Ninja come from the requirements file. iPad builds need Xcode with the
  iOS SDK and a developer team.
- First translation: [Ghidra 12.1.3](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.3_build)
  and a Java runtime compatible with it (tested with OpenJDK 26.0.1; set
  `JAVA_HOME` to the JDK directory).
- Your own supported GOG installation of Populous: The Beginning.

The executable must be `D3DPopTB.exe` with SHA-256:

```text
815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd
```

The loader refuses other binaries because translated addresses and data layouts
are tied to this image. Do not bypass the hash to add support for another version.

## Prepare your game installation

Building the app, running it, the game-backed suites and regenerating the
translation all require this step; generated code is never tracked.

Use a directory containing the executable and its `data`, `levels`, `objects`
and `sound` directories. Paths containing spaces are supported when quoted.

```sh
.venv/bin/python tools/setup.py \
  --install "/path/to/your/Populous installation" \
  --ghidra-home "/path/to/ghidra_12.1.3_PUBLIC" \
  --java-home "/path/to/your/jdk/Contents/Home"
```

Setup verifies the executable, links the installation at ignored `original/gog/`,
fetches the pinned [pop3-rev annotation metadata](https://github.com/hrttf111/pop3-rev/tree/60408e4e99b76ab2e5461897c8e1b33756360eaa)
into ignored `analysis/annotations`, and exports translation inputs into
ignored `analysis/decompiled`. It does not download the game. Existing links
to another installation and dirty metadata checkouts are preserved and
reported. The first export can take several minutes. `--link-only` validates
the game link without running Ghidra.

## Build and run

```sh
.venv/bin/python tools/build.py --regenerate --jobs 8   # first time, and after translator changes in the kit
.venv/bin/python tools/build.py --jobs 8                # afterwards
open build/PopRecomp.app
```

`--target smoke` builds the offscreen scripted host, `--target headless` the
minimal boot host, `--target fixture` the parity fixture and `--target plugins`
every mod plugin under `mods/`. `--target ios` builds, signs and installs the
iPad app (`RECOMP_IOS_TEAM` or `--team`). The CMake tree lives in
`build/cmake/<preset>`; every artifact keeps its documented path under `build/`.
`RECOMP_PROFILE_DIR` selects a separate profile for an interactive run.

## Check your change

```sh
.venv/bin/python tools/test.py             # the kit's portable suites; no game files required
.venv/bin/python -m pytest -q tests        # this repository's config tests
.venv/bin/python tools/test.py --native    # runtime, DirectX and Metal tests against the game
.venv/bin/python tools/test.py --mods      # build the app first; real game-backed mod tests
.venv/bin/python tools/test.py --gameplay  # build the app first; isolated native Options/gameplay run
.venv/bin/python tools/test.py --integration  # roots, plugins, headless, smoke and fixture runs
.venv/bin/python kit/tools/format.py       # handwritten native code style (kit sources)
```

The test runner describes missing prerequisites rather than silently skipping a
suite. See [docs/testing.md](docs/testing.md).

## Updating the kit

`git -C kit checkout <commit>` then commit the submodule pointer here, with a
changelog line naming what changed. Keep the pin on a kit tag when one exists.
