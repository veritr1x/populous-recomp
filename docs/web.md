# Browser build and optional streaming

Use this repository's pinned `kit/`, your own hash-matched installation, and
the preparation/analysis steps in [CONTRIBUTING.md](../CONTRIBUTING.md).
Install the kit's hashed Python requirements and activate Emscripten 6.0.11.
On Windows, use `.venv/Scripts/python.exe` instead of `.venv/bin/python`.

```sh
.venv/bin/python tools/build.py --target web --regenerate --jobs 4
.venv/bin/python kit/tools/web_launcher.py --game-dir "$PWD" --out build/web-stream-site \
    --web-build populous=build/web/recomp --stream populous --serve 2028
```

After the first translation, omit `--regenerate` when translator inputs have
not changed. Open `http://127.0.0.1:2028/` and choose **Stream game**.
The command stages a private installation and manifest into the site. It
preserves executable/module hash checks, bundle exclusions and prepared
auxiliary modules. Original installation files are unchanged.

If analysis uses a minimal executable copy, select the full installation
explicitly: replace `--stream populous` with
`--stream "populous=/path/to/full installation"`. That directory must contain
the configured executable with its pinned hash and required data folders.
This changes the staging source only, preserving configured analysis inputs.
Keep generated game data and all local evidence private.

The worker mounts game files read-only at `/data`, using 4 MiB HTTP ranges
and a 64 MiB download cache. This is a cache bound, not total browser memory.
The kit supplies a corrected fetch backend without modifying the installed
SDK. Saves and settings use a separate writable OPFS profile.

**Import folder**, ZIP import and **Play** remain available. Play uses an
imported installation; Stream game selects hosting explicitly. Direct player
URLs prefer imported data when available. Both modes share saves on the same
origin. For games with prepared auxiliary modules, import a prepared bundle
with the pinned modules at their configured names; the staged `data/` folder
is such a bundle. A raw install may keep those modules elsewhere.

To prepare an import-only site, use a separate `--out` directory and omit
`--stream`. Previously staged assets are not removed by omitting the flag.
The server supplies HEAD, byte ranges, Wasm MIME and COOP/COEP. Other hosting
must preserve those semantics and same-origin asset URLs; files larger than
64 MiB require ranges. Streaming has no persistent offline asset cache.

Keep the hostname and port consistent to reuse saves. Export saves from
**Manage** before clearing site storage. Browser compatibility results are
recorded below; compilation alone does not establish gameplay.

## Local validation

Updated to recomp-kit `1cbd81ca8e6e1ef2cf33f1c27a06f69a00e64b82`, including
upstream main, optional streaming, the corrected fetch backend, and shared
fixed-function WGSL/empty-transfer fixes. The real Emscripten 6.0.11 game
build succeeds; this was not a stub build.

On Windows/Chromium 145 with hardware WebGPU, streaming mounted 4,670 files
and rendered menus and the 3D solar-system level selection. Testing exposed a
shared coverage-output alpha mismatch; the updated kit fixes it, and all six
representative WebGPU pipeline variants then validate. Starting a level still
aborts with `legacy replay failed for frame` followed by `Aborted()`.
No failed asset request or WebGPU validation error accompanies that failure.
The browser backend's unsupported synchronous GPU readback is a candidate,
not a proven diagnosis. This is not a gameplay pass. Five portable game tests
pass; level-start compatibility remains unresolved.

These are bounded local checks, not a full playthrough or performance test.
The 64 MiB limit applies only to the download cache. Local build logs,
screenshots and browser diagnostics remain in ignored directories.
