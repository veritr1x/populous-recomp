# Portable Build System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the xcrun shell build scripts with one CMake build that produces every current macOS artifact and compiles the runtime, DirectX shims and mod layers on Linux and Windows, behind unchanged Python entry points.

**Architecture:** CMake object libraries reproduce the scripts' compile-everything-link-everything semantics; a small C platform layer (`src/recomp/platform/os.h`) replaces direct pthread, mmap, dlopen, dirent and Mach calls; translation, the build lock and the core-mod installer become Python tools that CMake and the entry points call. Hosts keep per-executable source lists so which strong host callbacks each binary carries stays exactly as today.

**Tech Stack:** CMake 3.24+ with presets, Ninja, clang (AppleClang on macOS, clang on Linux, GNU-driver clang + lld-link on Windows), Python 3.9+ with pytest/unittest, CTest labels.

**Spec:** `docs/superpowers/specs/2026-09-12-portable-build-system-design.md`

## Global Constraints

- Python 3.9 or later; every Python change must run on 3.9 (no `match`, no `|` union types).
- CMake minimum 3.24 (`$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`); presets file version 5.
- Compilers: clang only. Flags are GNU-style on every platform, including Windows.
- Output locations are fixed: `build/PopRecomp.app`, `build/recomp/<binaries>`, `build/recomp/librecomp_gen.a`, `build/recomp/liblua.a`, `build/recomp/mods/core`, `build/recomp/mods-fixtures/`, plugins beside their `mod.toml`, `build/texture-pack/`. CMake's own tree is `build/cmake/<preset>`.
- First-party libraries are CMake OBJECT libraries; each executable links the object libraries it needs directly. Lua and `recomp_gen` are STATIC.
- Optimization: `-O2` for `recomp_runtime`, `recomp_dx`, `recomp_mods`, `lua`, `recomp_gen`; `-O1` for test binaries, hosts other than the app, and plugins. Debug configurations use `-O0 -g`.
- Warning sets are exactly the scripts': host `-Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter`; strict `-Wall -Wextra -Wno-unused-parameter`; werror `-Wall -Wextra -Werror` (UI layer and compositor tests); generated `-Wall -Wextra -Wno-unused`.
- No `#ifdef` on platform in runtime, dx, mods or shared host code outside `os_posix.cpp` and `os_win32.cpp`.
- Format touched native code with `.venv/bin/python tools/format.py --write`. Never format `third_party/` or generated code.
- Never commit anything under `build/`, `original/`, `analysis/`. Run `.venv/bin/python tools/check_repo.py` before every commit that adds files.
- Test-only entry points compile under `POPM_TESTING=1`; those binaries need their own object sets (`recomp_runtime_testing`, `recomp_mods_testing`).

## Environment constraints for the executor

- The development checkout is on macOS with Xcode clang 21, CMake 4.4, Ninja 1.13, Docker and `gh`.
- **It has no game files and no generated code** (`original/gog` and `build/recomp/gen` are absent). Every step marked **[game-equipped checkout]** cannot be run here; it is verified by the user, or by the executor on a checkout where `tools/setup.py` and a translation have run. Everything else must be run here.
- Linux is verified in a Docker container (Task 12). Windows only in CI (Task 13); push to a remote you can push to and watch the `Contributor checks` workflow with `gh run watch`.
- Work on branch `portable-build-system` (already exists with the spec committed).

## File structure

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | project, standards, options, output roots, subdirectories, `check_binaries` |
| `CMakePresets.json` | `macos`, `linux`, `windows` and `-debug` variants, Ninja, `build/cmake/<preset>` |
| `cmake/Warnings.cmake` | warning sets, `pop_optimize`, `pop_test_binary` |
| `cmake/Plugins.cmake` | `pop_add_plugin` for MODULE libraries |
| `cmake/Translate.cmake` | `POP_TRANSLATE`, `recomp_gen`, `pop_link_gen` |
| `cmake/MacBundle.cmake` | `pop_mac_bundle` post-build finishing |
| `src/recomp/platform/os.h`, `os_posix.cpp`, `os_win32.cpp`, `tests/platform_tests.cpp`, `CMakeLists.txt` | the platform layer and its tests |
| `src/recomp/runtime/CMakeLists.txt` | `recomp_runtime`, `recomp_runtime_testing`, `recomp_snapshot`, `runtime_tests`, `pop_fixture`, `pop_fixture_trace`, `profile_tests` |
| `src/recomp/dx/CMakeLists.txt` | `recomp_dx`, `recomp_dx_null`, header check, `dx_tests`, `null_host_link` |
| `src/recomp/dx/tests/null_host_link.cpp` | the weak-default link proof |
| `src/recomp/mods/CMakeLists.txt` | `lua`, `recomp_mods`, `recomp_mods_testing`, fixtures, `mods_tests`, `roots_probe` |
| `src/recomp/host/CMakeLists.txt` | `host_common` sources, hosts, app, host tests, `present_events_tests` |
| `mods/CMakeLists.txt` | `core_mods`, `example_mods`, `smoke_probe`, `plugins`, `test_build_core` |
| `tools/recomp/buildlock.py` | lock module and CLI; `buildlock.sh` delegates to it |
| `tools/recomp/build_core.py` | core-mod installer |
| `tools/recomp/finish_bundle.py` | plist patch, resources, core mods, texture pack, codesign |
| `tools/recomp/snapshot_gen.py` | consistent copy of `build/recomp/gen` |
| `tools/build.py`, `tools/test.py` | entry points over CMake and CTest |
| `tools/recomp/tests/test_buildlock.py`, `test_translate_publish.py`, `test_build_core.py`, `tests/test_build_py.py` | new Python tests |

---

### Task 1: CMake skeleton for the portable libraries and their tests

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Warnings.cmake`, `src/recomp/runtime/CMakeLists.txt`, `src/recomp/dx/CMakeLists.txt`, `src/recomp/mods/CMakeLists.txt`
- Modify: `requirements-dev.txt`, `.gitignore`

**Interfaces:**
- Produces: targets `recomp_runtime`, `recomp_snapshot`, `recomp_dx`, `recomp_dx_null`, `lua`, `recomp_mods`, `host_api_header_test` (object), `runtime_tests`, `dx_tests`; functions `pop_optimize(target level)`, `pop_test_binary(target)`; variables `POP_ROOT`, `POP_OUT`, `POP_WARN_HOST`, `POP_WARN_STRICT`, `POP_WARN_WERROR`, `POP_WARN_GEN`; custom target `check_binaries`; CTest labels `nogame`, `game`.

- [ ] **Step 1: Pin CMake and Ninja in the venv**

Append to `requirements-dev.txt`:

```text
cmake==4.1.2
ninja==1.13.0
```

Run: `.venv/bin/python -m pip install -r requirements-dev.txt && .venv/bin/cmake --version && .venv/bin/ninja --version`
Expected: both print versions. If a pin does not exist on PyPI, use the newest 4.x `cmake` and 1.13.x `ninja` that do and record the versions you used.

- [ ] **Step 2: Ignore user presets**

Append `CMakeUserPresets.json` to `.gitignore` after the `/.codex/` line.

- [ ] **Step 3: Write `cmake/Warnings.cmake`**

```cmake
# The warning sets the shell scripts used, named so a target says which one it
# wants instead of repeating the flags.
set(POP_WARN_HOST -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter)
set(POP_WARN_STRICT -Wall -Wextra -Wno-unused-parameter)
set(POP_WARN_WERROR -Wall -Wextra -Werror)
set(POP_WARN_GEN -Wall -Wextra -Wno-unused)

# Optimization is per target, as the scripts had it: -O2 for what players run,
# -O1 for hosts and tests. A Debug configuration leaves this out and gets -O0.
function(pop_optimize target level)
  target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O${level}>)
endfunction()

# Every test executable registers here so `check_binaries` builds exactly the
# suite this platform has; tools/test.py --compile-only builds that target.
function(pop_test_binary target)
  add_dependencies(check_binaries ${target})
endfunction()
```

- [ ] **Step 4: Write the root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(PopulousRecomp LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization is a per-target decision (cmake/Warnings.cmake). The
# configuration only decides whether -O0 is forced.
foreach(lang C CXX OBJCXX)
  set(CMAKE_${lang}_FLAGS_RELEASE "-g")
  set(CMAKE_${lang}_FLAGS_DEBUG "-g -O0")
endforeach()

set(POP_ROOT ${CMAKE_SOURCE_DIR})
set(POP_OUT ${POP_ROOT}/build/recomp)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${POP_OUT})

set(POP_TRANSLATE AUTO CACHE STRING "AUTO, ON or OFF: define targets that need build/recomp/gen")
set_property(CACHE POP_TRANSLATE PROPERTY STRINGS AUTO ON OFF)
set(POP_TRACE_DIR "" CACHE PATH "Snapshot directory for pop_fixture_trace")
set(POP_RECOMP_APP_NAME PopRecomp CACHE STRING "macOS bundle name")
if(NOT POP_RECOMP_APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid POP_RECOMP_APP_NAME: ${POP_RECOMP_APP_NAME}")
endif()

# tools/build.py passes its own interpreter; a bare configure finds one.
if(NOT Python3_EXECUTABLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

enable_testing()
add_custom_target(check_binaries)

include(cmake/Warnings.cmake)
add_subdirectory(src/recomp/runtime)
add_subdirectory(src/recomp/dx)
add_subdirectory(src/recomp/mods)
```

- [ ] **Step 5: Write `CMakePresets.json`**

```json
{
  "version": 5,
  "cmakeMinimumRequired": {"major": 3, "minor": 24, "patch": 0},
  "configurePresets": [
    {
      "name": "base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/cmake/${presetName}",
      "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}
    },
    {
      "name": "macos", "inherits": "base",
      "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Darwin"}
    },
    {
      "name": "linux", "inherits": "base",
      "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux"},
      "cacheVariables": {"CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++"}
    },
    {
      "name": "windows", "inherits": "base",
      "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows"},
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++",
        "CMAKE_EXE_LINKER_FLAGS": "-fuse-ld=lld",
        "CMAKE_SHARED_LINKER_FLAGS": "-fuse-ld=lld",
        "CMAKE_MODULE_LINKER_FLAGS": "-fuse-ld=lld"
      }
    },
    {"name": "macos-debug", "inherits": "macos", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
    {"name": "linux-debug", "inherits": "linux", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
    {"name": "windows-debug", "inherits": "windows", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}}
  ],
  "buildPresets": [
    {"name": "macos", "configurePreset": "macos"},
    {"name": "linux", "configurePreset": "linux"},
    {"name": "windows", "configurePreset": "windows"},
    {"name": "macos-debug", "configurePreset": "macos-debug"},
    {"name": "linux-debug", "configurePreset": "linux-debug"},
    {"name": "windows-debug", "configurePreset": "windows-debug"}
  ],
  "testPresets": [
    {"name": "macos", "configurePreset": "macos", "output": {"outputOnFailure": true}},
    {"name": "linux", "configurePreset": "linux", "output": {"outputOnFailure": true}},
    {"name": "windows", "configurePreset": "windows", "output": {"outputOnFailure": true}},
    {"name": "macos-debug", "configurePreset": "macos-debug", "output": {"outputOnFailure": true}},
    {"name": "linux-debug", "configurePreset": "linux-debug", "output": {"outputOnFailure": true}},
    {"name": "windows-debug", "configurePreset": "windows-debug", "output": {"outputOnFailure": true}}
  ]
}
```

- [ ] **Step 6: Write `src/recomp/runtime/CMakeLists.txt`**

```cmake
# The guest runtime. Object libraries, so every source is linked into every
# consumer exactly as the shell scripts linked every object.
set(POP_RUNTIME_SOURCES
  memory.cpp loader.cpp imports.cpp cpu.cpp profile.cpp kernel32.cpp
  mods_seam.cpp user32.cpp misc.cpp)

add_library(recomp_runtime OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(recomp_runtime PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime 2)

# POPM_TESTING compiles the test-only entry points (loader reset, owner
# counter seam, scheduler latch reset). None exists in a real build.
add_library(recomp_runtime_testing OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime_testing PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recomp_runtime_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_runtime_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime_testing 1)

# Linked by the hosts, the fixture and the mod tests, not by the unit tests.
add_library(recomp_snapshot OBJECT snapshot.cpp)
target_link_libraries(recomp_snapshot PUBLIC recomp_runtime)
target_compile_options(recomp_snapshot PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_snapshot 2)

if(NOT WIN32)
  # The suite forks and waits; it is POSIX-only until sub-project 3.
  add_executable(runtime_tests tests/runtime_tests.cpp tests/stub_recomp_call.cpp)
  target_link_libraries(runtime_tests PRIVATE recomp_runtime)
  target_compile_options(runtime_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(runtime_tests 1)
  pop_test_binary(runtime_tests)
  add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(runtime_tests PROPERTIES LABELS game)
endif()
```

- [ ] **Step 7: Write `src/recomp/dx/CMakeLists.txt`**

```cmake
set(POP_DX_SOURCES
  com.cpp dx.cpp host_api.cpp display_stubs.cpp ddraw.cpp d3d.cpp
  dsound.cpp dinput.cpp qmixer.cpp weanetr.cpp)

add_library(recomp_dx OBJECT ${POP_DX_SOURCES})
target_link_libraries(recomp_dx PUBLIC recomp_runtime)
target_compile_options(recomp_dx PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx 2)

# The same shims with every host callback a strong no-op: a binary linking
# these can neither draw nor play sound. The parity fixture and the profile
# tests use it.
add_library(recomp_dx_null OBJECT ${POP_DX_SOURCES})
target_link_libraries(recomp_dx_null PUBLIC recomp_runtime)
target_compile_definitions(recomp_dx_null PUBLIC RECOMP_NULL_HOST)
target_compile_options(recomp_dx_null PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx_null 1)

# host_api.h is a C header too. The C11 consumer is an object the DX tests
# link; the header is also parsed as C++17 on its own.
add_library(host_api_header_test OBJECT tests/host_api_header_test.c)
target_include_directories(host_api_header_test PRIVATE ${POP_ROOT})
target_compile_options(host_api_header_test PRIVATE -Wall -Wextra)
pop_optimize(host_api_header_test 1)
add_test(NAME host_api_header_check
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter
          -I${POP_ROOT} -fsyntax-only -x c++ ${CMAKE_CURRENT_SOURCE_DIR}/host_api.h)
set_tests_properties(host_api_header_check PROPERTIES LABELS nogame)

add_executable(dx_tests tests/dx_tests.cpp
  ${POP_ROOT}/src/recomp/runtime/tests/stub_recomp_call.cpp
  $<TARGET_OBJECTS:host_api_header_test>)
target_link_libraries(dx_tests PRIVATE recomp_dx recomp_runtime)
target_compile_options(dx_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(dx_tests 1)
pop_test_binary(dx_tests)
add_test(NAME dx_tests COMMAND dx_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(dx_tests PROPERTIES LABELS nogame)
```

- [ ] **Step 8: Write `src/recomp/mods/CMakeLists.txt`**

```cmake
# Vendored Lua 5.4: the library only, lua.c and luac.c are not vendored.
file(GLOB POP_LUA_SOURCES CONFIGURE_DEPENDS ${POP_ROOT}/third_party/lua/*.c)
add_library(lua STATIC ${POP_LUA_SOURCES})
set_target_properties(lua PROPERTIES
  C_STANDARD 99 C_EXTENSIONS OFF
  ARCHIVE_OUTPUT_DIRECTORY ${POP_OUT} OUTPUT_NAME lua)
target_include_directories(lua PUBLIC ${POP_ROOT}/third_party/lua)
target_compile_options(lua PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(lua PRIVATE
  $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
  $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX>)
pop_optimize(lua 2)

# The mod foundation. Globbed as the scripts globbed, so a new module is
# picked up by the next configure.
file(GLOB POP_MODS_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/lua/*.cpp)

add_library(recomp_mods OBJECT ${POP_MODS_SOURCES})
target_include_directories(recomp_mods PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(recomp_mods PUBLIC lua recomp_runtime)
target_compile_options(recomp_mods PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_mods 2)

add_library(recomp_mods_testing OBJECT ${POP_MODS_SOURCES})
target_include_directories(recomp_mods_testing PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(recomp_mods_testing PUBLIC lua recomp_runtime_testing)
target_compile_definitions(recomp_mods_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_mods_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_mods_testing 1)
```

- [ ] **Step 9: Configure and build**

Run:
```sh
.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/cmake --build --preset macos --target check_binaries lua recomp_mods
```
Expected: configure succeeds; `build/recomp/dx_tests`, `build/recomp/runtime_tests` and `build/recomp/liblua.a` exist. Fix any compile error by matching the script flags, not by changing sources.

- [ ] **Step 10: Compare the DX tests with the shell build**

Run: `sh src/recomp/dx/build_tests.sh --no-run && build/recomp/dx_tests > /tmp/dx-shell.txt; .venv/bin/cmake --build --preset macos --target dx_tests && build/recomp/dx_tests > /tmp/dx-cmake.txt; diff /tmp/dx-shell.txt /tmp/dx-cmake.txt && echo SAME`
Expected: `SAME`, and the last line of each output reports zero failures. (The shell script overwrote the CMake binary in between, which is why the CMake target is rebuilt before its run.)

Run: `.venv/bin/ctest --preset macos -L nogame`
Expected: `dx_tests` and `host_api_header_check` pass. **[game-equipped checkout]**: `.venv/bin/ctest --preset macos -L game` passes `runtime_tests`.

- [ ] **Step 11: Commit**

```sh
.venv/bin/python tools/check_repo.py
git add CMakeLists.txt CMakePresets.json cmake/Warnings.cmake src/recomp/runtime/CMakeLists.txt src/recomp/dx/CMakeLists.txt src/recomp/mods/CMakeLists.txt requirements-dev.txt .gitignore
git commit -m "Add CMake targets for the runtime, DirectX shims, mods and their tests"
```

---

### Task 2: Plugins in CMake and the Python core-mod installer

**Files:**
- Create: `cmake/Plugins.cmake`, `tools/recomp/build_core.py`, `tools/recomp/tests/test_build_core.py`, `mods/CMakeLists.txt`
- Modify: `CMakeLists.txt`, `src/recomp/mods/CMakeLists.txt`, `mods/core/tests/roots_tests.sh`

**Interfaces:**
- Consumes: `pop_optimize`, `POP_WARN_STRICT`, `POP_OUT`, `Python3_EXECUTABLE`.
- Produces: `pop_add_plugin(<target> SOURCE <f> OUTPUT_DIR <d> OUTPUT_NAME <n> [INCLUDE_FIRST <d>] [WARNINGS <flags...>] [OPTIONS <flags...>])`; targets `core_mods`, `example_mods`, `smoke_probe`, `mod_fixtures`, `plugins`, `roots_probe`; Python `build_core.install(source: Path, dest: Path, cc, api_include: Path, system=None) -> int` raising `subprocess.CalledProcessError` or `FileNotFoundError`; `build_core.plugin_extension(system=None) -> str`.

- [ ] **Step 1: Write the failing installer tests**

`tools/recomp/tests/test_build_core.py`:

```python
"""The core-mod installer: staging, atomic swap, failure behavior and byte-identical rebuilds."""

import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("build_core", Path(__file__).parents[1] / "build_core.py")
build_core = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_core)
ROOT = Path(__file__).resolve().parents[3]
CC = os.environ.get("POP_CC") or shutil.which("clang") or shutil.which("cc")
PROBE_C = "int core_packaging_probe(void) { return 12; }\n"
PROBE_TOML = 'id = "core.packaging.probe"\n[plugin]\npath = "probe.dylib"\n'


def tree_digest(root):
    """Names and bytes of every file under root, in sorted order; mtimes are not part of it."""
    digest = hashlib.sha256()
    for path in sorted(p for p in Path(root).rglob("*") if p.is_file()):
        digest.update(str(path.relative_to(root)).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


@unittest.skipUnless(CC, "no C compiler on PATH; set POP_CC")
class BuildCoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pop-core-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.source = self.tmp / "mods/core"
        probe = self.source / "probe"
        (probe / "assets").mkdir(parents=True)
        (probe / "probe.c").write_text(PROBE_C)
        (probe / "mod.toml").write_text(PROBE_TOML)
        (probe / "assets/payload").write_text("asset payload\n")
        self.dest = self.tmp / "build/recomp/mods/core"

    def install(self):
        return build_core.install(self.source, self.dest, CC, ROOT / "src/recomp/mods")

    def plugin(self):
        return self.dest / "probe" / ("probe" + build_core.plugin_extension())

    def test_compiles_plugin_copies_assets_and_leaves_source_clean(self):
        self.assertEqual(self.install(), 1)
        self.assertGreater(self.plugin().stat().st_size, 0)
        self.assertEqual((self.dest / "probe/assets/payload").read_text(), "asset payload\n")
        self.assertEqual(sorted(p.name for p in (self.source / "probe").iterdir()),
                         ["assets", "mod.toml", "probe.c"])
        self.assertFalse(list(self.dest.parent.glob("core.stage.*")))

    def test_compile_failure_preserves_previous_install(self):
        self.install()
        before = self.plugin().read_bytes()
        (self.source / "probe/probe.c").write_text("not valid C\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.install()
        self.assertEqual(self.plugin().read_bytes(), before)
        self.assertFalse(list(self.dest.parent.glob("core.stage.*")))

    def test_missing_plugin_preserves_previous_install(self):
        self.install()
        before = self.plugin().read_bytes()
        (self.source / "probe/probe.c").unlink()
        with self.assertRaises(FileNotFoundError):
            self.install()
        self.assertEqual(self.plugin().read_bytes(), before)

    def test_two_builds_are_byte_identical(self):
        self.install()
        first = tree_digest(self.dest)
        for path in self.source.rglob("*.c"):
            st = path.stat()
            os.utime(path, (st.st_atime, st.st_mtime + 10))
        self.install()
        self.assertEqual(tree_digest(self.dest), first)

    def test_cli_refuses_an_install_root_outside_build(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"),
                                 "--dest", str(self.tmp / "elsewhere"), "--cc", CC],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported install root", result.stderr)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `.venv/bin/python -m unittest tools/recomp/tests/test_build_core.py`
Expected: fails to load `build_core.py` (FileNotFoundError / AttributeError).

- [ ] **Step 3: Write `tools/recomp/build_core.py`**

```python
#!/usr/bin/env python3
"""Install the bundled core mods: compile each plugin reproducibly, then swap the install root."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def plugin_extension(system=None):
    """The shared-library suffix the mod loader expects on this platform."""
    system = system or platform.system()
    return {"Darwin": ".dylib", "Windows": ".dll"}.get(system, ".so")


def linker_accepts(cc, flag, scratch):
    """Whether the linker takes `flag`, learned by linking an empty module with it."""
    probe = scratch / ".link-probe"
    result = subprocess.run([str(cc), "-x", "c", "-", "-shared", flag, "-o", str(probe)],
                            input="", capture_output=True, text=True)
    return result.returncode == 0


def compile_flags(cc, name, scratch, system=None):
    """Flags for one plugin: reproducible, stripped, position independent, undefined symbols
    left for load time because the API arrives as a pointer rather than by linking."""
    system = system or platform.system()
    flags = ["-std=c11", "-O1", "-g0", "-fPIC", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    if system == "Darwin":
        # Keep LC_UUID (dyld requires it) but derive it from content, omit
        # debug maps, and never use the random staging path as the id.
        flags += ["-dynamiclib", "-Wl,-undefined,dynamic_lookup", "-Wl,-S",
                  "-Wl,-install_name,@rpath/" + name]
        if linker_accepts(cc, "-Wl,-reproducible", scratch):
            flags.append("-Wl,-reproducible")
    elif system == "Windows":
        flags += ["-shared", "-fuse-ld=lld", "-Wl,/Brepro"]
    else:
        flags += ["-shared", "-Wl,--build-id=none"]
    return flags


def manifest_plugin(toml_path):
    """The [plugin] path a manifest names, or None."""
    section = None
    for line in toml_path.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            section = stripped
        elif section == "[plugin]" and stripped.startswith("path"):
            _, _, value = stripped.partition("=")
            return value.strip().strip('"')
    return None


def install(source, dest, cc, api_include, system=None):
    """Stage a copy of `source` beside `dest`, compile every <mod>/*.c into it, verify each
    manifest's plugin exists, then replace `dest` by rename. Raises CalledProcessError on a
    compile failure and FileNotFoundError on a missing plugin; `dest` is untouched either way.
    Returns the number of plugins built."""
    system = system or platform.system()
    source, dest = Path(source), Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=dest.name + ".stage.", dir=dest.parent))
    try:
        shutil.copytree(source, stage, dirs_exist_ok=True)
        env = dict(os.environ, ZERO_AR_DATE="1")
        built = 0
        for src in sorted(stage.glob("*/*.c")):
            out = src.with_suffix(plugin_extension(system))
            command = [str(cc)] + compile_flags(cc, out.name, stage, system)
            command += ["-I", str(api_include), str(src), "-o", str(out)]
            subprocess.run(command, check=True, env=env, cwd=stage)
            built += 1
        for toml in sorted(stage.glob("*/mod.toml")):
            plugin = manifest_plugin(toml)
            if plugin is None:
                continue
            literal = toml.parent / plugin
            # A manifest written on one platform names that platform's suffix;
            # the loader applies the same substitution at run time.
            local = literal.with_suffix(plugin_extension(system))
            if not literal.is_file() and not local.is_file():
                raise FileNotFoundError("missing plugin %s: %s" % (toml, plugin))
        for probe in stage.glob(".link-probe*"):
            probe.unlink()
        if dest.exists():
            shutil.rmtree(dest)
        stage.rename(dest)
        return built
    finally:
        if stage.exists():
            shutil.rmtree(stage, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=ROOT / "build/recomp/mods/core")
    parser.add_argument("--source", type=Path, default=ROOT / "mods/core")
    parser.add_argument("--cc", required=True, help="C compiler driver (clang)")
    parser.add_argument("--api-include", type=Path, default=ROOT / "src/recomp/mods")
    args = parser.parse_args()
    dest = args.dest.resolve()
    # Only build trees are install roots: the source tree stays clean.
    if "build" not in dest.parts:
        parser.error("unsupported install root: %s" % dest)
    try:
        built = install(args.source.resolve(), dest, args.cc, args.api_include.resolve())
    except (subprocess.CalledProcessError, FileNotFoundError) as error:
        parser.exit(1, "build_core: %s\n" % error)
    print("core mods: installed %s (%d plugins)" % (dest, built))


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the installer tests**

Run: `POP_CC=$(xcrun -f clang) .venv/bin/python -m unittest -v tools/recomp/tests/test_build_core.py`
Expected: 5 tests pass. If `test_two_builds_are_byte_identical` fails, print both `otool -l` outputs for the dylibs and remove whatever varies (it is almost always a missing `-Wl,-reproducible` or a leftover probe file).

- [ ] **Step 5: Write `cmake/Plugins.cmake`**

```cmake
# pop_add_plugin(<target> SOURCE <c file> OUTPUT_DIR <dir> OUTPUT_NAME <stem>
#                [INCLUDE_FIRST <dir>] [WARNINGS <flags>...] [OPTIONS <flags>...])
#
# A mod plugin: one C file, a MODULE library named <stem> with the platform's
# extension, compiled against src/recomp/mods for pop_mod_api.h. Undefined
# symbols are left for load time on Apple because the API arrives as a pointer;
# ELF modules allow them by default and COFF plugins reference nothing.
function(pop_add_plugin target)
  cmake_parse_arguments(ARG "" "SOURCE;OUTPUT_DIR;OUTPUT_NAME;INCLUDE_FIRST" "WARNINGS;OPTIONS" ${ARGN})
  add_library(${target} MODULE ${ARG_SOURCE})
  set_target_properties(${target} PROPERTIES
    PREFIX "" OUTPUT_NAME ${ARG_OUTPUT_NAME}
    LIBRARY_OUTPUT_DIRECTORY ${ARG_OUTPUT_DIR}
    C_STANDARD 11 C_EXTENSIONS OFF)
  if(APPLE)
    # MODULE libraries default to .so on Apple; the loader wants .dylib.
    set_target_properties(${target} PROPERTIES SUFFIX ".dylib")
    target_link_options(${target} PRIVATE -Wl,-undefined,dynamic_lookup)
  endif()
  if(ARG_INCLUDE_FIRST)
    target_include_directories(${target} BEFORE PRIVATE ${ARG_INCLUDE_FIRST})
  endif()
  target_include_directories(${target} PRIVATE ${POP_ROOT}/src/recomp/mods)
  target_compile_options(${target} PRIVATE ${ARG_WARNINGS} ${ARG_OPTIONS})
  pop_optimize(${target} 1)
endfunction()
```

- [ ] **Step 6: Write `mods/CMakeLists.txt`**

```cmake
# The plugins the tree ships: bundled core mods through the installer, the
# examples beside their manifests, and the smoke probe.
add_custom_target(core_mods
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/build_core.py
          --dest ${POP_OUT}/mods/core --cc ${CMAKE_C_COMPILER}
  WORKING_DIRECTORY ${POP_ROOT}
  COMMENT "Installing core mods into build/recomp/mods/core"
  VERBATIM)

file(GLOB POP_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*/*.c)
add_custom_target(example_mods)
foreach(src ${POP_EXAMPLE_SOURCES})
  get_filename_component(stem ${src} NAME_WE)
  get_filename_component(dir ${src} DIRECTORY)
  pop_add_plugin(example_${stem} SOURCE ${src} OUTPUT_DIR ${dir} OUTPUT_NAME ${stem}
    WARNINGS ${POP_WARN_STRICT} OPTIONS -g)
  add_dependencies(example_mods example_${stem})
endforeach()

pop_add_plugin(smoke_probe SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/smoke/probe.c
  OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/smoke OUTPUT_NAME probe OPTIONS -g)

add_custom_target(plugins DEPENDS core_mods example_mods smoke_probe mod_fixtures)

add_test(NAME test_build_core
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_build_core.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(test_build_core PROPERTIES LABELS nogame ENVIRONMENT "POP_CC=${CMAKE_C_COMPILER}")
```

- [ ] **Step 7: Add fixtures and the roots probe to `src/recomp/mods/CMakeLists.txt`**

Append:

```cmake
# Plugin fixtures the loader tests load. Every .c becomes a module of the same
# name; old_cpu is built with the older header AHEAD of the real one so its
# sizeof(pop_cpu_v1) really is the older value.
file(GLOB POP_FIXTURE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.c)
add_custom_target(mod_fixtures)
foreach(src ${POP_FIXTURE_SOURCES})
  get_filename_component(stem ${src} NAME_WE)
  set(first "")
  if(stem STREQUAL "old_cpu")
    set(first INCLUDE_FIRST ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_header)
  endif()
  pop_add_plugin(fixture_${stem} SOURCE ${src} OUTPUT_DIR ${POP_OUT}/mods-fixtures
    OUTPUT_NAME ${stem} ${first} OPTIONS -g)
  add_dependencies(mod_fixtures fixture_${stem})
endforeach()

# The roots resolver, run from a relocated app-shaped directory by roots_tests.sh.
add_executable(roots_probe roots.cpp ${POP_ROOT}/mods/core/tests/roots_probe.cpp)
target_include_directories(roots_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
pop_optimize(roots_probe 1)
pop_test_binary(roots_probe)
if(NOT WIN32)
  add_test(NAME roots_tests
    COMMAND sh ${POP_ROOT}/mods/core/tests/roots_tests.sh $<TARGET_FILE:roots_probe>
    WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(roots_tests PROPERTIES LABELS nogame)
endif()
```

- [ ] **Step 8: Wire the new files into the root and repoint `roots_tests.sh`**

In `CMakeLists.txt`, after `include(cmake/Warnings.cmake)` add `include(cmake/Plugins.cmake)`, and after the `src/recomp/mods` subdirectory add `add_subdirectory(mods)`.

In `mods/core/tests/roots_tests.sh` replace the `xcrun clang++ ... -o "$TEST/probe"` command (three lines) with:

```sh
PROBE=${1:?usage: roots_tests.sh <path to roots_probe>}
cp "$PROBE" "$TEST/probe"
```

- [ ] **Step 9: Build and run**

Run:
```sh
.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/cmake --build --preset macos --target plugins roots_probe
ls build/recomp/mods/core/display/display.dylib build/recomp/mods-fixtures/ mods/examples/logger/logger.dylib mods/smoke/probe.dylib
.venv/bin/ctest --preset macos -L nogame
```
Expected: the four paths exist; `mods-fixtures` holds one `.dylib` per fixture `.c` (18); CTest passes `dx_tests`, `host_api_header_check`, `test_build_core`, `roots_tests`.

Compare with the shell installer: `sh mods/core/build_core.sh build/recomp/mods/core` then `.venv/bin/cmake --build --preset macos --target core_mods`, then `cmp` is not meaningful across compilers' probe paths, so instead run `mods/core/tests/reproducibility_tests.sh` (still present) and confirm it passes with the shell version, and `test_build_core` passes with the Python version. Both hold the same guarantee.

- [ ] **Step 10: Commit**

```sh
.venv/bin/python tools/check_repo.py
git add cmake/Plugins.cmake tools/recomp/build_core.py tools/recomp/tests/test_build_core.py mods/CMakeLists.txt CMakeLists.txt src/recomp/mods/CMakeLists.txt mods/core/tests/roots_tests.sh
git commit -m "Build mod plugins with CMake and a Python core-mod installer"
```

---

### Task 3: Generated code, the parity fixture, profile and mods tests

**Files:**
- Create: `cmake/Translate.cmake`
- Modify: `CMakeLists.txt`, `src/recomp/runtime/CMakeLists.txt`, `src/recomp/mods/CMakeLists.txt`

**Interfaces:**
- Produces: variables `POP_GEN_DIR`, `POP_HAVE_GEN`; target `recomp_gen` (only when generated code exists); function `pop_link_gen(target)`; targets `pop_fixture`, `pop_fixture_trace`, `profile_tests`, `mods_tests`; CTest label `mods`.

- [ ] **Step 1: Write `cmake/Translate.cmake`**

```cmake
# The translated game. tools/build.py writes build/recomp/gen; this file turns
# it into build/recomp/librecomp_gen.a and says which targets can exist.
set(POP_GEN_DIR ${POP_OUT}/gen)
set(POP_HAVE_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but ${POP_GEN_DIR}/table.c is missing; run tools/build.py --regenerate")
else()
  message(STATUS "build/recomp/gen is absent: PopRecomp, pop_headless, pop_smoke, pop_fixture, "
                 "mods_tests, present_events_tests and profile_tests are not defined until "
                 "tools/build.py translates")
endif()

if(POP_HAVE_GEN)
  file(GLOB POP_GEN_SOURCES CONFIGURE_DEPENDS ${POP_GEN_DIR}/chunk_*.c ${POP_GEN_DIR}/table.c)
  add_library(recomp_gen STATIC ${POP_GEN_SOURCES})
  set_target_properties(recomp_gen PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${POP_OUT} OUTPUT_NAME recomp_gen)
  # -I<gen> for x86.h beside the sources, -I<root> for the canonical copy,
  # -I<runtime> for intrinsics.h: the same three the shell script passed.
  target_include_directories(recomp_gen PRIVATE ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime)
  target_include_directories(recomp_gen INTERFACE ${POP_GEN_DIR})
  target_compile_options(recomp_gen PRIVATE ${POP_WARN_GEN})
  pop_optimize(recomp_gen 2)
endif()

# The portable spelling of -Wl,-force_load: every generated object is kept
# whether or not anything references it, because the dispatch table is
# reached by address.
function(pop_link_gen target)
  target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>")
  target_include_directories(${target} PRIVATE ${POP_GEN_DIR})
endfunction()
```

In the root `CMakeLists.txt` add `include(cmake/Translate.cmake)` right after `include(cmake/Plugins.cmake)`.

- [ ] **Step 2: Add the fixture, trace fixture and profile tests to `src/recomp/runtime/CMakeLists.txt`**

Append:

```cmake
if(TARGET recomp_gen)
  # The headless parity fixture: null host, real generated code, core mods installed.
  add_executable(pop_fixture fixture.cpp)
  target_link_libraries(pop_fixture PRIVATE recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua)
  target_compile_options(pop_fixture PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture 1)
  pop_link_gen(pop_fixture)
  add_dependencies(pop_fixture core_mods)

  add_executable(profile_tests tests/profile_tests.cpp)
  target_link_libraries(profile_tests PRIVATE recomp_runtime recomp_dx_null)
  target_compile_options(profile_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(profile_tests 1)
  pop_link_gen(profile_tests)
  pop_test_binary(profile_tests)
  add_test(NAME profile_tests_disabled COMMAND profile_tests disabled WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(profile_tests_disabled PROPERTIES LABELS game ENVIRONMENT POPM_PROFILE=0)
  add_test(NAME profile_tests_enabled COMMAND profile_tests enabled WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(profile_tests_enabled PROPERTIES LABELS game ENVIRONMENT POPM_PROFILE=1)
endif()

if(POP_TRACE_DIR)
  # Caller-supplied trace wrappers over a snapshot of gen/: the override header
  # is force-included ahead of funcs.h so every FN_<addr> macro is already set.
  file(GLOB POP_TRACE_SOURCES CONFIGURE_DEPENDS ${POP_TRACE_DIR}/gen/chunk_*.c ${POP_TRACE_DIR}/gen/table.c)
  add_library(recomp_gen_trace OBJECT ${POP_TRACE_SOURCES} ${POP_TRACE_DIR}/trace_wrappers.c)
  target_include_directories(recomp_gen_trace PRIVATE ${POP_TRACE_DIR} ${POP_TRACE_DIR}/gen ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime)
  target_compile_options(recomp_gen_trace PRIVATE ${POP_WARN_GEN} -include ${POP_TRACE_DIR}/trace_override.h)
  pop_optimize(recomp_gen_trace 1)
  add_executable(pop_fixture_trace fixture.cpp)
  target_link_libraries(pop_fixture_trace PRIVATE recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua recomp_gen_trace)
  target_include_directories(pop_fixture_trace PRIVATE ${POP_TRACE_DIR}/gen)
  target_compile_options(pop_fixture_trace PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture_trace 1)
endif()
```

- [ ] **Step 3: Add `mods_tests` to `src/recomp/mods/CMakeLists.txt`**

Append:

```cmake
if(TARGET recomp_gen)
  add_library(mods_api_header_test OBJECT tests/api_header_test.c)
  target_compile_definitions(mods_api_header_test PRIVATE POPM_TESTING=1)
  target_include_directories(mods_api_header_test PRIVATE ${POP_ROOT} ${POP_ROOT}/src/recomp/runtime ${POP_GEN_DIR})
  target_compile_options(mods_api_header_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
  pop_optimize(mods_api_header_test 1)

  file(GLOB POP_MODS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(mods_tests ${POP_MODS_TEST_SOURCES} $<TARGET_OBJECTS:mods_api_header_test>)
  target_link_libraries(mods_tests PRIVATE recomp_runtime_testing recomp_snapshot recomp_mods_testing lua)
  target_compile_options(mods_tests PRIVATE ${POP_WARN_HOST})
  pop_optimize(mods_tests 1)
  pop_link_gen(mods_tests)
  add_dependencies(mods_tests mod_fixtures core_mods)
  add_test(NAME mods_tests COMMAND mods_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(mods_tests PROPERTIES LABELS mods)
endif()
```

Note `recomp_snapshot` is compiled without `POPM_TESTING`, as `build_tests.sh` compiled `snapshot.cpp` on the same command line as everything else with the define. `snapshot.cpp` does not reference `POPM_TESTING`, so the object is identical; keep the shared one.

- [ ] **Step 4: Configure both ways**

Run: `.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python` and confirm the status line says the gen directory is absent and lists the undefined targets. Run: `.venv/bin/cmake --preset macos -DPOP_TRANSLATE=ON` and confirm it fails with the message naming `tools/build.py --regenerate`. Run: `.venv/bin/cmake --preset macos -DPOP_TRANSLATE=AUTO` to restore.

**[game-equipped checkout]**: after `bash tools/recomp/build.sh` has produced `build/recomp/gen`, run `.venv/bin/cmake --preset macos && .venv/bin/cmake --build --preset macos --target pop_fixture profile_tests mods_tests` and then `.venv/bin/ctest --preset macos -L game -R profile`; then `POPM_TEST_GAME_VIEW_SNAPSHOT=<snapshot from a tools/test.py --mods run> .venv/bin/ctest --preset macos -L mods`. Expected: all pass, and `ls -l build/recomp/librecomp_gen.a` shows the archive.

- [ ] **Step 5: Commit**

```sh
git add cmake/Translate.cmake CMakeLists.txt src/recomp/runtime/CMakeLists.txt src/recomp/mods/CMakeLists.txt
git commit -m "Build the generated archive, parity fixture and mod tests with CMake"
```

---

### Task 4: Hosts, the app bundle and the host tests

**Files:**
- Create: `src/recomp/host/CMakeLists.txt`, `cmake/MacBundle.cmake`, `tools/recomp/finish_bundle.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `recomp_runtime`, `recomp_snapshot`, `recomp_dx`, `recomp_mods`, `lua`, `pop_link_gen`, `core_mods`, `Python3_EXECUTABLE`.
- Produces: object library `host_common`; targets `PopRecomp` (bundle), `pop_headless`, `pop_smoke`, `host_tests`, `compositor_tests`, `ui_layer_tests`, `present_events_tests`; function `pop_mac_bundle(target)`; CLI `finish_bundle.py --bundle DIR --name NAME --cc CC [--pack DIR]`.

- [ ] **Step 1: Write `tools/recomp/finish_bundle.py`**

This is the tail of `app_build.sh`: rename the bundle identity, package the probe list, install core mods, package the texture pack, seal.

```python
#!/usr/bin/env python3
"""Finish an assembled app bundle: identity, resources, core mods, texture pack, signature."""

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def rename_identity(plist_path, name):
    """A bundle built under another name gets its own identity, as app_build.sh did."""
    if name == "PopRecomp":
        return
    data = plistlib.loads(plist_path.read_bytes())
    data.update(CFBundleName=name, CFBundleDisplayName=name, CFBundleExecutable=name,
                CFBundleIdentifier="io.github.veritr1x.populousrecomp." + name.lower())
    plist_path.write_bytes(plistlib.dumps(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--pack", type=Path,
                        default=Path(os.environ.get("POPM_TEXTURE_PACK_DIR") or ROOT / "build/texture-pack"))
    args = parser.parse_args()
    contents = args.bundle / "Contents"
    resources = contents / "Resources"
    resources.mkdir(parents=True, exist_ok=True)
    rename_identity(contents / "Info.plist", args.name)
    # The committed probe list. A missing list is labelled a baseline fallback
    # by the settings layer; packaging never manufactures measurements.
    probes = ROOT / "tools/recomp/baseline/classic-modes.json"
    if probes.is_file():
        shutil.copy(probes, resources / "classic-modes.json")
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"),
                    "--dest", str(resources / "mods/core"), "--cc", args.cc], check=True, cwd=ROOT)
    if (args.pack / "manifest.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/package_texture_pack.py"),
                        str(args.pack), str(resources / "texture-pack")], check=True, cwd=ROOT)
    # Seal after every resource is in place: the linker signature alone does
    # not cover the resource envelope.
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(args.bundle)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(args.bundle)], check=True)
    print("built %s" % args.bundle.relative_to(ROOT) if args.bundle.is_relative_to(ROOT) else args.bundle)


if __name__ == "__main__":
    main()
```

`Path.is_relative_to` is 3.9+, which is the floor.

- [ ] **Step 2: Write `cmake/MacBundle.cmake`**

```cmake
# pop_mac_bundle(<target>): make <target> a .app in build/, named by
# POP_RECOMP_APP_NAME, with Info.plist copied verbatim and the resources,
# core mods, texture pack and ad-hoc signature applied after the link.
function(pop_mac_bundle target)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${POP_RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_ROOT}/build
    MACOSX_BUNDLE_INFO_PLIST ${POP_ROOT}/src/recomp/host/Info.plist)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/finish_bundle.py
            --bundle ${POP_ROOT}/build/${POP_RECOMP_APP_NAME}.app
            --name ${POP_RECOMP_APP_NAME} --cc ${CMAKE_C_COMPILER}
    WORKING_DIRECTORY ${POP_ROOT}
    COMMENT "Finishing ${POP_RECOMP_APP_NAME}.app"
    VERBATIM)
endfunction()
```

- [ ] **Step 3: Write `src/recomp/host/CMakeLists.txt`**

Each executable lists exactly the host sources its shell script listed, so which strong `host_*` callbacks a binary carries does not change.

```cmake
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})
set(BACKENDS ${POP_ROOT}/src/backends/cpu)

# Portable host code shared by every host that boots the guest.
add_library(host_common OBJECT boot.cpp report_lock.cpp)
target_include_directories(host_common PUBLIC ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime)
target_link_libraries(host_common PUBLIC recomp_runtime)
target_compile_options(host_common PRIVATE ${POP_WARN_HOST})
pop_optimize(host_common 1)

# Compiling Objective-C++ under ARC, as every script did; a no-op for C++.
set(POP_OBJC_ARC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

if(NOT APPLE)
  return()
endif()

set(FW_FOUNDATION "-framework Foundation")
set(FW_METAL "-framework Metal" "-framework MetalKit" "-framework CoreText"
             "-framework CoreVideo" "-framework CoreGraphics" "-framework QuartzCore")
set(FW_AUDIO "-framework AVFoundation" "-framework AudioToolbox")

# pop_host(<target> SOURCES ... FRAMEWORKS ... [LIBS ...]): a host or a test
# binary over the runtime, shims, mods and Lua.
function(pop_host target)
  cmake_parse_arguments(ARG "GEN;UI_LAYER" "OPT" "SOURCES;FRAMEWORKS;LIBS" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} ${ARG_FRAMEWORKS})
  target_include_directories(${target} PRIVATE ${POP_ROOT} ${POP_ROOT}/src ${POP_ROOT}/src/recomp/runtime ${POP_ROOT}/third_party/lua)
  target_compile_options(${target} PRIVATE ${POP_WARN_HOST} ${POP_OBJC_ARC})
  if(ARG_UI_LAYER)
    target_compile_definitions(${target} PRIVATE POPM_PRESENT_HAS_UI_LAYER=1)
  endif()
  if(NOT ARG_OPT)
    set(ARG_OPT 1)
  endif()
  pop_optimize(${target} ${ARG_OPT})
  if(ARG_GEN)
    pop_link_gen(${target})
  endif()
endfunction()

set(POP_CORE_LIBS recomp_runtime recomp_snapshot recomp_dx recomp_mods lua host_common)
set(POP_PRESENT_SOURCES ${HOST}/present_pixels.cpp ${HOST}/present_thread.mm ${HOST}/compositor.mm
                        ${HOST}/ui_layer.mm ${HOST}/d3d_render.mm ${BACKENDS}/indexed_frame.cpp)

if(TARGET recomp_gen)
  pop_host(pop_headless GEN
    SOURCES ${HOST}/headless_main.cpp ${HOST}/page_overlay.cpp
            ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_AUDIO})
  add_dependencies(pop_headless core_mods)

  pop_host(pop_smoke GEN UI_LAYER
    SOURCES ${HOST}/smoke_main.mm ${HOST}/script.cpp ${HOST}/input.mm ${HOST}/input_gate.cpp
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/audio_math.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  add_dependencies(pop_smoke core_mods smoke_probe)

  pop_host(PopRecomp GEN UI_LAYER OPT 2
    SOURCES ${HOST}/main.mm ${HOST}/present.mm ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES}
            ${HOST}/input.mm ${HOST}/input_gate.cpp
            ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp ${HOST}/midi.mm
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS "-framework Cocoa" ${FW_METAL} ${FW_AUDIO})
  pop_mac_bundle(PopRecomp)

  # Real loader, hooks and DirectDraw seal/service with fake GPU acks.
  pop_host(present_events_tests GEN UI_LAYER
    SOURCES ${POP_ROOT}/src/recomp/mods/tests/present_events_tests.mm
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.mm ${HOST}/input_gate.cpp
    LIBS recomp_runtime_testing recomp_snapshot recomp_dx recomp_mods_testing lua
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  add_test(NAME present_events_tests COMMAND present_events_tests ${POP_ROOT} WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(present_events_tests PROPERTIES LABELS mods)
endif()

# Headless host tests: offscreen Metal, no window, no audio device.
pop_host(host_tests UI_LAYER
  SOURCES ${HOST}/tests/host_tests.mm ${HOST}/tests/test_frame_builder.mm
          ${POP_ROOT}/src/recomp/runtime/tests/stub_recomp_call.cpp
          ${HOST}/report_lock.cpp ${HOST}/script.cpp ${HOST}/midi.mm ${HOST}/present.mm
          ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.mm ${HOST}/input_gate.cpp
          ${HOST}/audio.mm ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp
          ${POP_ROOT}/src/recomp/dx/com.cpp ${POP_ROOT}/src/recomp/dx/host_api.cpp
          ${POP_ROOT}/src/recomp/dx/display_stubs.cpp ${POP_ROOT}/src/recomp/dx/dinput.cpp
  LIBS recomp_runtime
  FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL} ${FW_AUDIO})
pop_test_binary(host_tests)
add_test(NAME host_tests COMMAND host_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_tests PROPERTIES LABELS gpu)

add_executable(compositor_tests ${HOST}/compositor.mm ${HOST}/tests/compositor_tests.mm)
target_include_directories(compositor_tests PRIVATE ${POP_ROOT})
target_compile_options(compositor_tests PRIVATE ${POP_WARN_WERROR} ${POP_OBJC_ARC})
target_link_libraries(compositor_tests PRIVATE ${FW_FOUNDATION} "-framework Metal")
pop_optimize(compositor_tests 1)
pop_test_binary(compositor_tests)
add_test(NAME compositor_tests COMMAND compositor_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(compositor_tests PROPERTIES LABELS gpu)

add_executable(ui_layer_tests ${HOST}/ui_layer.mm ${HOST}/tests/test_frame_builder.mm ${HOST}/tests/ui_layer_tests.mm)
target_compile_options(ui_layer_tests PRIVATE ${POP_WARN_WERROR})
pop_optimize(ui_layer_tests 1)
pop_test_binary(ui_layer_tests)
add_test(NAME ui_layer_tests COMMAND ui_layer_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(ui_layer_tests PROPERTIES LABELS nogame)
```

Note `host_tests` links `report_lock.cpp` from the host directory as a source rather than `host_common`, because `host_common` also carries `boot.cpp`, which the shell script did not link into this suite.

In the root `CMakeLists.txt` add `include(cmake/MacBundle.cmake)` after the Translate include and `add_subdirectory(src/recomp/host)` last.

- [ ] **Step 4: Build the host tests and run them**

Run:
```sh
.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/cmake --build --preset macos --target check_binaries
.venv/bin/ctest --preset macos -L "nogame|gpu"
```
Expected: builds `host_tests`, `compositor_tests`, `ui_layer_tests`; the two `-Werror` suites compile clean. `ui_layer_tests` and `compositor_tests` pass. If `host_tests` fails only because `original/gog/D3DPopTB.exe` is absent, change its label to `game` in this file, note it in the commit message, and rerun with `-L "nogame|gpu"`. Compare the pass count with `sh src/recomp/host/build_tests.sh` if it runs here.

**[game-equipped checkout]**: `.venv/bin/cmake --build --preset macos --target PopRecomp pop_headless pop_smoke`, then `open build/PopRecomp.app` reaches the menu; `codesign --verify --deep --strict build/PopRecomp.app` passes; `ls build/PopRecomp.app/Contents/Resources/mods/core/display/display.dylib build/PopRecomp.app/Contents/Resources/classic-modes.json` exist. Run `sh src/recomp/host/tests/integration_tests.sh` after Task 11 repoints it.

- [ ] **Step 5: Commit**

```sh
.venv/bin/python tools/check_repo.py
git add cmake/MacBundle.cmake tools/recomp/finish_bundle.py src/recomp/host/CMakeLists.txt CMakeLists.txt
git commit -m "Build the macOS hosts, app bundle and host tests with CMake"
```

---

### Task 5: The platform layer and its tests

**Files:**
- Create: `src/recomp/platform/os.h`, `src/recomp/platform/os_posix.cpp`, `src/recomp/platform/tests/platform_tests.cpp`, `src/recomp/platform/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: object library `recomp_platform`; the C API below, used by Tasks 6 to 8 and 13.

- [ ] **Step 1: Write `src/recomp/platform/os.h`**

```c
// os.h - the platform layer: what the runtime, the DirectX shims, the mod
// foundation and the shared host code need from the operating system. One
// POSIX implementation (macOS, Linux) and one Win32 implementation; nothing
// above this header includes a platform header, nothing here knows the guest.
//
// Every function is plain C with C linkage so C plugins and C++ hosts share
// it. Errors are reported the C way: -1 or NULL, errno where POSIX sets it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Threads. A created thread is an opaque handle; identity is a 64-bit id that
// compares with ==, because the scheduler asks "is the caller this thread"
// far more often than it joins anything.
// ---------------------------------------------------------------------------
typedef struct OsThread OsThread;
typedef uint64_t OsThreadId;
// `stack_bytes` 0 means the platform default. NULL when the thread could not
// be created; nothing has started in that case.
OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes);
void os_thread_join(OsThread *t);   // waits, then frees the handle
void os_thread_detach(OsThread *t); // frees the handle; the thread runs on
void os_thread_exit(void);          // ends the calling thread; never returns
OsThreadId os_thread_self(void);
OsThreadId os_thread_id_of(const OsThread *t);

// ---------------------------------------------------------------------------
// Virtual memory: a zero-filled, readable, writable, page-aligned region.
// ---------------------------------------------------------------------------
void *os_vm_reserve(size_t bytes); // NULL on failure
void os_vm_release(void *p, size_t bytes);

// ---------------------------------------------------------------------------
// Plugins.
// ---------------------------------------------------------------------------
void *os_dlopen(const char *path);
void *os_dlopen_noload(const char *path); // a handle only if already loaded
void *os_dlsym(void *handle, const char *name);
int os_dlclose(void *handle);             // 0 on success
const char *os_dlerror(void);             // text for the last failure
const char *os_plugin_extension(void);    // ".dylib", ".so" or ".dll"

// ---------------------------------------------------------------------------
// Paths. UTF-8 everywhere; the Win32 implementation converts.
// ---------------------------------------------------------------------------
typedef struct OsStat {
    uint64_t size;
    int64_t atime, mtime, ctime; // seconds since the epoch
    int is_dir, is_regular, is_symlink, is_readonly;
} OsStat;
int os_stat(const char *path, OsStat *out);  // follows symlinks; 0 or -1
int os_lstat(const char *path, OsStat *out); // does not follow
int os_mkdir(const char *path);              // 0, or -1 (an existing directory is -1, as mkdir)
int os_rename(const char *from, const char *to); // replaces an existing destination file
int os_unlink(const char *path);
int os_getcwd(char *buf, size_t cap);        // 0 or -1
int os_chdir(const char *path);
// Calls `fn` for every entry except "." and "..", in directory order. A
// nonzero return from `fn` stops the walk. -1 when the directory cannot be
// opened, 0 otherwise.
typedef int (*OsListDirFn)(const char *name, void *user);
int os_listdir(const char *dir, OsListDirFn fn, void *user);
// The trailing XXXXXX of `template_path` is replaced in place; returns an open
// read-write descriptor for the new file, or -1.
int os_mkstemp(char *template_path);

// ---------------------------------------------------------------------------
// Descriptors. Binary mode always; created files are mode 0644.
// ---------------------------------------------------------------------------
enum { OS_O_RDONLY = 0, OS_O_WRONLY = 1, OS_O_RDWR = 2,
       OS_O_CREAT = 0x40, OS_O_EXCL = 0x80, OS_O_TRUNC = 0x200 };
enum { OS_SEEK_SET = 0, OS_SEEK_CUR = 1, OS_SEEK_END = 2 };
int os_fd_open(const char *path, int flags);
int64_t os_fd_read(int fd, void *buf, size_t n);        // bytes read, 0 at EOF, -1 on error
int64_t os_fd_write(int fd, const void *buf, size_t n); // bytes written, -1 on error
int64_t os_fd_seek(int fd, int64_t off, int whence);    // new offset or -1
int os_fd_close(int fd);
int os_fd_dup(int fd);
int os_fd_fsync(int fd);
int os_fd_truncate(int fd, int64_t length);
int os_fd_stat(int fd, OsStat *out);
// A FILE over an open descriptor, which then owns it.
void *os_fdopen(int fd, const char *mode);

// ---------------------------------------------------------------------------
// Process.
// ---------------------------------------------------------------------------
int os_exe_path(char *buf, size_t cap); // 0 or -1; NUL-terminated
// Report a fatal signal inside guest code. `what` is "SIGSEGV", "SIGBUS" or
// "an abort from the runtime". Returns 1 when handlers were installed and 0
// where the platform has no equivalent yet.
typedef void (*OsFaultFn)(const char *what);
int os_install_fault_handlers(OsFaultFn fn);

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
uint64_t os_monotonic_ns(void); // never goes backwards; arbitrary origin
uint64_t os_wall_time_us(void); // microseconds since the Unix epoch
void os_sleep_us(uint64_t us);

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------
int os_strcasecmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing `src/recomp/platform/tests/platform_tests.cpp`**

```cpp
// platform_tests.cpp - the platform layer, checked without a guest, a window
// or a GPU. Runs on every platform the layer supports.
#include "../os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)

namespace {

struct ThreadReport {
    OsThreadId seen_self = 0;
    int ran = 0;
};

void *report_self(void *arg) {
    ThreadReport *r = (ThreadReport *)arg;
    r->seen_self = os_thread_self();
    r->ran = 1;
    return nullptr;
}

void *deep_stack(void *arg) {
    // 400 KB of stack, which fits a 512 KB request and not a 64 KB one.
    volatile char buf[400 * 1024];
    buf[0] = 1;
    buf[sizeof buf - 1] = 2;
    *(int *)arg = buf[0] + buf[sizeof buf - 1];
    return nullptr;
}

int collect_name(const char *name, void *user) {
    ((std::vector<std::string> *)user)->push_back(name);
    return 0;
}

std::string scratch_dir() {
    const char *base = getenv("POP_TEST_DIR");
    std::string dir = std::string(base && *base ? base : "build/recomp") + "/platform-test";
    os_mkdir(dir.c_str());
    return dir;
}

void test_threads() {
    ThreadReport r;
    OsThread *t = os_thread_create(report_self, &r, 0);
    CHECK(t != nullptr);
    OsThreadId id = os_thread_id_of(t);
    os_thread_join(t);
    CHECK(r.ran == 1);
    CHECK(r.seen_self == id);
    CHECK(r.seen_self != os_thread_self());

    int sum = 0;
    OsThread *big = os_thread_create(deep_stack, &sum, 512 * 1024);
    CHECK(big != nullptr);
    os_thread_join(big);
    CHECK(sum == 3);
}

void test_time() {
    uint64_t a = os_monotonic_ns();
    os_sleep_us(2000);
    uint64_t b = os_monotonic_ns();
    CHECK(b > a);
    CHECK(b - a >= 1000000ull); // at least 1 ms elapsed
    CHECK(os_wall_time_us() > 1600000000ull * 1000000ull); // after 2020
}

void test_vm() {
    size_t bytes = 1 << 20;
    uint8_t *p = (uint8_t *)os_vm_reserve(bytes);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p[0] == 0 && p[bytes - 1] == 0);
        p[bytes - 1] = 7;
        CHECK(p[bytes - 1] == 7);
        os_vm_release(p, bytes);
    }
}

void test_paths_and_descriptors() {
    std::string dir = scratch_dir();
    std::string sub = dir + "/listed";
    os_unlink((sub + "/a.txt").c_str());
    os_unlink((sub + "/b.txt").c_str());
    os_mkdir(sub.c_str());
    CHECK(os_mkdir(sub.c_str()) == -1); // already there, as mkdir reports it

    int fd = os_fd_open((sub + "/a.txt").c_str(), OS_O_RDWR | OS_O_CREAT | OS_O_TRUNC);
    CHECK(fd >= 0);
    CHECK(os_fd_write(fd, "hello", 5) == 5);
    CHECK(os_fd_seek(fd, 0, OS_SEEK_SET) == 0);
    char buf[8] = {0};
    CHECK(os_fd_read(fd, buf, sizeof buf) == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    OsStat st;
    CHECK(os_fd_stat(fd, &st) == 0 && st.size == 5 && st.is_regular && !st.is_dir);
    CHECK(os_fd_truncate(fd, 2) == 0);
    CHECK(os_fd_fsync(fd) == 0);
    int dup = os_fd_dup(fd);
    CHECK(dup >= 0 && dup != fd);
    CHECK(os_fd_close(dup) == 0);
    CHECK(os_fd_close(fd) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == 0 && st.size == 2);
    CHECK(os_fd_open((sub + "/a.txt").c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL) == -1);

    std::string tmpl = sub + "/b.XXXXXX";
    std::vector<char> t(tmpl.begin(), tmpl.end());
    t.push_back(0);
    int tfd = os_mkstemp(t.data());
    CHECK(tfd >= 0);
    CHECK(strncmp(t.data(), (sub + "/b.").c_str(), sub.size() + 3) == 0);
    CHECK(strcmp(t.data() + sub.size() + 3, "XXXXXX") != 0);
    FILE *f = (FILE *)os_fdopen(tfd, "wb");
    CHECK(f != nullptr);
    if (f)
        fclose(f);
    CHECK(os_rename(t.data(), (sub + "/b.txt").c_str()) == 0);
    CHECK(os_rename((sub + "/b.txt").c_str(), (sub + "/a.txt").c_str()) == 0); // replaces

    std::vector<std::string> names;
    CHECK(os_listdir(sub.c_str(), collect_name, &names) == 0);
    CHECK(names.size() == 1 && names[0] == "a.txt");
    CHECK(os_listdir((sub + "/nowhere").c_str(), collect_name, &names) == -1);

    CHECK(os_stat(sub.c_str(), &st) == 0 && st.is_dir);
    CHECK(os_lstat(sub.c_str(), &st) == 0 && !st.is_symlink);
    CHECK(os_unlink((sub + "/a.txt").c_str()) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == -1);

    char cwd[4096];
    CHECK(os_getcwd(cwd, sizeof cwd) == 0 && cwd[0] != 0);
    CHECK(os_chdir(cwd) == 0);
}

void test_process_and_strings() {
    char exe[4096];
    CHECK(os_exe_path(exe, sizeof exe) == 0);
    CHECK(strstr(exe, "platform_tests") != nullptr);
    CHECK(os_strcasecmp("Data", "DATA") == 0);
    CHECK(os_strcasecmp("a", "b") < 0);
    const char *ext = os_plugin_extension();
    CHECK(ext[0] == '.' && strlen(ext) >= 3);
    CHECK(os_dlopen_noload("/definitely/not/loaded") == nullptr);
    CHECK(os_dlopen("/definitely/not/a/plugin") == nullptr);
    CHECK(os_dlerror() != nullptr);
}

} // namespace

int main() {
    test_threads();
    test_time();
    test_vm();
    test_paths_and_descriptors();
    test_process_and_strings();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all platform tests passed\n");
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 3: Write `src/recomp/platform/CMakeLists.txt`**

```cmake
if(WIN32)
  set(POP_OS_SOURCE os_win32.cpp)
else()
  set(POP_OS_SOURCE os_posix.cpp)
endif()
add_library(recomp_platform OBJECT ${POP_OS_SOURCE})
target_include_directories(recomp_platform PUBLIC ${POP_ROOT})
target_compile_options(recomp_platform PRIVATE ${POP_WARN_STRICT})
pop_optimize(recomp_platform 2)
if(NOT WIN32 AND NOT APPLE)
  target_link_libraries(recomp_platform PUBLIC dl pthread)
endif()

add_executable(platform_tests tests/platform_tests.cpp)
target_link_libraries(platform_tests PRIVATE recomp_platform)
target_compile_options(platform_tests PRIVATE ${POP_WARN_STRICT})
pop_optimize(platform_tests 1)
pop_test_binary(platform_tests)
add_test(NAME platform_tests COMMAND platform_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(platform_tests PROPERTIES LABELS nogame)
```

Add `add_subdirectory(src/recomp/platform)` to the root `CMakeLists.txt` before the runtime subdirectory. `os_win32.cpp` is written in Task 13; until then configure fails on Windows, which nothing exercises yet.

- [ ] **Step 4: Run the test to see it fail**

Run: `.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python && .venv/bin/cmake --build --preset macos --target platform_tests`
Expected: the build fails because `os_posix.cpp` does not exist.

- [ ] **Step 5: Write `src/recomp/platform/os_posix.cpp`**

```cpp
// os_posix.cpp - the platform layer on macOS and Linux. The only file above
// third_party/ that may include a POSIX or Mach header besides os_win32.cpp.
#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

struct OsThread {
    pthread_t handle;
};

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!t)
        return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    int rc = pthread_create(&t->handle, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return nullptr;
    }
    return t;
}
void os_thread_join(OsThread *t) {
    pthread_join(t->handle, nullptr);
    free(t);
}
void os_thread_detach(OsThread *t) {
    pthread_detach(t->handle);
    free(t);
}
void os_thread_exit(void) {
    pthread_exit(nullptr);
}
OsThreadId os_thread_self(void) {
    return (OsThreadId)(uintptr_t)pthread_self();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return (OsThreadId)(uintptr_t)t->handle;
}

void *os_vm_reserve(size_t bytes) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void os_vm_release(void *p, size_t bytes) {
    munmap(p, bytes);
}

void *os_dlopen(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void *os_dlopen_noload(const char *path) {
    return dlopen(path, RTLD_NOLOAD | RTLD_LOCAL);
}
void *os_dlsym(void *handle, const char *name) {
    return dlsym(handle, name);
}
int os_dlclose(void *handle) {
    return dlclose(handle);
}
const char *os_dlerror(void) {
    const char *e = dlerror();
    return e ? e : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

static void fill_stat(const struct stat &st, OsStat *out) {
    out->size = (uint64_t)st.st_size;
    out->atime = (int64_t)st.st_atime;
    out->mtime = (int64_t)st.st_mtime;
    out->ctime = (int64_t)st.st_ctime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
    out->is_readonly = (st.st_mode & S_IWUSR) ? 0 : 1;
}
int os_stat(const char *path, OsStat *out) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_lstat(const char *path, OsStat *out) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_mkdir(const char *path) {
    return mkdir(path, 0755);
}
int os_rename(const char *from, const char *to) {
    return rename(from, to);
}
int os_unlink(const char *path) {
    return unlink(path);
}
int os_getcwd(char *buf, size_t cap) {
    return getcwd(buf, cap) ? 0 : -1;
}
int os_chdir(const char *path) {
    return chdir(path);
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(e->d_name, user) != 0)
            break;
    }
    closedir(d);
    return 0;
}
int os_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

static int native_flags(int flags) {
    int f = 0;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f = O_WRONLY;
        break;
    case OS_O_RDWR:
        f = O_RDWR;
        break;
    default:
        f = O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= O_CREAT;
    if (flags & OS_O_EXCL)
        f |= O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= O_TRUNC;
    return f;
}
int os_fd_open(const char *path, int flags) {
    return open(path, native_flags(flags), 0644);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    return (int64_t)read(fd, buf, n);
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    return (int64_t)write(fd, buf, n);
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return (int64_t)lseek(fd, (off_t)off, whence);
}
int os_fd_close(int fd) {
    return close(fd);
}
int os_fd_dup(int fd) {
    return dup(fd);
}
int os_fd_fsync(int fd) {
    return fsync(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return ftruncate(fd, (off_t)length);
}
int os_fd_stat(int fd, OsStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
#endif
}

static OsFaultFn g_fault_fn = nullptr;
static void fault_trampoline(int sig) {
    const char *what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGABRT ? "an abort from the runtime"
                                        : "a fatal signal";
    g_fault_fn(what);
}
int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    signal(SIGSEGV, fault_trampoline);
    signal(SIGBUS, fault_trampoline);
    signal(SIGABRT, fault_trampoline);
    return 1;
}

uint64_t os_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t os_wall_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
void os_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
}

int os_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

} // extern "C"
```

- [ ] **Step 6: Build and run the platform tests**

Run: `.venv/bin/cmake --build --preset macos --target platform_tests && .venv/bin/ctest --preset macos -R platform_tests --output-on-failure`
Expected: `all platform tests passed`. Format: `.venv/bin/python tools/format.py --write && .venv/bin/python tools/format.py`.

- [ ] **Step 7: Commit**

```sh
git add src/recomp/platform CMakeLists.txt
git commit -m "Add the platform layer with a POSIX implementation and tests"
```

---

### Task 6: Convert the runtime to the platform layer

**Files:**
- Modify: `src/recomp/runtime/memory.cpp`, `src/recomp/runtime/kernel32.cpp`, `src/recomp/runtime/misc.cpp`, `src/recomp/runtime/snapshot.cpp`, `src/recomp/runtime/fixture.cpp`, `src/recomp/runtime/CMakeLists.txt`, `src/recomp/dx/CMakeLists.txt`, `src/recomp/mods/CMakeLists.txt`, `src/recomp/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `os.h` from Task 5.
- Produces: `recomp_runtime` compiles with no POSIX header; every executable that links `recomp_runtime` now also links `recomp_platform`.

The mapping below is applied to every listed site. Each row is a literal before/after; apply it with the file open, not with a blind sed, because several sites read fields of `struct stat` afterwards.

| Before | After |
| --- | --- |
| `#include <pthread.h>`, `<dirent.h>`, `<fcntl.h>`, `<sys/stat.h>`, `<sys/mman.h>`, `<sys/time.h>`, `<unistd.h>`, `<strings.h>` | remove; add `#include "../platform/os.h"` (and `<mutex>`, `<condition_variable>`, `<chrono>` where used) |
| `pthread_mutex_t g_x = PTHREAD_MUTEX_INITIALIZER;` | `std::mutex g_x;` |
| `pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;` | `std::condition_variable_any g_cv;` |
| `pthread_mutex_lock(&g_x);` | `g_x.lock();` |
| `pthread_mutex_unlock(&g_x);` | `g_x.unlock();` |
| `pthread_cond_broadcast(&g_cv);` | `g_cv.notify_all();` |
| `pthread_t tid{};` (a stored identity) | `OsThreadId tid = 0;` |
| `pthread_self()` | `os_thread_self()` |
| `pthread_equal(a, b)` | `a == b` |
| `pthread_exit(nullptr);` | `os_thread_exit();` |
| `struct stat st; if (stat(p, &st) == 0)` | `OsStat st; if (os_stat(p, &st) == 0)` |
| `S_ISDIR(st.st_mode)` / `S_ISREG(st.st_mode)` / `S_ISLNK(st.st_mode)` | `st.is_dir` / `st.is_regular` / `st.is_symlink` |
| `!(st.st_mode & S_IWUSR)` | `st.is_readonly` |
| `st.st_size`, `st.st_ctime`, `st.st_atime`, `st.st_mtime` | `st.size`, `st.ctime`, `st.atime`, `st.mtime` |
| `mkdir(p, 0755)` | `os_mkdir(p)` |
| `unlink(p)` / `rename(a, b)` | `os_unlink(p)` / `os_rename(a, b)` |
| `strcasecmp(a, b)` | `os_strcasecmp(a, b)` |
| `open(p, flags, 0644)` with `O_*` bits | `os_fd_open(p, flags)` with `OS_O_*` bits |
| `read(fd, b, n)` / `write(fd, b, n)` into `ssize_t n` | `os_fd_read` / `os_fd_write` into `int64_t n` |
| `lseek(fd, (off_t)off, whence)` into `off_t` | `os_fd_seek(fd, (int64_t)off, whence)` into `int64_t` |
| `fstat(fd, &st)` | `os_fd_stat(fd, &st)` with `OsStat st` |
| `close(fd)` / `dup(fd)` / `fsync(fd)` / `ftruncate(fd, pos)` | `os_fd_close` / `os_fd_dup` / `os_fd_fsync` / `os_fd_truncate` |
| `clock_gettime(CLOCK_MONOTONIC, &ts)` into seconds | `os_monotonic_ns() * 1e-9` |
| `gettimeofday(&tv, nullptr)` into microseconds | `os_wall_time_us()` |
| `usleep(us)` | `os_sleep_us(us)` |

- [ ] **Step 1: `memory.cpp`**

Replace `#include <sys/mman.h>` with `#include "../platform/os.h"`. In `mem_init` and `mem_shutdown`:

```cpp
void mem_init() {
    if (g_mem) {
        os_vm_release(g_mem, GUEST_SIZE);
        g_mem = nullptr;
    }
    void *p = os_vm_reserve(GUEST_SIZE);
    if (!p) {
        fprintf(stderr, "[popm] fatal: cannot map %u bytes of guest memory\n", GUEST_SIZE);
        abort();
    }
    g_mem = (uint8_t *)p;
    heap_reset();
}
```
and the `munmap(g_mem, GUEST_SIZE)` in `mem_shutdown` becomes `os_vm_release(g_mem, GUEST_SIZE)`.

- [ ] **Step 2: `kernel32.cpp` threads and scheduler**

Includes: apply the table; add `<mutex>`, `<condition_variable>`, `<chrono>`.

`GuestThread::tid` becomes `OsThreadId tid = 0;`. The two globals become:

```cpp
std::mutex g_sched_m;
std::condition_variable_any g_sched_cv;
```

Every `pthread_mutex_lock(&g_sched_m)`, `unlock`, `pthread_cond_broadcast(&g_sched_cv)` follows the table (about 80 sites; `grep -n 'pthread_' src/recomp/runtime/kernel32.cpp` must print nothing when done).

The timed wait at the end of `sched_wait_locked` (the `struct timespec ts; clock_gettime(CLOCK_REALTIME, ...)` block through `pthread_cond_timedwait`) becomes:

```cpp
    g_sched_cv.wait_for(g_sched_m, std::chrono::duration<double>(wait));
```

`sched_now`:

```cpp
double sched_now() {
    return (double)os_monotonic_ns() * 1e-9;
}
```

`thread_spawn`'s create-and-detach:

```cpp
    threads().push_back(t);
    OsThread *host = os_thread_create(thread_host_main, t, 0);
    if (!host) {
        threads().pop_back();
        heap_free(t->stack_lo);
        heap_free(t->teb);
        delete t;
        return false;
    }
    t->tid = os_thread_id_of(host);
    os_thread_detach(host);
    t->spawned = true;
    return true;
```

`cur_thread()->tid = pthread_self();` becomes `cur_thread()->tid = os_thread_self();`. `pthread_equal(pthread_self(), candidate->tid)` becomes `os_thread_self() == candidate->tid`. `pthread_exit(nullptr);` becomes `os_thread_exit();`.

- [ ] **Step 3: `kernel32.cpp` files and directories**

`listing()`:

```cpp
int collect_listing(const char *name, void *user) {
    auto *entries = (std::map<std::string, std::string> *)user;
    entries->emplace(lower(name), name);
    return 0;
}

const std::map<std::string, std::string> &listing(const std::string &dir) {
    auto it = dir_cache().find(dir);
    if (it != dir_cache().end())
        return it->second;
    std::map<std::string, std::string> entries;
    os_listdir(dir.c_str(), collect_listing, &entries);
    return dir_cache().emplace(dir, std::move(entries)).first->second;
}
```

`attrs_for`:

```cpp
uint32_t attrs_for(const OsStat &st) {
    uint32_t a = st.is_dir ? FILE_ATTRIBUTE_DIRECTORY_ : FILE_ATTRIBUTE_ARCHIVE_;
    if (st.is_readonly)
        a |= FILE_ATTRIBUTE_READONLY_;
    return a;
}
```

`put_filetime(uint32_t addr, time_t t)` becomes `put_filetime(uint32_t addr, int64_t t)`; its callers pass `st.ctime`, `st.atime`, `st.mtime`.

The `CreateFile` flag construction:

```cpp
    int flags = want_write ? (access & 0x80000000u ? OS_O_RDWR : OS_O_WRONLY) : OS_O_RDONLY;
    switch (disp) {
    case 1:
        flags |= OS_O_CREAT | OS_O_EXCL;
        break; // CREATE_NEW
    case 2:
        flags |= OS_O_CREAT | OS_O_TRUNC;
        break; // CREATE_ALWAYS
    case 4:
        flags |= OS_O_CREAT;
        break; // OPEN_ALWAYS
    case 5:
        flags |= OS_O_TRUNC;
        break; // TRUNCATE_EXISTING
    default:
        break; // OPEN_EXISTING
    }
    int fd = os_fd_open(host.c_str(), flags);
```

Then, per the table: `read`/`write` (lines near 712, 740, 1203) with `int64_t n`; `lseek` (764, 820) with `int64_t pos` and `OS_SEEK_*` in place of `SEEK_*`; `fstat` (784, 1177); `close` (802); `fsync` (810); `ftruncate` (821); `dup` (1181); `stat` (326, 475, 836, 913); `mkdir` (861); `unlink` (885); `rename` (898); `strcasecmp` (3449, 3470). `k_SetEndOfFile`:

```cpp
    int64_t pos = os_fd_seek(o->fd, 0, OS_SEEK_CUR);
    set_eax(c, os_fd_truncate(o->fd, pos) == 0 ? 1 : 0);
```

- [ ] **Step 4: `misc.cpp`, `snapshot.cpp`, `fixture.cpp`**

`misc.cpp`: `now_us` returns `os_wall_time_us();`; `mkdir(path.substr(0, pos).c_str(), 0755)` becomes `os_mkdir(path.substr(0, pos).c_str())`; the `stat`/`S_ISREG` check at line 995 becomes `OsStat st; if (os_stat(host.c_str(), &st) == 0 && st.is_regular)`; `strcasecmp` at 357 becomes `os_strcasecmp`; includes per the table.

`snapshot.cpp`: `mkdir(acc.c_str(), 0755)` becomes `os_mkdir(acc.c_str())`; replace `<sys/stat.h>` with `"../platform/os.h"`.

`fixture.cpp`: remove `#include <unistd.h>` (nothing in the file uses it).

- [ ] **Step 5: Link the platform layer**

In `src/recomp/runtime/CMakeLists.txt`, add `target_link_libraries(recomp_runtime PUBLIC recomp_platform)` and the same for `recomp_runtime_testing`. Because object files do not propagate, add `recomp_platform` to the `target_link_libraries` list of every executable defined so far: `runtime_tests`, `pop_fixture`, `pop_fixture_trace`, `profile_tests` (runtime), `dx_tests` (dx), `mods_tests`, `roots_probe` is not yet a user (Task 7), and in `src/recomp/host/CMakeLists.txt` append `recomp_platform` to `POP_CORE_LIBS` and to the `LIBS` of `host_tests` and `present_events_tests`.

- [ ] **Step 6: Verify**

Run:
```sh
grep -nE 'pthread_|<dirent|<fcntl|<sys/stat|<sys/mman|<sys/time|<unistd|<strings\.h|strcasecmp\(|clock_gettime|gettimeofday|\bmmap\(' src/recomp/runtime/*.cpp src/recomp/runtime/*.h
```
Expected: no output. Then `.venv/bin/python tools/format.py --write`, `.venv/bin/cmake --build --preset macos --target check_binaries` and `.venv/bin/ctest --preset macos -L "nogame|gpu"`. Expected: build clean, all pass. **[game-equipped checkout]**: `.venv/bin/ctest --preset macos -L game` passes `runtime_tests` and both `profile_tests`.

- [ ] **Step 7: Commit**

```sh
git add src/recomp/runtime src/recomp/dx/CMakeLists.txt src/recomp/mods/CMakeLists.txt src/recomp/host/CMakeLists.txt
git commit -m "Route the runtime's threads, files, memory and clocks through the platform layer"
```

---

### Task 7: Convert the mod foundation, with plugin extension substitution

**Files:**
- Modify: `src/recomp/mods/loader.cpp`, `overlay.cpp`, `run_record.cpp`, `settings.cpp`, `host_services.cpp`, `roots.cpp`, `src/recomp/mods/tests/loader_tests.cpp`, `replay_tests_bridge.cpp`, `display_projection_tests.cpp`, `src/recomp/mods/CMakeLists.txt`

**Interfaces:**
- Consumes: `os.h`.
- Produces: the loader rule "a manifest's plugin path whose file is missing is retried with `os_plugin_extension()`"; `roots.cpp` portable.

- [ ] **Step 1: Write the failing loader test**

In `src/recomp/mods/tests/loader_tests.cpp`, after `install(...)`, add:

```cpp
// The fixture named with THIS platform's extension, for manifests and copies.
std::string plug(const char *stem) {
    return std::string(stem) + os_plugin_extension();
}
```

and, as a new suite beside the others (find `MOD_TEST_SUITE(loader_` for placement):

```cpp
MOD_TEST_SUITE(loader_plugin_extension_substitution) {
    fresh();
    // The manifest names a suffix from another platform; the shipped file has
    // this platform's. The loader must find it by stem.
    install("x", manifest("ext.sub", "[plugin]\npath = \"good_a.plugin\"\n"), plug("good_a").c_str());
    MOD_CHECK(mods_load_all());
    MOD_CHECK_EQ(mods_record_status("ext.sub"), POP_OK);
}
```

Run (**[game-equipped checkout]**, since `mods_tests` needs the archive): `.venv/bin/cmake --build --preset macos --target mods_tests && build/recomp/mods_tests 2>&1 | grep -A1 extension_substitution`. Expected: `FAILED` with a `dlopen failed` reason. Without a game-equipped checkout, confirm the file compiles: `.venv/bin/cmake --build --preset macos --target recomp_mods_testing` cannot compile tests; instead run `clang++ -std=c++20 -fsyntax-only -I. -Isrc -Isrc/recomp/runtime -Ibuild/recomp/gen -DPOPM_TESTING=1 src/recomp/mods/tests/loader_tests.cpp` and accept a failure only on the missing `funcs.h`.

- [ ] **Step 2: `loader.cpp`**

Replace `<dirent.h>` and `<dlfcn.h>` includes with `#include "../platform/os.h"`. The plugin open becomes:

```cpp
        if (why.empty() && !m.plugin_path.empty()) {
            std::string path = m.dir + "/" + m.plugin_path;
            OsStat st;
            if (os_stat(path.c_str(), &st) != 0) {
                // A manifest written on one platform names that platform's
                // extension; the plugin shipped for this one has the same stem.
                size_t dot = m.plugin_path.rfind('.');
                if (dot != std::string::npos)
                    path = m.dir + "/" + m.plugin_path.substr(0, dot) + os_plugin_extension();
            }
            c.handle = os_dlopen(path.c_str());
            if (!c.handle) {
                why = std::string("dlopen failed: ") + os_dlerror();
```

Every remaining `dlsym(` becomes `os_dlsym(` and `dlclose(` becomes `os_dlclose(` (lines 413, 686, 690, 692, 808). If `loader.cpp` uses `opendir`/`readdir` for mod discovery, convert with the `os_listdir` callback pattern from Task 6 Step 3; `grep -n 'opendir\|readdir\|DIR \*' src/recomp/mods/loader.cpp` shows the sites.

- [ ] **Step 3: `overlay.cpp`**

Includes per the table (`<dirent.h>`, `<sys/stat.h>`, `<unistd.h>` go; `os.h` comes). `resolve_in`'s case-insensitive lookup:

```cpp
struct NameMatch {
    std::string wanted_lower;
    std::string found;
    bool hit = false;
};
int match_name(const char *name, void *user) {
    NameMatch *m = (NameMatch *)user;
    if (lower(name) == m->wanted_lower) {
        m->found = name;
        m->hit = true;
        return 1;
    }
    return 0;
}
```

and inside the loop:

```cpp
        NameMatch match{lower(name), std::string(), false};
        if (os_listdir(host.c_str(), match_name, &match) != 0)
            return false;
        if (match.hit)
            name = match.found;
        bool found = match.hit;
        host += "/" + name;
```

`stat`/`lstat`/`S_ISDIR`/`S_ISLNK`/`S_ISREG`/`mkdir` per the table. In `copy_up`: `int fd = os_mkstemp(temp.data());`, `FILE *dest = (FILE *)os_fdopen(fd, "wb");`, `os_fd_close(fd)`, `os_unlink(temp.c_str())`, `os_rename(temp.c_str(), to.c_str())`. The enumeration at line 341 collects into `out` through a callback struct holding `seen`, `out` and `host`, following the `collect_listing` shape.

- [ ] **Step 4: `run_record.cpp`, `settings.cpp`, `host_services.cpp`, `roots.cpp`**

`run_record.cpp`: `pthread_mutex_t g_payloads_lock = PTHREAD_MUTEX_INITIALIZER;` becomes `std::mutex g_payloads_lock;` with `.lock()`/`.unlock()` at the four sites; `hash_tree`'s directory read becomes:

```cpp
    std::vector<std::string> names;
    if (os_listdir(dir.c_str(), collect_name, &names) != 0) {
        if (missing)
            *missing = true;
        return h;
    }
```
with `int collect_name(const char *name, void *user) { ((std::vector<std::string> *)user)->push_back(name); return 0; }` above it; `stat`/`S_ISDIR`/`S_ISREG` per the table; `rename(tmp.c_str(), path)` becomes `os_rename(tmp.c_str(), path)`. Includes: `<dirent.h>`, `<pthread.h>`, `<sys/stat.h>` out; `os.h`, `<mutex>` in.

`settings.cpp`: `<unistd.h>` out, `os.h` in; `mkstemp` → `os_mkstemp`; `fdopen` → `(FILE *)os_fdopen`; `close(fd)` → `os_fd_close(fd)`; `unlink` → `os_unlink`; `fsync(fd)` → `os_fd_fsync(fd)`; `rename` → `os_rename`.

`host_services.cpp`: `<pthread.h>` out, `os.h` in; `pthread_t g_main{};` → `OsThreadId g_main = 0;`; `g_main = pthread_self();` → `g_main = os_thread_self();`; `pthread_equal(pthread_self(), g_main)` → `os_thread_self() == g_main`.

`roots.cpp` in full:

```cpp
#include "roots.h"
#include "../platform/os.h"
#include <cstdlib>
#include <string>

// The core root defaults to the checkout's build tree. Inside a macOS bundle
// the executable sits in Contents/MacOS and the packaged core mods beside it
// in Contents/Resources, so a relocated bundle still finds its own plugins.
ModsRoots mods_roots() {
    const char *core = std::getenv("POPM_CORE_MODS_DIR");
    const char *user = std::getenv("POPM_MODS_DIR");
    ModsRoots roots{core && *core ? core : "build/recomp/mods/core", user && *user ? user : "mods"};
    if (!core || !*core) {
        char path[4096];
        if (os_exe_path(path, sizeof path) == 0) {
            std::string exe(path);
            auto slash = exe.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = exe.substr(0, slash);
                if (dir.ends_with("/Contents/MacOS"))
                    roots.core = dir + "/../Resources/mods/core";
            }
        }
    }
    return roots;
}
```

In `src/recomp/mods/CMakeLists.txt` add `recomp_platform` to `roots_probe`'s `target_link_libraries` (add the call: `target_link_libraries(roots_probe PRIVATE recomp_platform)`).

- [ ] **Step 5: Tests that load fixtures**

`loader_tests.cpp`: replace `#include <dlfcn.h>`, `<sys/stat.h>`, `<unistd.h>` with `#include "../../platform/os.h"`; every `dlopen(X, RTLD_NOW | RTLD_LOCAL)` → `os_dlopen(X)`; every `dlopen(X, RTLD_NOLOAD)` → `os_dlopen_noload(X)`; `dlsym` → `os_dlsym`; `dlclose` → `os_dlclose`; every string literal `"<stem>.dylib"` passed to `install` or built into a path → `plug("<stem>").c_str()` (for `install`'s third argument) or `plug("<stem>")` (inside `std::string` concatenation), and inside manifest text `path = \"<stem>.dylib\"` → `path = \"" + plug("<stem>") + "\"`. Line 248's `"build/recomp/mods/core/display/display.dylib"` becomes `("build/recomp/mods/core/display/" + plug("display")).c_str()`. `grep -n 'dylib\|dlopen\|dlsym\|dlclose\|RTLD' src/recomp/mods/tests/loader_tests.cpp` must print nothing when done.

`replay_tests_bridge.cpp` line 221: `os_dlopen(("build/recomp/mods-fixtures/capture_mod" + std::string(os_plugin_extension())).c_str())`; `dlsym`/`dlclose` per the table; the `#include <dlfcn.h>` at line 209 → `#include "../../platform/os.h"`.

`display_projection_tests.cpp` lines 95 to 105 and 367: the same three substitutions with `display_projection` as the stem.

`manifest_tests.cpp` keeps its literal `widescreen.dylib`: it tests parsing, not loading.

- [ ] **Step 6: Verify**

```sh
grep -nE 'pthread_|<dirent|<dlfcn|<sys/stat|<unistd|\bdlopen\(|\bdlsym\(|\bdlclose\(|mkstemp\(|\bstat\(|opendir|__APPLE__|_NSGetExecutablePath' src/recomp/mods/*.cpp src/recomp/mods/tests/loader_tests.cpp src/recomp/mods/tests/replay_tests_bridge.cpp src/recomp/mods/tests/display_projection_tests.cpp
```
Expected: no output. `.venv/bin/python tools/format.py --write`; `.venv/bin/cmake --build --preset macos --target check_binaries recomp_mods roots_probe && .venv/bin/ctest --preset macos -L "nogame|gpu"` all pass, including `roots_tests`. **[game-equipped checkout]**: `.venv/bin/python tools/test.py --mods` passes once Task 11 lands; until then, build `mods_tests` and run it with the snapshot environment as in Task 3 Step 4, and confirm `loader_plugin_extension_substitution ok`.

- [ ] **Step 7: Commit**

```sh
git add src/recomp/mods
git commit -m "Route the mod foundation through the platform layer and resolve plugin extensions per platform"
```

---

### Task 8: Convert the shared host code and audit `long`

**Files:**
- Modify: `src/recomp/host/boot.cpp`, `src/recomp/host/report_lock.cpp`, `src/recomp/host/present_pixels.cpp`, plus whatever the `long` audit touches

- [ ] **Step 1: `boot.cpp`**

Includes: `<pthread.h>`, `<signal.h>`, `<unistd.h>` out; `"../platform/os.h"` in (`<setjmp.h>`, `<time.h>` stay only if still used; `<time.h>` goes once both `clock_gettime` sites are converted).

`pthread_t g_guest_thread;` → `OsThreadId g_guest_thread = 0;`. Both `now_seconds()`-style bodies at lines 45 to 48 and 150 become `return (double)os_monotonic_ns() * 1e-9;`. `usleep(200 * 1000)` at 297 → `os_sleep_us(200 * 1000)`. The two `pthread_equal(pthread_self(), g_guest_thread)` → `os_thread_self() == g_guest_thread`. `g_guest_thread = pthread_self();` → `g_guest_thread = os_thread_self();`.

The fault handler signature and body:

```cpp
void fault_handler(const char *name) {
    // guest_current_context() reads only thread-local state and a vector that
    // is never resized while guest code runs, so it is safe to ask here.
    X86 *c = guest_current_context();
```
(delete the `const char *name = sig == SIGSEGV ? ...` computation; the rest of the body is unchanged).

Installation and the watchdog:

```cpp
    if (g_opt.signal_handlers && !os_install_fault_handlers(fault_handler))
        fprintf(stderr, "[host] fault reporting is not available on this platform\n");

    g_t0 = now_seconds();
    g_guest_thread = os_thread_self();
    g_guest_thread_known = true;
    g_bail_armed = true;
    if (g_opt.deadline_seconds > 0.0 || g_opt.close_watchdog_grace > 0.0) {
        OsThread *watchdog = os_thread_create(watchdog_main, nullptr, 0);
        if (watchdog)
            os_thread_detach(watchdog);
        else
            fprintf(stderr, "[host] could not start the watchdog; a wedged guest will hang\n");
    }
```

- [ ] **Step 2: `report_lock.cpp`**

```cpp
#include "boot.h"
#include "../platform/os.h"

#include <mutex>

namespace {
std::mutex g_report_m;
}

void boot_report_lock() {
    g_report_m.lock();
}
void boot_report_unlock() {
    g_report_m.unlock();
}

bool boot_report_trylock_for(double seconds) {
    for (int i = 0; i < (int)(seconds * 100.0) + 1; ++i) {
        if (g_report_m.try_lock())
            return true;
        os_sleep_us(10 * 1000);
    }
    return false;
}
```
Keep the file's header comment.

- [ ] **Step 3: `present_pixels.cpp`**

`mkdir(acc.c_str(), 0755)` at line 208 → `os_mkdir(acc.c_str())`; `#include <sys/stat.h>` → `#include "../platform/os.h"`.

- [ ] **Step 4: The `long` audit**

Run: `grep -rnE '\b(unsigned )?long\b' src/recomp/runtime src/recomp/dx src/recomp/mods src/recomp/host/*.cpp tools/recomp/runtime/x86.h | grep -vE 'long long|long double|tests/|//'`.

For each hit decide: a comment (leave); `long` holding a byte count, offset, pointer or 64-bit value (change to `int64_t`, `size_t` or `intptr_t`); `long` feeding a C API that takes `long` such as `ftell`/`strtol` (leave, but cast the result into a 64-bit local if it is stored). Known hits: `fixture.cpp:211 long n = ftell(f);` stays (it is what `ftell` returns; add `if (n < 0) ...` only if missing). Record the decision for every non-comment hit in the commit message body as `file:line kept|changed`.

- [ ] **Step 5: Verify**

```sh
grep -nE 'pthread_|<signal|<unistd|<sys/stat|clock_gettime|usleep\(' src/recomp/host/boot.cpp src/recomp/host/report_lock.cpp src/recomp/host/present_pixels.cpp src/recomp/host/input_gate.cpp src/recomp/host/audio_math.cpp src/recomp/host/audio_capture.cpp src/recomp/host/script.cpp src/recomp/host/page_overlay.cpp
```
Expected: no output. `.venv/bin/python tools/format.py --write && .venv/bin/cmake --build --preset macos --target check_binaries && .venv/bin/ctest --preset macos -L "nogame|gpu"` pass. **[game-equipped checkout]**: build `pop_headless` and run `POP_RECOMP_MAX_FRAMES=60 POP_RECOMP_FRAME_EVERY=0 build/recomp/pop_headless`; expected the run summary prints and exits 0; then `kill -SEGV` a second run mid-way and confirm the `[host]` fault report still names the guest EIP.

- [ ] **Step 6: Commit**

```sh
git add src/recomp
git commit -m "Route shared host code through the platform layer and audit long for LLP64"
```

---

### Task 9: The build lock in Python

**Files:**
- Create: `tools/recomp/buildlock.py`, `tools/recomp/tests/test_buildlock.py`
- Modify: `tools/recomp/buildlock.sh`

**Interfaces:**
- Produces: module `buildlock` with `held() -> bool`, `class BuildLock(root, what, wait=900.0)` context manager whose `fd` is `None` when the lock was already held, `run(root, what, command, wait=900.0) -> int`; CLI `buildlock.py run ROOT WHAT COMMAND...` honoring `BUILDLOCK_WAIT`.

- [ ] **Step 1: Write the failing tests**

`tools/recomp/tests/test_buildlock.py`:

```python
"""The build lock: one holder at a time, released when a holder dies, skipped when inherited."""

import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

MODULE = Path(__file__).parents[1] / "buildlock.py"
spec = importlib.util.spec_from_file_location("buildlock", MODULE)
buildlock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(buildlock)

CONTENDER = r"""
import os, sys, time
sys.path.insert(0, sys.argv[1]); import buildlock
root = sys.argv[2]
with buildlock.BuildLock(root, "contender %d" % os.getpid(), wait=30):
    marker = os.path.join(root, "inside")
    try:
        os.mkdir(marker)
    except FileExistsError:
        with open(os.path.join(root, "violations"), "a") as f:
            f.write("%d\n" % os.getpid())
        sys.exit(1)
    time.sleep(0.01)
    os.rmdir(marker)
"""

HOLDER = r"""
import os, sys, time
sys.path.insert(0, sys.argv[1]); import buildlock
with buildlock.BuildLock(sys.argv[2], "holder", wait=30):
    open(os.path.join(sys.argv[2], "held"), "w").close()
    time.sleep(60)
"""


class BuildLockTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="buildlock-")
        self.env = {k: v for k, v in os.environ.items() if k != "BUILDLOCK_HELD"}

    def test_contenders_never_overlap(self):
        for _ in range(20):
            procs = [subprocess.Popen([sys.executable, "-c", CONTENDER, str(MODULE.parent), self.root],
                                      env=self.env) for _ in range(4)]
            for p in procs:
                self.assertEqual(p.wait(), 0)
        self.assertFalse(os.path.exists(os.path.join(self.root, "violations")))
        self.assertFalse(os.path.exists(os.path.join(self.root, "inside")))

    def test_killed_holder_releases_the_lock(self):
        holder = subprocess.Popen([sys.executable, "-c", HOLDER, str(MODULE.parent), self.root], env=self.env)
        deadline = time.time() + 10
        while not os.path.exists(os.path.join(self.root, "held")) and time.time() < deadline:
            time.sleep(0.02)
        self.assertTrue(os.path.exists(os.path.join(self.root, "held")))
        with self.assertRaises(TimeoutError):
            with buildlock.BuildLock(self.root, "probe", wait=0.5):
                pass
        holder.send_signal(signal.SIGKILL if hasattr(signal, "SIGKILL") else signal.SIGTERM)
        holder.wait()
        with buildlock.BuildLock(self.root, "after", wait=5) as lock:
            self.assertIsNotNone(lock.fd)

    def test_inherited_lock_is_not_taken_again(self):
        os.environ["BUILDLOCK_HELD"] = "1"
        try:
            with buildlock.BuildLock(self.root, "child") as lock:
                self.assertIsNone(lock.fd)
            self.assertFalse(os.path.exists(os.path.join(self.root, "build/recomp/.lock")))
        finally:
            del os.environ["BUILDLOCK_HELD"]

    def test_cli_runs_the_command_under_the_lock_and_returns_its_status(self):
        probe = "import os, sys; sys.exit(7 if os.environ.get('BUILDLOCK_HELD') == '1' else 3)"
        result = subprocess.run([sys.executable, str(MODULE), "run", self.root, "cli test",
                                 sys.executable, "-c", probe], env=self.env)
        self.assertEqual(result.returncode, 7)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run them to see them fail**

Run: `.venv/bin/python -m unittest tools/recomp/tests/test_buildlock.py`
Expected: import failure on the missing module.

- [ ] **Step 3: Write `tools/recomp/buildlock.py`**

```python
#!/usr/bin/env python3
"""An advisory lock over build/recomp, shared by every build and game-backed test.

    tools/recomp/buildlock.py run ROOT DESCRIPTION COMMAND...   # from shell scripts
    with buildlock.BuildLock(root, "what I am doing"):          # from Python

The kernel owns the lock and releases it when the holder exits however it
exits, so there is no stale state to detect. BUILDLOCK_HELD=1 in the
environment says a parent already holds it and it must not be taken again;
the context manager sets that for its own children.
"""

import os
import subprocess
import sys
import threading

LOCK_RELATIVE = os.path.join("build", "recomp", ".lock")
DEFAULT_WAIT = 900.0


def held():
    """Whether a parent process already holds the lock."""
    return os.environ.get("BUILDLOCK_HELD") == "1"


def _try_lock(fd):
    """Take the lock without waiting; False when somebody else has it."""
    if os.name == "nt":
        import msvcrt
        os.lseek(fd, 0, os.SEEK_SET)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False
    import fcntl
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def _lock_blocking(fd):
    """Wait for the lock. Windows region locks give up after ten tries, so retry."""
    if os.name == "nt":
        import msvcrt
        import time
        while True:
            os.lseek(fd, 0, os.SEEK_SET)
            try:
                msvcrt.locking(fd, msvcrt.LK_LOCK, 1)
                return
            except OSError:
                time.sleep(0.1)
    import fcntl
    fcntl.flock(fd, fcntl.LOCK_EX)


def _read_note(fd):
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        note = os.read(fd, 256).decode("utf-8", "replace").replace("\0", "").strip()
        return note or "another build"
    except OSError:
        return "another build"


def _write_note(fd, text):
    # Truncating does not move the offset, and a waiter that sat in the lock
    # still has one from its own read; seek first or the note lands after NULs.
    os.ftruncate(fd, 0)
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(fd, (text + "\n").encode())
    os.fsync(fd)


class BuildLock(object):
    """Holds build/recomp/.lock for a with-block. `fd` is None when a parent held it already."""

    def __init__(self, root, what, wait=DEFAULT_WAIT):
        self.path = os.path.join(str(root), LOCK_RELATIVE)
        self.what = what
        self.wait = wait
        self.fd = None
        self._exported = False

    def __enter__(self):
        if held():
            return self
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o644)
        if not _try_lock(fd):
            # Say who we are waiting for, once. Advisory only: correctness
            # comes from the lock, not from this note.
            print("buildlock: waiting for %s" % _read_note(fd), file=sys.stderr, flush=True)
            done = threading.Event()

            def acquire():
                _lock_blocking(fd)
                done.set()

            threading.Thread(target=acquire, daemon=True).start()
            if not done.wait(self.wait):
                # The waiter thread is still parked in the lock call; the
                # descriptor is left to it and dies with the process.
                raise TimeoutError("buildlock: %s still held after %gs; giving up" % (self.path, self.wait))
        _write_note(fd, "%s (pid %d)" % (self.what, os.getpid()))
        self.fd = fd
        os.environ["BUILDLOCK_HELD"] = "1"
        self._exported = True
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.fd is not None:
            try:
                os.ftruncate(self.fd, 0)
            except OSError:
                pass
            os.close(self.fd)  # the kernel drops the lock with the descriptor
            self.fd = None
        if self._exported:
            os.environ.pop("BUILDLOCK_HELD", None)
            self._exported = False
        return False


def run(root, what, command, wait=DEFAULT_WAIT):
    """Runs `command` with the lock held and returns its exit status.

    On POSIX the locked descriptor is handed to the child, so the lock stands
    for as long as anything the child started is still running, even if this
    wrapper is killed. Windows has no inheritable region lock; there the lock
    lives exactly as long as this process."""
    with BuildLock(root, what, wait) as lock:
        kwargs = {}
        if lock.fd is not None and os.name != "nt":
            kwargs["pass_fds"] = (lock.fd,)
        return subprocess.call(command, **kwargs)


def main(argv):
    if len(argv) < 4 or argv[0] != "run":
        print("usage: buildlock.py run ROOT DESCRIPTION COMMAND...", file=sys.stderr)
        return 2
    wait = float(os.environ.get("BUILDLOCK_WAIT", DEFAULT_WAIT))
    try:
        return run(argv[1], argv[2], argv[3:], wait)
    except TimeoutError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run the tests**

Run: `.venv/bin/python -m unittest -v tools/recomp/tests/test_buildlock.py`
Expected: 4 tests pass in under 30 seconds.

- [ ] **Step 5: Make `buildlock.sh` delegate**

Replace everything from `# buildlock.sh run ROOT DESCRIPTION COMMAND...` through the end of the `if [ "${1:-}" = "run" ]; then ... fi` block (the inline Python heredoc included) with:

```sh
# buildlock.sh run ROOT DESCRIPTION COMMAND...
#   Runs COMMAND with build/recomp/.lock held, and exits with its status.
#   The lock itself lives in tools/recomp/buildlock.py; this is the shell entry.
if [ "${1:-}" = "run" ]; then
    _root=$2
    shift 1
    exec "$(buildlock_python "$_root")" "$_root/tools/recomp/buildlock.py" run "$@"
fi
```

`buildlock_acquire`, `buildlock_release`, `buildlock_python` and the header comment stay. Update the header's "HOW IT WORKS" sentence that describes the inline Python to say the lock is implemented in `buildlock.py`.

Run: `sh tools/recomp/tests/test_buildlock.sh 20 4`
Expected: the shell race test still passes (no `VIOLATION`, exit 0).

- [ ] **Step 6: Commit**

```sh
git add tools/recomp/buildlock.py tools/recomp/buildlock.sh tools/recomp/tests/test_buildlock.py
git commit -m "Move the build lock into a Python module shared by scripts and entry points"
```

---

### Task 10: `tools/build.py` over CMake, translation publishing, and the snapshot tool

**Files:**
- Modify: `tools/build.py`
- Create: `tools/recomp/tests/test_translate_publish.py`, `tests/test_build_py.py`, `tools/recomp/snapshot_gen.py`

**Interfaces:**
- Consumes: `buildlock.BuildLock`.
- Produces: `build.py` CLI `[--regenerate] [--target app|smoke|headless|fixture|gen|plugins] [--jobs N] [--preset P] [--config Release|Debug]`; module functions `default_preset(system=None)`, `preset_name(preset, config)`, `archive_path(root, system=None)`, `publish_generated(root, translate)`, `parse_args(argv, system=None)`, `configure(preset, extra=())`, `build(preset, targets, jobs)`, `cmake_tool(name)`; `snapshot_gen.py DIR` CLI.

- [ ] **Step 1: Write the failing publish tests**

`tools/recomp/tests/test_translate_publish.py`:

```python
"""Publishing a translation: a failure leaves the published tree alone; a success swaps it whole."""

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("build_py", Path(__file__).parents[3] / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="publish-"))
        (self.root / "tools/recomp/runtime").mkdir(parents=True)
        (self.root / "tools/recomp/runtime/x86.h").write_text("// x86\n")
        self.gen = self.root / "build/recomp/gen"
        self.gen.mkdir(parents=True)
        (self.gen / "table.c").write_text("old\n")
        (self.gen / "symbols.json").write_text('{"generation": 1}\n')
        (self.root / "build/recomp/symbols.json").write_text('{"generation": 1}\n')

    def test_failed_translation_leaves_the_published_tree_untouched(self):
        def failing(stage):
            (stage / "table.c").write_text("half\n")
            raise RuntimeError("translator died")

        with self.assertRaises(RuntimeError):
            build_py.publish_generated(self.root, failing)
        self.assertEqual((self.gen / "table.c").read_text(), "old\n")
        self.assertEqual(sorted(p.name for p in self.gen.parent.iterdir()), ["gen", "symbols.json"])

    def test_successful_translation_publishes_sources_header_and_symbols(self):
        def succeeding(stage):
            (stage / "table.c").write_text("new\n")
            (stage / "chunk_000.c").write_text("void fn_00401000(void) {}\n")
            (stage / "symbols.json").write_text('{"generation": 2}\n')

        build_py.publish_generated(self.root, succeeding)
        self.assertEqual((self.gen / "table.c").read_text(), "new\n")
        self.assertTrue((self.gen / "chunk_000.c").is_file())
        self.assertEqual((self.gen / "x86.h").read_text(), "// x86\n")
        self.assertEqual((self.root / "build/recomp/symbols.json").read_text(), '{"generation": 2}\n')
        self.assertEqual(sorted(p.name for p in self.gen.parent.iterdir()), ["gen", "symbols.json"])

    def test_first_translation_works_without_a_previous_tree(self):
        import shutil
        shutil.rmtree(self.gen)
        build_py.publish_generated(self.root, lambda stage: (stage / "table.c").write_text("first\n"))
        self.assertEqual((self.gen / "table.c").read_text(), "first\n")


if __name__ == "__main__":
    unittest.main()
```

`tests/test_build_py.py`:

```python
"""tools/build.py argument handling and CMake invocation, without CMake or game files."""

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("build_py", Path(__file__).parents[1] / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)


class BuildPyTests(unittest.TestCase):
    def test_default_preset_follows_the_operating_system(self):
        self.assertEqual(build_py.default_preset("Darwin"), "macos")
        self.assertEqual(build_py.default_preset("Linux"), "linux")
        self.assertEqual(build_py.default_preset("Windows"), "windows")

    def test_debug_config_selects_the_debug_preset(self):
        self.assertEqual(build_py.preset_name("macos", "Release"), "macos")
        self.assertEqual(build_py.preset_name("linux", "Debug"), "linux-debug")

    def test_archive_path_per_platform(self):
        root = Path("/r")
        self.assertEqual(build_py.archive_path(root, "Darwin"), root / "build/recomp/librecomp_gen.a")
        self.assertEqual(build_py.archive_path(root, "Windows"), root / "build/recomp/recomp_gen.lib")

    def test_macos_hosts_are_refused_elsewhere(self):
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--target", "smoke"], system="Linux")
        args, _ = build_py.parse_args(["--target", "fixture"], system="Linux")
        self.assertEqual(args.preset, "linux")

    def test_jobs_must_be_positive(self):
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--jobs", "0"], system="Darwin")

    def test_build_passes_targets_and_parallelism_to_cmake(self):
        with patch.object(build_py.subprocess, "run") as run:
            build_py.build("macos", ["pop_smoke"], 6)
        command = run.call_args[0][0]
        self.assertIn("--build", command)
        self.assertEqual(command[command.index("--preset") + 1], "macos")
        self.assertEqual(command[command.index("--parallel") + 1], "6")
        self.assertEqual(command[command.index("--target") + 1:], ["pop_smoke"])


if __name__ == "__main__":
    unittest.main()
```

Run: `.venv/bin/python -m unittest tools/recomp/tests/test_translate_publish.py tests/test_build_py.py`
Expected: attribute errors on the missing functions.

- [ ] **Step 2: Rewrite `tools/build.py`**

```python
#!/usr/bin/env python3
"""Build the native app through CMake, regenerating original-game code only when needed."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

# What each --target builds. `plugins` is every mod plugin the tree ships.
TARGETS = {
    "app": ["PopRecomp"],
    "smoke": ["pop_smoke"],
    "headless": ["pop_headless"],
    "fixture": ["pop_fixture"],
    "gen": ["recomp_gen"],
    "plugins": ["plugins"],
}
MACOS_ONLY = {"app", "smoke", "headless"}
NEEDS_GEN = {"app", "smoke", "headless", "fixture", "gen"}


def default_preset(system=None):
    """The CMake preset for this operating system."""
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system or platform.system()]


def preset_name(preset, config):
    """Debug builds live in their own binary directory, so they are their own preset."""
    return preset if config == "Release" else preset + "-debug"


def archive_path(root=ROOT, system=None):
    """Where CMake writes the translated archive on this platform."""
    name = "recomp_gen.lib" if (system or platform.system()) == "Windows" else "librecomp_gen.a"
    return Path(root) / "build/recomp" / name


def cmake_tool(name):
    """Prefer the venv's pinned cmake/ctest beside this interpreter, then PATH."""
    beside = Path(sys.executable).parent / name
    if beside.exists():
        return str(beside)
    return shutil.which(name) or name


def configure(preset, extra=()):
    subprocess.run([cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable] + list(extra),
                   cwd=ROOT, check=True)


def build(preset, targets, jobs):
    subprocess.run([cmake_tool("cmake"), "--build", "--preset", preset, "--parallel", str(jobs), "--target"] + list(targets),
                   cwd=ROOT, check=True)


def publish_generated(root, translate):
    """Stage a translation, then publish gen/ and symbols.json by rename.

    `translate(stage_dir)` writes the sources and raises on failure; the
    published tree is untouched in that case. Publishing is renames only, so
    a reader under the same lock never sees half a generation."""
    root = Path(root)
    recomp = root / "build/recomp"
    recomp.mkdir(parents=True, exist_ok=True)
    gen, old = recomp / "gen", recomp / "gen.old"
    stage = recomp / ("gen.new.%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    try:
        translate(stage)
        # x86.h sits beside the generated sources so #include "x86.h" resolves.
        shutil.copy(root / "tools/recomp/runtime/x86.h", stage / "x86.h")
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)
    if gen.exists():
        gen.rename(old)
    stage.rename(gen)
    symbols = gen / "symbols.json"
    if symbols.is_file():
        temporary = recomp / ("symbols.json.new.%d" % os.getpid())
        shutil.copy(symbols, temporary)
        temporary.replace(recomp / "symbols.json")
    shutil.rmtree(old, ignore_errors=True)


def run_translator(stage):
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/translate.py"), "--out", str(stage),
                    "--report", str(ROOT / "build/recomp/translate-report.json")], cwd=ROOT, check=True)


def texture_pack():
    """Compile the redistributable material-detail layer when its inputs are newer.
    Original-game replacement textures remain optional, locally prepared pack entries."""
    detail = ROOT / "build/texture-pack/terrain-detail.popt"
    artwork = ROOT / "assets/terrain/materials-v1.png"
    compiler = ROOT / "tools/recomp/terrain_detail.py"
    if (not detail.is_file() or not (detail.parent / "manifest.json").is_file()
            or detail.stat().st_mtime < max(artwork.stat().st_mtime, compiler.stat().st_mtime)):
        subprocess.run([sys.executable, str(compiler), "--source", str(artwork),
                        "--output", str(detail.parent)], cwd=ROOT, check=True)


def parse_args(argv, system=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true", help="Regenerate and compile translated C")
    parser.add_argument("--target", choices=sorted(TARGETS), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--preset", default=default_preset(system), help="CMake configure preset")
    parser.add_argument("--config", choices=("Release", "Debug"), default="Release")
    args = parser.parse_args(argv)
    if args.target in MACOS_ONLY and (system or platform.system()) != "Darwin":
        parser.error("The %s host currently builds on macOS; use --target fixture, gen or plugins elsewhere"
                     % args.target)
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    return args, parser


def main():
    """Check inputs, translate under the build lock when needed, then configure and build."""
    args, parser = parse_args(sys.argv[1:])
    if args.target in NEEDS_GEN and not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    preset = preset_name(args.preset, args.config)
    try:
        with buildlock.BuildLock(ROOT, "tools/build.py"):
            if args.target in NEEDS_GEN and (args.regenerate or not archive_path().is_file()):
                if not (ROOT / "analysis/decompiled/D3DPopTB.exe/functions.tsv").is_file():
                    parser.error("Translation listings are missing; run tools/setup.py without --link-only")
                publish_generated(ROOT, run_translator)
            if args.target == "app":
                texture_pack()
            configure(preset)
            build(preset, TARGETS[args.target], args.jobs)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode or 1, "Build failed; see the compiler output above.\n")
    except TimeoutError as error:
        parser.exit(1, "%s\n" % error)


if __name__ == "__main__":
    main()
```

Run: `.venv/bin/python -m unittest -v tools/recomp/tests/test_translate_publish.py tests/test_build_py.py`
Expected: 9 tests pass.

- [ ] **Step 3: Exercise the entry point without a game**

Run: `.venv/bin/python tools/build.py --target plugins && ls mods/examples/logger/logger.dylib build/recomp/mods/core/display/display.dylib`
Expected: configures, builds every plugin, both files exist. Run: `.venv/bin/python tools/build.py --target app`; expected error `Prepare your own game installation with tools/setup.py first`.

**[game-equipped checkout]**: `.venv/bin/python tools/build.py --regenerate --jobs 8` translates (watch for `gen.new.<pid>` appearing then disappearing), builds `librecomp_gen.a`, the app and its texture pack; `open build/PopRecomp.app` reaches the menu. `.venv/bin/python tools/build.py --target smoke` and `--target headless` build. A second `tools/build.py` without `--regenerate` does not translate.

- [ ] **Step 4: Write `tools/recomp/snapshot_gen.py`**

```python
#!/usr/bin/env python3
"""Copy build/recomp/gen to a directory under the build lock and verify it is one whole generation."""

import argparse
from pathlib import Path
import re
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

PROTOTYPE = re.compile(r"^void fn_[0-9a-f]{8}", re.M)


def snapshot(destination, gen=ROOT / "build/recomp/gen", attempts=5, pause=5.0):
    """Copy gen/ and check that every prototype in funcs.h has a definition in the chunks.
    Returns the function count; raises RuntimeError when no consistent copy could be taken."""
    for attempt in range(1, attempts + 1):
        shutil.rmtree(destination, ignore_errors=True)
        destination.mkdir(parents=True)
        try:
            for path in list(gen.glob("*.c")) + list(gen.glob("*.h")) + [gen / "symbols.json"]:
                if path.is_file():
                    shutil.copy(path, destination / path.name)
        except OSError:
            print("snapshot_gen: build/recomp/gen is being rewritten, retrying (%d)" % attempt, file=sys.stderr)
            time.sleep(pause)
            continue
        required = [destination / "table.c", destination / "funcs.h", destination / "x86.h"]
        if not all(p.is_file() for p in required):
            print("snapshot_gen: incomplete snapshot, retrying (%d)" % attempt, file=sys.stderr)
            time.sleep(pause)
            continue
        prototypes = set(PROTOTYPE.findall((destination / "funcs.h").read_text()))
        definitions = set()
        for chunk in destination.glob("chunk_*.c"):
            definitions.update(PROTOTYPE.findall(chunk.read_text()))
        if prototypes and prototypes == definitions:
            return len(prototypes)
        print("snapshot_gen: torn snapshot (%d prototypes, %d definitions), retrying (%d)"
              % (len(prototypes), len(definitions), attempt), file=sys.stderr)
        time.sleep(pause)
    raise RuntimeError("could not take a consistent snapshot of build/recomp/gen")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        with buildlock.BuildLock(ROOT, "snapshot_gen.py"):
            count = snapshot(args.destination.resolve())
    except (RuntimeError, TimeoutError) as error:
        parser.exit(1, "snapshot_gen: %s\n" % error)
    print("snapshot: %d functions in %s" % (count, args.destination))


if __name__ == "__main__":
    main()
```

Test it against a synthetic tree: `mkdir -p /tmp/g && printf 'void fn_00401000(X86 *c);\n' > /tmp/g/funcs.h && printf 'void fn_00401000(X86 *c) {}\n' > /tmp/g/chunk_000.c && touch /tmp/g/table.c /tmp/g/x86.h && .venv/bin/python -c "import sys; sys.path.insert(0,'tools/recomp'); import snapshot_gen, pathlib; print(snapshot_gen.snapshot(pathlib.Path('/tmp/gsnap'), pathlib.Path('/tmp/g'), attempts=1))"`
Expected: prints `1`. Then remove `/tmp/g/chunk_000.c` and rerun; expected `RuntimeError` after one torn-snapshot message.

- [ ] **Step 5: Commit**

```sh
.venv/bin/python tools/check_repo.py
git add tools/build.py tools/recomp/snapshot_gen.py tools/recomp/tests/test_translate_publish.py tests/test_build_py.py
git commit -m "Drive CMake from tools/build.py with atomic translation publishing"
```

---

### Task 11: `tools/test.py` over CTest, and repointing the run-only scripts

**Files:**
- Modify: `tools/test.py`, `Makefile`, `tools/recomp/tests/test_translate.py`, `tools/recomp/mods_test.sh`, `tools/recomp/mode_probe.sh`, `src/recomp/host/tests/integration_tests.sh`

- [ ] **Step 1: Rewrite `tools/test.py`**

```python
#!/usr/bin/env python3
"""Run explicit contributor suites, with game-backed checks isolated from player saves."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

spec = importlib.util.spec_from_file_location("build_py", ROOT / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)

PORTABLE_TESTS = [
    "tests/test_setup.py", "tests/test_build_py.py",
    "tools/recomp/tests/test_mode_probe.py", "tools/recomp/tests/test_texture_pack.py",
    "tools/recomp/tests/test_terrain_detail.py", "tools/recomp/tests/test_buildlock.py",
    "tools/recomp/tests/test_translate_publish.py",
]


def run(args, env=None):
    """Propagate a test command's failure instead of treating missing coverage as success."""
    subprocess.run([str(arg) for arg in args], cwd=ROOT, env=env, check=True)


def ctest(preset, labels, env):
    """Run the CTest entries whose label matches `labels` (a regex)."""
    run([build_py.cmake_tool("ctest"), "--preset", preset, "-L", labels, "--output-on-failure"], env)


def native(preset, env, jobs, run_tests):
    """Build every test binary this platform has; run the suites that need no snapshot."""
    build_py.configure(preset)
    build_py.build(preset, ["check_binaries"], jobs)
    if run_tests:
        ctest(preset, "nogame|game|gpu", env)


def mods(preset, env, jobs):
    """Generate the real entity fixture locally, then run the mod suites under one build lock."""
    with buildlock.BuildLock(ROOT, "mod tests"):
        build_py.configure(preset)
        build_py.build(preset, ["pop_fixture", "mods_tests", "present_events_tests"], jobs)
        output = ROOT / "build/tests"
        output.mkdir(parents=True, exist_ok=True)
        case = Path(tempfile.mkdtemp(prefix="mod-fixture-", dir=output))
        fixture_env = dict(env, POPM_NO_MODS="1", POP_RECOMP_FIXTURE="frames:32",
                           POP_RECOMP_OUT=str(case / "snapshots"),
                           POPM_PROFILE_DIR=str(case / "profile"),
                           POPM_REGISTRY=str(case / "registry.json"),
                           POPM_RUN_RECORD=str(case / "run.json"))
        print("Mod fixture diagnostics: %s" % case, flush=True)
        with (case / "fixture.log").open("w") as log:
            result = subprocess.run([str(ROOT / "build/recomp/pop_fixture")], cwd=ROOT, env=fixture_env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
        result.check_returncode()
        snapshot = case / "snapshots/frame32._data_00598000.bin"
        if not snapshot.is_file():
            raise RuntimeError("The entity fixture was not captured; inspect %s" % case)
        ctest(preset, "mods", dict(env, POPM_TEST_GAME_VIEW_SNAPSHOT=str(snapshot)))


def gameplay(jobs):
    """Replay native Options and movement in a unique profile; retain diagnostics under build/."""
    run([sys.executable, "tools/build.py", "--target", "smoke", "--jobs", jobs])
    spec = importlib.util.spec_from_file_location("mode_probe", ROOT / "tools/recomp/mode_probe.py")
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    output = ROOT / "build/gameplay"
    output.mkdir(parents=True, exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="options-", dir=output))
    script = (ROOT / "tools/recomp/smoke/native-options.script").read_text()
    pack = ROOT / "build/texture-pack"
    manifest = json.loads((pack / "manifest.json").read_text()) if (pack / "manifest.json").is_file() else {}
    # Material detail ships from project artwork. Original-game replacement
    # textures are optional, so only assert HD replacements when some exist.
    absent = []
    if not manifest.get("textures"):
        absent.append("expect hd_draws")
    if not manifest.get("terrain_detail"):
        absent.append("expect terrain_detail_draws")
    script = "\n".join(line for line in script.splitlines()
                       if not line.startswith(tuple(absent))) + "\n"
    path = case / "input.script"
    path.write_text(script)
    env = probe.probe_environment((640, 480, 16), path, case)
    env.pop("POP_SMOKE_CLASSIC_PROBE", None)
    env.update(POP_SMOKE_DRAWABLE="1280x960",
               POPM_DDRAW_MODES="640x480x8,640x480x16,800x600x16,3840x2160x16",
               POPM_CORE_MODS_DIR=str(ROOT / "build/recomp/mods/core"),
               POPM_TEXTURE_PACK_DIR=str(pack))
    print("Gameplay diagnostics: %s" % case, flush=True)
    with (case / "smoke.log").open("w") as log:
        result = subprocess.run([str(ROOT / "build/recomp/pop_smoke")], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, timeout=300)
    result.check_returncode()
    text = (case / "smoke.log").read_text()
    for mode in ("800x600", "3840x2160", "640x480"):
        if "display mode %s 16bpp" % mode not in text:
            raise RuntimeError("The gameplay run did not reach %s; inspect %s" % (mode, case))
    if "all expectations met" not in text:
        raise RuntimeError("Gameplay assertions did not complete; inspect %s" % case)


def main():
    """Choose portable, compile-only or game-backed suites and check their prerequisites."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--native", action="store_true", help="Build and run the native suites")
    group.add_argument("--mods", action="store_true", help="Real game-backed mod tests")
    group.add_argument("--gameplay", action="store_true", help="Scripted native Options and gameplay run")
    group.add_argument("--compile-only", action="store_true", help="Build the native test binaries only")
    parser.add_argument("--preset", default=build_py.default_preset())
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    game_backed = args.mods or args.gameplay
    if game_backed and platform.system() != "Darwin":
        parser.error("Game-backed suites require macOS")
    if (game_backed or args.native) and not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("This suite needs your game installation; run tools/setup.py first, "
                     "or run `ctest --preset <preset> -L nogame` for the portable suites")
    if game_backed and not build_py.archive_path().is_file():
        parser.error("Build the game with tools/build.py before running this suite")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("POPM_", "POP_RECOMP_", "POP_SMOKE_", "POP_HOST_"))}
    env["PY"] = sys.executable
    try:
        if args.gameplay:
            gameplay(args.jobs)
        elif args.mods:
            mods(args.preset, env, args.jobs)
        elif args.native or args.compile_only:
            native(args.preset, env, args.jobs, run_tests=args.native)
        else:
            run([sys.executable, "-m", "pytest", "-q"] + PORTABLE_TESTS, env)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError, TimeoutError) as error:
        parser.exit(1, "Tests failed: %s\n" % error)


if __name__ == "__main__":
    main()
```

Run: `.venv/bin/python tools/test.py` (portable pytest, 7 files) and `.venv/bin/python tools/test.py --compile-only`. Expected: both pass. `.venv/bin/python tools/test.py --native` here must refuse with the game-installation message that also names the `ctest -L nogame` alternative; then `.venv/bin/ctest --preset macos -L "nogame|gpu" --output-on-failure` passes. **[game-equipped checkout]**: `--native`, `--mods` and `--gameplay` all pass.

- [ ] **Step 2: Repoint `test_translate.py`**

Replace the constant `BUILDLOCK = os.path.join(ROOT, "tools/recomp/buildlock.sh")` with `BUILDLOCK = os.path.join(ROOT, "tools/recomp/buildlock.py")`, and in `reexec_under_lock` replace the `os.execv(BUILDLOCK, [BUILDLOCK, "run", ...])` call with:

```python
    os.execv(sys.executable, [sys.executable, BUILDLOCK, "run", ROOT,
                              os.path.relpath(script, ROOT),
                              sys.executable, script] + sys.argv[1:])
```

In `build()`, replace `subprocess.check_call([os.path.join(ROOT, "tools/recomp/build.sh")])` with `subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/build.py"), "--target", "gen"])`. Update the module docstring's "via tools/recomp/build.sh" and the message at line 234 to say `tools/build.py --target gen`. The dylib link that follows keeps `-Wl,-force_load,LIB_A` unchanged.

- [ ] **Step 3: Repoint the shell scripts**

`tools/recomp/mods_test.sh`: replace the three lines `src/recomp/mods/lua/build_lua.sh`, `mods/examples/build_examples.sh`, `tools/recomp/smoke_build.sh` with:

```sh
PY=${PY:-$ROOT/.venv/bin/python}
"$PY" tools/build.py --target plugins
"$PY" tools/build.py --target smoke
```

`tools/recomp/mode_probe.sh`: replace `"$ROOT/tools/recomp/smoke_build.sh"` with `"${PY:-$ROOT/.venv/bin/python}" "$ROOT/tools/build.py" --target smoke`.

`src/recomp/host/tests/integration_tests.sh`: after the `buildlock_acquire` line add `PY=${PY:-$ROOT/.venv/bin/python}`; replace the `xcrun clang ... mods/smoke/probe.c -o mods/smoke/probe.dylib` command (two lines) with `"$PY" tools/build.py --target plugins >/dev/null`; replace `tools/recomp/headless_build.sh >/dev/null` with `"$PY" tools/build.py --target headless >/dev/null`, `tools/recomp/smoke_build.sh >/dev/null` with `"$PY" tools/build.py --target smoke >/dev/null`, and `tools/recomp/parity_build.sh >/dev/null` with `"$PY" tools/build.py --target fixture >/dev/null`. `make recomp` stays.

`Makefile`: no verb changes; update the `help` lines to `'Build:   make all (tools/build.py)'` and add `@echo 'Presets: tools/build.py --preset macos|linux|windows --config Release|Debug'`.

- [ ] **Step 4: Verify**

`grep -rn 'build\.sh\|_build\.sh\|build_tests\.sh\|build_core\.sh\|build_lua\.sh\|build_examples\.sh\|build_fixtures\.sh' tools src mods --include='*.sh' --include='*.py' | grep -v 'tools/recomp/tests/test_buildlock.sh'` prints nothing. `sh -n` each edited shell script. `.venv/bin/python tools/test.py` passes. **[game-equipped checkout]**: `sh src/recomp/host/tests/integration_tests.sh` ends with `the mod runtime ships in every host`; `sh tools/recomp/mods_test.sh` passes; `.venv/bin/python tools/recomp/tests/test_translate.py -n 50` passes.

- [ ] **Step 5: Commit**

```sh
git add tools/test.py Makefile tools/recomp/tests/test_translate.py tools/recomp/mods_test.sh tools/recomp/mode_probe.sh src/recomp/host/tests/integration_tests.sh
git commit -m "Run the native suites through CTest and repoint run-only scripts at tools/build.py"
```

---

### Task 12: Linux: the null-host link proof and a Docker verification

**Files:**
- Create: `src/recomp/dx/tests/null_host_link.cpp`
- Modify: `src/recomp/dx/CMakeLists.txt`, `src/recomp/platform/os_posix.cpp` (only if Linux needs it)

- [ ] **Step 1: Write `null_host_link.cpp`**

```cpp
// null_host_link.cpp - proves the weak host defaults resolve on this platform.
//
// Links the DirectX shims and the shared host code with no strong host
// callback anywhere, then calls a few of the defaults. On ELF and Mach-O this
// is what every partial host already relies on; on COFF it is the thing the
// spec lists as the port's main risk, so it gets a binary of its own.
#include "../host_api.h"
#include "../../host/boot.h"

#include <stdio.h>

int main() {
    host_set_display_mode(640, 480, 8);
    host_present(nullptr, 0, 0, 8, nullptr, 0);
    host_d3d_begin_scene();
    host_d3d_end_scene();
    boot_report_lock();
    boot_report_unlock();
    puts("null host linked: weak defaults resolved");
    return 0;
}
```

- [ ] **Step 2: Register it**

Append to `src/recomp/dx/CMakeLists.txt`:

```cmake
add_executable(null_host_link tests/null_host_link.cpp)
target_link_libraries(null_host_link PRIVATE recomp_platform recomp_runtime recomp_dx host_common)
target_compile_options(null_host_link PRIVATE ${POP_WARN_HOST})
pop_optimize(null_host_link 1)
pop_test_binary(null_host_link)
add_test(NAME null_host_link COMMAND null_host_link WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(null_host_link PROPERTIES LABELS nogame)
```

`host_common` is defined in `src/recomp/host/CMakeLists.txt`, which is added after `dx`; CMake resolves target names at generate time, so the order is fine.

Run: `.venv/bin/cmake --build --preset macos --target null_host_link && .venv/bin/ctest --preset macos -R null_host_link`
Expected: pass.

- [ ] **Step 3: Verify the portable layers on Linux in Docker**

Run from the checkout root:

```sh
docker run --rm -v "$PWD":/src -w /src ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq clang lld cmake ninja-build python3 >/dev/null &&
  cmake --preset linux -DPOP_TRANSLATE=OFF -DPython3_EXECUTABLE=/usr/bin/python3 &&
  cmake --build --preset linux --target check_binaries &&
  ctest --preset linux -L nogame --output-on-failure'
```

Expected: configure, build and the `nogame` label pass (`platform_tests`, `dx_tests`, `host_api_header_check`, `null_host_link`, `test_build_core`, `roots_tests`). Fix each failure in the smallest portable way:

- `MAP_ANON` undefined: in `os_posix.cpp` add `#ifndef MAP_ANON` / `#define MAP_ANON MAP_ANONYMOUS` / `#endif`.
- A missing `-ldl`/`-lpthread`: already on `recomp_platform`; if another target needs them, link `recomp_platform` there.
- `ends_with` or other C++20 library gaps: Ubuntu 24.04's clang 18 with libstdc++ 14 has them; do not downgrade the standard.
- Anything in runtime/dx/mods that is genuinely macOS-only: stop, record it, and fix it through `os.h`, not with `#ifdef`.

The container leaves `build/cmake/linux` owned by root; `sudo rm -rf build/cmake/linux` afterwards or leave it (it is ignored).

- [ ] **Step 4: Commit**

```sh
git add src/recomp/dx/tests/null_host_link.cpp src/recomp/dx/CMakeLists.txt src/recomp/platform/os_posix.cpp
git commit -m "Prove weak host defaults link and verify the portable layers on Linux"
```

---

### Task 13: Windows: the Win32 platform layer and the CI matrix

**Files:**
- Create: `src/recomp/platform/os_win32.cpp`
- Modify: `.github/workflows/checks.yml`, `src/recomp/dx/tests/dx_tests.cpp` (only its `<unistd.h>` include)

- [ ] **Step 1: Write `src/recomp/platform/os_win32.cpp`**

```cpp
// os_win32.cpp - the platform layer on Windows. Compiled by clang's GNU
// driver against the MSVC ABI and the Windows SDK; the only first-party file
// besides os_posix.cpp that includes a platform header. Paths are UTF-8 at
// the API and UTF-16 at the system calls.
#include "os.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

namespace {

std::wstring widen(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

std::string narrow(const wchar_t *wide) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// FILETIME counts 100 ns ticks since 1601; the API speaks Unix seconds.
int64_t filetime_seconds(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000000ull) - 11644473600ll;
}

int copy_out(const std::string &s, char *buf, size_t cap) {
    if (s.size() + 1 > cap)
        return -1;
    memcpy(buf, s.c_str(), s.size() + 1);
    for (char *c = buf; *c; ++c)
        if (*c == '\\')
            *c = '/';
    return 0;
}

struct ThreadStart {
    void *(*fn)(void *);
    void *arg;
};

unsigned __stdcall trampoline(void *p) {
    ThreadStart start = *(ThreadStart *)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

char g_dlerror[256];
void remember_dlerror() {
    snprintf(g_dlerror, sizeof g_dlerror, "Windows error %lu", (unsigned long)GetLastError());
}

int native_flags(int flags) {
    int f = _O_BINARY;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f |= _O_WRONLY;
        break;
    case OS_O_RDWR:
        f |= _O_RDWR;
        break;
    default:
        f |= _O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= _O_CREAT;
    if (flags & OS_O_EXCL)
        f |= _O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= _O_TRUNC;
    return f;
}

int stat_attributes(const wchar_t *path, OsStat *out, bool follow) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d))
        return -1;
    bool reparse = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (follow && reparse) {
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return -1;
        BY_HANDLE_FILE_INFORMATION info;
        BOOL ok = GetFileInformationByHandle(h, &info);
        CloseHandle(h);
        if (!ok)
            return -1;
        d.dwFileAttributes = info.dwFileAttributes;
        d.ftCreationTime = info.ftCreationTime;
        d.ftLastAccessTime = info.ftLastAccessTime;
        d.ftLastWriteTime = info.ftLastWriteTime;
        d.nFileSizeHigh = info.nFileSizeHigh;
        d.nFileSizeLow = info.nFileSizeLow;
        reparse = false;
    }
    out->size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    out->atime = filetime_seconds(d.ftLastAccessTime);
    out->mtime = filetime_seconds(d.ftLastWriteTime);
    out->ctime = filetime_seconds(d.ftCreationTime);
    out->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out->is_symlink = reparse ? 1 : 0;
    out->is_regular = (!out->is_dir && !reparse) ? 1 : 0;
    out->is_readonly = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;
    return 0;
}

} // namespace

struct OsThread {
    HANDLE handle;
    DWORD id;
};

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof *start);
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!start || !t) {
        free(start);
        free(t);
        return nullptr;
    }
    start->fn = fn;
    start->arg = arg;
    unsigned id = 0;
    uintptr_t h = _beginthreadex(nullptr, (unsigned)stack_bytes, trampoline, start, 0, &id);
    if (!h) {
        free(start);
        free(t);
        return nullptr;
    }
    t->handle = (HANDLE)h;
    t->id = id;
    return t;
}
void os_thread_join(OsThread *t) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}
void os_thread_detach(OsThread *t) {
    CloseHandle(t->handle);
    free(t);
}
void os_thread_exit(void) {
    _endthreadex(0);
}
OsThreadId os_thread_self(void) {
    return GetCurrentThreadId();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return t->id;
}

void *os_vm_reserve(size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void os_vm_release(void *p, size_t bytes) {
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
}

void *os_dlopen(const char *path) {
    HMODULE m = LoadLibraryW(widen(path).c_str());
    if (!m)
        remember_dlerror();
    return m;
}
void *os_dlopen_noload(const char *path) {
    return GetModuleHandleW(widen(path).c_str());
}
void *os_dlsym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
}
int os_dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
const char *os_dlerror(void) {
    return g_dlerror[0] ? g_dlerror : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
    return ".dll";
}

int os_stat(const char *path, OsStat *out) {
    return stat_attributes(widen(path).c_str(), out, true);
}
int os_lstat(const char *path, OsStat *out) {
    return stat_attributes(widen(path).c_str(), out, false);
}
int os_mkdir(const char *path) {
    return CreateDirectoryW(widen(path).c_str(), nullptr) ? 0 : -1;
}
int os_rename(const char *from, const char *to) {
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
int os_unlink(const char *path) {
    return DeleteFileW(widen(path).c_str()) ? 0 : -1;
}
int os_getcwd(char *buf, size_t cap) {
    wchar_t w[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW(sizeof w / sizeof w[0], w);
    if (!n || n >= sizeof w / sizeof w[0])
        return -1;
    return copy_out(narrow(w), buf, cap);
}
int os_chdir(const char *path) {
    return SetCurrentDirectoryW(widen(path).c_str()) ? 0 : -1;
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    std::wstring pattern = widen(dir) + L"\\*";
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern.c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (wcscmp(f.cFileName, L".") == 0 || wcscmp(f.cFileName, L"..") == 0)
            continue;
        if (fn(narrow(f.cFileName).c_str(), user) != 0)
            break;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return 0;
}
int os_mkstemp(char *template_path) {
    size_t len = strlen(template_path);
    if (len < 6 || strcmp(template_path + len - 6, "XXXXXX") != 0)
        return -1;
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)GetTickCount() ^ (GetCurrentThreadId() * 2654435761u);
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (int i = 0; i < 6; ++i) {
            seed = seed * 1103515245u + 12345u;
            template_path[len - 6 + i] = alphabet[(seed >> 16) % 36];
        }
        int fd = _wopen(widen(template_path).c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                        _S_IREAD | _S_IWRITE);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

int os_fd_open(const char *path, int flags) {
    return _wopen(widen(path).c_str(), native_flags(flags), _S_IREAD | _S_IWRITE);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    // _read takes an unsigned count; loop so a large guest read still works.
    int64_t total = 0;
    char *p = (char *)buf;
    while (n) {
        unsigned chunk = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        int got = _read(fd, p, chunk);
        if (got < 0)
            return total ? total : -1;
        total += got;
        p += got;
        n -= (size_t)got;
        if ((unsigned)got < chunk)
            break;
    }
    return total;
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    int64_t total = 0;
    const char *p = (const char *)buf;
    while (n) {
        unsigned chunk = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        int put = _write(fd, p, chunk);
        if (put < 0)
            return total ? total : -1;
        total += put;
        p += put;
        n -= (size_t)put;
        if ((unsigned)put < chunk)
            break;
    }
    return total;
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return _lseeki64(fd, off, whence);
}
int os_fd_close(int fd) {
    return _close(fd);
}
int os_fd_dup(int fd) {
    return _dup(fd);
}
int os_fd_fsync(int fd) {
    return _commit(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return _chsize_s(fd, length) == 0 ? 0 : -1;
}
int os_fd_stat(int fd, OsStat *out) {
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0)
        return -1;
    out->size = (uint64_t)st.st_size;
    out->atime = st.st_atime;
    out->mtime = st.st_mtime;
    out->ctime = st.st_ctime;
    out->is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
    out->is_regular = (st.st_mode & _S_IFREG) ? 1 : 0;
    out->is_symlink = 0;
    out->is_readonly = (st.st_mode & _S_IWRITE) ? 0 : 1;
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return _fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
    wchar_t w[32768];
    DWORD n = GetModuleFileNameW(nullptr, w, 32768);
    if (!n || n >= 32768)
        return -1;
    return copy_out(narrow(w), buf, cap);
}
int os_install_fault_handlers(OsFaultFn fn) {
    // A vectored exception handler is sub-project 3 work; say so honestly.
    (void)fn;
    return 0;
}

uint64_t os_monotonic_ns(void) {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
uint64_t os_wall_time_us(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10ull - 11644473600000000ull;
}
void os_sleep_us(uint64_t us) {
    Sleep((DWORD)((us + 999) / 1000));
}

int os_strcasecmp(const char *a, const char *b) {
    return _stricmp(a, b);
}

} // extern "C"
```

Syntax-check it here even though it cannot link: `clang++ -std=c++20 -fsyntax-only --target=x86_64-pc-windows-msvc src/recomp/platform/os_win32.cpp` will fail for missing SDK headers on a Mac; that is expected. Instead run `.venv/bin/python tools/format.py` to check formatting only.

- [ ] **Step 2: Remove the DX tests' unused POSIX include**

In `src/recomp/dx/tests/dx_tests.cpp` delete `#include <unistd.h>` (line 31; Task 6's grep found no `unistd` symbol used in the file). Rebuild `dx_tests` on macOS to confirm.

- [ ] **Step 3: Rewrite `.github/workflows/checks.yml`**

```yaml
name: Contributor checks

on:
  push:
    branches: [main]
  pull_request:
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: checks-${{ github.ref }}
  cancel-in-progress: true

jobs:
  tooling:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v7.0.1
      - name: Install pinned contributor tools
        run: |
          python3 -m venv .venv
          .venv/bin/python -m pip install -r requirements-dev.txt
      - name: Test source tooling without game files
        run: .venv/bin/python tools/test.py
      - name: Check native code formatting
        run: .venv/bin/python tools/format.py
      - name: Check repository boundaries and documentation links
        run: .venv/bin/python tools/check_repo.py

  native-compile:
    # The game and locally generated code are not available in public CI.
    # Each platform configures with POP_TRANSLATE=OFF, builds every test
    # binary it has, and runs the suites that need neither game files nor a
    # GPU. No original assets, bundles, profiles or diagnostics are uploaded.
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: macos-15
            preset: macos
            labels: "nogame|gpu"
          - os: ubuntu-24.04
            preset: linux
            labels: nogame
          - os: windows-2025
            preset: windows
            labels: nogame
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v7.0.1
      - name: Install clang and lld (Linux)
        if: runner.os == 'Linux'
        run: sudo apt-get update -qq && sudo apt-get install -y -qq clang lld
      - name: Enter the Visual Studio developer environment (Windows)
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1
      - name: Install CMake and Ninja
        shell: bash
        run: |
          python3 -m venv .ci
          if [ -d .ci/Scripts ]; then BIN=.ci/Scripts; else BIN=.ci/bin; fi
          "$BIN/python" -m pip install cmake==4.1.2 ninja==1.13.0
          if [ "$RUNNER_OS" = Windows ]; then
            cygpath -w "$PWD/$BIN" >> "$GITHUB_PATH"
          else
            echo "$PWD/$BIN" >> "$GITHUB_PATH"
          fi
      - name: Configure
        run: cmake --preset ${{ matrix.preset }} -DPOP_TRANSLATE=OFF
      - name: Build every test binary this platform has
        run: cmake --build --preset ${{ matrix.preset }} --target check_binaries
      - name: Run the suites that need no game files
        run: ctest --preset ${{ matrix.preset }} -L "${{ matrix.labels }}" --output-on-failure
```

Use the same `cmake`/`ninja` pins as `requirements-dev.txt`. On the Windows runner `clang` is the preinstalled LLVM on `PATH`; if configure reports it missing, add a step `choco install llvm --no-progress -y` before Configure.

- [ ] **Step 4: Push and watch CI**

```sh
git add src/recomp/platform/os_win32.cpp src/recomp/dx/tests/dx_tests.cpp .github/workflows/checks.yml
git commit -m "Add the Win32 platform layer and a three-platform CI matrix"
git push -u <remote-you-can-push-to> portable-build-system
gh run watch --exit-status
```

If you cannot push to `origin`, push to your fork and run `gh run watch` there, or open a draft pull request so the workflow runs on `pull_request`.

Expected: all three matrix legs green. Iterate on the Windows leg with these rules: fixes go in `os_win32.cpp`, in `CMakeLists.txt` files, or are genuinely portable source changes (a missing `<cstdint>`, a `long` that should be `int64_t`, `ssize_t` that should be `int64_t`). If a runtime, dx or mods file needs a Windows-only code path that `os.h` cannot absorb, stop and report it as a spec change rather than adding an `#ifdef`. Known likely items: `<strings.h>` leftovers, `S_IS*` leftovers the Task 6 grep did not cover in `.h` files, `%zd`/`%ld` format strings, `setenv`/`unsetenv` in test files that are not built on Windows anyway, and `__attribute__((weak))` resolution which `null_host_link` exists to prove. If weak resolution fails on COFF, stop: that is the spec's named fallback (per-group null libraries) and needs a decision.

Also confirm in the Windows log that `ctest` ran `platform_tests`, `dx_tests`, `null_host_link`, `host_api_header_check` and `test_build_core`.

---

### Task 14: Remove the shell build scripts and update the documentation

**Files:**
- Delete: `tools/recomp/build.sh`, `tools/recomp/app_build.sh`, `tools/recomp/headless_build.sh`, `tools/recomp/smoke_build.sh`, `tools/recomp/parity_build.sh`, `src/recomp/mods/lua/build_lua.sh`, `mods/core/build_core.sh`, `mods/examples/build_examples.sh`, `src/recomp/mods/tests/fixtures/build_fixtures.sh`, `src/recomp/runtime/build_tests.sh`, `src/recomp/runtime/build_profile_tests.sh`, `src/recomp/dx/build_tests.sh`, `src/recomp/host/build_tests.sh`, `src/recomp/host/tests/build_ui_layer_tests.sh`, `src/recomp/host/tests/build_compositor_tests.sh`, `src/recomp/mods/build_tests.sh`, `src/recomp/mods/tests/build_present_events_tests.sh`, `mods/core/tests/packaging_tests.sh`, `mods/core/tests/reproducibility_tests.sh`
- Modify: `README.md`, `CONTRIBUTING.md`, `AGENTS.md`, `docs/testing.md`, `docs/architecture.md`, `tools/recomp/README.md`, `src/recomp/host/README.md`, `src/recomp/runtime/README.md`, `src/recomp/dx/README.md`, `mods/core/README.md`, `CHANGELOG.md`

- [ ] **Step 1: Delete the scripts**

```sh
git rm tools/recomp/build.sh tools/recomp/app_build.sh tools/recomp/headless_build.sh tools/recomp/smoke_build.sh tools/recomp/parity_build.sh src/recomp/mods/lua/build_lua.sh mods/core/build_core.sh mods/examples/build_examples.sh src/recomp/mods/tests/fixtures/build_fixtures.sh src/recomp/runtime/build_tests.sh src/recomp/runtime/build_profile_tests.sh src/recomp/dx/build_tests.sh src/recomp/host/build_tests.sh src/recomp/host/tests/build_ui_layer_tests.sh src/recomp/host/tests/build_compositor_tests.sh src/recomp/mods/build_tests.sh src/recomp/mods/tests/build_present_events_tests.sh mods/core/tests/packaging_tests.sh mods/core/tests/reproducibility_tests.sh
grep -rn 'build\.sh\|_build\.sh\|build_tests\.sh\|build_core\.sh\|build_lua\.sh\|build_examples\.sh\|build_fixtures\.sh\|packaging_tests\.sh\|reproducibility_tests\.sh' --include='*.md' --include='*.py' --include='*.sh' --include='*.cpp' --include='*.mm' --include='*.h' --include='*.c' --include='*.txt' --include='*.cmake' --include='*.yml' . | grep -v '^./build/' | grep -v 'docs/superpowers/'
```
Every remaining hit is a documentation line to fix in the next step (source comments that name a script get the same treatment: name the CMake target or `tools/build.py` instead).

- [ ] **Step 2: Documentation, file by file**

`README.md`: in "Find your way around" add a row `| `CMakeLists.txt`, `cmake/` | The build: targets per directory, presets for macOS, Linux and Windows |`. In "Play on macOS" replace the sentence "The Python tooling tests also run on Linux." with "The runtime, adapter and mod layers compile and test on Linux and Windows; a playable host for them is future work." In "Status" replace "additional operating systems still need validation" with "Linux and Windows hosts do not exist yet; their portable layers are compiled and tested in CI".

`CONTRIBUTING.md`: under Prerequisites replace "Native builds: Apple Silicon macOS, Xcode Command Line Tools and Git." with:

```markdown
- Native builds on macOS: Apple Silicon, Xcode Command Line Tools and Git. CMake
  and Ninja come from `requirements-dev.txt`.
- Portable-layer builds on Linux: clang and lld (`apt-get install clang lld`).
- Portable-layer builds on Windows: LLVM's clang, a Visual Studio developer
  command prompt for the Windows SDK, and `tools/build.py --target fixture`
  or `tools/test.py --compile-only`. Linux and Windows build and test the
  runtime, adapters and mod foundation only; no game host exists for them yet.
```
Under "Build and run" replace the paragraph after the regenerate command with:

```markdown
`--target smoke` builds the offscreen scripted host, `--target headless` the
minimal boot host, `--target fixture` the parity fixture and `--target plugins`
every mod plugin. `--preset` and `--config Debug` pick the CMake preset; the
CMake tree lives in `build/cmake/<preset>` and every artifact keeps its documented
path under `build/`. Build outputs and your default writable profile stay in
`build/`. `POPM_PROFILE_DIR` selects a separate profile for an interactive run.
Keep the app in the checkout; moving it requires explicitly configuring its game path.
```
Under "Check your change" the commands are unchanged; append `.venv/bin/python tools/test.py --native  # also compiles and runs the portable suites on Linux and Windows` as a comment update to the existing `--native` line.

`AGENTS.md`: replace "Run relevant suites from docs/testing.md." with "Run relevant suites from docs/testing.md; native code builds only through tools/build.py and tools/test.py, never by invoking compilers directly." Add a bullet: "Platform calls go through src/recomp/platform/os.h. No `#ifdef` on the platform outside os_posix.cpp and os_win32.cpp."

`docs/testing.md`: in the table change the `--compile-only` row to "Every native test binary this platform has compiles (macOS, Linux, Windows)" with "No"; change `--native` to "Portable suites everywhere; runtime, adapters, offscreen Metal and UI tests on macOS" with "Partly: `game`-labeled suites need the image". After the table add:

```markdown
Native suites are CTest entries with labels: `nogame` runs everywhere and in CI,
`game` needs your installation, `gpu` needs a Metal device, `mods` needs the
translated archive and the entity snapshot `tools/test.py --mods` captures. Run
one directly with `.venv/bin/ctest --preset macos -L nogame` or `-R dx_tests`.
```
Replace the "Current local evidence" paragraph's "local native suites from the standalone checkout" with "local native suites built through CMake from the standalone checkout, and the Linux and Windows portable-layer suites in CI".

`docs/architecture.md`: in "Current boundaries" append: "Platform services (threads, virtual memory, plugins, files, clocks) go through `src/recomp/platform/os.h`, with POSIX and Win32 implementations; the build is CMake with presets per platform (`CMakePresets.json`)."

`tools/recomp/README.md`: replace the `build.sh, buildlock.sh` row with `| `buildlock.sh`, `buildlock.py` | The shared process lock over build/recomp, as a shell entry and a Python module |`; replace the `app_build.sh, ...` row with `| `build_core.py`, `finish_bundle.py`, `snapshot_gen.py` | Install core mods reproducibly, finish the app bundle, copy one consistent generation of gen/ |`; in the `oracle.py, parity_build.sh` row drop `parity_build.sh` and say "`pop_fixture` is a CMake target (`tools/build.py --target fixture`)". Replace the paragraph starting "`build.sh` publishes its generated directory and archive together" with "`tools/build.py` publishes the generated directory and `symbols.json` by rename under the lock, then CMake rebuilds `librecomp_gen.a` from it; a reader under the same lock never sees half a generation."

`src/recomp/host/README.md`: the "built by" column becomes `tools/build.py --target headless`, `tools/build.py` (or `make all`), `tools/build.py --target smoke`; the `build_tests.sh` row becomes `| CMake targets `host_tests`, `compositor_tests`, `ui_layer_tests` | built by `tools/test.py --compile-only`, run by `--native` |`; the Tests section's command block becomes:

```
.venv/bin/python tools/test.py --native          # build and run
.venv/bin/python tools/test.py --compile-only    # build only
.venv/bin/ctest --preset macos -R host_tests     # one suite
```

`src/recomp/runtime/README.md` and `src/recomp/dx/README.md`: the same three-line block replaces `build_tests.sh` usage, with `-R runtime_tests` and `-R dx_tests` respectively.

`mods/core/README.md`: "Every host build invokes `build_core.sh`" becomes "Every host build runs `tools/recomp/build_core.py` through the `core_mods` CMake target"; the "Run `mods/core/build_core.sh`" paragraph becomes "Run `.venv/bin/python tools/build.py --target plugins` to install for headless hosts; the app target installs into its own bundle."; the reproducibility paragraph names `tools/recomp/tests/test_build_core.py` (a `nogame` CTest entry) instead of the two shell suites; "`--reuse-core`" is gone, so drop that sentence.

`CHANGELOG.md` under Unreleased:

```markdown
- Build with CMake presets for macOS, Linux and Windows through the unchanged
  `tools/build.py` and `tools/test.py`; the xcrun shell scripts are gone.
- Add a platform layer (`src/recomp/platform/os.h`) so the runtime, adapters
  and mod foundation compile and pass their portable tests on Linux and Windows.
- Mod plugins resolve their file extension per platform; a manifest written on
  macOS loads its `.so` or `.dll` counterpart unchanged.
- CI compiles and tests the portable layers on macOS, Ubuntu and Windows.
```

- [ ] **Step 3: Verify everything that can be verified here**

```sh
.venv/bin/python tools/check_repo.py
.venv/bin/python tools/format.py
.venv/bin/python tools/test.py
.venv/bin/python tools/test.py --compile-only
.venv/bin/ctest --preset macos -L "nogame|gpu" --output-on-failure
sh tools/recomp/tests/test_buildlock.sh 20 4
git status --short   # nothing under build/, original/, analysis/
```
Expected: all pass. Then push and `gh run watch --exit-status`: all four jobs green.

**[game-equipped checkout]** final acceptance, run by the user or on a game-equipped clone of this branch:

```sh
.venv/bin/python tools/build.py --regenerate --jobs 8
open build/PopRecomp.app                 # reaches the menu; Command-Q exits cleanly
.venv/bin/python tools/test.py --native
.venv/bin/python tools/test.py --mods
.venv/bin/python tools/test.py --gameplay
sh src/recomp/host/tests/integration_tests.sh
sh tools/recomp/mods_test.sh
.venv/bin/python tools/recomp/tests/test_translate.py -n 50
```
Every command exits 0. Report exactly which of these ran and where.

- [ ] **Step 4: Commit**

```sh
git add -A README.md CONTRIBUTING.md AGENTS.md docs tools src mods CHANGELOG.md
git commit -m "Remove the shell build scripts and document the CMake build"
```

Then invoke `superpowers:finishing-a-development-branch`.

---

## Self-review notes

- Spec coverage: Targets table → Tasks 1 to 5, 12; output locations → Tasks 1 to 4; generated code and translation step → Tasks 3 and 10; build lock → Task 9; platform layer table → Tasks 5, 6, 7, 8, 13; plugins and `build_core.py` → Tasks 2, 7; Python entry points and the translator test → Tasks 10, 11; CTest labels → every `add_test`; CI → Task 13; documentation and removed files → Task 14; risks: weak defaults → Task 12/13 `null_host_link`, `long` → Task 8, lock → Task 9, paths → Tasks 1 to 4, Windows/Linux visibility → Tasks 12 and 13.
- Names used across tasks: `pop_optimize`, `pop_test_binary`, `pop_add_plugin`, `pop_link_gen`, `pop_mac_bundle`, `pop_host`, `check_binaries`, `recomp_platform`, `recomp_runtime`, `recomp_runtime_testing`, `recomp_snapshot`, `recomp_dx`, `recomp_dx_null`, `recomp_mods`, `recomp_mods_testing`, `recomp_gen`, `host_common`, `core_mods`, `example_mods`, `smoke_probe`, `mod_fixtures`, `plugins`, `roots_probe`, `null_host_link`; Python `buildlock.BuildLock`, `build_py.configure/build/cmake_tool/archive_path/publish_generated`, `build_core.install/plugin_extension`; C `os_*` as declared in Task 5.

## Execution notes (2026-09-12)

Deviations found while executing, all landed in the task commits:

- `os.h` gained `os_rmdir`, `os_write_stderr_raw` and `os_exit_immediately`:
  `RemoveDirectory` had one caller in `kernel32.cpp`, and the fault handler's
  raw `write(2, ...)` and `_exit` had none of the platform layer's cover.
- `kernel32.cpp` had one `pthread_cond_wait` beside the timed wait; it became
  `g_sched_cv.wait(g_sched_m)`.
- `capture_seam.cpp` includes the page-tracking harness from `src/recomp/native`
  (mprotect and sigaction). Windows compiles `capture_seam_win32.cpp` instead,
  which reports capture as unavailable. Porting page tracking is later work.
- `run_record.cpp` hashed `st_mode & S_IFMT` for special files; it now hashes
  one fixed kind code. Regular files and directories hash as before.
- `dx_tests.cpp` used `getcwd`; it now uses `os_getcwd`.
- `build_core.py` sets `SDKROOT` on macOS when absent, because a compiler named
  by its toolchain path finds no libSystem without it.
- `host_common` (boot.cpp) was not compiled by any `check_binaries` target on
  macOS until `null_host_link` existed; that target now keeps it honest.
- The Linux and macOS builds share `build/recomp/` for binaries, so a Docker
  run on the same checkout overwrites the macOS test executables; delete them
  and rebuild afterwards.
- The checks workflow does not run on a feature-branch push; it was dispatched
  with `gh workflow run checks.yml --ref portable-build-system`.
- `host_tests` failed on the macOS CI runner at an audio-level assertion
  (`peak > 0.095f` after an offline AVAudioEngine render) while passing
  locally; it is labelled `device` and runs only through `tools/test.py
  --native` on a real machine. `kernel32.cpp` also needed `os_localtime` and
  `os_gmtime` for Windows.
