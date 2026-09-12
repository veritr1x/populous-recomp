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
- No change to test logic; only to how test binaries are built and run, plus
  the smallest edits that let existing tests compile on Linux.
- No release packaging.
- No change to the translator's output or to `x86.h`.
- No Windows port of the runtime and mods test binaries. They use `fork`,
  `mkdtemp` and load plugins by `.dylib` name; they stay POSIX-only until
  sub-project 3 gives Windows a host to test against.

## Facts the design rests on

- Every host callback in `src/recomp/dx/host_api.h` has a weak no-op default in
  `host_api.cpp`. Hosts and tests define a subset strongly and rely on the weak
  defaults for the rest. The DX tests define present and D3D callbacks and use
  weak audio and input. `src/recomp/runtime/mods_seam.cpp` uses the same
  pattern for the mod foundation.
- The runtime and the emitter in `tools/recomp/translate.py` use
  `__attribute__((weak))`, `__builtin_expect`, `__atomic_load_n` and
  `__has_include`. MSVC supports none of these; clang-cl supports all of them.
- The generated archive is linked with `-force_load` so unreferenced members
  survive. CMake 3.24 expresses the same intent portably with
  `$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>`, which becomes `-force_load` on
  Apple, `--whole-archive` on ELF and `/WHOLEARCHIVE` on COFF.
- The differential translator test `tools/recomp/tests/test_translate.py`
  links `build/recomp/librecomp_gen.a` into a test dylib with `-force_load`,
  and `tools/build.py` tests for that archive to decide whether to translate.
  The archive has to keep existing at that path.
- Guest threads are real OS threads under a cooperative baton, created with
  the default stack size. The scheduler records each thread's identity and
  compares it against the calling thread, and ends host threads with
  `pthread_exit`. One test creates a thread with an explicit 512 KB stack.
  `std::thread` cannot set a stack size or expose a comparable id cheaply, so
  thread creation, identity and exit need a platform wrapper.
- Guest file handles are POSIX file descriptors: `open`, `read`, `write`,
  `lseek`, `fstat`, `dup`, `fsync`, `ftruncate` and `close` in `kernel32.cpp`.
  Windows has the same operations under `_open` and friends with 64-bit
  variants, so a thin descriptor wrapper covers them.
- Direct POSIX use outside the `.mm` files is small: pthread mutex, condition
  variable and thread calls in `kernel32.cpp`, `host_services.cpp`,
  `run_record.cpp`, `boot.cpp` and `report_lock.cpp`; `mmap` in `memory.cpp`;
  `dlopen` in `mods/loader.cpp` and tests; `dirent`, `stat`, `mkstemp` in
  `kernel32.cpp`, `overlay.cpp`, `run_record.cpp`, `settings.cpp`,
  `snapshot.cpp`, `misc.cpp` and `present_pixels.cpp`; `_NSGetExecutablePath`
  in `roots.cpp` and `main.mm`; `signal()` in `boot.cpp`; `strcasecmp` in
  `kernel32.cpp` and `misc.cpp`.
- Seven run-only shell scripts and the translator test take the build lock
  through `tools/recomp/buildlock.sh`, which already delegates to an inline
  Python `flock` program.
- The core-mod installer `mods/core/build_core.sh` has two shell suites that
  copy it into a scratch tree and prove that a compile failure preserves the
  previous install and that two builds are byte-identical.
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
cmake/Translate.cmake                recomp_gen and POP_TRANSLATE
cmake/Warnings.cmake                 the warning sets the scripts use today
cmake/MacBundle.cmake                Info.plist, resources, ad-hoc codesign
cmake/Plugins.cmake                  MODULE-library helper for mod plugins
src/recomp/platform/os.h             the platform layer's interface
src/recomp/platform/os_posix.cpp     macOS and Linux implementation
src/recomp/platform/os_win32.cpp     Windows implementation
src/recomp/platform/tests/platform_tests.cpp
src/recomp/runtime/CMakeLists.txt    recomp_runtime and its tests
src/recomp/dx/CMakeLists.txt         recomp_dx, header check and its tests
src/recomp/mods/CMakeLists.txt       recomp_mods, lua, fixtures and its tests
src/recomp/host/CMakeLists.txt       host_common, host_macos, executables, tests
mods/CMakeLists.txt                  core, example and smoke plugins
tools/recomp/buildlock.py            the lock, as a module and a CLI
tools/recomp/build_core.py           the core-mod installer
tools/recomp/snapshot_gen.py         consistent copy of build/recomp/gen
```

Each directory's `CMakeLists.txt` owns the targets for the code in that
directory, so a reader finds a target beside its sources.

The first-party libraries are CMake object libraries, not archives. The shell
scripts compile every source and link every object; an archive would let the
linker drop members nobody references, which changes what a weak default or a
static initializer contributes. Object libraries reproduce the scripts exactly.
Each executable lists the object libraries it needs directly, because CMake
does not propagate object files transitively. Lua and the generated code stay
archives, as they are today.

### Targets

| Target | Kind | Sources | Platforms |
| --- | --- | --- | --- |
| `recomp_platform` | object | `src/recomp/platform/os_posix.cpp` or `os_win32.cpp` | all |
| `recomp_runtime` | object | `src/recomp/runtime/*.cpp` except `fixture.cpp` and tests | all |
| `recomp_dx` | object | `src/recomp/dx/*.cpp` including `host_api.cpp` | all |
| `recomp_dx_null` | object | the same sources with `RECOMP_NULL_HOST` | all |
| `lua` | static | `third_party/lua/*.c` minus `lua.c` and `luac.c` | all |
| `recomp_mods` | object | `src/recomp/mods/*.cpp`, `src/recomp/mods/lua/*.cpp` | all |
| `recomp_gen` | static, published as `build/recomp/librecomp_gen.a` | `build/recomp/gen/chunk_*.c`, `table.c` | all, when `POP_TRANSLATE` is not OFF |
| `host_common` | object | `boot.cpp`, `report_lock.cpp`, `input_gate.cpp`, `present_pixels.cpp`, `page_overlay.cpp`, `audio_math.cpp`, `audio_capture.cpp`, `script.cpp`, `src/backends/cpu/indexed_frame.cpp` | all |
| `host_macos` | object | every `.mm` in `src/recomp/host/` except the three `*_main.mm` | macOS |
| `PopRecomp` | app bundle | `main.mm` | macOS |
| `pop_headless` | executable | `headless_main.cpp` | macOS |
| `pop_smoke` | executable | `smoke_main.mm` | macOS |
| `pop_fixture` | executable | `fixture.cpp` with `recomp_dx_null` | all |
| `pop_fixture_trace` | executable | as `pop_fixture` against the snapshot in `POP_TRACE_DIR` | all, when set |
| `core_mods` | custom | runs `tools/recomp/build_core.py` with the configured compiler | all |
| `mod_fixtures` | MODULE libraries | `src/recomp/mods/tests/fixtures/*.c`; `old_cpu` gets `old_header` first on the include path | all |
| `example_mods` | MODULE libraries | `mods/examples/*/*.c`, output beside each `mod.toml` | all |
| `smoke_probe` | MODULE library | `mods/smoke/probe.c`, output in `mods/smoke/` | all |
| `roots_probe` | executable | `roots.cpp`, `mods/core/tests/roots_probe.cpp` | all |
| `null_host_link` | executable | `src/recomp/dx/tests/null_host_link.cpp` | all |
| `host_api_header_check` | object + custom | `host_api_header_test.c` as C11; `host_api.h` syntax-only as C++17 | all |
| `platform_tests`, `runtime_tests`, `dx_tests`, `profile_tests`, `mods_tests`, `present_events_tests`, `host_tests`, `compositor_tests`, `ui_layer_tests` | test executables | as the current scripts list them | per suite |
| `check_binaries` | custom | depends on every test executable that exists on this platform | all |

Compile definitions that select behavior today stay as target properties:
`POPM_PRESENT_HAS_UI_LAYER=1` where `ui_layer.mm` is linked, `POPM_TESTING=1`
on the mods tests, `RECOMP_NULL_HOST` on the parity fixture and profile tests,
`LUA_USE_MACOSX` or `LUA_USE_LINUX` on Lua. Per-target optimization levels stay
as they are: `-O2` for the app, Lua and generated code, `-O1` elsewhere.

The app bundle keeps `POP_RECOMP_APP_NAME` as a cache variable: a different name
produces a second bundle with its own identifier, as `app_build.sh` does. The
core mods install root is `<bundle>/Contents/Resources/mods/core` for the app and
`build/recomp/mods/core` for the other hosts, exactly as today.

### Output locations

The CMake binary directory is `build/cmake/<preset>`. Final artifacts are
written to their current locations through output-directory properties, so
every downstream script and document path stays valid:

| Artifact | Location |
| --- | --- |
| App bundle | `build/PopRecomp.app` |
| Hosts, fixtures and test executables | `build/recomp/` |
| Generated archive | `build/recomp/librecomp_gen.a` |
| Lua archive | `build/recomp/liblua.a` |
| Core mods | `build/recomp/mods/core` or the bundle's resources |
| Test fixture plugins | `build/recomp/mods-fixtures/` |
| Example and smoke plugins | beside their `mod.toml` |
| Texture pack | `build/texture-pack/` |

### Generated code

`recomp_gen` is a static library whose sources are globbed from
`build/recomp/gen` with `CONFIGURE_DEPENDS`, so a regeneration that changes the
set of chunks is picked up by the next build without an explicit reconfigure.
Its archive is written to `build/recomp/librecomp_gen.a`. Executables that
need the game link it through `$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>`.

`POP_TRANSLATE` is a cache option with three values:

- `AUTO` (default): use `build/recomp/gen` if it exists; otherwise configure
  without game targets and print which targets are unavailable.
- `ON`: require `build/recomp/gen`; fail configure if absent.
- `OFF`: never define game targets. CI uses this.

### Translation step

`tools/build.py --regenerate` owns translation. Under the build lock it:

1. Runs `translate.py --out <stage>` where `<stage>` is
   `build/recomp/gen.new.<pid>`, with `--report build/recomp/translate-report.json`.
2. Copies `tools/recomp/runtime/x86.h` beside the staged sources.
3. Renames the existing `gen` to `gen.old`, the stage to `gen`, publishes
   `symbols.json` by rename, and removes `gen.old`.
4. Runs the CMake build, which recompiles `recomp_gen` from the new sources.

A failure in step 1 or 2 removes the stage and leaves the published `gen`
untouched. Because the archive is rebuilt from `gen` under the same lock, a
torn pair cannot be published.

The `--snapshot DIR` verb of `parity_build.sh` becomes
`tools/recomp/snapshot_gen.py DIR`: copy `gen` under the lock and verify that
every prototype in `funcs.h` has a definition in the chunks, retrying as today.
The `--trace DIR` verb becomes the CMake cache variable `POP_TRACE_DIR`; when set,
`pop_fixture_trace` compiles the snapshot in that directory with the caller's
`trace_override.h` force-included and links the caller's `trace_wrappers.c`.

### Build lock

`tools/recomp/buildlock.py` is the lock. As a module it offers a context
manager used by `tools/build.py` and `tools/test.py`; as a CLI it offers the
`run ROOT WHAT COMMAND...` verb the shell scripts use. `buildlock.sh` keeps its
`run` verb and `buildlock_acquire` function and delegates to the Python CLI
instead of an inline program. The translator test's re-exec helper calls the
Python CLI directly.

The lock is an advisory lock on `build/recomp/.lock`: `fcntl.flock` on POSIX,
`msvcrt.locking` on Windows. It is released by the kernel when the holder exits
however it exits. On POSIX the locked descriptor is passed to the child so the
lock outlives a killed wrapper as long as the child runs; Windows has no
equivalent and holds the lock for the wrapper's lifetime. The `BUILDLOCK_HELD=1`
environment convention stays so a script started under the lock does not try to
take it again.

The lock exists because several contributors and agents build concurrently in
one checkout and Ninja does not serialize concurrent invocations. CMake's
dependency graph handles ordering within one invocation.

### Platform layer

`src/recomp/platform/os.h` is a C header usable from C++ with `extern "C"`
linkage. It provides:

| Area | Functions | POSIX | Win32 |
| --- | --- | --- | --- |
| Threads | `os_thread_create(fn, arg, stack_bytes)`, `os_thread_join`, `os_thread_detach`, `os_thread_exit`, `os_thread_self`, `os_thread_id_of` | pthread | `_beginthreadex`, `GetCurrentThreadId` |
| Virtual memory | `os_vm_reserve(bytes)`, `os_vm_release(ptr, bytes)` | `mmap`/`munmap` | `VirtualAlloc`/`VirtualFree` |
| Plugins | `os_dlopen`, `os_dlopen_noload`, `os_dlsym`, `os_dlclose`, `os_dlerror`, `os_plugin_extension()` | dlfcn, `.dylib` or `.so` | `LoadLibraryW`, `GetModuleHandleW`, `.dll` |
| Paths | `os_stat`, `os_lstat`, `os_mkdir`, `os_rename`, `os_unlink`, `os_getcwd`, `os_chdir`, `os_listdir(path, fn, user)`, `os_mkstemp` | POSIX | Win32 with UTF-8 to UTF-16 conversion |
| Descriptors | `os_fd_open`, `os_fd_read`, `os_fd_write`, `os_fd_seek`, `os_fd_close`, `os_fd_dup`, `os_fd_fsync`, `os_fd_truncate`, `os_fd_stat` | POSIX | `_open` family, `_commit`, `_chsize_s`, `_fstat64` |
| Process | `os_exe_path(buf, cap)`, `os_install_fault_handlers(fn)` | `_NSGetExecutablePath` or `/proc/self/exe`; `signal()` | `GetModuleFileNameW`; returns 0, nothing installed |
| Time | `os_monotonic_ns()`, `os_wall_time_us()`, `os_sleep_us()` | `clock_gettime`, `gettimeofday`, `usleep` | `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime`, `Sleep` |
| Strings | `os_strcasecmp` | `strcasecmp` | `_stricmp` |

Thread ids are `uint64_t` and compare with `==`; a created thread is an opaque
`OsThread *` whose id `os_thread_id_of` reports. Mutexes and condition variables
move to `std::mutex` and `std::condition_variable_any`, which accepts a plain
`std::mutex` and so keeps the scheduler's explicit lock and unlock calls
one-for-one. The scheduler's `pthread_cond_timedwait` becomes `wait_for`.

Every existing call site is converted; no `#ifdef` stays in runtime, dx, mods or
`host_common` code except inside `os_posix.cpp` and `os_win32.cpp`. `roots.cpp`
loses its `__APPLE__` block and asks `os_exe_path`. `main.mm` is macOS-only and
keeps its own call.

The fault reporter in `boot.cpp` becomes `fault_handler(const char *name)` and is
installed through `os_install_fault_handlers`, which maps SIGSEGV, SIGBUS and
SIGABRT to names on POSIX and returns 0 on Windows, where `boot_run` logs that
fault reporting is unavailable. A vectored exception handler is sub-project 3
work.

The 73 `long` uses are audited during the conversion. Any that hold a
pointer-sized or 64-bit value become `int64_t`, `intptr_t` or `size_t`; the
ones that are time or file offsets are checked against the API they feed.

### Plugins on each platform

Core mods, example mods, the smoke probe and test fixtures build as MODULE
libraries with the platform's default extension. The loader resolves the `path`
field of `mod.toml` by replacing its extension with `os_plugin_extension()` when
the written file does not exist, so a manifest that says `logger.dylib` finds
`logger.so` on Linux. The loader tests gain one case for that rule. Tests that
name fixture files use `os_plugin_extension()` instead of `.dylib`.

`tools/recomp/build_core.py` replaces `build_core.sh`. It takes the destination
root, the compiler and the mods API include directory; stages a copy of
`mods/core`, compiles each `<mod>/*.c` with the same reproducibility flags as
today (`-g0`, `-Wl,-S`, `-Wl,-reproducible` when the linker accepts it,
`ZERO_AR_DATE=1`, `-install_name @rpath/<name>` on macOS; `-fPIC` and
`-shared` on Linux; `/LD` semantics through clang on Windows), verifies every
manifest's plugin exists, and swaps the destination by rename. The `core_mods`
CMake target runs it with `CMAKE_C_COMPILER`. Its two shell suites become
`tools/recomp/tests/test_build_core.py`, run by CTest with the label `nogame`.

### Python entry points

`tools/build.py` keeps `--regenerate`, `--target` and `--jobs`. `--target`
gains `gen` (translate if needed and build only the archive), `fixture`
(`pop_fixture`) and `plugins` (core, example, fixture and smoke plugins). It
gains `--preset` (default from the OS) and `--config Debug|Release` (default
Release; Debug selects the `<preset>-debug` preset). It refuses `--target
app|smoke|headless` off macOS with the same message shape it uses today, checks
the game inputs as today, takes the lock, translates when asked or when the
archive is absent, configures, builds, and runs the texture-pack step when its
inputs are newer.

`tools/test.py` keeps every flag. `--native` builds `check_binaries` and runs
CTest with labels `nogame`, `game` and `gpu`; `--compile-only` builds
`check_binaries` and runs nothing. `--mods` keeps its Python orchestration:
build `pop_fixture`, `mods_tests` and `present_events_tests`, run the entity
fixture into a scratch directory as today, then run CTest label `mods` with the
snapshot path in the environment. `--gameplay` is unchanged except that it
builds through `build.py`. The environment scrub of `POPM_*`, `POP_RECOMP_*`,
`POP_SMOKE_*` and `POP_HOST_*` stays.

`tools/recomp/tests/test_translate.py` re-executes under the lock through the
Python CLI and builds the archive with `tools/build.py --target gen`. Its own
dylib link keeps `-force_load` on the archive.

The Makefile keeps its verbs and calls the Python entry points.

### CTest labels

- `nogame`: needs neither game files nor a GPU. Runs on every CI platform where
  the binary builds.
- `game`: loads the guest image or reads game data. Local only.
- `gpu`: uses an offscreen Metal texture. macOS local and macOS CI.
- `mods`: needs the generated archive and the entity snapshot; driven by
  `tools/test.py --mods`.

| Test | Label | Platforms |
| --- | --- | --- |
| `platform_tests` | nogame | all |
| `null_host_link` | nogame | all |
| `dx_tests` | nogame | all |
| `host_api_header_check` | nogame | all |
| `test_build_core.py` | nogame | all |
| `ui_layer_tests` | nogame | macOS |
| `compositor_tests` | gpu | macOS |
| `host_tests` | gpu | macOS |
| `runtime_tests` | game | macOS, Linux |
| `profile_tests` (two runs) | game | macOS, Linux |
| `mods_tests`, `present_events_tests` | mods | macOS |

The runtime tests open the game image and use `fork`; they are `game` and
POSIX-only. The DX tests need neither game files nor a GPU today; if a case
turns out to read game data during implementation, it moves to `game` and the
table is corrected.

### CI

The tooling job is unchanged. The compile job becomes a matrix over
`macos-15`, `ubuntu-24.04` and `windows-2025` with `POP_TRANSLATE=OFF`. Each
runner configures with its preset, builds `check_binaries`, and runs the
`nogame` label. The macOS runner also runs the `gpu` label. Ubuntu installs clang and Ninja. Windows installs LLVM and Ninja, enters a
Visual Studio developer environment for the Windows SDK, and builds with the
GNU-style `clang` driver targeting the MSVC ABI and `lld-link`, so compiler
flags are the same on every platform. No game files, generated code, bundles, profiles or
diagnostics are uploaded.

The development checkout has no game files and no generated code, so every
game-backed suite is verified by whoever holds a game-equipped checkout; the
plan says so at each step that needs it. Linux is verified locally in a Docker
container and in CI. Windows cannot be exercised on the development machine,
which is a Mac; its verification is the CI matrix on the feature branch.

### Documentation

Every mention of a shell build script is replaced with the Python entry point or
the CMake target. Files: `README.md`, `CONTRIBUTING.md`, `AGENTS.md`,
`docs/testing.md`, `docs/architecture.md` boundaries paragraph,
`tools/recomp/README.md`, `src/recomp/host/README.md`,
`src/recomp/runtime/README.md`, `src/recomp/dx/README.md`,
`mods/core/README.md`, and the Unreleased section of `CHANGELOG.md`.
CONTRIBUTING gains Linux and Windows prerequisites and states plainly that
those platforms compile and test the portable layers only until the SDL3 and
Vulkan host exists.

`tools/check_repo.py` needs no rule change: CMake files are text and no new
artifact extension is introduced. `.gitignore` gains `CMakeUserPresets.json`.
`requirements-dev.txt` pins the `cmake` and `ninja` PyPI packages so the venv
provides both.

## Testing

**Equivalence on macOS.** The acceptance bar is the existing suites passing
against CMake outputs: `tools/test.py`, `--native`, `--mods`, `--gameplay`, and
the app launching to the menu. The last is a manual check for the user.

**New automated tests**

- `tools/recomp/tests/test_buildlock.py`: two concurrent holders serialize; a
  killed holder releases the lock; `BUILDLOCK_HELD=1` skips re-acquisition.
  The shell race test `test_buildlock.sh` stays and now exercises the delegating
  shell entry.
- `tools/recomp/tests/test_translate_publish.py`: a translate that fails
  leaves `gen` untouched and no stage behind; a success publishes `gen` and
  `symbols.json` atomically. Uses a fake translate callable.
- `tests/test_build_py.py`: argument validation, preset selection and target
  mapping, run without CMake by mocking `subprocess.run`.
- `tools/recomp/tests/test_build_core.py`: the installer compiles a probe
  plugin, copies nested assets, leaves the source tree clean, preserves the
  previous install on a compile failure and on a missing plugin, and produces
  byte-identical output across two builds with shifted mtimes.
- A loader test case for plugin extension substitution.
- `platform_tests`: thread creation with a stack size and id round trip,
  monotonic clock monotonicity, directory listing, `mkstemp`, descriptor
  round trip, plugin extension, executable path and virtual memory reserve.
- `null_host_link`: links `recomp_dx` and `host_common` with no strong
  callbacks and calls one weak default; exits 0.

## Migration order

Landed on one branch, tree building at every step:

1. CMake targets for the portable libraries and their tests beside the shell
   scripts, verified to produce the same test results on macOS.
2. Plugins, hosts and the remaining tests in CMake. The app bundle, headless,
   smoke and fixture hosts pass the existing suites.
3. Platform layer with its tests; conversion of runtime, mods, `host_common`
   and test call sites; `long` audit. Verified with the CMake suites.
4. Python: `buildlock.py`, `build_core.py`, `snapshot_gen.py`, `build.py`,
   `test.py`, Makefile, translator test and run-only scripts repointed.
5. Linux and Windows: presets, `os_win32.cpp`, `null_host_link`, CI matrix.
6. Removal of the shell build scripts and documentation.

## Removed files

`tools/recomp/build.sh`, `app_build.sh`, `headless_build.sh`, `smoke_build.sh`,
`parity_build.sh`, `src/recomp/mods/lua/build_lua.sh`, `mods/core/build_core.sh`,
`mods/examples/build_examples.sh`,
`src/recomp/mods/tests/fixtures/build_fixtures.sh`,
`src/recomp/runtime/build_tests.sh`, `src/recomp/runtime/build_profile_tests.sh`,
`src/recomp/dx/build_tests.sh`, `src/recomp/host/build_tests.sh`,
`src/recomp/host/tests/build_ui_layer_tests.sh`,
`src/recomp/host/tests/build_compositor_tests.sh`,
`src/recomp/mods/build_tests.sh`,
`src/recomp/mods/tests/build_present_events_tests.sh`,
`mods/core/tests/packaging_tests.sh`, `mods/core/tests/reproducibility_tests.sh`.

Kept and repointed at CMake outputs: `tools/recomp/buildlock.sh`,
`tools/recomp/tests/test_buildlock.sh`, `tools/recomp/mode_probe.sh`,
`tools/recomp/mods_test.sh`, `src/recomp/host/tests/integration_tests.sh`,
`mods/core/tests/roots_tests.sh`.

## Risks

| Risk | Mitigation |
| --- | --- |
| Weak-default host callbacks fail to resolve on COFF | `null_host_link` and the DX tests on the Windows runner prove it. Fallback: split `host_api.cpp` into per-group no-op static libraries that partial hosts link explicitly. |
| `long` audit misses a 64-bit assumption | Compile with `-Wshorten-64-to-32` on the Windows preset for the runtime, dx and mods targets; CI treats new warnings as errors there. |
| Concurrent builds clobber the CMake binary directory | The Python lock wraps every configure and build. |
| Artifact paths drift from what downstream scripts expect | The output table above fixes the locations; `tools/test.py --mods`, `--gameplay` and `integration_tests.sh` exercise them. |
| Windows or Linux breakage invisible on the Mac | Every task that touches them ends with a push and a CI check; the plan names the job to watch. |
