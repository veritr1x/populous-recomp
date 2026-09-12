# Contributing

Start with a small change you can explain and verify. Useful areas include input
and window behavior, rendering correctness, audio scheduling, documentation,
portable tests and mod examples. Open an issue before a large architecture change.

## Prerequisites

- Python 3.9 or later; create `.venv` and install `requirements-dev.txt`.
- Native builds: Apple Silicon macOS, Xcode Command Line Tools and Git.
- First translation: [Ghidra 12.1.3](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.3_build).
- A Java runtime compatible with that Ghidra distribution. The documented setup
  was tested with OpenJDK 26.0.1; set `JAVA_HOME` to the JDK directory.
- Your own supported GOG installation of Populous: The Beginning.

The executable must be `D3DPopTB.exe` with SHA-256:

```text
815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd
```

The loader refuses other binaries because translated addresses and data layouts
are tied to this image. Do not bypass the hash to add support for another version.

## Prepare your game installation

Use a directory containing the executable and its `data`, `levels`, `objects`
and `sound` directories. Paths containing spaces are supported when quoted.

```sh
.venv/bin/python tools/setup.py \
  --game-dir "/path/to/your/Populous installation" \
  --ghidra-home "/path/to/ghidra_12.1.3_PUBLIC" \
  --java-home "/path/to/your/jdk/Contents/Home"
```

Setup verifies the executable, links the installation at ignored `original/gog/`,
fetches the pinned [pop3-rev annotation metadata](https://github.com/hrttf111/pop3-rev/tree/60408e4e99b76ab2e5461897c8e1b33756360eaa),
and exports translation inputs into ignored `analysis/`. It does not download the
game. Existing links to another installation and dirty metadata checkouts are
preserved and reported. The first export can take several minutes.

If inputs already exist, `--link-only` validates the game link without running
Ghidra. `GHIDRA_HOME` and `JAVA_HOME` can supply the tool paths instead of flags.

## Build and run

```sh
.venv/bin/python tools/build.py --jobs 8
open build/PopRecomp.app
```

The first build translates and compiles the original functions. Subsequent builds
reuse that archive and rebuild the handwritten host. After editing instruction
translation or its C helpers, regenerate explicitly:

```sh
.venv/bin/python tools/build.py --regenerate --jobs 8
```

Use `--target smoke` for the offscreen scripted host or `--target headless` for
the minimal boot host. Build outputs and your default writable profile stay in
`build/`. `POPM_PROFILE_DIR` selects a separate profile for an interactive run.
Keep the app in the checkout; moving it requires explicitly configuring its game path.

## Check your change

```sh
.venv/bin/python tools/test.py           # No game files required
.venv/bin/python tools/format.py         # Check handwritten C/C++/Objective-C
.venv/bin/python tools/test.py --native  # Runtime, DirectX and Metal tests on macOS
.venv/bin/python tools/test.py --mods    # Build the app first; real game-backed mod tests
.venv/bin/python tools/test.py --gameplay # Build the app first; isolated native Options/gameplay run
```

The test runner describes missing prerequisites rather than silently skipping a
requested suite. Detailed limits and the manual input/audio checklist are in
[Testing](docs/testing.md). Save files and logs from tests use ignored scratch
profiles; do not attach original game files or personal saves to issues.

## Code conventions

- Use descriptive names in handwritten code and keep functions focused on one job.
- Comment each major function's purpose, significant inputs/outputs, ownership,
  failure behavior and thread assumptions. Explain unusual arithmetic or layout
  constraints where they occur. Avoid comments that merely restate the name.
- Run `.venv/bin/python tools/format.py --write` for native code. Do not format
  `third_party/` or generated game code; their upstream/generated layout is intentional.
- Keep guest addresses as 32-bit values. Use the memory helpers rather than
  casting guest addresses into host pointers. See [Architecture](docs/architecture.md).
- Preserve simulation timing independently of render rate. Document whether a
  measurement describes simulation, GPU completion or frames actually displayed.
- Add focused regression coverage for behavior changes. A screenshot or counter
  alone does not establish playable-game correctness.
- Keep generated output, original game files, SDKs, credentials and private run
  artifacts out of commits. `tools/check_repo.py` checks tracked publication inputs.

## Submit a pull request

Fork the repository, create a descriptive branch, and keep the change focused.
Describe the problem, resulting behavior, relevant implementation decision and
what you tested. Include a screenshot for UI changes and system/resolution details
for performance reports. Update **Unreleased** in [CHANGELOG.md](CHANGELOG.md)
for user-visible changes. Pull requests run checks without publishing game assets.

Contributions to the handwritten implementation use [LICENSE](LICENSE). Preserve
upstream notices; do not add assets or source you do not have permission to contribute.
