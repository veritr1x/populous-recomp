# Portable build system and runtime

Sub-project 1 of the multi-platform port. Date: 2026-09-12.

## Roadmap context

The port has four sub-projects, each with its own spec and plan. Decisions made
for the whole port, so later specs do not reopen them:

| Decision | Choice |
| --- | --- |
| Distribution model | Release binaries embed the compiled translation. Players point the binary at their own `D3DPopTB.exe`; the SHA-256 gate stays. This changes the repository's publication policy and NOTICE when sub-project 4 lands. |
| Order | 1. Portable build system and runtime. 2. Host abstraction: refactor the macOS host behind window, GPU, audio and input interfaces with Metal as the first backend. 3. SDL3 window/input/audio plus a Vulkan GPU backend for Windows and Linux. 4. Release pipeline. Android, iOS and web are later work. |
| Renderers | Metal on macOS and later iOS. Vulkan on Windows, Linux and later Android. WebGPU when web is attempted. |
| Platform layer for new hosts | SDL3, fetched and pinned by CMake, not vendored. |
| Compiler | Clang on every platform. MSVC is not supported. |

This document covers sub-project 1 only.

## Goal

One CMake build that produces every artifact the shell scripts produce today
on macOS, with identical behavior, and compiles the runtime, DirectX shims and
mod layers on Linux and Windows. The Python entry points keep their command
lines. The shell build scripts are removed.

After this sub-project macOS is still the only playable platform. Linux and
Windows are compile-and-test targets for the portable layers only.

## Non-goals

- No new host, renderer, window, input or audio code.
- No change to test logic; only to how test binaries are built and run.
- No release packaging.
- No change to the translator's output or to `x86.h`.

## Facts the design rests on

- Every host callback in `src/recomp/dx/host_api.h` has a weak no-op default in
  `host_api.cpp`. Hosts and tests define a subset strongly and rely on the weak
  defaults for the rest. The DX tests define present and D3D callbacks and use
  weak audio and input.
- The runtime and the emitter in `tools/recomp/translate.py` use
  `__attribute__((weak))`, `__builtin_expect`, `__atomic_load_n` and
  `__has_include`. MSVC supports none of these; clang-cl supports all of them.
- The generated archive is linked with `-force_load` so unreferenced members
  survive. A CMake object library links the objects directly and needs no
  linker flag.
- Guest threads are real OS threads created with an explicit stack size
  (`pthread_attr_setstacksize`) under a cooperative baton. `std::thread` cannot
  set a stack size, so thread creation needs a platform wrapper.
- Direct POSIX use outside the `.mm` files is small: pthread mutex, condition
  variable and thread calls in `kernel32.cpp`, `host_services.cpp` and
  `run_record.cpp`; `mmap` in `memory.cpp`; `dlopen` in `mods/loader.cpp` and
  tests; `dirent` and `stat` in `kernel32.cpp`, `overlay.cpp`, `run_record.cpp`
  and `snapshot.cpp`; `_NSGetExecutablePath` in `roots.cpp` and `main.mm`;
  `signal()` in `boot.cpp`.
- The mod API is a function table passed to `pop_mod_init`, so plugins do not
  need undefined symbols against the host executable. Windows DLLs work
  without exporting anything from the executable.
- `long` appears 73 times in first-party native code outside tests. Windows
  keeps `long` at 32 bits.
- `tools/recomp/build.sh` protects one invariant: `build/recomp/gen` and the
  compiled archive always describe the same generation. Everything is staged
  and published by rename, under a process lock shared by every script that
  reads or writes `build/recomp`.

## Design

### Layout

```
CMakeLists.txt                       project, options, per-toolchain flags
CMakePresets.json                    macos, linux, windows and -debug variants
cmake/Translate.cmake                recomp_gen object library and POP_TRANSLATE
cmake/Warnings.cmake                 the warning sets the scripts use today
cmake/MacBundle.cmake                Info.plist, resources, ad-hoc codesign
src/recomp/platform/os.h             the platform layer's interface
src/recomp/platform/os_posix.cpp     macOS and Linux implementation
src/recomp/platform/os_win32.cpp     Windows implementation
src/recomp/runtime/CMakeLists.txt    recomp_runtime
src/recomp/dx/CMakeLists.txt         recomp_dx
src/recomp/mods/CMakeLists.txt       recomp_mods, lua
src/recomp/host/CMakeLists.txt       host_common, host_macos, executables
mods/core/CMakeLists.txt             core plugins
src/recomp/mods/tests/fixtures/CMakeLists.txt   fixture plugins
```

Each directory's `CMakeLists.txt` owns the targets for the code in that
directory, so a reader finds a target beside its sources.

### Targets

| Target | Kind | Sources | Platforms |
| --- | --- | --- | --- |
| `recomp_platform` | static | `src/recomp/platform/` | all |
| `recomp_runtime` | static | `src/recomp/runtime/*.cpp` except `fixture.cpp` and tests | all |
| `recomp_dx` | static | `src/recomp/dx/*.cpp` including `host_api.cpp` | all |
| `lua` | static | `third_party/lua/*.c` minus `lua.c` and `luac.c` | all |
| `recomp_mods` | static | `src/recomp/mods/*.cpp`, `src/recomp/mods/lua/*.cpp` | all |
| `recomp_gen` | object | `build/recomp/gen/chunk_*.c`, `table.c` | all, when `POP_TRANSLATE` is not OFF |
| `host_common` | static | the portable host files: `boot.cpp`, `report_lock.cpp`, `input_gate.cpp`, `present_pixels.cpp`, `page_overlay.cpp`, `audio_math.cpp`, `audio_capture.cpp`, `script.cpp`, `src/backends/cpu/indexed_frame.cpp` | all |
| `host_macos` | static | every `.mm` in `src/recomp/host/` | macOS |
| `PopRecomp` | app bundle | `main.mm` plus the above | macOS |
| `pop_headless` | executable | `headless_main.cpp` | macOS |
| `pop_smoke` | executable | `smoke_main.mm` | macOS |
| `pop_fixture` | executable | `fixture.cpp`, `snapshot.cpp`, built with `RECOMP_NULL_HOST` | all |
| `pop_fixture_trace` | executable | as `pop_fixture` with `POP_TRACE_DIR` wrappers | all, optional |
| `core_mods` | custom | one MODULE library per `mods/core/*/*.c` plus copied `mod.toml` and Lua | all |
| `mod_fixtures` | custom | one MODULE library per `src/recomp/mods/tests/fixtures/*.c`; `old_cpu` gets the old header path first | all |
| `runtime_tests`, `dx_tests`, `mods_tests`, `present_events_tests`, `host_tests`, `compositor_tests`, `ui_layer_tests`, `profile_tests` | test executables | as the current scripts list them | per suite |
| `texture_pack` | custom | `terrain_detail.py` then `package_texture_pack.py` | all |
| `null_host_link` | executable | `recomp_dx` and `host_common` with no strong callbacks | all |

Compile definitions that select behavior today stay as target properties:
`POPM_PRESENT_HAS_UI_LAYER=1` where `ui_layer.mm` is linked, `POPM_TESTING=1`
on the mods tests, `RECOMP_NULL_HOST` on the parity fixture and profile tests.
`host_api.h` is still compiled once as C11 and once as C++17 by a
`host_api_header_check` target, as the DX test script does.

The app bundle keeps `POP_RECOMP_APP_NAME` as a cache variable: a different name
produces a second bundle with its own identifier, as `app_build.sh` does. The
core mods install root is `<bundle>/Contents/Resources/mods/core` for the app and
`build/recomp/mods/core` for the other hosts, exactly as today.

### Generated code

`recomp_gen` is an object library. Its sources are globbed from
`build/recomp/gen` with `CONFIGURE_DEPENDS`, so a regeneration that changes the
set of chunks is picked up by the next build without an explicit reconfigure.
Executables that need the game link `recomp_gen` as objects, which keeps every
`fn_` definition and removes the need for `-force_load` or `--whole-archive`.

`POP_TRANSLATE` is a cache option with three values:

- `AUTO` (default): use `build/recomp/gen` if it exists; otherwise configure
  without game targets and print which targets are unavailable.
- `ON`: require `build/recomp/gen`; fail configure if absent.
- `OFF`: never define game targets. CI uses this.

The generated directory stays at `build/recomp/gen` and the symbols index at
`build/recomp/symbols.json`, because the parity snapshot tool, the mods tests
include path and the hookable-symbol consumers already use those paths. The
CMake binary directory is `build/cmake/<preset>`; final artifacts are copied or
written to their current locations (`build/PopRecomp.app`, `build/recomp/pop_*`,
`build/recomp/*_tests`, `build/recomp/mods/core`, `build/recomp/mods-fixtures`,
`build/texture-pack`) so every downstream script and document path stays valid.

### Translation step

`tools/build.py --regenerate` owns translation. Under the build lock it:

1. Runs `translate.py --out <stage>` where `<stage>` is
   `build/recomp/gen.new.<pid>`, with `--report build/recomp/translate-report.json`.
2. Copies `tools/recomp/runtime/x86.h` beside the staged sources.
3. Renames the existing `gen` to `gen.old`, the stage to `gen`, publishes
   `symbols.json` by rename, and removes `gen.old`.
4. Runs the CMake build, which recompiles `recomp_gen` from the new sources.

A failure in step 1 or 2 removes the stage and leaves the published `gen`
untouched. Because the objects live in the CMake binary directory and are
rebuilt from `gen` under the same lock, a torn pair cannot be published.

The `--snapshot DIR` verb of `parity_build.sh` becomes
`tools/recomp/snapshot_gen.py DIR`: copy `gen` under the lock and verify that
every prototype in `funcs.h` has a definition in the chunks, retrying as today.
The `--trace DIR` verb becomes the CMake cache variable `POP_TRACE_DIR`; when set,
`pop_fixture_trace` compiles the snapshot in that directory with the caller's
`trace_override.h` force-included and links the caller's `trace_wrappers.c`.

### Build lock

`tools/build.py` and `tools/test.py` hold an advisory lock on
`build/recomp/.lock` for the duration of any configure, build, translate or
game-backed test. POSIX uses `fcntl.flock`; Windows uses `msvcrt.locking`. The
lock is released by the kernel when the holder exits however it exits. The
`BUILDLOCK_HELD=1` environment convention stays so a script started under the
lock does not try to take it again.

The lock exists because several contributors and agents build concurrently in
one checkout and Ninja does not serialize concurrent invocations. CMake's
dependency graph handles ordering within one invocation.

### Platform layer

`src/recomp/platform/os.h` is a C++ header with `extern "C"` linkage so
plain-C consumers can include it. It provides:

| Area | Functions | POSIX | Win32 |
| --- | --- | --- | --- |
| Threads | `os_thread_create(fn, arg, stack_bytes)`, `os_thread_join`, `os_thread_detach`, `os_thread_exit`, `os_thread_self`, `os_thread_equal` | pthread | `_beginthreadex` and handles |
| Virtual memory | `os_vm_reserve(bytes)`, `os_vm_release(ptr, bytes)` | `mmap`/`munmap` | `VirtualAlloc`/`VirtualFree` |
| Plugins | `os_dlopen`, `os_dlsym`, `os_dlclose`, `os_dlerror`, `os_plugin_extension()` | dlfcn, `.dylib` or `.so` | `LoadLibraryW`, `.dll` |
| Files | `os_listdir(path, callback)`, `os_stat`, `os_mkdir`, `os_rename`, `os_unlink`, `os_fsync`, `os_ftruncate`, `os_getcwd`, `os_chdir`, `os_access` | POSIX | Win32 with UTF-8 to UTF-16 conversion |
| Process | `os_exe_path()` | `_NSGetExecutablePath` or `/proc/self/exe` | `GetModuleFileNameW` |
| Time | `os_monotonic_ns()`, `os_wall_time_ms()`, `os_sleep_us()` | `clock_gettime`, `gettimeofday`, `usleep` | `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime`, waitable timer |

Mutexes and condition variables move to `std::mutex`, `std::condition_variable`
and `std::unique_lock`. The scheduler's `pthread_cond_timedwait` becomes
`wait_until` on a steady clock. `pthread_mutex_trylock` becomes `try_lock`.

Every existing call site is converted; no `#ifdef` stays in runtime, dx or mods
code except inside `os_posix.cpp` and `os_win32.cpp`. `roots.cpp` loses its
`__APPLE__` block and asks `os_exe_path()`.

The fault reporter in `boot.cpp` uses `signal()` for SIGSEGV, SIGBUS and
SIGABRT. On Windows the `signal_handlers` option logs that fault reporting is
unavailable and installs nothing. A vectored exception handler is sub-project 3
work, when a Windows host exists to be debugged.

The 73 `long` uses are audited during the pthread conversion. Any that hold a
pointer-sized or 64-bit value become `int64_t`, `intptr_t` or `size_t`; the
ones that are time or file offsets are checked against the API they feed.

### Plugins on each platform

Core mods, example mods and test fixtures build as CMake MODULE libraries with
the platform's default extension. The loader resolves the `path` field of
`mod.toml` by replacing its extension with `os_plugin_extension()` when the
written file does not exist, so a manifest that says `logger.dylib` finds
`logger.dll` on Windows. The manifest tests gain one case for that rule. The
old flags `-undefined dynamic_lookup` and `-install_name @rpath/...` and the
reproducible-link probe are expressed as macOS-only target properties; the
reproducibility test for core mods keeps running on macOS.

### Python entry points

`tools/build.py` keeps `--regenerate`, `--target app|smoke|headless` and
`--jobs`, and gains `--preset` (default chosen from the OS) and `--config
Debug|Release` (default Release, matching today's `-O2` app and `-O1` hosts as
per-target flags). It refuses `--target app|smoke|headless` off macOS with the
same message shape it uses today, checks the game inputs as today, takes the
lock, translates when asked or when `gen` is absent, configures, builds, and
runs the texture-pack step when its inputs are newer.

`tools/test.py` keeps every flag. `--native` builds the test targets and runs
CTest with labels `nogame`, `game` and `gpu`; `--compile-only` builds them and
runs nothing. `--mods` and `--gameplay` keep their Python orchestration and
call CMake targets instead of shell scripts. The environment scrub of `POPM_*`,
`POP_RECOMP_*`, `POP_SMOKE_*` and `POP_HOST_*` stays.

The Makefile keeps its verbs and calls the Python entry points.

### CTest labels

- `nogame`: needs neither game files nor a GPU. Runs on every CI platform.
- `game`: loads the guest image or reads game data. Local only.
- `gpu`: uses an offscreen Metal texture. macOS local and macOS CI.

Each existing test binary is inspected during implementation and labeled by
what it opens. The runtime tests open the game image and are `game`. The DX
tests, UI layer tests and compositor tests are candidates for `nogame`; the
compositor tests currently link Metal and are `gpu` until sub-project 2 moves
their arithmetic behind the GPU interface.

### CI

The tooling job is unchanged. The compile job becomes a matrix over
`macos-15`, `ubuntu-24.04` and `windows-2025` with `POP_TRANSLATE=OFF`. Each
runner configures with its preset, builds every target that needs no generated
code, links `null_host_link`, and runs the `nogame` label. The macOS runner also
runs the `gpu` label. Ubuntu installs clang and Ninja; Windows installs LLVM
and Ninja and uses the Windows SDK already on the runner. No game files,
generated code, bundles, profiles or diagnostics are uploaded.

### Documentation

Every mention of a shell build script is replaced with the Python entry point or
the CMake target. Files: `README.md`, `CONTRIBUTING.md`, `AGENTS.md`,
`docs/testing.md`, `docs/code-guide.md`, the boundaries paragraph of
`docs/architecture.md`, `tools/recomp/README.md`, `src/recomp/host/README.md`,
and the Unreleased section of `CHANGELOG.md`. CONTRIBUTING gains Linux and
Windows prerequisites and states plainly that those platforms compile and test
the portable layers only until the SDL3 and Vulkan host exists.

`tools/check_repo.py` needs no rule change: CMake files are text and no new
artifact extension is introduced. `.gitignore` gains `CMakeUserPresets.json`.

## Testing

**Equivalence on macOS.** The acceptance bar is the existing suites passing
against CMake outputs: `tools/test.py`, `--native`, `--mods`, `--gameplay`, and
the app launching to the menu. The last is a manual check for the user.

**New automated tests**

- `tools/recomp/tests/test_buildlock.py`: two concurrent `build.py` invocations
  serialize; a killed holder releases the lock; `BUILDLOCK_HELD=1` skips
  re-acquisition. Replaces `test_buildlock.sh`.
- `tools/recomp/tests/test_translate_publish.py`: a translate that fails
  leaves `gen` untouched and no stage behind; a success publishes `gen` and
  `symbols.json` atomically. Uses a fake translator script.
- `tests/test_build_py.py`: argument validation and preset selection, run
  without CMake by mocking `subprocess.run`.
- A manifest test case for extension substitution in the mod loader.
- `null_host_link` on every platform, run as a `nogame` CTest that exits 0.

**Portable-layer tests on Linux and Windows.** The `nogame` label. The
platform layer gets a small `platform_tests` binary covering thread creation
with a stack size, plugin load of a fixture module, directory listing and the
monotonic clock's monotonicity.

## Migration order

Landed on one branch, tree building at every step:

1. Add `src/recomp/platform/`, convert pthread, mmap, dlopen, dirent and Mach
   call sites, audit `long`. Verify with the existing shell scripts.
2. Add CMake files beside the scripts. Verify the CMake outputs pass the same
   suites.
3. Switch `tools/build.py` and `tools/test.py` to CMake. Add the Python lock
   and its tests. Add `snapshot_gen.py`.
4. Remove the shell build scripts, update CI and documentation, add the
   Windows and Linux presets and CI runners.

## Removed files

`tools/recomp/build.sh`, `app_build.sh`, `headless_build.sh`, `smoke_build.sh`,
`parity_build.sh`, `buildlock.sh`, `tools/recomp/tests/test_buildlock.sh`,
`src/recomp/mods/lua/build_lua.sh`, `mods/core/build_core.sh`,
`src/recomp/mods/tests/fixtures/build_fixtures.sh`,
`src/recomp/runtime/build_tests.sh`, `src/recomp/runtime/build_profile_tests.sh`,
`src/recomp/dx/build_tests.sh`, `src/recomp/host/build_tests.sh`,
`src/recomp/host/tests/build_ui_layer_tests.sh`,
`src/recomp/host/tests/build_compositor_tests.sh`,
`src/recomp/mods/build_tests.sh`,
`src/recomp/mods/tests/build_present_events_tests.sh`.

Run-only scripts stay and are repointed at CMake outputs: `mode_probe.sh`,
`mods_test.sh`, `src/recomp/host/tests/integration_tests.sh`,
`mods/core/tests/reproducibility_tests.sh`.

## Risks

| Risk | Mitigation |
| --- | --- |
| Weak-default host callbacks fail to resolve on COFF | `null_host_link` and the DX tests on the Windows runner prove it. Fallback: split `host_api.cpp` into per-group no-op static libraries that partial hosts link explicitly. |
| `long` audit misses a 64-bit assumption | Compile with `-Wshorten-64-to-32` and `-Wconversion` on the Windows preset for the runtime, dx and mods targets; treat new warnings as errors in CI. |
| Concurrent builds clobber the CMake binary directory | The Python lock wraps every configure and build. |
| Artifact paths drift from what downstream scripts expect | The target table above fixes the output locations; `tools/test.py --mods` and `--gameplay` exercise them. |
| Ninja or CMake missing on a contributor machine | `requirements-dev.txt` pins the `cmake` and `ninja` PyPI packages so the venv provides both. |
