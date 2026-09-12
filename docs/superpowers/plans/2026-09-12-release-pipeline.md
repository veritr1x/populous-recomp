# Release Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A player downloads one archive per platform from GitHub Releases, launches it, picks their GOG `D3DPopTB.exe` once, and plays; every `main` push refreshes a `latest` pre-release.

**Architecture:** The generated translation becomes a tracked `translation/` directory so CI builds the real hosts. One layout unit resolves resources and the profile from the executable's location (bundle, `resources/` beside the exe, or a developer checkout). The SDL host resolves the game path in a fixed order ending in a native file dialog with a hash check. A packaging script and a release workflow produce and publish the three archives.

**Tech Stack:** CMake presets, Python 3 tools, SDL3 dialogs and message boxes, GitHub Actions with `gh release`.

**Spec:** `docs/superpowers/specs/2026-09-12-release-pipeline-design.md`

## Global Constraints

- The supported exe is the one whose SHA-256 is `LOADER_EXPECTED_SHA256` in `src/recomp/runtime/loader.cpp`; there is no hash bypass.
- No game data, exe, artwork, audio or levels in the repository or the archives. The terrain texture pack (`assets/terrain/materials-v1.png`, project-made) is the only pack shipped.
- Environment overrides keep working: `POP_RECOMP_EXE`, `POPM_PROFILE_DIR`, `POPM_CORE_MODS_DIR`, `POPM_MODS_DIR`, `POPM_TEXTURE_PACK_DIR`.
- Developer flow unchanged: from a checkout, `original/gog/D3DPopTB.exe`, `build/recomp/profile`, no dialog.
- The layout unit lives in `src/recomp/mods/layout.{h,cpp}` (not `host/`, as the spec first said): the mods layer's roots, overlay, settings and run-record need it and sit below the host.
- macOS ad-hoc signing only; Linux release built on `ubuntu-22.04`.
- Branch `release-pipeline` (holds the spec). Commit after each task; run `tools/format.py` before committing C++ and `tools/check_repo.py` before committing docs.
- Keep reports short (the user asked for economy).

## File structure

| Path | Responsibility |
| --- | --- |
| `translation/` | tracked generated C, `funcs.h`, `x86.h`, `symbols.json` |
| `cmake/Translate.cmake` | prefer `build/recomp/gen`, else `translation/` |
| `tools/build.py` | `--publish-tracked` |
| `tools/recomp/tests/test_translation.py` | hash consistency + chunk presence (nogame) |
| `src/recomp/platform/os.h`, `os_posix.cpp`, `os_win32.cpp` | `os_user_data_dir` |
| `src/recomp/mods/layout.{h,cpp}` | `HostLayout`, `host_layout()`, `host_resource()`, test seam |
| `src/recomp/mods/tests/layout_tests.cpp` | layout resolution (nogame) |
| `src/recomp/mods/roots.cpp`, `overlay.cpp`, `settings.cpp`, `run_record.cpp`, `src/recomp/host/d3d_render.cpp`, `src/recomp/host/sdl/main.cpp` | consume the layout |
| `src/recomp/host/game_path.{h,cpp}` | resolution order, saved path, hash check (no SDL) |
| `src/recomp/host/tests/host_tests.cpp` | `test_game_path` |
| `src/recomp/host/sdl/main.cpp` | `--exe`, `--version`, `--probe-layout`, picker, message box |
| `src/recomp/host/version.h.in` → generated `version.h` | `POP_RECOMP_VERSION` |
| `tools/recomp/package.py`, `tools/recomp/release/README.txt` | archives |
| `.github/workflows/release.yml`, `.github/workflows/checks.yml` | release and CI |
| `NOTICE`, `README.md`, `CONTRIBUTING.md`, `docs/superpowers/PROGRESS.md` | policy and progress |

---

### Task 1: Track the translation and build the hosts in CI

**Files:**
- Create: `translation/` (copied from `build/recomp/gen`), `tools/recomp/tests/test_translation.py`, `.gitattributes`
- Modify: `cmake/Translate.cmake`, `tools/build.py`, `tools/test.py` (PORTABLE_TESTS), `.github/workflows/checks.yml`, `NOTICE`, `README.md`, `CONTRIBUTING.md`, `src/recomp/host/CMakeLists.txt` (register the test)

**Interfaces:**
- Produces: `POP_GEN_DIR` resolving to `translation/` when `build/recomp/gen/table.c` is absent; `tools/build.py --regenerate --publish-tracked`.

- [ ] **Step 1: The failing test**

`tools/recomp/tests/test_translation.py`:

```python
"""The tracked translation matches the loader's pinned exe digest and is complete."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TRANSLATION = ROOT / "translation"


class TrackedTranslation(unittest.TestCase):
    def test_digest_matches_loader(self):
        loader = (ROOT / "src/recomp/runtime/loader.cpp").read_text()
        pinned = re.search(r'LOADER_EXPECTED_SHA256 =\s*"([0-9a-f]{64})"', loader).group(1)
        symbols = json.loads((TRANSLATION / "symbols.json").read_text())
        self.assertEqual(symbols["exe_sha256"], pinned)

    def test_every_chunk_present(self):
        table = (TRANSLATION / "table.c").read_text()
        for name in sorted(set(re.findall(r"\b(chunk_\d{3})\b", table))):
            self.assertTrue((TRANSLATION / f"{name}.c").is_file(), name)
        self.assertTrue((TRANSLATION / "x86.h").is_file())
        self.assertTrue((TRANSLATION / "funcs.h").is_file())


if __name__ == "__main__":
    unittest.main()
```

Run: `python3 -m unittest tools/recomp/tests/test_translation.py` → FAIL (no `translation/`). If `table.c` names chunks by another pattern (check `grep -o "chunk_[0-9]*" build/recomp/gen/table.c | head`), adjust the regex to that pattern before going on.

- [ ] **Step 2: Copy the tracked tree and point CMake at it**

```bash
mkdir -p translation && cp build/recomp/gen/chunk_*.c build/recomp/gen/table.c build/recomp/gen/funcs.h build/recomp/gen/x86.h build/recomp/gen/symbols.json translation/
printf 'translation/*.c linguist-generated=true\ntranslation/*.h linguist-generated=true\ntranslation/symbols.json linguist-generated=true\n' > .gitattributes
du -sh translation
```

`cmake/Translate.cmake`: replace the `set(POP_GEN_DIR ...)` line and the `elseif(EXISTS ...)` chain with:

```cmake
# A developer's fresh regeneration in build/recomp/gen wins; otherwise the
# tracked translation in translation/ (the one supported GOG build).
if(EXISTS ${POP_OUT}/gen/table.c)
  set(POP_GEN_DIR ${POP_OUT}/gen)
else()
  set(POP_GEN_DIR ${POP_ROOT}/translation)
endif()
set(POP_HAVE_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: PopRecomp, pop_headless, pop_smoke, pop_fixture, "
                 "mods_tests, present_events_tests and profile_tests are not defined")
endif()
```

`tools/build.py`: add `parser.add_argument("--publish-tracked", action="store_true", help="Copy the regenerated translation into translation/ for committing")` and, in `main()` after `publish_generated(ROOT, run_translator)`:

```python
            if args.publish_tracked:
                publish_tracked(ROOT)
```

with

```python
def publish_tracked(root):
    """Replace translation/ with build/recomp/gen wholesale; the caller commits it."""
    root = Path(root)
    gen, tracked = root / "build/recomp/gen", root / "translation"
    shutil.rmtree(tracked, ignore_errors=True)
    tracked.mkdir()
    for path in sorted(gen.iterdir()):
        if path.suffix in (".c", ".h") or path.name == "symbols.json":
            shutil.copy(path, tracked / path.name)
    print("published %d files to translation/" % sum(1 for _ in tracked.iterdir()))
```

Also make `--publish-tracked` imply `--regenerate` (`if args.publish_tracked: args.regenerate = True` right after `parse_args`). In `main()`, the check `args.target in NEEDS_GEN and not (ROOT / "original/gog/D3DPopTB.exe").is_file()` must only apply when regenerating: change it to `if args.regenerate and not (...)`, so a contributor without the game can build the app from the tracked translation.

Register the test: `tools/test.py` PORTABLE_TESTS gets `"tools/recomp/tests/test_translation.py"`; `src/recomp/host/CMakeLists.txt` gets, next to `shader_drift_check`:

```cmake
add_test(NAME translation_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_translation.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(translation_check PROPERTIES LABELS nogame)
```

- [ ] **Step 3: Prove a clean configure builds the hosts from the tracked tree**

```bash
python3 -m unittest tools/recomp/tests/test_translation.py
mv build/recomp/gen build/recomp/gen.aside
.venv/bin/cmake --preset macos 2>&1 | grep -E "Translation:|No translation"
.venv/bin/cmake --build --preset macos --target pop_headless 2>&1 | tail -1
mv build/recomp/gen.aside build/recomp/gen
.venv/bin/cmake --preset macos 2>&1 | grep "Translation:"
```

Expected: test OK; `Translation: .../translation`; pop_headless links; then `Translation: .../build/recomp/gen` again.

- [ ] **Step 4: CI builds the hosts; policy text**

`.github/workflows/checks.yml`: `run: cmake --preset ${{ matrix.preset }}` (drop `-DPOP_TRANSLATE=OFF`); after `check_binaries` add `- name: Build the hosts` / `run: cmake --build --preset ${{ matrix.preset }} --target PopRecomp pop_headless pop_smoke`. Update the job comment ("The game files are not available in public CI; the tracked translation lets every platform build the hosts.").

`NOTICE` line 10 area, replace "Original game files and locally generated translations are not distributed in this repository and are not relicensed by its MIT license." with:

```
Original game files are not distributed in this repository and are not relicensed by
its MIT license. The directory translation/ holds a translation of the one supported
GOG build of D3DPopTB.exe (SHA-256 815ba8a5...eacd), generated from that executable
by this project's tools; it is distributed so prebuilt binaries can be published,
under the same terms as the handwritten code, and claims nothing over the original
work's behaviour or ownership. The game's executable, data, artwork, audio and levels
are still required from the player's own copy and are never included.
```

`README.md`: in the third-party table add `| \`translation/\` | Generated translation of the supported GOG executable; regenerate with \`tools/build.py --regenerate --publish-tracked\` |`; in "Play on macOS" replace the two-line build/open with a pointer to Releases ("Download the archive for your platform from the Releases page; see the README.txt inside. To build from source:" then the existing commands). `CONTRIBUTING.md` "Prepare your game installation": add one sentence that building the app no longer requires the translation step; regenerating does.

- [ ] **Step 5: Commit (large)**

```bash
.venv/bin/python tools/check_repo.py && git add translation .gitattributes cmake/Translate.cmake tools/build.py tools/test.py tools/recomp/tests/test_translation.py .github/workflows/checks.yml NOTICE README.md CONTRIBUTING.md src/recomp/host/CMakeLists.txt
git commit -m "Track the translation of the supported GOG build; CI builds the hosts"
```

---

### Task 2: `os_user_data_dir`

**Files:**
- Modify: `src/recomp/platform/os.h`, `os_posix.cpp`, `os_win32.cpp`, `src/recomp/platform/tests/platform_tests.cpp`

**Interfaces:**
- Produces: `int os_user_data_dir(const char *app, char *buf, size_t cap);` 0 or -1; no trailing separator; the directory is not created.

- [ ] **Step 1: Failing test** (append to `test_paths_and_descriptors` or `test_process_and_strings`)

```cpp
    char data[4096];
    CHECK(os_user_data_dir("PopRecompTest", data, sizeof data) == 0);
    CHECK(strstr(data, "PopRecompTest") != nullptr);
    CHECK(data[strlen(data) - 1] != '/' && data[strlen(data) - 1] != '\\');
```

Run: build `platform_tests` → compile error.

- [ ] **Step 2: Implement**

`os.h`: `// Per-user data directory for `app` (not created): ~/Library/Application Support/<app>,\n// %APPDATA%\\<app>, $XDG_DATA_HOME/<app> or ~/.local/share/<app>. 0 or -1.\nint os_user_data_dir(const char *app, char *buf, size_t cap);`

`os_posix.cpp`:

```cpp
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    const char *home = getenv("HOME");
    char base[4096];
#ifdef __APPLE__
    if (!home || !*home)
        return -1;
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(base, sizeof base, "%s", xdg);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.local/share", home);
    else
        return -1;
#endif
    int n = snprintf(buf, cap, "%s/%s", base, app);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
```

`os_win32.cpp`:

```cpp
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", w, MAX_PATH + 1);
    if (!n || n > MAX_PATH)
        return -1;
    std::string s = narrow(w) + "\\" + app;
    if (s.size() + 1 > cap)
        return -1;
    memcpy(buf, s.c_str(), s.size() + 1);
    return 0;
}
```

- [ ] **Step 3: Test and commit**

`build/recomp/platform_tests` → all pass. `git commit -am "Platform: os_user_data_dir"` (after `tools/format.py`).

---

### Task 3: The layout unit and its consumers

**Files:**
- Create: `src/recomp/mods/layout.h`, `src/recomp/mods/layout.cpp`, `src/recomp/mods/tests/layout_tests.cpp`
- Modify: `src/recomp/mods/CMakeLists.txt` (layout in `recomp_mods` glob automatically; `roots_probe` adds `layout.cpp`; new `layout_tests` nogame target), `src/recomp/mods/roots.cpp`, `overlay.cpp`, `settings.cpp`, `run_record.cpp`, `src/recomp/host/d3d_render.cpp`, `src/recomp/host/sdl/main.cpp` (`classic_modes_path`)

**Interfaces:**
- Produces:

```cpp
// layout.h - where this executable's resources and the player's profile live.
#pragma once
#include <string>
struct HostLayout {
    std::string resources_dir; // "" when nothing was found
    std::string profile_dir;
    std::string checkout_root; // "" outside a checkout
    bool developer = false;    // checkout_root is set
};
const HostLayout &host_layout();
// resources_dir + "/" + rel, with the developer mapping for the three names
// "mods/core", "texture-pack" and "classic-modes.json"; "" when unknown.
std::string host_resource(const char *rel);
// Test seam: recompute from this executable path and this environment
// (nullptr restores the real path). Not thread-safe; tests only.
void host_layout_set_exe_path_for_test(const char *exe_path);
```

- [ ] **Step 1: Failing test**

`src/recomp/mods/tests/layout_tests.cpp` (its own tiny CHECK macro like `platform_tests.cpp`):

```cpp
#include "../layout.h"
#include "../../platform/os.h"
#include <stdio.h>
#include <string.h>
#include <string>

static int g_checks = 0, g_failures = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static std::string temp_root() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pop-layout-XXXXXX", os_temp_dir());
    return os_mkdtemp(dir) == 0 ? dir : "";
}
static void touch(const std::string &path) {
    if (FILE *f = fopen(path.c_str(), "wb"))
        fclose(f);
}
static void mkdir_p(const std::string &path) {
    std::string acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < path.size())
            acc.push_back(path[i]);
    }
}

int main() {
    os_unsetenv("POPM_PROFILE_DIR");
    std::string root = temp_root();
    CHECK(!root.empty());
    // 1. resources/ beside the executable (Windows and Linux archives).
    mkdir_p(root + "/portable/resources/mods/core");
    touch(root + "/portable/PopRecomp");
    host_layout_set_exe_path_for_test((root + "/portable/PopRecomp").c_str());
    CHECK(host_layout().resources_dir == root + "/portable/resources");
    CHECK(!host_layout().developer);
    CHECK(host_resource("mods/core") == root + "/portable/resources/mods/core");
    CHECK(host_layout().profile_dir.find("PopRecomp") != std::string::npos);
    CHECK(host_layout().profile_dir.find(root) == std::string::npos); // per-user, not beside the exe
    // 2. A macOS bundle.
    mkdir_p(root + "/X.app/Contents/MacOS");
    mkdir_p(root + "/X.app/Contents/Resources");
    touch(root + "/X.app/Contents/MacOS/X");
    host_layout_set_exe_path_for_test((root + "/X.app/Contents/MacOS/X").c_str());
    CHECK(host_layout().resources_dir == root + "/X.app/Contents/Resources");
    CHECK(host_resource("classic-modes.json") == root + "/X.app/Contents/Resources/classic-modes.json");
    // 3. A checkout: the marker file above the executable.
    mkdir_p(root + "/co/tools/recomp/baseline");
    mkdir_p(root + "/co/build/recomp");
    touch(root + "/co/tools/recomp/baseline/classic-modes.json");
    touch(root + "/co/build/recomp/pop_headless");
    host_layout_set_exe_path_for_test((root + "/co/build/recomp/pop_headless").c_str());
    CHECK(host_layout().developer);
    CHECK(host_layout().checkout_root == root + "/co");
    CHECK(host_resource("mods/core") == root + "/co/build/recomp/mods/core");
    CHECK(host_resource("texture-pack") == root + "/co/build/texture-pack");
    CHECK(host_resource("classic-modes.json") == root + "/co/tools/recomp/baseline/classic-modes.json");
    CHECK(host_layout().profile_dir == root + "/co/build/recomp/profile");
    // 4. POPM_PROFILE_DIR wins everywhere.
    os_setenv("POPM_PROFILE_DIR", "/elsewhere/profile");
    host_layout_set_exe_path_for_test((root + "/co/build/recomp/pop_headless").c_str());
    CHECK(host_layout().profile_dir == "/elsewhere/profile");
    os_unsetenv("POPM_PROFILE_DIR");
    // 5. Nothing found: empty resources, per-user profile.
    mkdir_p(root + "/bare");
    touch(root + "/bare/exe");
    host_layout_set_exe_path_for_test((root + "/bare/exe").c_str());
    CHECK(host_layout().resources_dir.empty());
    CHECK(host_resource("mods/core").empty());
    CHECK(!host_layout().developer);
    host_layout_set_exe_path_for_test(nullptr);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all layout tests passed\n");
    return g_failures ? 1 : 0;
}
```

CMake (in `src/recomp/mods/CMakeLists.txt`, outside the `recomp_gen` block):

```cmake
add_executable(layout_tests layout.cpp tests/layout_tests.cpp)
target_include_directories(layout_tests PRIVATE ${POP_ROOT})
target_link_libraries(layout_tests PRIVATE recomp_platform)
target_compile_options(layout_tests PRIVATE ${POP_WARN_STRICT})
pop_optimize(layout_tests 1)
pop_test_binary(layout_tests)
add_test(NAME layout_tests COMMAND layout_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(layout_tests PROPERTIES LABELS nogame)
```

and `roots_probe` sources gain `layout.cpp`. Run: configure + build `layout_tests` → compile error (no layout.h).

- [ ] **Step 2: Implement `layout.cpp`**

```cpp
#include "layout.h"
#include "../platform/os.h"
#include <stdlib.h>
#include <string.h>

namespace {
std::string g_test_exe;
HostLayout g_layout;
bool g_computed = false;

bool exists(const std::string &p) {
    OsStat st;
    return os_stat(p.c_str(), &st) == 0;
}
std::string parent(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? "" : p.substr(0, s);
}
bool ends_with(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

HostLayout compute() {
    HostLayout l;
    std::string exe = g_test_exe;
    if (exe.empty()) {
        char path[4096];
        if (os_exe_path(path, sizeof path) == 0)
            exe = path;
    }
    for (char &c : exe)
        if (c == '\\')
            c = '/';
    const std::string dir = parent(exe);
    if (ends_with(dir, "/Contents/MacOS"))
        l.resources_dir = parent(dir) + "/Resources";
    else if (exists(dir + "/resources"))
        l.resources_dir = dir + "/resources";
    else {
        std::string up = dir;
        for (int depth = 0; depth < 12 && !up.empty(); ++depth) {
            if (exists(up + "/tools/recomp/baseline/classic-modes.json")) {
                l.checkout_root = up;
                l.developer = true;
                l.resources_dir = up;
                break;
            }
            up = parent(up);
        }
    }
    const char *env = getenv("POPM_PROFILE_DIR");
    if (env && *env)
        l.profile_dir = env;
    else if (l.developer)
        l.profile_dir = l.checkout_root + "/build/recomp/profile";
    else {
        char buf[4096];
        if (os_user_data_dir("PopRecomp", buf, sizeof buf) == 0)
            l.profile_dir = buf;
        else
            l.profile_dir = "profile"; // no home at all: beside the cwd, as before
    }
    return l;
}
} // namespace

const HostLayout &host_layout() {
    if (!g_computed) {
        g_layout = compute();
        g_computed = true;
    }
    return g_layout;
}

std::string host_resource(const char *rel) {
    const HostLayout &l = host_layout();
    if (l.resources_dir.empty())
        return "";
    if (l.developer) {
        if (strcmp(rel, "mods/core") == 0)
            return l.checkout_root + "/build/recomp/mods/core";
        if (strcmp(rel, "texture-pack") == 0)
            return l.checkout_root + "/build/texture-pack";
        if (strcmp(rel, "classic-modes.json") == 0)
            return l.checkout_root + "/tools/recomp/baseline/classic-modes.json";
    }
    return l.resources_dir + "/" + rel;
}

void host_layout_set_exe_path_for_test(const char *exe_path) {
    g_test_exe = exe_path ? exe_path : "";
    g_computed = false;
}
```

Run `layout_tests` → all pass.

- [ ] **Step 3: Consumers**

- `roots.cpp`: `#include "layout.h"`; default core root becomes `host_resource("mods/core")` when `POPM_CORE_MODS_DIR` is unset (fall back to `"build/recomp/mods/core"` when it returns ""); drop the bundle special case (the layout covers it). `roots_probe` still passes `roots_tests.py` (the relocated-app case resolves through the layout).
- `overlay.cpp` `mods_overlay_reset`: `profile() = env && *env ? env : host_layout().profile_dir;`. `settings.cpp` weak fallback and `run_record.cpp`: same expression via `host_layout().profile_dir` (include `layout.h`).
- `d3d_render.cpp`: `packPath = packEnv ? packEnv : host_resource("texture-pack");` with `"build/texture-pack"` when empty; delete the bundle special case.
- `sdl/main.cpp` `classic_modes_path()`: `std::string p = host_resource("classic-modes.json"); return p.empty() ? "tools/recomp/baseline/classic-modes.json" : p;`.

Verify: `ctest --preset macos -L "nogame|gpu"` green (roots, layout, host tests), `tools/test.py --mods` green, one gameplay run green.

- [ ] **Step 4: Commit**

`git add -A src && git commit -m "One install layout: resources and profile resolved from the executable"`.

---

### Task 4: Version string and the two probe flags

**Files:**
- Create: `src/recomp/host/version.h.in`
- Modify: `CMakeLists.txt` (POP_RECOMP_VERSION, configure_file), `src/recomp/host/CMakeLists.txt` (include dir), `src/recomp/host/Info.plist` (CFBundleShortVersionString from the version), `cmake/MacBundle.cmake` or `finish_bundle.py` (write the version into the plist), `src/recomp/host/sdl/main.cpp`

**Interfaces:**
- Produces: `#define POP_RECOMP_VERSION "<git describe>"` in the generated `version.h`; flags `--version` and `--probe-layout` (exit 0).

- [ ] **Step 1: Version at configure time**

`CMakeLists.txt` after `project(...)`:

```cmake
# The version is the git description of the commit being built; a tarball
# without git gets "unknown".
find_package(Git QUIET)
set(POP_RECOMP_VERSION "unknown" CACHE STRING "Version string baked into the hosts")
if(POP_RECOMP_VERSION STREQUAL "unknown" AND GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                  WORKING_DIRECTORY ${POP_ROOT} OUTPUT_VARIABLE POP_GIT_DESCRIBE
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(POP_GIT_DESCRIBE)
    set(POP_RECOMP_VERSION ${POP_GIT_DESCRIBE})
  endif()
endif()
configure_file(${POP_ROOT}/src/recomp/host/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
```

`version.h.in`: `#pragma once\n#define POP_RECOMP_VERSION "@POP_RECOMP_VERSION@"\n`. In `src/recomp/host/CMakeLists.txt`'s `pop_host`, add `${CMAKE_BINARY_DIR}/generated` to the include directories. `finish_bundle.py` gains `--version` and writes `CFBundleShortVersionString` (and `CFBundleVersion`) into the plist; `MacBundle.cmake` passes `--version ${POP_RECOMP_VERSION}`.

- [ ] **Step 2: Flags in `sdl/main.cpp`**

Replace `(void)argc; (void)argv;` with an argument loop before anything else:

```cpp
    const char *exe_flag = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("PopRecomp %s (%s)\n", POP_RECOMP_VERSION, gpu::default_backend_name());
            return 0;
        }
        if (strcmp(argv[i], "--probe-layout") == 0) {
            const HostLayout &l = host_layout();
            printf("resources_dir=%s\nprofile_dir=%s\ndeveloper=%d\n", l.resources_dir.c_str(),
                   l.profile_dir.c_str(), int(l.developer));
            return 0;
        }
        if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc)
            exe_flag = argv[++i];
        else if (strncmp(argv[i], "--exe=", 6) == 0)
            exe_flag = argv[i] + 6;
        else {
            fprintf(stderr, "usage: PopRecomp [--exe <D3DPopTB.exe>] [--version] [--probe-layout]\n");
            return 2;
        }
    }
```

(`exe_flag` is consumed in Task 5; until then pass it through `POP_RECOMP_EXE` semantics by `if (exe_flag) os_setenv("POP_RECOMP_EXE", exe_flag);`.) Includes: `"version.h"`, `"../../mods/layout.h"`.

- [ ] **Step 3: Verify and commit**

```bash
.venv/bin/cmake --preset macos > /dev/null && .venv/bin/cmake --build --preset macos --target PopRecomp
build/PopRecomp.app/Contents/MacOS/PopRecomp --version
(cd /tmp && ~/Documents/Tests/populous-recomp-checkout/build/PopRecomp.app/Contents/MacOS/PopRecomp --probe-layout)
plutil -p build/PopRecomp.app/Contents/Info.plist | grep ShortVersion
```

Expected: `PopRecomp v<describe> (metal)`; the probe reports the bundle's Resources and the checkout's profile (developer=1, since the bundle sits under the checkout's `build/`). Commit: "Version string, --version and --probe-layout".

---

### Task 5: Finding the game: resolution order, saved path, hash check, picker

**Files:**
- Create: `src/recomp/host/game_path.h`, `src/recomp/host/game_path.cpp`
- Modify: `src/recomp/runtime/loader.h/.cpp` (`loader_hash_file`), `src/recomp/host/sdl/main.cpp`, `src/recomp/host/tests/host_tests.cpp`, `src/recomp/host/CMakeLists.txt` (game_path.cpp in `PopRecomp` and `host_tests` sources)

**Interfaces:**
- Produces:

```cpp
// game_path.h - which D3DPopTB.exe to load, without any UI.
#pragma once
#include <string>
enum class GamePathSource { Flag, Environment, Checkout, Saved, None };
struct GamePath {
    std::string exe;       // "" when nothing resolved
    GamePathSource source = GamePathSource::None;
};
// Order: `flag`, POP_RECOMP_EXE, the checkout's original/gog above the
// executable (developer mode), <profile_dir>/game-path.txt when its file
// exists and hashes to the supported digest.
GamePath game_path_resolve(const char *flag);
// True when `path` exists and its SHA-256 is LOADER_EXPECTED_SHA256.
bool game_path_is_supported(const std::string &path, std::string *digest_out);
// Writes <profile_dir>/game-path.txt (creating profile_dir); false on failure.
bool game_path_save(const std::string &path);
```

and `std::string loader_hash_file(const char *path);` in `loader.h` ("" when unreadable).

- [ ] **Step 1: Failing test** in `host_tests.cpp`:

```cpp
static void test_game_path() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pop-gamepath-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    os_setenv("POPM_PROFILE_DIR", dir);
    os_unsetenv("POP_RECOMP_EXE");
    host_layout_set_exe_path_for_test((std::string(dir) + "/nowhere/exe").c_str()); // not a checkout
    // Nothing saved: None.
    CHECK(game_path_resolve(nullptr).source == GamePathSource::None);
    // A flag wins without any check.
    CHECK(game_path_resolve("/x/D3DPopTB.exe").source == GamePathSource::Flag);
    // A saved path to a wrong file is ignored.
    std::string wrong = std::string(dir) + "/wrong.exe";
    FILE *f = fopen(wrong.c_str(), "wb");
    fputs("not the game", f);
    fclose(f);
    std::string digest;
    CHECK(!game_path_is_supported(wrong, &digest));
    CHECK(digest.size() == 64);
    CHECK(game_path_save(wrong));
    CHECK(game_path_resolve(nullptr).source == GamePathSource::None);
    // The real game, when present, is accepted and remembered.
    OsStat st;
    if (os_stat("original/gog/D3DPopTB.exe", &st) == 0) {
        CHECK(game_path_is_supported("original/gog/D3DPopTB.exe", nullptr));
        CHECK(game_path_save("original/gog/D3DPopTB.exe"));
        GamePath g = game_path_resolve(nullptr);
        CHECK(g.source == GamePathSource::Saved && g.exe == "original/gog/D3DPopTB.exe");
    }
    os_unsetenv("POPM_PROFILE_DIR");
    host_layout_set_exe_path_for_test(nullptr);
}
```

Register it in the test table as `{"game path", test_game_path}` (it takes no renderer: use whichever list holds renderer-free tests; if all take `D3DRenderer *`, give it an unused parameter). Run: compile error.

- [ ] **Step 2: Implement**

`loader.cpp`:

```cpp
std::string loader_hash_file(const char *path) {
    std::vector<uint8_t> file;
    if (!read_file(path, file))
        return "";
    return sha256_hex(file);
}
```

`game_path.cpp`:

```cpp
#include "game_path.h"
#include "../mods/layout.h"
#include "../platform/os.h"
#include "../runtime/loader.h"
#include <stdio.h>
#include <stdlib.h>

static std::string saved_file() {
    return host_layout().profile_dir + "/game-path.txt";
}

bool game_path_is_supported(const std::string &path, std::string *digest_out) {
    std::string digest = loader_hash_file(path.c_str());
    if (digest_out)
        *digest_out = digest;
    return !digest.empty() && digest == LOADER_EXPECTED_SHA256;
}

bool game_path_save(const std::string &path) {
    const std::string dir = host_layout().profile_dir;
    std::string acc;
    for (size_t i = 0; i <= dir.size(); ++i) { // mkdir -p
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < dir.size())
            acc.push_back(dir[i]);
    }
    FILE *f = fopen(saved_file().c_str(), "wb");
    if (!f)
        return false;
    fputs(path.c_str(), f);
    fputc('\n', f);
    return fclose(f) == 0;
}

GamePath game_path_resolve(const char *flag) {
    GamePath g;
    if (flag && *flag) {
        g.exe = flag;
        g.source = GamePathSource::Flag;
        return g;
    }
    if (const char *env = getenv("POP_RECOMP_EXE"); env && *env) {
        g.exe = env;
        g.source = GamePathSource::Environment;
        return g;
    }
    const HostLayout &l = host_layout();
    if (l.developer) {
        std::string candidate = l.checkout_root + "/original/gog/D3DPopTB.exe";
        OsStat st;
        if (os_stat(candidate.c_str(), &st) == 0) {
            g.exe = candidate;
            g.source = GamePathSource::Checkout;
            return g;
        }
    }
    if (FILE *f = fopen(saved_file().c_str(), "rb")) {
        char line[4096] = {0};
        if (fgets(line, sizeof line, f)) {
            std::string path(line);
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            if (!path.empty() && game_path_is_supported(path, nullptr)) {
                g.exe = path;
                g.source = GamePathSource::Saved;
            }
        }
        fclose(f);
    }
    return g;
}
```

`sdl/main.cpp`: replace `find_exe_relative_to_bundle()` and its error with:

```cpp
    GamePath game = game_path_resolve(exe_flag);
    if (game.source == GamePathSource::Checkout)
        os_chdir(host_layout().checkout_root.c_str()); // developer runs keep relative paths
```

then, after `SDL_Init` and the (hidden) window creation and before the renderer:

```cpp
    if (game.exe.empty() && !pick_game_exe(&game.exe))
        return 2;
```

with, in the anonymous namespace:

```cpp
struct DialogResult {
    bool done = false;
    std::string path; // empty: cancelled or failed
};
void SDLCALL on_dialog(void *userdata, const char *const *files, int) {
    auto *r = static_cast<DialogResult *>(userdata);
    if (files && files[0])
        r->path = files[0];
    else if (!files)
        fprintf(stderr, "PopRecomp: file dialog failed: %s\n", SDL_GetError());
    r->done = true;
}
// The picker: a native dialog for D3DPopTB.exe, the hash check, and the saved path.
bool pick_game_exe(std::string *out) {
    for (;;) {
        DialogResult r;
        const SDL_DialogFileFilter filters[] = {{"Populous executable", "exe"}};
        SDL_ShowOpenFileDialog(on_dialog, &r, g_window, filters, 1, nullptr, false);
        while (!r.done) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT)
                    return false;
            SDL_Delay(10);
        }
        if (r.path.empty()) {
            fprintf(stderr, "PopRecomp: no game selected. Pass --exe <path to D3DPopTB.exe> to "
                            "skip the dialog.\n");
            return false;
        }
        std::string digest;
        if (game_path_is_supported(r.path, &digest)) {
            if (!game_path_save(r.path))
                fprintf(stderr, "PopRecomp: could not remember the game path\n");
            *out = r.path;
            return true;
        }
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose again"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
        std::string text = "This is not the supported GOG build of D3DPopTB.exe.\n\nExpected SHA-256:\n" +
                           std::string(LOADER_EXPECTED_SHA256) + "\n\nThis file:\n" +
                           (digest.empty() ? std::string("(unreadable)") : digest);
        SDL_MessageBoxData box{SDL_MESSAGEBOX_ERROR, g_window, "Populous: The Beginning", text.c_str(), 2,
                               buttons, nullptr};
        int choice = 0;
        if (!SDL_ShowMessageBox(&box, &choice) || choice == 0)
            return false;
    }
}
```

Where the window is created hidden it stays hidden until after the pick; the picker needs `g_window` so move the window creation before the pick (the GPU device creation already precedes it). `boot_load` receives `game.exe`. Add `game_path.cpp` to the `PopRecomp` and `host_tests` source lists.

- [ ] **Step 3: Verify**

```bash
.venv/bin/cmake --build --preset macos --target check_binaries PopRecomp && build/recomp/host_tests 2>&1 | grep -E "game path|failures"
```

Manual (this Mac): copy `build/PopRecomp.app` to `/tmp/PopTest/PopRecomp.app`, run `/tmp/PopTest/PopRecomp.app/Contents/MacOS/PopRecomp` → the dialog appears; pick `original/gog/D3DPopTB.exe` → the game starts; quit; relaunch → no dialog; `cat "$HOME/Library/Application Support/PopRecomp/game-path.txt"`. Pick a wrong file once → the mismatch box, Choose again. Note in the plan's execution notes that the manual check ran.

- [ ] **Step 4: Commit** "Find the game: resolution order, saved path, hash check and picker".

---

### Task 6: Packaging script, README.txt, release workflow

**Files:**
- Create: `tools/recomp/package.py`, `tools/recomp/release/README.txt`, `.github/workflows/release.yml`
- Modify: `tools/test.py` (nothing), `.gitignore` (`/dist/`)

**Interfaces:**
- Produces: `python3 tools/recomp/package.py --preset {macos,linux,windows} --version <v> --out dist/` → prints the archive path.

- [ ] **Step 1: `package.py`**

```python
#!/usr/bin/env python3
"""Package a finished build into the release archive for one platform.

macOS: PopRecomp-<v>-macos-arm64.zip holding PopRecomp.app (as finish_bundle.py
built it), LICENSE, NOTICE, README.txt. Windows/Linux: a folder with the
executable, resources/{mods/core,texture-pack,classic-modes.json}, LICENSE,
NOTICE, README.txt, zipped (Windows) or tar.gz'd (Linux)."""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ["LICENSE", "NOTICE"]


def stage_resources(dest, cc):
    resources = dest / "resources"
    resources.mkdir(parents=True)
    shutil.copy(ROOT / "tools/recomp/baseline/classic-modes.json", resources / "classic-modes.json")
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"),
                    "--dest", str(resources / "mods/core"), "--cc", cc], check=True, cwd=ROOT)
    pack = ROOT / "build/texture-pack"
    if (pack / "manifest.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/package_texture_pack.py"),
                        str(pack), str(resources / "texture-pack")], check=True, cwd=ROOT)


def add_docs(dest):
    for name in DOCS:
        shutil.copy(ROOT / name, dest / name)
    shutil.copy(ROOT / "tools/recomp/release/README.txt", dest / "README.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preset", choices=("macos", "linux", "windows"), required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--cc", default=os.environ.get("CC", "clang"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    stage = args.out / f"stage-{args.preset}"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    name = f"PopRecomp-{args.version}-" + {"macos": "macos-arm64", "linux": "linux-x64",
                                           "windows": "windows-x64"}[args.preset]
    if args.preset == "macos":
        app = ROOT / "build/PopRecomp.app"
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        shutil.copytree(app, stage / "PopRecomp.app", symlinks=True)
        add_docs(stage)
        archive = args.out / f"{name}.zip"
        # ditto keeps the bundle's metadata and signature; zipfile does not.
        subprocess.run(["ditto", "-c", "-k", "--keepParent", "--sequesterRsrc", str(stage), str(archive)],
                       check=True)
    else:
        folder = stage / "PopRecomp"
        folder.mkdir()
        exe = ROOT / ("build/recomp/PopRecomp.exe" if args.preset == "windows" else "build/recomp/PopRecomp")
        shutil.copy(exe, folder / exe.name)
        stage_resources(folder, args.cc)
        add_docs(folder)
        if args.preset == "windows":
            archive = args.out / f"{name}.zip"
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
                for path in sorted(folder.rglob("*")):
                    z.write(path, path.relative_to(stage))
        else:
            archive = args.out / f"{name}.tar.gz"
            with tarfile.open(archive, "w:gz") as t:
                t.add(folder, arcname="PopRecomp")
    shutil.rmtree(stage)
    print(archive)


if __name__ == "__main__":
    main()
```

(`ditto` with `--keepParent` archives `stage` as a top-level folder; if the zip should open to `PopRecomp.app` directly, run ditto on each item instead. Check with `unzip -l`.)

`tools/recomp/release/README.txt`:

```
Populous: The Beginning - native port (PopRecomp)

You need your own copy of the game: the GOG release of Populous: The Beginning.
This download contains no game files.

First run
  macOS:   right-click PopRecomp.app, choose Open, confirm once (the app is not notarized).
  Windows: run PopRecomp.exe; if SmartScreen appears choose "More info" then "Run anyway".
  Linux:   run ./PopRecomp from the PopRecomp folder; a Vulkan driver must be installed.
A file dialog asks for D3DPopTB.exe. Pick it from your GOG installation. The game
starts, and the choice is remembered, so later launches need no dialog.

Only the GOG build is supported (SHA-256 815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd).
Any other file is refused with a message that shows both digests.

Options
  PopRecomp --exe <path to D3DPopTB.exe>   skip the dialog and the saved path
  PopRecomp --version                      print the version and GPU backend
  PopRecomp --probe-layout                 print where resources and the profile live
  POP_GPU_BACKEND=vulkan|metal             pick the GPU backend (macOS defaults to Metal)

Your settings and saves live in
  macOS:   ~/Library/Application Support/PopRecomp
  Windows: %APPDATA%\PopRecomp
  Linux:   ~/.local/share/PopRecomp (or $XDG_DATA_HOME/PopRecomp)
Replacing this folder with a newer download keeps them.

In game: F10 opens Options. Escape (held) releases the mouse. Command-Q or Alt-F4 quits.
Source, issues and releases: https://github.com/veritr1x/populous-recomp
```

Add `/dist/` to `.gitignore`.

- [ ] **Step 2: Local packaging check (macOS)**

```bash
.venv/bin/python tools/build.py --target app
.venv/bin/python tools/recomp/package.py --preset macos --version $(git describe --tags --always)
unzip -l dist/PopRecomp-*-macos-arm64.zip | head
mkdir -p /tmp/poprel && cd /tmp/poprel && unzip -oq ~/Documents/Tests/populous-recomp-checkout/dist/PopRecomp-*-macos-arm64.zip && ./*/PopRecomp.app/Contents/MacOS/PopRecomp --probe-layout
```

Expected: the probe prints the unpacked bundle's `Contents/Resources`, `developer=0`, and the per-user profile.

- [ ] **Step 3: `release.yml`**

```yaml
name: Release

on:
  push:
    branches: [main]
    tags: ["v*"]

permissions:
  contents: read

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: macos-15
            preset: macos
          - os: ubuntu-22.04
            preset: linux
          - os: windows-2025
            preset: windows
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v7.0.1
        with:
          fetch-depth: 0
      - name: Install clang, lld and SDL3's build dependencies (Linux)
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y -qq clang lld build-essential pkg-config libasound2-dev \
            libpulse-dev libaudio-dev libjack-dev libsndio-dev libx11-dev libxext-dev \
            libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
            libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
            libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev \
            libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev
      - name: Enter the Visual Studio developer environment (Windows)
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1
      - name: Install CMake, Ninja and the texture pack tools
        shell: bash
        run: |
          python3 -m venv .ci
          if [ -d .ci/Scripts ]; then BIN=.ci/Scripts; else BIN=.ci/bin; fi
          "$BIN/python" -m pip install cmake==4.1.2 ninja==1.13.0 -r tools/recomp/texture-pack-requirements.txt
          if [ "$RUNNER_OS" = Windows ]; then cygpath -w "$PWD/$BIN" >> "$GITHUB_PATH"; else echo "$PWD/$BIN" >> "$GITHUB_PATH"; fi
          echo "VERSION=$(git describe --tags --always)" >> "$GITHUB_ENV"
      - name: Build
        shell: bash
        run: |
          python tools/recomp/terrain_detail.py --source assets/terrain/materials-v1.png --output build/texture-pack
          cmake --preset ${{ matrix.preset }} -DPOP_RECOMP_VERSION="$VERSION"
          cmake --build --preset ${{ matrix.preset }} --target PopRecomp core_mods
      - name: Package
        shell: bash
        run: python tools/recomp/package.py --preset ${{ matrix.preset }} --version "$VERSION" --out dist
      - name: Smoke the archive from another directory
        shell: bash
        run: |
          mkdir -p "$RUNNER_TEMP/smoke" && cd "$RUNNER_TEMP/smoke"
          case "${{ matrix.preset }}" in
            macos) unzip -oq "$GITHUB_WORKSPACE"/dist/*.zip; EXE=$(find . -path '*/Contents/MacOS/PopRecomp' | head -1);;
            linux) tar xzf "$GITHUB_WORKSPACE"/dist/*.tar.gz; EXE=./PopRecomp/PopRecomp;;
            windows) unzip -oq "$GITHUB_WORKSPACE"/dist/*.zip; EXE=./PopRecomp/PopRecomp.exe;;
          esac
          "$EXE" --version
          "$EXE" --probe-layout | tee probe.txt
          grep -q "developer=0" probe.txt
          grep -qi "resources" probe.txt
      - uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.preset }}
          path: dist/PopRecomp-*

  publish:
    needs: build
    runs-on: ubuntu-24.04
    permissions:
      contents: write
    steps:
      - uses: actions/checkout@v7.0.1
        with:
          fetch-depth: 0
      - uses: actions/download-artifact@v4
        with:
          path: dist
          merge-multiple: true
      - name: Publish
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          ls -l dist
          if [[ "$GITHUB_REF" == refs/tags/v* ]]; then
            gh release create "$GITHUB_REF_NAME" dist/PopRecomp-* --generate-notes --title "$GITHUB_REF_NAME"
          else
            LAST=$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null || echo "")
            NOTES=$(git log --oneline ${LAST:+$LAST..}HEAD | head -50)
            git tag -f latest && git push -f origin latest
            gh release delete latest --yes --cleanup-tag 2>/dev/null || true
            gh release create latest dist/PopRecomp-* --prerelease --title "Latest (main)" \
              --notes "Rolling build of main at ${GITHUB_SHA::7}.${LAST:+ Changes since $LAST:}
          $NOTES"
          fi
```

(The Linux smoke needs no display: `--version` and `--probe-layout` return before `SDL_Init`. Keep them before it in `main`. On Windows the smoke runs from `bash`; `PopRecomp.exe` prints to the console because it is a console subsystem binary, which it is today.)

- [ ] **Step 4: Commit and run**

```bash
git add tools/recomp/package.py tools/recomp/release/README.txt .github/workflows/release.yml .gitignore
git commit -m "Packaging script and the release workflow"
git push -u origin release-pipeline
gh workflow run checks.yml --ref release-pipeline
```

`release.yml` only runs on `main` and tags, so it is exercised by the merge (Task 7). To test it before merging, run it once by hand: temporarily add `workflow_dispatch:` to its `on:` block (keep it: it is useful) and `gh workflow run release.yml --ref release-pipeline`; the publish step then creates `latest` from the branch, which the merge run replaces. Watch with `gh run watch <id> --exit-status`.

---

### Task 7: Verification, docs, merge

**Files:**
- Modify: `docs/superpowers/PROGRESS.md`, this plan (execution notes), memory `multi-platform-port-roadmap.md`

- [ ] **Step 1: Full local verification**

```bash
rm -f build/recomp/profile/POP3.CD/SAVE/CONFIG00.DAT
.venv/bin/ctest --preset macos -L "nogame|gpu|moltenvk" --output-on-failure
.venv/bin/python tools/test.py && .venv/bin/python tools/test.py --mods && .venv/bin/python tools/test.py --gameplay
POP_GPU_BACKEND=vulkan .venv/bin/python tools/test.py --gameplay
.venv/bin/python tools/format.py && .venv/bin/python tools/check_repo.py
```

Then the manual archive check from Task 5 Step 3 once more on the packaged zip from Task 6 (outside the checkout; also with `POP_GPU_BACKEND=vulkan`).

- [ ] **Step 2: Docs**

PROGRESS.md: row 4 → "Done on branch `release-pipeline` (pending merge)" with the CI and release run ids; "Now": the `latest` release URL, what a person still has to do (Windows/Linux archive runs on hardware; consider a `v0.1.0` tag), environment notes (`--exe`, `--version`, `--probe-layout`, `dist/`, `--publish-tracked`). Append "Execution notes" to this plan. Memory: sub-project 4 done pending merge; all four sub-projects complete.

- [ ] **Step 3: Finish**

Use superpowers:finishing-a-development-branch (base `main`). After the merge, confirm the `Release` workflow ran on `main` and `gh release view latest` lists three assets.
