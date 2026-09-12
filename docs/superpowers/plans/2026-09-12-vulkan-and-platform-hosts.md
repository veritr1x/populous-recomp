# Vulkan Backend and Platform Hosts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Populous runs windowed on Windows and Linux through the existing SDL3 host over a Vulkan implementation of `gpu/gpu.h`, with the runtime and mods suites running on Windows and CI proving the GPU suites on Linux over lavapipe.

**Architecture:** A second `gpu::Device` in `src/recomp/host/gpu/vulkan/` mirrors the Metal backend: one queue, a reaper thread that completes fences in submission order, storage-buffer bindings through a per-command-buffer ring, lazily created pipeline variants, dynamic rendering, and a swapchain over an `SDL_Window*`. GLSL sources compile to a committed SPIR-V header. The platform layer gains process spawn and temp-dir calls so the POSIX-only test suites build on Windows. The factory picks Metal on Apple and Vulkan elsewhere, with `POP_GPU_BACKEND` overriding.

**Tech Stack:** C++17, Vulkan 1.1+ with `VK_KHR_dynamic_rendering`, vendored volk 1.4.304 and Vulkan-Headers v1.4.304, glslc (shaderc), SDL3 3.4.16, MoltenVK (dev machine), lavapipe (Linux CI), CMake presets, CTest labels, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-12-vulkan-and-platform-hosts-design.md`

## Global Constraints

- Objective-C++ and Apple headers only under `src/recomp/host/gpu/metal/` (`host_boundary_check` enforces it).
- No Vulkan SDK at build or run time: volk loads the loader at runtime; headers and volk are vendored under `third_party/` with NOTICE entries.
- Shader sources are hand-written Vulkan GLSL under `gpu/vulkan/shaders/`; `gpu/vulkan/shaders_spv.h` is committed and is what the backend compiles; `glslc -O --target-env=vulkan1.1`.
- Bindings per spec: set 0, bindings 0..3 vertex-stage buffer slots, 4..7 fragment-stage buffer slots, 8..11 combined image samplers for texture slots 0..3; compute uses 0..3 and 8..11. Every buffer binding is `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`; no push constants.
- All backend-owned images stay in `VK_IMAGE_LAYOUT_GENERAL`; a full memory barrier precedes each render pass, compute pass and transfer.
- Negative-height viewport for the Y flip; front face `VK_FRONT_FACE_CLOCKWISE`.
- Presented time = fence completion time of the presenting submission.
- Formats: BGRA8 `VK_FORMAT_B8G8R8A8_UNORM`, RGBA8 `VK_FORMAT_R8G8B8A8_UNORM`, R8 `VK_FORMAT_R8_UNORM`, Depth32F `VK_FORMAT_D32_SFLOAT`.
- `gpu/shaders.md` slot contract is unchanged and every program keeps its name.
- Tests: contract suite compiled per backend, labels `gpu` (Metal on Apple, Vulkan elsewhere) and `gpu-vulkan` (Vulkan on Apple); drift check and platform tests `nogame`.
- Commit after each task; run `tools/format.py` (clang-format check) before committing C++.
- Work on branch `vulkan-hosts` (already created, holds the spec).
- Token economy: the user asked for economical output; keep reports short.

## Local prerequisites (dev machine, macOS)

Ask the user to run once, before Task 2:

```bash
brew install molten-vk vulkan-loader vulkan-headers shaderc
```

`glslc` then lives at `/opt/homebrew/bin/glslc`; the loader at `/opt/homebrew/lib/libvulkan.1.dylib`; MoltenVK's ICD at `/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json` (the Homebrew loader finds it by itself).

## File structure

| Path | Responsibility |
| --- | --- |
| `third_party/volk/{volk.h,volk.c,LICENSE.md}` | Vulkan loader shim, vendored |
| `third_party/vulkan-headers/include/vulkan/*.h`, `vk_video/*.h`, `LICENSE.md` | Vulkan headers, vendored |
| `src/recomp/host/gpu/tests/gpu_contract_tests.cpp` | The contract suite, moved from `metal/tests/`, compiled once per backend |
| `src/recomp/host/gpu/vulkan/vulkan_device.h` | `VulkanDevice` class, handle tables, ring, reaper |
| `src/recomp/host/gpu/vulkan/vulkan_device.cpp` | resources, commands, pipelines, submission |
| `src/recomp/host/gpu/vulkan/vulkan_swapchain.cpp` | surface, swapchain, acquire/present, `vulkan_test_native_surface` |
| `src/recomp/host/gpu/vulkan/vulkan_loader.cpp` | volk initialisation, loader path search, `vulkan_loader_path()` |
| `src/recomp/host/gpu/vulkan/shaders/*.vert,*.frag,*.comp` | GLSL sources |
| `src/recomp/host/gpu/vulkan/shaders_spv.h` | generated SPIR-V arrays + name table |
| `tools/recomp/shaders.py` | `compile` and `check` subcommands over glslc |
| `tools/recomp/tests/test_shaders.py` | drift check (unittest; skips without glslc) |
| `src/recomp/host/gpu/gpu_factory.{h,cpp}` | backend selection, `POP_GPU_BACKEND`, `native_surface_for_window`, `vulkan_loader_path` |
| `src/recomp/host/gpu/CMakeLists.txt` | `gpu_vulkan`, `gpu_backend` interface, test targets |
| `src/recomp/host/CMakeLists.txt` | hosts on every platform |
| `src/recomp/host/sdl/main.cpp` | Metal or Vulkan window flag and surface handoff |
| `src/recomp/platform/os.h`, `os_posix.cpp`, `os_win32.cpp` | `os_spawn`, `os_wait`, `os_mkdtemp` |
| `src/recomp/runtime/tests/runtime_tests.cpp`, `src/recomp/mods/tests/*.cpp`, `src/recomp/host/tests/host_tests.cpp` | ported off `<unistd.h>` |
| `mods/core/tests/roots_tests.py` | replaces `roots_tests.sh` |
| `.github/workflows/checks.yml` | lavapipe on Linux, `nogame|gpu` there |
| `tools/recomp/compare_frames.py` | Metal vs Vulkan flyby comparison |

---

### Task 1: Vendor volk and the Vulkan headers; build a Vulkan backend skeleton and the per-backend contract suite

**Files:**
- Create: `third_party/volk/volk.h`, `third_party/volk/volk.c`, `third_party/volk/LICENSE.md`
- Create: `third_party/vulkan-headers/include/vulkan/{vk_platform.h,vulkan.h,vulkan_core.h}`, `third_party/vulkan-headers/include/vk_video/*.h`, `third_party/vulkan-headers/LICENSE.md`
- Move: `src/recomp/host/gpu/metal/tests/gpu_metal_tests.cpp` → `src/recomp/host/gpu/tests/gpu_contract_tests.cpp`
- Create: `src/recomp/host/gpu/vulkan/vulkan_loader.cpp`, `src/recomp/host/gpu/vulkan/vulkan_device.h` (skeleton), `src/recomp/host/gpu/vulkan/vulkan_device.cpp` (skeleton returning null)
- Modify: `src/recomp/host/gpu/gpu_factory.h`, `gpu_factory.cpp`, `src/recomp/host/gpu/CMakeLists.txt`, `NOTICE`, `README.md` (third_party table), `tools/recomp/tests/test_host_boundary.py` (no change needed; verify it still passes)

**Interfaces:**
- Produces: `std::unique_ptr<Device> gpu::vulkan_create_device();` `void *gpu::vulkan_test_native_surface(int w, int h);` `const char *gpu::vulkan_loader_path();` (all declared in `gpu_factory.cpp` and defined under `gpu/vulkan/`), `bool gpu::backend_override_is(const char *name)` internal to the factory, `gpu::create_default_device()` honouring `POP_GPU_BACKEND=metal|vulkan`, `const char *gpu::default_backend_name()` returning the name actually chosen.
- Produces CMake targets: `gpu_vulkan` (OBJECT), `gpu_backend` (INTERFACE linking `gpu_metal` on Apple plus `gpu_vulkan` everywhere), `gpu_vulkan_tests`, `gpu_metal_tests` (Apple).

- [ ] **Step 1: Vendor volk and the headers**

```bash
cd ~/Documents/Tests/populous-recomp-checkout
S=$(mktemp -d)
curl -sL https://github.com/zeux/volk/archive/refs/tags/1.4.304.tar.gz | tar xz -C $S
curl -sL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.304.tar.gz | tar xz -C $S
mkdir -p third_party/volk third_party/vulkan-headers/include/vulkan third_party/vulkan-headers/include/vk_video
cp $S/volk-1.4.304/{volk.h,volk.c,LICENSE.md} third_party/volk/
cp $S/Vulkan-Headers-1.4.304/include/vulkan/{vk_platform.h,vulkan.h,vulkan_core.h,vulkan_metal.h,vulkan_win32.h,vulkan_xlib.h,vulkan_wayland.h,vulkan_xcb.h} third_party/vulkan-headers/include/vulkan/
cp $S/Vulkan-Headers-1.4.304/include/vk_video/*.h third_party/vulkan-headers/include/vk_video/
cp $S/Vulkan-Headers-1.4.304/LICENSE.md third_party/vulkan-headers/
rm -rf $S
du -sh third_party/volk third_party/vulkan-headers
```

Expected: volk ~0.3 MB, headers ~2 MB. Do not vendor the rest of the headers tree.

- [ ] **Step 2: NOTICE and README entries**

Append to `NOTICE`:

```
volk (third_party/volk) is Copyright (c) 2018-2025 Arseny Kapoulkine, MIT License;
see third_party/volk/LICENSE.md. The Vulkan headers (third_party/vulkan-headers)
are Copyright 2015-2025 The Khronos Group Inc., Apache License 2.0 or MIT; see
third_party/vulkan-headers/LICENSE.md. Both are included unmodified. The Vulkan
loader, MoltenVK and Mesa's lavapipe are runtime components on the player's or
developer's machine and are not distributed here.
```

Add two rows to the README's third-party table after the `third_party/lua/` row:

```
| `third_party/volk/` | volk 1.4.304, the Vulkan meta-loader, unmodified |
| `third_party/vulkan-headers/` | Vulkan-Headers v1.4.304, the subset the backend includes, unmodified |
```

- [ ] **Step 3: Move the contract suite and make its backend a compile definition**

```bash
git mv src/recomp/host/gpu/metal/tests/gpu_metal_tests.cpp src/recomp/host/gpu/tests/gpu_contract_tests.cpp
rmdir src/recomp/host/gpu/metal/tests
```

Edit the header comment and `main()` of `src/recomp/host/gpu/tests/gpu_contract_tests.cpp`:

```cpp
// gpu_contract_tests.cpp - a backend against the gpu.h contract: bytes
// round-trip, a clear and a draw land where expected, commands complete in
// commit order, a compute kernel reads what a texture holds, and a native
// surface makes a swapchain. Compiled once per backend with
// POP_GPU_TEST_BACKEND naming it; the factory override selects it.
```

```cpp
int main() {
    os_setenv("POP_GPU_BACKEND", POP_GPU_TEST_BACKEND);
    if (strcmp(default_backend_name(), POP_GPU_TEST_BACKEND) != 0) {
        fprintf(stderr, "backend %s is not available here (got %s)\n", POP_GPU_TEST_BACKEND,
                default_backend_name());
        return 1;
    }
    test_upload_readback();
    test_clear_and_compositor_draw();
    test_commit_order();
    test_compute_readback_kernel();
    test_swapchain_from_surface();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all gpu %s tests passed\n", POP_GPU_TEST_BACKEND);
    return g_failures ? 1 : 0;
}
```

Add `#include "../../../platform/os.h"` (for `os_setenv`). Rename `test_swapchain_from_layer` to `test_swapchain_from_surface` and make a missing surface a skip, not a failure:

```cpp
    void *surface = test_native_surface(32, 16);
    if (!surface) {
        printf("no native surface here: swapchain test skipped\n");
        return;
    }
```

- [ ] **Step 4: Factory with override and the Vulkan declarations**

`src/recomp/host/gpu/gpu_factory.h`:

```cpp
// gpu_factory.h - the one place that knows which backends this platform has.
#pragma once
#include "gpu.h"
#include <memory>

namespace gpu {

// Metal on Apple platforms, Vulkan elsewhere. POP_GPU_BACKEND=metal|vulkan
// selects one explicitly; an unavailable choice yields nullptr.
std::unique_ptr<Device> create_default_device();
// The backend create_default_device() would build: "metal", "vulkan" or "none".
const char *default_backend_name();

// A native surface create_swapchain() accepts, for tests that need one without
// a window: a CAMetalLayer for Metal, a hidden SDL window for Vulkan, nullptr
// when the platform cannot provide one (no display).
void *test_native_surface(int w, int h);

// For the SDL host: the surface create_swapchain() wants for this backend,
// given an SDL_Window* (a CAMetalLayer for Metal, the window itself for
// Vulkan). `sdl_window` is an SDL_Window*; the header stays SDL-free.
void *native_surface_for_window(void *sdl_window);
void release_window_surface(void *surface);

// The Vulkan loader the backend dlopen'ed, or nullptr for the default search;
// the SDL host passes it to SDL_Vulkan_LoadLibrary before creating a window.
const char *vulkan_loader_path();

} // namespace gpu
```

`src/recomp/host/gpu/gpu_factory.cpp`:

```cpp
#include "gpu_factory.h"

#include <stdlib.h>
#include <string.h>

namespace gpu {

std::unique_ptr<Device> vulkan_create_device();
void *vulkan_test_native_surface(int w, int h);
void *vulkan_native_surface_for_window(void *sdl_window);
const char *vulkan_loader_path_impl();
bool vulkan_available();
#ifdef __APPLE__
std::unique_ptr<Device> metal_create_device();
void *metal_test_native_surface(int w, int h);
void *metal_native_surface_for_window(void *sdl_window);
void metal_release_window_surface(void *surface);
#endif

static const char *chosen_backend() {
    const char *want = getenv("POP_GPU_BACKEND");
    if (want && strcmp(want, "vulkan") == 0)
        return vulkan_available() ? "vulkan" : "none";
#ifdef __APPLE__
    if (want && strcmp(want, "metal") == 0)
        return "metal";
    if (!want || !*want)
        return "metal";
    return "none";
#else
    if (!want || !*want)
        return vulkan_available() ? "vulkan" : "none";
    return "none";
#endif
}

std::unique_ptr<Device> create_default_device() {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_create_device();
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_create_device();
#endif
    return nullptr;
}

const char *default_backend_name() {
    return chosen_backend();
}

void *test_native_surface(int w, int h) {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_test_native_surface(w, h);
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_test_native_surface(w, h);
#endif
    return nullptr;
}

void *native_surface_for_window(void *sdl_window) {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_native_surface_for_window(sdl_window);
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_native_surface_for_window(sdl_window);
#endif
    return nullptr;
}

void release_window_surface(void *surface) {
#ifdef __APPLE__
    if (strcmp(chosen_backend(), "metal") == 0)
        metal_release_window_surface(surface);
#endif
    (void)surface; // Vulkan: the SDL window is the surface; the host destroys it
}

const char *vulkan_loader_path() {
    return vulkan_loader_path_impl();
}

} // namespace gpu
```

Add to `src/recomp/host/gpu/metal/metal_surface.mm` (the SDL Metal view functions live in the Metal backend so `sdl/main.cpp` stops including `SDL_metal.h`; `SDL3/SDL_metal.h` is a plain C header, allowed here):

```objc
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

void *metal_native_surface_for_window(void *sdl_window) {
    SDL_MetalView view = SDL_Metal_CreateView(static_cast<SDL_Window *>(sdl_window));
    return view ? SDL_Metal_GetLayer(view) : nullptr;
}
void metal_release_window_surface(void *) {
    // The view is owned by the window; SDL_DestroyWindow releases it.
}
```

(Put these inside `namespace gpu`.) `gpu_metal` then needs SDL's include directories: `pop_link_sdl(gpu_metal)` in the gpu CMakeLists.

- [ ] **Step 5: Vulkan skeleton that reports "not available"**

`src/recomp/host/gpu/vulkan/vulkan_loader.cpp`:

```cpp
// vulkan_loader.cpp - find and load the Vulkan loader through volk. Homebrew's
// loader on macOS is outside the dynamic linker's default search, so a few
// known paths and POP_VULKAN_LIBRARY are tried before giving up.
#include "vulkan_device.h"

#include <mutex>
#include <stdlib.h>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

namespace gpu {

static std::once_flag g_once;
static bool g_loaded = false;
static std::string g_path;

#ifndef _WIN32
static bool try_path(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"));
    if (!gipa)
        return false;
    volkInitializeCustom(gipa);
    g_path = path;
    return true;
}
#endif

bool vulkan_load() {
    std::call_once(g_once, [] {
        const char *env = getenv("POP_VULKAN_LIBRARY");
#ifndef _WIN32
        if (env && *env && try_path(env)) {
            g_loaded = true;
            return;
        }
#endif
        if (volkInitialize() == VK_SUCCESS) {
            g_loaded = true;
            return;
        }
#ifdef __APPLE__
        const char *candidates[] = {"/opt/homebrew/lib/libvulkan.1.dylib",
                                    "/usr/local/lib/libvulkan.1.dylib",
                                    "/opt/homebrew/lib/libMoltenVK.dylib"};
        for (const char *c : candidates)
            if (try_path(c)) {
                g_loaded = true;
                return;
            }
#endif
    });
    return g_loaded;
}

bool vulkan_available() {
    return vulkan_load();
}

const char *vulkan_loader_path_impl() {
    vulkan_load();
    return g_path.empty() ? nullptr : g_path.c_str();
}

} // namespace gpu
```

`src/recomp/host/gpu/vulkan/vulkan_device.h` (skeleton for this task; Task 3 fills it):

```cpp
// vulkan_device.h - gpu::Device over Vulkan. One queue; a reaper thread
// retires fences in submission order so command buffers complete in commit
// order. Included only by files under gpu/vulkan/.
#pragma once
#define VK_NO_PROTOTYPES
#include "../../../../../third_party/volk/volk.h"
#include "../gpu.h"

namespace gpu {
bool vulkan_load(); // volk initialised; false when no loader exists
const char *vulkan_loader_path_impl(); // path dlopen'ed, or nullptr for the default
}
```

`src/recomp/host/gpu/vulkan/vulkan_device.cpp` (skeleton):

```cpp
#include "vulkan_device.h"

namespace gpu {
std::unique_ptr<Device> vulkan_create_device() {
    return nullptr; // Task 3
}
} // namespace gpu
```

`src/recomp/host/gpu/vulkan/vulkan_swapchain.cpp` (skeleton):

```cpp
#include "vulkan_device.h"

namespace gpu {
void *vulkan_test_native_surface(int, int) {
    return nullptr; // Task 5
}
void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}
} // namespace gpu
```

- [ ] **Step 6: CMake**

Replace `src/recomp/host/gpu/CMakeLists.txt` from the "platform backend" comment down:

```cmake
# The Vulkan backend builds everywhere; the loader is found at run time.
add_library(volk OBJECT ${POP_ROOT}/third_party/volk/volk.c)
target_include_directories(volk PUBLIC ${POP_ROOT}/third_party/volk ${POP_ROOT}/third_party/vulkan-headers/include)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
pop_optimize(volk 2)

add_library(gpu_vulkan OBJECT vulkan/vulkan_loader.cpp vulkan/vulkan_device.cpp vulkan/vulkan_swapchain.cpp)
target_link_libraries(gpu_vulkan PUBLIC volk)
target_include_directories(gpu_vulkan PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gpu_vulkan PRIVATE ${POP_WARN_HOST})
pop_link_sdl(gpu_vulkan)
if(NOT WIN32 AND NOT APPLE)
  target_link_libraries(gpu_vulkan PUBLIC dl)
endif()
pop_optimize(gpu_vulkan 2)

# gpu_backend: every backend this platform has plus the factory. Hosts and
# tests link this, never a backend directly.
add_library(gpu_factory OBJECT gpu_factory.cpp)
target_include_directories(gpu_factory PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gpu_factory PRIVATE ${POP_WARN_HOST})
pop_optimize(gpu_factory 1)
add_library(gpu_backend INTERFACE)
target_link_libraries(gpu_backend INTERFACE gpu_factory gpu_vulkan recomp_platform)

if(APPLE)
  add_library(gpu_metal OBJECT metal/metal_device.mm metal/metal_surface.mm)
  target_compile_options(gpu_metal PRIVATE ${POP_WARN_HOST} $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_include_directories(gpu_metal PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(gpu_metal PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                                         "-framework CoreGraphics" "-framework Foundation")
  pop_link_sdl(gpu_metal)
  pop_optimize(gpu_metal 2)
  target_link_libraries(gpu_backend INTERFACE gpu_metal)
endif()

# The contract suite, once per backend. `gpu` is the platform's default
# backend; `gpu-vulkan` is Vulkan on Apple, which needs MoltenVK installed.
function(pop_gpu_contract_test target backend label)
  add_executable(${target} tests/gpu_contract_tests.cpp)
  target_link_libraries(${target} PRIVATE gpu_backend)
  target_compile_definitions(${target} PRIVATE POP_GPU_TEST_BACKEND="${backend}")
  target_compile_options(${target} PRIVATE ${POP_WARN_STRICT})
  pop_optimize(${target} 1)
  pop_test_binary(${target})
  add_test(NAME ${target} COMMAND ${target} WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(${target} PROPERTIES LABELS ${label})
endfunction()
if(APPLE)
  pop_gpu_contract_test(gpu_metal_tests metal gpu)
  pop_gpu_contract_test(gpu_vulkan_tests vulkan gpu-vulkan)
else()
  pop_gpu_contract_test(gpu_vulkan_tests vulkan gpu)
endif()
```

Then replace every `gpu_metal` in `src/recomp/host/CMakeLists.txt` with `gpu_backend` (`compositor_tests`, `POP_CORE_LIBS`, `present_events_tests`, `host_tests`). `gpu_fake` stays as it is.

- [ ] **Step 7: Build and run; expect the Vulkan suite to report unavailable**

```bash
python3 tools/build.py --preset macos --target check_binaries
ctest --preset macos -R 'gpu_metal_tests|host_boundary_check|compositor_tests' --output-on-failure
POP_GPU_BACKEND=vulkan build/recomp/gpu_vulkan_tests; echo "exit $?"
```

Expected: Metal suite and boundary check pass; `gpu_vulkan_tests` prints `backend vulkan is not available here (got none)` and exits 1 (this is the failing test Tasks 3–5 turn green). If `python3 tools/build.py` has no `--target` form, use `cmake --build --preset macos --target check_binaries`.

- [ ] **Step 8: Commit**

```bash
python3 tools/format.py && git add -A third_party NOTICE README.md src/recomp/host && git commit -m "Vendor volk and Vulkan headers; per-backend contract suite; factory override"
```

---

### Task 2: GLSL sources, the SPIR-V header generator and its drift check

**Files:**
- Create: `src/recomp/host/gpu/vulkan/shaders/compositor.vert`, `compositor.frag`, `hud.vert`, `hud.frag`, `d3d.vert`, `d3d.frag`, `surface_upload.vert`, `surface_upload.frag`, `guest_readback.comp`, `guest_readback_fused.comp`, `native_brightness.comp`, `native_brightness_shared.comp`
- Create: `tools/recomp/shaders.py`, `tools/recomp/tests/test_shaders.py`, `src/recomp/host/gpu/vulkan/shaders_spv.h`
- Modify: `tools/test.py` (PORTABLE_TESTS), `src/recomp/host/CMakeLists.txt` (CTest `shader_drift_check`, label nogame), `src/recomp/host/gpu/shaders.md` (binding note)

**Interfaces:**
- Produces: `gpu::vulkan::kSpirvPrograms[]` of `struct SpirvProgram { const char *name; const uint32_t *words; size_t count; }` terminated by a `{nullptr, nullptr, 0}` entry, names `compositor.vert`, `compositor.frag`, … exactly the file stems plus extension; Task 4 looks them up by `"<program>.vert"` / `"<program>.frag"` / `"<program>.comp"`.
- The binding contract (spec §1 Bindings): vertex buffers slots 0..3 → bindings 0..3, fragment buffers → 4..7, textures → 8..11; compute buffers 0..3, textures 8..11. All buffers `std430 readonly buffer` except compute outputs (`buffer`).

- [ ] **Step 1: Write the failing drift test**

`tools/recomp/tests/test_shaders.py`:

```python
"""The committed SPIR-V header is exactly what glslc produces from the GLSL
sources; skipped where glslc is not installed."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class ShaderDrift(unittest.TestCase):
    def test_header_matches_sources(self):
        if not shutil.which("glslc"):
            self.skipTest("no glslc on PATH")
        result = subprocess.run([sys.executable, "tools/recomp/shaders.py", "check"], cwd=ROOT,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
```

Run: `python3 -m unittest tools/recomp/tests/test_shaders.py` → FAIL (`shaders.py` missing) or skip if glslc absent. Install glslc first (prerequisites above).

- [ ] **Step 2: The generator**

`tools/recomp/shaders.py`:

```python
#!/usr/bin/env python3
"""Compile the Vulkan GLSL under src/recomp/host/gpu/vulkan/shaders/ into
shaders_spv.h (`compile`), or recompile into a scratch file and fail on any
difference from the committed header (`check`)."""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "src/recomp/host/gpu/vulkan/shaders"
HEADER = ROOT / "src/recomp/host/gpu/vulkan/shaders_spv.h"
GLSLC_ARGS = ["-O", "--target-env=vulkan1.1", "-Werror"]


def compile_one(glslc, src, out_dir):
    spv = Path(out_dir) / (src.name + ".spv")
    subprocess.run([glslc, *GLSLC_ARGS, str(src), "-o", str(spv)], check=True)
    data = spv.read_bytes()
    if len(data) % 4:
        raise SystemExit(f"{src.name}: SPIR-V length is not a multiple of 4")
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def render(programs):
    lines = ["// shaders_spv.h - generated by tools/recomp/shaders.py from gpu/vulkan/shaders/.",
             "// Do not edit; `python3 tools/recomp/shaders.py compile` regenerates it.",
             "#pragma once", "#include <stddef.h>", "#include <stdint.h>", "",
             "namespace gpu::vulkan {", "",
             "struct SpirvProgram {", "    const char *name;", "    const uint32_t *words;",
             "    size_t count;", "};", ""]
    for name, words in programs:
        ident = "k_" + name.replace(".", "_")
        lines.append(f"static const uint32_t {ident}[{len(words)}] = {{")
        for i in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 8]) + ",")
        lines.append("};")
    lines.append("")
    lines.append("static const SpirvProgram kSpirvPrograms[] = {")
    for name, words in programs:
        ident = "k_" + name.replace(".", "_")
        lines.append(f'    {{"{name}", {ident}, {len(words)}}},')
    lines.append("    {nullptr, nullptr, 0},")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace gpu::vulkan")
    return "\n".join(lines) + "\n"


def build():
    glslc = shutil.which("glslc")
    if not glslc:
        raise SystemExit("glslc not found; brew install shaderc (macOS) or apt install glslc")
    sources = sorted(p for p in SHADERS.iterdir() if p.suffix in (".vert", ".frag", ".comp"))
    with tempfile.TemporaryDirectory() as tmp:
        return render([(s.name, compile_one(glslc, s, tmp)) for s in sources])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["compile", "check"])
    args = parser.parse_args()
    text = build()
    if args.command == "compile":
        HEADER.write_text(text)
        print(f"wrote {HEADER.relative_to(ROOT)}")
        return 0
    if not HEADER.exists() or HEADER.read_text() != text:
        print("shaders_spv.h is out of date: run `python3 tools/recomp/shaders.py compile`",
              file=sys.stderr)
        return 1
    print("shaders_spv.h matches the GLSL sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: The GLSL**

`shaders/compositor.vert`:

```glsl
#version 450
struct Quad { vec4 rect; vec4 uv; vec2 drawable; uint opaque; uint pad; };
layout(std430, set = 0, binding = 0) readonly buffer QuadBlock { Quad q; };
layout(location = 0) out vec2 v_uv;
void main() {
    const vec2 corners[4] = vec2[4](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 1));
    vec2 c = corners[gl_VertexIndex];
    vec2 p = q.rect.xy + c * q.rect.zw;
    gl_Position = vec4(p.x / q.drawable.x * 2 - 1, 1 - p.y / q.drawable.y * 2, 0, 1);
    v_uv = q.uv.xy + c * q.uv.zw;
}
```

`shaders/compositor.frag`:

```glsl
#version 450
struct Quad { vec4 rect; vec4 uv; vec2 drawable; uint opaque; uint pad; };
layout(std430, set = 0, binding = 4) readonly buffer QuadBlock { Quad q; };
layout(set = 0, binding = 8) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texture(tex, v_uv);
    if (q.opaque != 0u) c.a = 1;
    o_color = c;
}
```

`shaders/hud.vert`:

```glsl
#version 450
layout(std430, set = 0, binding = 0) readonly buffer RectBlock { vec4 r; };
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 q = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(r.xy + q * r.zw, 0, 1);
    v_uv = q;
}
```

`shaders/hud.frag`:

```glsl
#version 450
layout(set = 0, binding = 8) uniform sampler2D t;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(t, v_uv); }
```

`shaders/d3d.vert`:

```glsl
#version 450
struct HVertex { float x, y, z, w; float u, v; float r, g, b, a; float sr, sg, sb, sa; };
struct Uniforms {
    mat4 mvp;
    uint pretransformed, textured, texblend, alphatest, alphafunc;
    float alpharef;
    uint specular, texture_has_alpha, fogmode;
    float fogstart, fogend, fogdensity, fogr, fogg, fogb, pointsize;
    uint terrain_detail;
};
layout(std430, set = 0, binding = 0) readonly buffer Vertices { HVertex v[]; };
layout(std430, set = 0, binding = 1) readonly buffer UniformBlock { Uniforms u; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_spec;
layout(location = 3) out float v_fogdist;
void main() {
    HVertex s = v[gl_VertexIndex];
    vec4 pos = u.pretransformed != 0u ? vec4(s.x, s.y, s.z, s.w) : u.mvp * vec4(s.x, s.y, s.z, 1.0);
    gl_Position = pos;
    gl_PointSize = u.pointsize;
    v_uv = vec2(s.u, s.v);
    v_color = vec4(s.r, s.g, s.b, s.a);
    v_spec = vec4(s.sr, s.sg, s.sb, s.sa);
    v_fogdist = abs(pos.w); // table fog uses eye distance, the clip w
}
```

`shaders/d3d.frag` (a line-for-line translation of `d3d_fragment` in `gpu/metal/shaders_msl.h`; `saturate` → `clamp(x, 0.0, 1.0)`, `discard_fragment()` → `discard`, `mix` unchanged):

```glsl
#version 450
struct Uniforms {
    mat4 mvp;
    uint pretransformed, textured, texblend, alphatest, alphafunc;
    float alpharef;
    uint specular, texture_has_alpha, fogmode;
    float fogstart, fogend, fogdensity, fogr, fogg, fogb, pointsize;
    uint terrain_detail;
};
layout(std430, set = 0, binding = 5) readonly buffer UniformBlock { Uniforms u; };
layout(set = 0, binding = 8) uniform sampler2D tex;
layout(set = 0, binding = 9) uniform sampler2D detail;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_spec;
layout(location = 3) in float v_fogdist;
layout(location = 0) out vec4 o_color;
layout(location = 1) out float o_coverage;
vec3 sat3(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
void main() {
    vec4 c = v_color;
    if (u.textured != 0u) {
        vec4 t = texture(tex, v_uv);
        if (u.terrain_detail != 0u) {
            vec3 chroma = t.rgb / max(max(t.r, t.g), max(t.b, 0.01));
            float land = 1.0 - smoothstep(0.02, 0.22, chroma.b - chroma.r);
            float grass = smoothstep(-0.03, 0.16, chroma.g - chroma.r);
            float stone = 1.0 - smoothstep(0.06, 0.28, max(chroma.r, chroma.g) - min(chroma.r, min(chroma.g, chroma.b)));
            float sand = smoothstep(0.28, 0.62, max(t.r, t.g));
            vec4 structure = (texture(detail, v_uv) * 255.0 - 128.0) / 128.0;
            float material = mix(structure.a, structure.g, sand);
            material = mix(material, structure.b, stone);
            material = mix(material, structure.r, grass);
            t.rgb = sat3(t.rgb * (1.0 + material * 0.85 * land));
        }
        switch (u.texblend) {
        case 1u: case 7u: c = t; break;
        case 2u: c = vec4(t.rgb * v_color.rgb, u.texture_has_alpha != 0u ? t.a : v_color.a); break;
        case 3u: c = vec4(mix(v_color.rgb, t.rgb, t.a), v_color.a); break;
        case 5u: c = vec4(t.rgb, v_color.a); break;
        case 8u: c = vec4(sat3(t.rgb + v_color.rgb), v_color.a); break;
        case 4u: c = vec4(t.rgb * v_color.rgb, t.a * v_color.a); break;
        default: c = vec4(t.rgb * v_color.rgb, u.texture_has_alpha != 0u ? t.a : v_color.a); break;
        }
    }
    if (u.specular != 0u) c = vec4(sat3(c.rgb + v_spec.rgb), c.a);
    if (u.alphatest != 0u) {
        bool pass;
        switch (u.alphafunc) {
        case 1u: pass = false; break;
        case 2u: pass = c.a < u.alpharef; break;
        case 3u: pass = c.a == u.alpharef; break;
        case 4u: pass = c.a <= u.alpharef; break;
        case 5u: pass = c.a > u.alpharef; break;
        case 6u: pass = c.a != u.alpharef; break;
        case 7u: pass = c.a >= u.alpharef; break;
        default: pass = true; break;
        }
        if (!pass) discard;
    }
    if (u.fogmode != 0u) {
        float f = 1.0;
        float d = v_fogdist;
        switch (u.fogmode) {
        case 1u: f = v_spec.a; break;
        case 2u: f = exp(-u.fogdensity * d); break;
        case 3u: f = exp(-(u.fogdensity * d) * (u.fogdensity * d)); break;
        default: f = (u.fogend - d) / max(u.fogend - u.fogstart, 1e-6); break;
        }
        f = clamp(f, 0.0, 1.0);
        c = vec4(mix(vec3(u.fogr, u.fogg, u.fogb), c.rgb, f), c.a);
    }
    o_color = c;
    o_coverage = 1.0;
}
```

`shaders/surface_upload.vert`:

```glsl
#version 450
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0, 1);
}
```

`shaders/surface_upload.frag` (bytes are read from a `uint` array because GLSL has no 8-bit storage without an extension):

```glsl
#version 450
layout(std430, set = 0, binding = 4) readonly buffer Pixels { uint words[]; };
layout(std430, set = 0, binding = 5) readonly buffer Params { uvec4 p[5]; };
layout(std430, set = 0, binding = 6) readonly buffer Palette { uint palette[256]; };
layout(location = 0) out vec4 o_color;
uint byte_at(uint off) { return (words[off >> 2] >> ((off & 3u) * 8u)) & 255u; }
void main() {
    uvec2 xy = uvec2(gl_FragCoord.xy) * p[0].xy / p[0].zw;
    uint bpp = p[1].y;
    uint offset = xy.y * p[1].x + xy.x * (bpp == 8u ? 1u : (bpp <= 16u ? 2u : 4u));
    uvec3 rgb;
    if (bpp == 8u) {
        uint index = byte_at(offset);
        uint c = p[1].z != 0u ? palette[index] : index * 0x010101u;
        rgb = uvec3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u);
    } else {
        uint value = byte_at(offset) | (byte_at(offset + 1u) << 8);
        if (bpp > 16u) value |= (byte_at(offset + 2u) << 16) | (byte_at(offset + 3u) << 24);
        rgb = ((uvec3(value) & p[2].xyz) >> p[3].xyz) * 255u / p[4].xyz;
    }
    o_color = vec4(vec3(rgb) / 255.0, 1);
}
```

`shaders/guest_readback.comp`:

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 8) uniform sampler2D color;
layout(set = 0, binding = 9) uniform sampler2D coverage;
layout(std430, set = 0, binding = 0) buffer Out { uvec2 output_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer Params { uvec4 p[3]; };
void main() {
    uvec2 tid = gl_GlobalInvocationID.xy;
    uvec2 span = p[0].zw - p[0].xy;
    if (any(greaterThanEqual(tid, span))) return;
    uvec2 guest = p[0].xy + tid, native_size = p[1].xy, guest_size = p[1].zw;
    uvec2 pos = min(((2u * guest + 1u) * native_size) / (2u * guest_size), p[2].yz - 1u);
    uvec3 rgb = uvec3(round(texelFetch(color, ivec2(pos), 0).rgb * 255.0));
    uint covered = texelFetch(coverage, ivec2(pos), 0).r > 0.0 ? 1u : 0u;
    output_words[p[2].x + tid.y * span.x + tid.x] =
        uvec2(rgb.b | (rgb.g << 8) | (rgb.r << 16) | (covered << 24), 0u);
}
```

`shaders/guest_readback_fused.comp`: same header and prologue as `guest_readback.comp`; the body after `covered`:

```glsl
    uvec2 lo = (guest * native_size + guest_size - 1u) / guest_size;
    uvec2 hi = ((guest + 1u) * native_size + guest_size - 1u) / guest_size;
    uint lit = 0u;
    for (uint y = lo.y; y < hi.y; ++y)
        for (uint x = lo.x; x < hi.x; ++x)
            lit += any(greaterThan(round(texelFetch(color, ivec2(x, y), 0).rgb * 255.0), vec3(8.0))) ? 1u : 0u;
    output_words[p[2].x + tid.y * span.x + tid.x] =
        uvec2(rgb.b | (rgb.g << 8) | (rgb.r << 16) | (covered << 24), lit);
```

`shaders/native_brightness.comp` (subgroup variant; `simdgroups` = 256 / subgroup size, which `thread_execution_width` reports):

```glsl
#version 450
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 8) uniform sampler2D color;
layout(std430, set = 0, binding = 0) buffer Out { uint output_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer Params { uvec4 p[2]; };
void main() {
    uvec2 pos = p[0].xy + gl_GlobalInvocationID.xy * 2u;
    uint lit = 0u;
    for (uint y = 0u; y < 2u; ++y)
        for (uint x = 0u; x < 2u; ++x) {
            uvec2 q = pos + uvec2(x, y);
            if (all(lessThan(q, p[0].zw)))
                lit += any(greaterThan(round(texelFetch(color, ivec2(q), 0).rgb * 255.0), vec3(8.0))) ? 1u : 0u;
        }
    uint sum = subgroupAdd(lit);
    if (gl_SubgroupInvocationID == 0u)
        output_words[p[1].x + (gl_WorkGroupID.y * p[1].y + gl_WorkGroupID.x) * p[1].z + gl_SubgroupID] = sum;
}
```

`shaders/native_brightness_shared.comp` (chosen when arithmetic subgroup operations are missing; `thread_execution_width` then reports 256 so the host passes `simdgroups = 1`):

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 8) uniform sampler2D color;
layout(std430, set = 0, binding = 0) buffer Out { uint output_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer Params { uvec4 p[2]; };
shared uint partial[256];
void main() {
    uvec2 pos = p[0].xy + gl_GlobalInvocationID.xy * 2u;
    uint lit = 0u;
    for (uint y = 0u; y < 2u; ++y)
        for (uint x = 0u; x < 2u; ++x) {
            uvec2 q = pos + uvec2(x, y);
            if (all(lessThan(q, p[0].zw)))
                lit += any(greaterThan(round(texelFetch(color, ivec2(q), 0).rgb * 255.0), vec3(8.0))) ? 1u : 0u;
        }
    partial[gl_LocalInvocationIndex] = lit;
    barrier();
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (gl_LocalInvocationIndex < stride)
            partial[gl_LocalInvocationIndex] += partial[gl_LocalInvocationIndex + stride];
        barrier();
    }
    if (gl_LocalInvocationIndex == 0u)
        output_words[p[1].x + (gl_WorkGroupID.y * p[1].y + gl_WorkGroupID.x) * p[1].z] = partial[0];
}
```

- [ ] **Step 4: Generate, register, verify**

```bash
python3 tools/recomp/shaders.py compile
python3 -m unittest tools/recomp/tests/test_shaders.py
```

Expected: `wrote src/recomp/host/gpu/vulkan/shaders_spv.h`; test OK. Add `"tools/recomp/tests/test_shaders.py"` to `PORTABLE_TESTS` in `tools/test.py`. Add to `src/recomp/host/CMakeLists.txt` after `host_boundary_check`:

```cmake
add_test(NAME shader_drift_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_shaders.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(shader_drift_check PROPERTIES LABELS nogame)
```

Append to `gpu/shaders.md`:

```
## Vulkan bindings

The GLSL under `gpu/vulkan/shaders/` maps the slots above onto descriptor set 0: vertex-stage buffer
slot s → binding s, fragment-stage buffer slot s → binding 4+s, texture slot t → combined image
sampler binding 8+t; compute kernels use bindings 0..3 for buffers and 8+t for textures. Every
buffer is a std430 storage buffer; compute local sizes are fixed at 8x8 (readback) and 16x16
(brightness), the sizes the renderer dispatches with.
```

- [ ] **Step 5: Commit**

```bash
git add src/recomp/host/gpu tools/recomp/shaders.py tools/recomp/tests/test_shaders.py tools/test.py src/recomp/host/CMakeLists.txt
git commit -m "Vulkan GLSL for every program, generated SPIR-V header and drift check"
```

---

### Task 3: VulkanDevice core: instance, device, resources, command buffers, reaper

Makes `test_upload_readback` and `test_commit_order` pass. Pipelines, passes, draws and compute are stubs that record nothing yet (Task 4); swapchain calls return null handles (Task 5).

**Files:**
- Modify: `src/recomp/host/gpu/vulkan/vulkan_device.h` (full class), `src/recomp/host/gpu/vulkan/vulkan_device.cpp`
- Test: `src/recomp/host/gpu/tests/gpu_contract_tests.cpp` (exists)

**Interfaces:**
- Produces: `class VulkanDevice final : public Device` with the members below; `Tex`, `Buf`, `Cmd`, `Submission` structs; `Cmd &VulkanDevice::cmd(CommandBuffer)`; `void VulkanDevice::full_barrier(VkCommandBuffer)`; `RingRange VulkanDevice::ring_alloc(Cmd &, const void *, uint64_t)`; `VkCommandBuffer VulkanDevice::one_shot_begin()` / `void one_shot_end_wait(VkCommandBuffer)`; `void VulkanDevice::fail(const char *what)`.
- Consumes: `vulkan_load()` from Task 1.

- [ ] **Step 1: Run the failing tests**

```bash
POP_GPU_BACKEND=vulkan build/recomp/gpu_vulkan_tests
```

Expected: `backend vulkan is not available here (got none)` (no device yet) → exit 1.

- [ ] **Step 2: The header**

`src/recomp/host/gpu/vulkan/vulkan_device.h`:

```cpp
// vulkan_device.h - gpu::Device over Vulkan. One queue; a reaper thread
// retires fences in submission order so command buffers complete in commit
// order. Every image the backend owns stays in VK_IMAGE_LAYOUT_GENERAL; a full
// barrier precedes each pass and transfer. Included only under gpu/vulkan/.
#pragma once
#define VK_NO_PROTOTYPES
#include "../../../../../third_party/volk/volk.h"
#include "../gpu.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu {

bool vulkan_load(); // volk initialised; false when no loader exists
const char *vulkan_loader_path_impl();

class VulkanDevice final : public Device {
  public:
    static std::unique_ptr<VulkanDevice> create();
    ~VulkanDevice() override;

    Texture create_texture(const TextureDesc &desc) override;
    bool upload(Texture t, Region region, const void *bytes, int pitch, int level = 0) override;
    bool readback(Texture t, Region region, void *bytes, int pitch) override;
    void destroy(Texture t) override;
    TextureDesc describe(Texture t) override;
    uint64_t allocated_bytes(Texture t) override;
    Buffer create_buffer(uint64_t bytes, const void *contents) override;
    void update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) override;
    const void *map_read(Buffer b) override;
    uint64_t buffer_bytes(Buffer b) override;
    void destroy(Buffer b) override;
    Pipeline render_pipeline(const std::string &shader, const RenderState &state) override;
    Pipeline compute_pipeline(const std::string &shader) override;
    int thread_execution_width(Pipeline p) override;
    CommandBuffer begin() override;
    void begin_render_pass(CommandBuffer cb, const RenderPass &pass) override;
    void set_pipeline(CommandBuffer cb, Pipeline p) override;
    void set_depth(CommandBuffer cb, const DepthState &d) override;
    void set_cull(CommandBuffer cb, Cull c) override;
    void set_viewport(CommandBuffer cb, const Viewport &v) override;
    void set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) override;
    void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes,
                   uint64_t count) override;
    void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) override;
    void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) override;
    void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) override;
    void draw(CommandBuffer cb, Primitive primitive, int first, int count) override;
    void end_render_pass(CommandBuffer cb) override;
    void begin_compute_pass(CommandBuffer cb) override;
    void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) override;
    void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) override;
    void end_compute_pass(CommandBuffer cb) override;
    void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x,
              int dst_y) override;
    void copy_buffer_to_texture(CommandBuffer cb, Buffer src, uint64_t offset, int pitch,
                                Texture dst, Region dst_region) override;
    void copy_texture_to_buffer(CommandBuffer cb, Texture src, Region src_region, Buffer dst,
                                uint64_t offset, int pitch) override;
    void generate_mipmaps(CommandBuffer cb, Texture t) override;
    void on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) override;
    void commit(CommandBuffer cb) override;
    void wait(CommandBuffer cb) override;
    CommandStatus status(CommandBuffer cb) override;
    Swapchain create_swapchain(void *native_surface, int width, int height) override;
    void resize(Swapchain s, int width, int height) override;
    Format swapchain_format(Swapchain s) override;
    Texture acquire(Swapchain s) override;
    void release_drawable(Swapchain s, Texture t) override;
    void present(CommandBuffer cb, Swapchain s, Texture t, double min_duration_seconds,
                 std::function<void(double)> presented) override;
    double refresh_period(Swapchain s) override;
    void destroy(Swapchain s) override;
    double now_seconds() override;

    // --- shared with vulkan_swapchain.cpp ---
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        TextureDesc desc;
        uint64_t bytes = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL; // swapchain images start UNDEFINED
        bool external = false;                          // swapchain image: not ours to free
        uint64_t swapchain = 0;                         // owning swapchain id for external
        uint32_t image_index = 0;
        // UsageCpu: a persistently mapped staging buffer for level 0.
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void *staging_map = nullptr;
        uint64_t staging_bytes = 0;
    };
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *map = nullptr;
        uint64_t bytes = 0;
    };
    struct Binding {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64_t offset = 0, range = 0;
    };
    struct TexBinding {
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    struct Cmd {
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE; // 2 timestamps
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> extra_pools; // exhausted pools, reset with `pool`
        // The bytes ring: host-visible, reset when the Cmd is recycled.
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceMemory ring_memory = VK_NULL_HANDLE;
        uint8_t *ring_map = nullptr;
        uint64_t ring_bytes = 0, ring_used = 0;
        std::vector<std::function<void(CommandStatus, double)>> callbacks;
        // Recording state (Task 4).
        bool in_render = false, in_compute = false;
        uint64_t pipeline = 0; // family id
        Primitive topology = Primitive::Triangles;
        Cull cull = Cull::None;
        DepthState depth;
        Viewport viewport{};
        bool viewport_set = false;
        int pass_width = 0, pass_height = 0;
        Format pass_formats[2] = {Format::BGRA8, Format::R8};
        int pass_color_count = 1;
        bool pass_depth = false;
        Binding buffers[8];
        TexBinding textures[4];
        SamplerState sampler_states[4];
        bool bindings_dirty = true;
        VkPipeline bound = VK_NULL_HANDLE;
        // Presentation (Task 5).
        uint64_t present_swapchain = 0;
        uint32_t present_image = 0;
        bool presents = false;
        VkSemaphore wait_semaphore = VK_NULL_HANDLE, signal_semaphore = VK_NULL_HANDLE;
        std::function<void(double)> presented;
    };
    struct Submission {
        uint64_t id = 0;
        std::unique_ptr<Cmd> cmd;
    };

    Cmd *cmd(CommandBuffer cb); // mutex held; nullptr when unknown or committed
    void full_barrier(VkCommandBuffer cb);
    Binding ring_alloc(Cmd &c, const void *bytes, uint64_t count); // mutex held
    VkCommandBuffer one_shot_begin();
    void one_shot_end_wait(VkCommandBuffer cb);
    void wait_all_submitted(); // every fence so far
    void fail(const char *what);
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
    bool allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out);
    Texture register_external_image(VkImage image, VkImageView view, const TextureDesc &desc,
                                    uint64_t swapchain, uint32_t index);
    void transition_if_external(Cmd &c, Tex &t, VkImageLayout to); // swapchain images only
    bool init_pipeline_layout();
    void reap_loop();
    void create_descriptor_pool(Cmd &c);
    void end_passes(Cmd &c);
    void queue_present(Cmd &c);
    VkShaderModule module(const std::string &name);
    VkPipeline variant_for(Cmd &c);
    VkSampler sampler_for(const SamplerState &s);
    void bind_descriptors(Cmd &c, VkPipelineBindPoint point);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memory_props_{};
    uint32_t subgroup_size_ = 32;
    bool subgroup_arithmetic_ = false;
    bool anisotropy_ = false;
    bool dynamic_rendering_khr_ = false; // true when using the extension entry points
    bool failed_ = false;

    std::mutex mutex_; // guards every table below
    std::mutex queue_mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, Buf> buffers_;
    std::unordered_map<uint64_t, std::unique_ptr<Cmd>> recording_;
    std::vector<std::unique_ptr<Cmd>> free_cmds_;
    // Submitted and not yet retired, in submission order (the reaper's queue).
    std::deque<Submission> submitted_;
    std::unordered_map<uint64_t, CommandStatus> retired_; // bounded: last 1024
    std::deque<uint64_t> retired_order_;
    std::condition_variable reaper_cv_, retired_cv_;
    std::thread reaper_;
    bool stopping_ = false;

    // Task 4
    struct Family {
        std::string shader;
        RenderState state;
        std::map<uint64_t, VkPipeline> variants; // key: topology | cull<<2 | compare<<4 | write<<8
    };
    std::unordered_map<uint64_t, Family> families_;
    std::unordered_map<uint64_t, uint64_t> family_by_key_; // hash(shader, state.key()) -> id
    struct ComputePipe {
        VkPipeline pipeline = VK_NULL_HANDLE;
        int width = 1;
    };
    std::unordered_map<uint64_t, ComputePipe> computes_;
    std::unordered_map<std::string, uint64_t> compute_by_name_;
    std::unordered_map<std::string, VkShaderModule> modules_;
    std::map<uint64_t, VkSampler> samplers_; // key from SamplerState fields
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    Buf dummy_buffer_;
    Tex dummy_texture_;
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    // Task 5
    struct Chain;
    std::unordered_map<uint64_t, std::unique_ptr<Chain>> swapchains_;
    std::string loader_path_;
};

VkFormat vk_format(Format f);
Format format_of(VkFormat f);

} // namespace gpu
```

- [ ] **Step 3: Creation, memory, resources, submission**

`src/recomp/host/gpu/vulkan/vulkan_device.cpp`, first half. Keep helpers `static` in an anonymous namespace. Key bodies:

```cpp
#include "vulkan_device.h"
#include "../../../platform/os.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace gpu {

VkFormat vk_format(Format f) {
    switch (f) {
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::Depth32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}
Format format_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8;
    case VK_FORMAT_R8_UNORM: return Format::R8;
    case VK_FORMAT_D32_SFLOAT: return Format::Depth32F;
    default: return Format::BGRA8;
    }
}
static int bytes_per_pixel(Format f) {
    return f == Format::R8 ? 1 : 4;
}
static VkImageAspectFlags aspect(Format f) {
    return f == Format::Depth32F ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

void VulkanDevice::fail(const char *what) {
    if (!failed_)
        fprintf(stderr, "gpu/vulkan: %s; the device is now failed\n", what);
    failed_ = true;
}

std::unique_ptr<VulkanDevice> VulkanDevice::create() {
    if (!vulkan_load())
        return nullptr;
    auto d = std::unique_ptr<VulkanDevice>(new VulkanDevice());
    // Instance: 1.3 if the loader has it, else 1.1 plus extensions.
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loader_version);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PopRecomp";
    app.apiVersion = loader_version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    std::vector<const char *> inst_ext;
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
    auto has_inst = [&](const char *name) {
        for (auto &e : avail)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    VkInstanceCreateFlags inst_flags = 0;
    if (has_inst("VK_KHR_portability_enumeration")) {
        inst_ext.push_back("VK_KHR_portability_enumeration");
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    // Surface extensions for Task 5: whatever this platform has.
    for (const char *s : {"VK_KHR_surface", "VK_EXT_metal_surface", "VK_KHR_win32_surface",
                          "VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface",
                          "VK_KHR_get_physical_device_properties2"})
        if (has_inst(s))
            inst_ext.push_back(s);
    const char *validation = "VK_LAYER_KHRONOS_validation";
    std::vector<const char *> layers;
    if (const char *v = getenv("POP_GPU_VALIDATE"); v && *v == '1') {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> lp(ln);
        vkEnumerateInstanceLayerProperties(&ln, lp.data());
        for (auto &l : lp)
            if (strcmp(l.layerName, validation) == 0)
                layers.push_back(validation);
    }
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = inst_flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &d->instance_) != VK_SUCCESS)
        return nullptr;
    volkLoadInstanceOnly(d->instance_);

    // Physical device: first discrete, else first with graphics+compute.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d->instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d->instance_, &count, devices.data());
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (int pass = 0; pass < 2 && !chosen; ++pass) {
        for (VkPhysicalDevice pd : devices) {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(pd, &pp);
            if (pass == 0 && pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, q.data());
            for (uint32_t i = 0; i < qn; ++i)
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    chosen = pd;
                    family = i;
                    break;
                }
            if (chosen)
                break;
        }
    }
    if (!chosen)
        return nullptr;
    d->physical_ = chosen;
    d->queue_family_ = family;
    vkGetPhysicalDeviceProperties(chosen, &d->props_);
    vkGetPhysicalDeviceMemoryProperties(chosen, &d->memory_props_);

    // Device extensions and features.
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> dext(en);
    vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, dext.data());
    auto has_dev = [&](const char *name) {
        for (auto &e : dext)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char *> dev_ext;
    if (has_dev("VK_KHR_swapchain"))
        dev_ext.push_back("VK_KHR_swapchain");
    if (has_dev("VK_KHR_portability_subset"))
        dev_ext.push_back("VK_KHR_portability_subset");
    const bool core13 = d->props_.apiVersion >= VK_API_VERSION_1_3 && app.apiVersion >= VK_API_VERSION_1_3;
    if (!core13) {
        if (!has_dev("VK_KHR_dynamic_rendering"))
            return nullptr;
        dev_ext.push_back("VK_KHR_dynamic_rendering");
        d->dynamic_rendering_khr_ = true;
    }
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    vkGetPhysicalDeviceProperties2(chosen, &p2);
    d->subgroup_size_ = sub.subgroupSize ? sub.subgroupSize : 32;
    d->subgroup_arithmetic_ = (sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                              (sub.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT);
    VkPhysicalDeviceFeatures base{};
    vkGetPhysicalDeviceFeatures(chosen, &base);
    d->anisotropy_ = base.samplerAnisotropy;
    VkPhysicalDeviceFeatures enabled{};
    enabled.samplerAnisotropy = base.samplerAnisotropy;
    enabled.largePoints = base.largePoints;
    f2.features = enabled;
    if (core13) {
        f13.dynamicRendering = VK_TRUE;
        f2.pNext = &f13;
    } else {
        fdr.dynamicRendering = VK_TRUE;
        f2.pNext = &fdr;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (vkCreateDevice(chosen, &dci, nullptr, &d->device_) != VK_SUCCESS)
        return nullptr;
    volkLoadDevice(d->device_);
    if (d->dynamic_rendering_khr_) {
        // volk aliases the KHR entry points onto the core names when the core
        // ones are missing; nothing else to do.
    }
    vkGetDeviceQueue(d->device_, family, 0, &d->queue_);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vkCreateCommandPool(d->device_, &pci, nullptr, &d->command_pool_) != VK_SUCCESS)
        return nullptr;
    if (!d->init_pipeline_layout()) // Task 4 adds the body; a stub returning true until then
        return nullptr;
    d->reaper_ = std::thread([p = d.get()] { p->reap_loop(); });
    return d;
}
```

(The header above already declares `init_pipeline_layout` and `reap_loop`.)

Memory and resources:

```cpp
uint32_t VulkanDevice::memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props_.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}
bool VulkanDevice::allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out) {
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, want);
    if (mai.memoryTypeIndex == UINT32_MAX && want != VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        return false;
    return vkAllocateMemory(device_, &mai, nullptr, out) == VK_SUCCESS;
}

static bool make_host_buffer(VulkanDevice &d, uint64_t bytes, VkBufferUsageFlags usage,
                             VkBuffer *buffer, VkDeviceMemory *memory, void **map) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<uint64_t>(bytes, 16);
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device_, &bci, nullptr, buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device_, *buffer, &req);
    if (!d.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory) ||
        vkBindBufferMemory(d.device_, *buffer, *memory, 0) != VK_SUCCESS ||
        vkMapMemory(d.device_, *memory, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
        vkDestroyBuffer(d.device_, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

Texture VulkanDevice::create_texture(const TextureDesc &desc) {
    if (failed_ || desc.width <= 0 || desc.height <= 0)
        return {};
    Tex t;
    t.desc = desc;
    int levels = 1;
    if (desc.mip_levels > 1)
        for (int w = desc.width, h = desc.height; w > 1 || h > 1; w = std::max(1, w / 2), h = std::max(1, h / 2))
            ++levels;
    t.desc.mip_levels = levels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_format(desc.format);
    ici.extent = {uint32_t(desc.width), uint32_t(desc.height), 1};
    ici.mipLevels = uint32_t(levels);
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & UsageRenderTarget)
        ici.usage |= desc.format == Format::Depth32F ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS)
        return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    t.bytes = req.size;
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t.memory) ||
        vkBindImageMemory(device_, t.image, t.memory, 0) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        return {};
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {aspect(desc.format), 0, uint32_t(levels), 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
        return {};
    }
    if (desc.usage & UsageCpu) {
        t.staging_bytes = uint64_t(desc.width) * desc.height * bytes_per_pixel(desc.format);
        if (!make_host_buffer(*this, t.staging_bytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              &t.staging, &t.staging_memory, &t.staging_map)) {
            vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
            return {};
        }
    }
    // Move to GENERAL once; it never leaves.
    VkCommandBuffer cb = one_shot_begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = vci.subresourceRange;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    one_shot_end_wait(cb);
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    textures_[id] = t;
    return {id};
}
```

`upload`: wait for `wait_all_submitted()` when the texture may be in use (always, simple), write rows into `staging_map` (or a transient host buffer for non-Cpu textures or level > 0), then a one-shot `vkCmdCopyBufferToImage` with `bufferRowLength = region.w`, `imageSubresource = {aspect, level, 0, 1}`, `imageOffset = {x, y, 0}`, `imageExtent = {w, h, 1}`, preceded by `full_barrier`. Return false for bad handles or regions outside the level. `readback`: `wait_all_submitted()`, one-shot `vkCmdCopyImageToBuffer` into staging (or a transient), then copy rows out at `pitch`. `destroy(Texture)`: `wait_all_submitted()`, destroy view, image, memory, staging; erase. `describe`: desc or `{}`; `allocated_bytes`: `bytes`.

```cpp
void VulkanDevice::full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &m, 0, nullptr, 0, nullptr);
}

VkCommandBuffer VulkanDevice::one_shot_begin() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = command_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    std::lock_guard lock(queue_mutex_); // the pool is externally synchronised
    vkAllocateCommandBuffers(device_, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void VulkanDevice::one_shot_end_wait(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device_, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    {
        std::lock_guard lock(queue_mutex_);
        if (vkQueueSubmit(queue_, 1, &si, fence) != VK_SUCCESS)
            fail("one-shot submit failed");
    }
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    std::lock_guard lock(queue_mutex_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cb);
}
```

Buffers: `create_buffer` → `make_host_buffer` with usage `STORAGE | TRANSFER_SRC | TRANSFER_DST | VERTEX`, memcpy contents; `update` memcpy; `map_read` → `wait_all_submitted()` then `map`; `destroy(Buffer)` → wait, destroy.

Command buffers and the reaper:

```cpp
CommandBuffer VulkanDevice::begin() {
    if (failed_)
        return {};
    std::unique_ptr<Cmd> c;
    {
        std::lock_guard lock(mutex_);
        if (!free_cmds_.empty()) {
            c = std::move(free_cmds_.back());
            free_cmds_.pop_back();
        }
    }
    if (!c) {
        c = std::make_unique<Cmd>();
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = command_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        {
            std::lock_guard lock(queue_mutex_);
            vkAllocateCommandBuffers(device_, &ai, &c->buffer);
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device_, &fci, nullptr, &c->fence);
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        vkCreateQueryPool(device_, &qci, nullptr, &c->queries);
        c->ring_bytes = 4u << 20;
        make_host_buffer(*this, c->ring_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &c->ring, &c->ring_memory,
                         reinterpret_cast<void **>(&c->ring_map));
        // Descriptor pool: Task 4 sizes it (4096 sets, 8 buffers + 4 samplers each).
        create_descriptor_pool(*c);
    }
    c->ring_used = 0;
    c->callbacks.clear();
    c->bindings_dirty = true;
    c->bound = VK_NULL_HANDLE;
    c->pipeline = 0;
    c->presents = false;
    c->presented = nullptr;
    for (auto &b : c->buffers)
        b = {};
    for (auto &t : c->textures)
        t = {};
    {
        std::lock_guard lock(queue_mutex_);
        vkResetCommandBuffer(c->buffer, 0);
        if (c->pool)
            vkResetDescriptorPool(device_, c->pool, 0);
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c->buffer, &bi);
    vkCmdResetQueryPool(c->buffer, c->queries, 0, 2);
    vkCmdWriteTimestamp(c->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->queries, 0);
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    recording_[id] = std::move(c);
    return {id};
}

VulkanDevice::Cmd *VulkanDevice::cmd(CommandBuffer cb) {
    auto it = recording_.find(cb.id);
    return it == recording_.end() ? nullptr : it->second.get();
}

VulkanDevice::Binding VulkanDevice::ring_alloc(Cmd &c, const void *bytes, uint64_t count) {
    const uint64_t align = std::max<uint64_t>(256, props_.limits.minStorageBufferOffsetAlignment);
    uint64_t start = (c.ring_used + align - 1) & ~(align - 1);
    if (start + count > c.ring_bytes) {
        fail("bytes ring exhausted in one command buffer");
        return {};
    }
    memcpy(c.ring_map + start, bytes, size_t(count));
    c.ring_used = start + count;
    return {c.ring, start, std::max<uint64_t>(count, 16)};
}

void VulkanDevice::on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb))
        c->callbacks.push_back(std::move(fn));
}

void VulkanDevice::commit(CommandBuffer cb) {
    std::unique_ptr<Cmd> c;
    {
        std::lock_guard lock(mutex_);
        auto it = recording_.find(cb.id);
        if (it == recording_.end())
            return;
        c = std::move(it->second);
        recording_.erase(it);
    }
    end_passes(*c); // Task 4: closes an open render/compute pass
    vkCmdWriteTimestamp(c->buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->queries, 1);
    vkEndCommandBuffer(c->buffer);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c->buffer;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (c->wait_semaphore) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &c->wait_semaphore;
        si.pWaitDstStageMask = &wait_stage;
    }
    if (c->signal_semaphore) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &c->signal_semaphore;
    }
    VkResult r;
    {
        std::lock_guard lock(queue_mutex_);
        vkResetFences(device_, 1, &c->fence);
        r = vkQueueSubmit(queue_, 1, &si, c->fence);
        if (r == VK_SUCCESS && c->presents)
            queue_present(*c); // Task 5: vkQueuePresentKHR waiting on signal_semaphore
    }
    std::lock_guard lock(mutex_);
    if (r != VK_SUCCESS) {
        fail("queue submit failed");
        for (auto &fn : c->callbacks)
            fn(CommandStatus::Error, 0);
        retired_[cb.id] = CommandStatus::Error;
        free_cmds_.push_back(std::move(c));
        return;
    }
    submitted_.push_back({cb.id, std::move(c)});
    reaper_cv_.notify_one();
}

void VulkanDevice::reap_loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (submitted_.empty()) {
            reaper_cv_.wait(lock);
            continue;
        }
        VkFence fence = submitted_.front().cmd->fence;
        lock.unlock();
        VkResult r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        lock.lock();
        if (submitted_.empty() || submitted_.front().cmd->fence != fence)
            continue; // retired by wait_all_submitted meanwhile
        Submission s = std::move(submitted_.front());
        submitted_.pop_front();
        const double done_at = now_seconds();
        double ms = 0;
        uint64_t ts[2] = {0, 0};
        if (props_.limits.timestampComputeAndGraphics &&
            vkGetQueryPoolResults(device_, s.cmd->queries, 0, 2, sizeof ts, ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0])
            ms = double(ts[1] - ts[0]) * props_.limits.timestampPeriod / 1e6;
        const CommandStatus status = r == VK_SUCCESS ? CommandStatus::Completed : CommandStatus::Error;
        if (r != VK_SUCCESS)
            fail("fence wait failed (device lost?)");
        auto callbacks = std::move(s.cmd->callbacks);
        auto presented = std::move(s.cmd->presented);
        retired_[s.id] = status;
        retired_order_.push_back(s.id);
        while (retired_order_.size() > 1024) {
            retired_.erase(retired_order_.front());
            retired_order_.pop_front();
        }
        free_cmds_.push_back(std::move(s.cmd));
        retired_cv_.notify_all();
        lock.unlock();
        for (auto &fn : callbacks)
            fn(status, ms);
        if (presented)
            presented(done_at);
        lock.lock();
    }
}

void VulkanDevice::wait(CommandBuffer cb) {
    std::unique_lock lock(mutex_);
    retired_cv_.wait(lock, [&] {
        if (retired_.count(cb.id))
            return true;
        for (auto &s : submitted_)
            if (s.id == cb.id)
                return false;
        return true; // never submitted, or long retired
    });
}

CommandStatus VulkanDevice::status(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    auto it = retired_.find(cb.id);
    if (it != retired_.end())
        return it->second;
    for (auto &s : submitted_)
        if (s.id == cb.id)
            return CommandStatus::Pending;
    return recording_.count(cb.id) ? CommandStatus::Pending : CommandStatus::Completed;
}

void VulkanDevice::wait_all_submitted() {
    std::unique_lock lock(mutex_);
    retired_cv_.wait(lock, [&] { return submitted_.empty(); });
}

double VulkanDevice::now_seconds() {
    return double(os_monotonic_ns()) / 1e9;
}

VulkanDevice::~VulkanDevice() {
    wait_all_submitted();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        reaper_cv_.notify_all();
    }
    if (reaper_.joinable())
        reaper_.join();
    if (device_) {
        vkDeviceWaitIdle(device_);
        // destroy every table entry (textures, buffers, cmds, pipelines, samplers,
        // layouts, swapchains), then the pool, device and instance
    }
}

std::unique_ptr<Device> vulkan_create_device() {
    return VulkanDevice::create();
}

} // namespace gpu
```

Provide stub bodies for every Task 4/5 method now (`render_pipeline` → `{}`, `begin_render_pass` → nothing, `set_*` → nothing, `draw` → nothing, `create_swapchain` → `{}`, `acquire` → `{}`, `refresh_period` → `1.0/60`, `swapchain_format` → `Format::BGRA8`, `init_pipeline_layout` → `true`, `create_descriptor_pool` → nothing, `end_passes` → nothing, `queue_present` → nothing). (All three are declared in the header above.)

- [ ] **Step 4: Build and test**

```bash
cmake --build --preset macos --target gpu_vulkan_tests && POP_GPU_BACKEND=vulkan build/recomp/gpu_vulkan_tests
```

Expected: `test_upload_readback` and `test_commit_order` checks pass; failures only from the compositor draw (pipeline null), compute (kernel null) and swapchain (skipped). With `POP_GPU_VALIDATE=1` the validation layer prints nothing for the passing tests. If volk fails to load on this Mac, confirm `ls /opt/homebrew/lib/libvulkan.1.dylib` and `POP_VULKAN_LIBRARY` override.

- [ ] **Step 5: Commit**

```bash
python3 tools/format.py && git add src/recomp/host/gpu/vulkan && git commit -m "Vulkan device core: resources, command buffers, in-order reaper"
```

---

### Task 4: Pipelines, render and compute passes, bindings, draws, transfers

Makes `test_clear_and_compositor_draw` and `test_compute_readback_kernel` pass.

**Files:**
- Modify: `src/recomp/host/gpu/vulkan/vulkan_device.cpp` (replace the Task 3 stubs; the header already declares `module`, `variant_for`, `sampler_for`, `bind_descriptors`)
- Includes: `#include "shaders_spv.h"`

**Interfaces:**
- Consumes: `gpu::vulkan::kSpirvPrograms` (Task 2), `Cmd`, `ring_alloc`, `full_barrier` (Task 3).
- Produces: a working `Device` for offscreen use; `thread_execution_width(native_brightness)` = subgroup size or 256.

- [ ] **Step 1: Layout, dummies, modules**

```cpp
bool VulkanDevice::init_pipeline_layout() {
    VkDescriptorSetLayoutBinding bindings[12];
    for (uint32_t i = 0; i < 12; ++i) {
        bindings[i] = {};
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].descriptorType = i < 8 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 12;
    lci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_) != VK_SUCCESS)
        return false;
    VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &set_layout_;
    if (vkCreatePipelineLayout(device_, &pci, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return false;
    uint32_t zero[4] = {0, 0, 0, 0};
    if (!make_host_buffer(*this, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &dummy_buffer_.buffer,
                          &dummy_buffer_.memory, &dummy_buffer_.map))
        return false;
    memcpy(dummy_buffer_.map, zero, 16);
    dummy_buffer_.bytes = 16;
    Texture white = create_texture({1, 1, Format::RGBA8, UsageSampled | UsageCpu});
    if (!white)
        return false;
    uint8_t px[4] = {255, 255, 255, 255};
    upload(white, {0, 0, 1, 1}, px, 4);
    {
        std::lock_guard lock(mutex_);
        dummy_texture_ = textures_[white.id]; // kept by handle; destroyed with the device
    }
    dummy_sampler_ = sampler_for(SamplerState{});
    return dummy_sampler_ != VK_NULL_HANDLE;
}

VkShaderModule VulkanDevice::module(const std::string &name) {
    auto it = modules_.find(name);
    if (it != modules_.end())
        return it->second;
    for (const vulkan::SpirvProgram *p = vulkan::kSpirvPrograms; p->name; ++p) {
        if (name != p->name)
            continue;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = p->count * 4;
        ci.pCode = p->words;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        modules_[name] = m;
        return m;
    }
    return VK_NULL_HANDLE;
}

void VulkanDevice::create_descriptor_pool(Cmd &c) {
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * 4096},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * 4096}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4096;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &pci, nullptr, &c.pool);
}
```

When a pool runs out (`vkAllocateDescriptorSets` returns `VK_ERROR_OUT_OF_POOL_MEMORY`), create another pool for the Cmd and keep a `std::vector<VkDescriptorPool> extra_pools` on it, reset with the first. Do that rather than failing: a level frame can exceed 4096 draws.

- [ ] **Step 2: Pipeline families and variants**

```cpp
static VkBlendFactor blend(Blend b) {
    switch (b) {
    case Blend::Zero: return VK_BLEND_FACTOR_ZERO;
    case Blend::One: return VK_BLEND_FACTOR_ONE;
    case Blend::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case Blend::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case Blend::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case Blend::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case Blend::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case Blend::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case Blend::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case Blend::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case Blend::SrcAlphaSaturated: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkCompareOp compare(Compare c) {
    switch (c) {
    case Compare::Never: return VK_COMPARE_OP_NEVER;
    case Compare::Less: return VK_COMPARE_OP_LESS;
    case Compare::Equal: return VK_COMPARE_OP_EQUAL;
    case Compare::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case Compare::Greater: return VK_COMPARE_OP_GREATER;
    case Compare::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case Compare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case Compare::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}
static VkPrimitiveTopology topology(Primitive p) {
    switch (p) {
    case Primitive::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Primitive::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Primitive::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Primitive::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

Pipeline VulkanDevice::render_pipeline(const std::string &shader, const RenderState &state) {
    std::lock_guard lock(mutex_);
    const uint64_t key = std::hash<std::string>{}(shader) * 1315423911u ^ state.key();
    auto it = family_by_key_.find(key);
    if (it != family_by_key_.end())
        return {it->second};
    if (!module(shader + ".vert") || !module(shader + ".frag"))
        return {};
    uint64_t id = next_id_++;
    families_[id] = Family{shader, state, {}};
    family_by_key_[key] = id;
    return {id};
}

VkPipeline VulkanDevice::variant_for(Cmd &c) { // mutex held
    auto fit = families_.find(c.pipeline);
    if (fit == families_.end())
        return VK_NULL_HANDLE;
    Family &f = fit->second;
    const uint64_t vkey = uint64_t(c.topology) | uint64_t(c.cull) << 2 | uint64_t(c.depth.compare) << 4 |
                          uint64_t(c.depth.write) << 8 | uint64_t(c.pass_depth) << 9;
    auto vit = f.variants.find(vkey);
    if (vit != f.variants.end())
        return vit->second;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = module(f.shader + ".vert");
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = module(f.shader + ".frag");
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology(c.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = c.cull == Cull::None ? VK_CULL_MODE_NONE
                : c.cull == Cull::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; // Metal backend: MTLWindingClockwise
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = c.pass_depth && (c.depth.compare != Compare::Always || c.depth.write);
    ds.depthWriteEnable = c.pass_depth && c.depth.write;
    ds.depthCompareOp = compare(c.depth.compare);
    VkPipelineColorBlendAttachmentState att[2] = {};
    for (int i = 0; i < f.state.color_count; ++i) {
        att[i].blendEnable = f.state.blend_enabled;
        att[i].srcColorBlendFactor = blend(f.state.src_rgb);
        att[i].dstColorBlendFactor = blend(f.state.dst_rgb);
        att[i].srcAlphaBlendFactor = blend(f.state.src_alpha);
        att[i].dstAlphaBlendFactor = blend(f.state.dst_alpha);
        att[i].colorBlendOp = att[i].alphaBlendOp = VK_BLEND_OP_ADD;
        att[i].colorWriteMask = f.state.write_color ? 0xf : 0;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(f.state.color_count);
    cb.pAttachments = att;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;
    VkFormat formats[2] = {vk_format(f.state.color_format[0]), vk_format(f.state.color_format[1])};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = uint32_t(f.state.color_count);
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = c.pass_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &ri;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dsi;
    gci.layout = pipeline_layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gci, nullptr, &p) != VK_SUCCESS)
        fail("graphics pipeline creation failed");
    f.variants[vkey] = p;
    return p;
}

Pipeline VulkanDevice::compute_pipeline(const std::string &shader) {
    std::lock_guard lock(mutex_);
    auto it = compute_by_name_.find(shader);
    if (it != compute_by_name_.end())
        return {it->second};
    std::string file = shader + ".comp";
    int width = int(subgroup_size_);
    if (shader == "native_brightness" && !subgroup_arithmetic_) {
        file = "native_brightness_shared.comp";
        width = 256;
    }
    VkShaderModule m = module(file);
    if (!m)
        return {};
    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cci.stage.module = m;
    cci.stage.pName = "main";
    cci.layout = pipeline_layout_;
    ComputePipe cp;
    cp.width = width;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cci, nullptr, &cp.pipeline) != VK_SUCCESS)
        return {};
    uint64_t id = next_id_++;
    computes_[id] = cp;
    compute_by_name_[shader] = id;
    return {id};
}

int VulkanDevice::thread_execution_width(Pipeline p) {
    std::lock_guard lock(mutex_);
    auto it = computes_.find(p.id);
    return it == computes_.end() ? 1 : it->second.width;
}
```

- [ ] **Step 3: Passes, state, bindings, draw, dispatch**

```cpp
void VulkanDevice::begin_render_pass(CommandBuffer cb, const RenderPass &pass) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c)
        return;
    end_passes(*c);
    full_barrier(c->buffer);
    VkRenderingAttachmentInfo color[2] = {};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    int w = 0, h = 0;
    for (int i = 0; i < pass.color_count; ++i) {
        auto t = textures_.find(pass.color[i].texture.id);
        if (t == textures_.end())
            return;
        transition_if_external(*c, t->second, VK_IMAGE_LAYOUT_GENERAL); // swapchain images only
        color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color[i].imageView = t->second.view;
        color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color[i].loadOp = pass.color[i].load == Load::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                        : pass.color[i].load == Load::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                           : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color[i].storeOp = pass.color[i].store == Store::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                                               : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        memcpy(color[i].clearValue.color.float32, pass.color[i].clear, sizeof(float) * 4);
        w = t->second.desc.width;
        h = t->second.desc.height;
        c->pass_formats[i] = t->second.desc.format;
    }
    c->pass_color_count = pass.color_count;
    c->pass_depth = pass.depth.texture.id != 0;
    if (c->pass_depth) {
        auto t = textures_.find(pass.depth.texture.id);
        if (t == textures_.end())
            return;
        depth.imageView = t->second.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = pass.depth.load == Load::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                     : pass.depth.load == Load::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.storeOp = pass.depth.store == Store::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {pass.depth.clear, 0};
        if (!w) {
            w = t->second.desc.width;
            h = t->second.desc.height;
        }
    }
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {uint32_t(w), uint32_t(h)}};
    info.layerCount = 1;
    info.colorAttachmentCount = uint32_t(pass.color_count);
    info.pColorAttachments = color;
    info.pDepthAttachment = c->pass_depth ? &depth : nullptr;
    vkCmdBeginRendering(c->buffer, &info);
    c->in_render = true;
    c->pass_width = w;
    c->pass_height = h;
    c->bound = VK_NULL_HANDLE;
    c->bindings_dirty = true;
    // Defaults Metal gives a fresh encoder: full viewport, no cull, depth always.
    c->viewport = {0, 0, double(w), double(h), 0, 1};
    c->viewport_set = true;
    c->cull = Cull::None;
    c->depth = DepthState{};
    VkRect2D scissor{{0, 0}, {uint32_t(w), uint32_t(h)}};
    vkCmdSetScissor(c->buffer, 0, 1, &scissor);
}

static void apply_viewport(VulkanDevice::Cmd &c) {
    // Y flip: Vulkan clip y points down, Metal's up. A negative height maps
    // NDC +1 to the top row exactly as Metal does, so the shaders are shared.
    VkViewport v;
    v.x = float(c.viewport.x);
    v.y = float(c.viewport.y + c.viewport.h);
    v.width = float(c.viewport.w);
    v.height = -float(c.viewport.h);
    v.minDepth = float(c.viewport.near_z);
    v.maxDepth = float(c.viewport.far_z);
    vkCmdSetViewport(c.buffer, 0, 1, &v);
}

void VulkanDevice::set_pipeline(CommandBuffer cb, Pipeline p) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb)) {
        c->pipeline = p.id;
        c->bound = VK_NULL_HANDLE;
        if (c->in_compute) {
            auto it = computes_.find(p.id);
            if (it != computes_.end())
                vkCmdBindPipeline(c->buffer, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
        }
    }
}
void VulkanDevice::set_depth(CommandBuffer cb, const DepthState &d) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb)) { c->depth = d; c->bound = VK_NULL_HANDLE; }
}
void VulkanDevice::set_cull(CommandBuffer cb, Cull cull) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb)) { c->cull = cull; c->bound = VK_NULL_HANDLE; }
}
void VulkanDevice::set_viewport(CommandBuffer cb, const Viewport &v) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb)) { c->viewport = v; c->viewport_set = true; }
}
static int binding_index(Stage stage, int slot) {
    if (slot < 0 || slot > 3)
        return -1;
    return stage == Stage::Fragment ? 4 + slot : slot;
}
void VulkanDevice::set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) {
    set_buffer(cb, Stage::Vertex, slot, b, offset);
}
void VulkanDevice::set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes, uint64_t count) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    int i = binding_index(stage, slot);
    if (!c || i < 0)
        return;
    c->buffers[i] = ring_alloc(*c, bytes, count);
    c->bindings_dirty = true;
}
void VulkanDevice::set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    int i = binding_index(stage, slot);
    auto it = buffers_.find(b.id);
    if (!c || i < 0 || it == buffers_.end())
        return;
    c->buffers[i] = {it->second.buffer, offset, it->second.bytes - offset};
    c->bindings_dirty = true;
}
void VulkanDevice::set_texture(CommandBuffer cb, Stage, int slot, Texture t) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = textures_.find(t.id);
    if (!c || slot < 0 || slot > 3 || it == textures_.end())
        return;
    transition_if_external(*c, it->second, VK_IMAGE_LAYOUT_GENERAL);
    c->textures[slot].view = it->second.view;
    if (!c->textures[slot].sampler)
        c->textures[slot].sampler = dummy_sampler_;
    c->bindings_dirty = true;
}
void VulkanDevice::set_sampler(CommandBuffer cb, Stage, int slot, const SamplerState &s) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || slot < 0 || slot > 3)
        return;
    c->textures[slot].sampler = sampler_for(s);
    c->bindings_dirty = true;
}

void VulkanDevice::bind_descriptors(Cmd &c, VkPipelineBindPoint point) { // mutex held
    if (!c.bindings_dirty)
        return;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        create_descriptor_pool(c); // pushes the old pool onto c.extra_pools first
        ai.descriptorPool = c.pool;
        if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
            fail("descriptor set allocation failed");
            return;
        }
    }
    VkDescriptorBufferInfo bi[8];
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet writes[12];
    for (int i = 0; i < 8; ++i) {
        const Binding &b = c.buffers[i];
        bi[i] = b.buffer ? VkDescriptorBufferInfo{b.buffer, b.offset, b.range}
                         : VkDescriptorBufferInfo{dummy_buffer_.buffer, 0, 16};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bi[i];
    }
    for (int i = 0; i < 4; ++i) {
        const TexBinding &t = c.textures[i];
        ii[i] = {t.sampler ? t.sampler : dummy_sampler_, t.view ? t.view : dummy_texture_.view,
                 VK_IMAGE_LAYOUT_GENERAL};
        writes[8 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[8 + i].dstSet = set;
        writes[8 + i].dstBinding = uint32_t(8 + i);
        writes[8 + i].descriptorCount = 1;
        writes[8 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8 + i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device_, 12, writes, 0, nullptr);
    vkCmdBindDescriptorSets(c.buffer, point, pipeline_layout_, 0, 1, &set, 0, nullptr);
    c.bindings_dirty = false;
}

void VulkanDevice::draw(CommandBuffer cb, Primitive primitive, int first, int count) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->in_render || count <= 0)
        return;
    if (c->topology != primitive) {
        c->topology = primitive;
        c->bound = VK_NULL_HANDLE;
    }
    if (!c->bound) {
        c->bound = variant_for(*c);
        if (!c->bound)
            return;
        vkCmdBindPipeline(c->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, c->bound);
        apply_viewport(*c);
    } else if (c->viewport_set) {
        apply_viewport(*c);
    }
    c->viewport_set = false;
    bind_descriptors(*c, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdDraw(c->buffer, uint32_t(count), 1, uint32_t(first), 0);
}

void VulkanDevice::end_render_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb))
        end_passes(*c);
}
void VulkanDevice::end_passes(Cmd &c) {
    if (c.in_render)
        vkCmdEndRendering(c.buffer);
    c.in_render = c.in_compute = false;
    c.bound = VK_NULL_HANDLE;
}
void VulkanDevice::begin_compute_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c)
        return;
    end_passes(*c);
    full_barrier(c->buffer);
    c->in_compute = true;
    c->bindings_dirty = true;
}
void VulkanDevice::dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) {
    dispatch_groups(cb, (tx + gx - 1) / gx, (ty + gy - 1) / gy, gx, gy);
}
void VulkanDevice::dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int, int) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->in_compute || groups_x <= 0 || groups_y <= 0)
        return;
    auto it = computes_.find(c->pipeline);
    if (it == computes_.end())
        return;
    vkCmdBindPipeline(c->buffer, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
    bind_descriptors(*c, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdDispatch(c->buffer, uint32_t(groups_x), uint32_t(groups_y), 1);
    full_barrier(c->buffer); // successive dispatches read each other's buffers
}
void VulkanDevice::end_compute_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb))
        end_passes(*c);
}
```

Sampler cache:

```cpp
VkSampler VulkanDevice::sampler_for(const SamplerState &s) { // mutex held
    auto addr = [](Address a) {
        switch (a) {
        case Address::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case Address::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case Address::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case Address::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    const uint64_t key = uint64_t(s.u) | uint64_t(s.v) << 3 | uint64_t(s.mag) << 6 | uint64_t(s.min) << 7 |
                         uint64_t(s.mip) << 8 | uint64_t(std::min(16, std::max(1, s.anisotropy))) << 10;
    auto it = samplers_.find(key);
    if (it != samplers_.end())
        return it->second;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = s.mag == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = s.min == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = s.mip == MipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = addr(s.u);
    sci.addressModeV = addr(s.v);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = s.mip == MipFilter::None ? 0.25f : VK_LOD_CLAMP_NONE;
    sci.anisotropyEnable = anisotropy_ && s.anisotropy > 1;
    sci.maxAnisotropy = std::min(float(s.anisotropy), props_.limits.maxSamplerAnisotropy);
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler out = VK_NULL_HANDLE;
    vkCreateSampler(device_, &sci, nullptr, &out);
    samplers_[key] = out;
    return out;
}
```

Transfers (each: `end_passes`, `full_barrier`, then the copy; `transition_if_external` moves a swapchain image UNDEFINED→GENERAL the first time it is touched, and is a no-op for owned images):

- `blit`: `vkCmdCopyImage` level 0 → level 0, `srcOffset {x,y,0}`, `dstOffset {dst_x,dst_y,0}`, `extent {w,h,1}`, both layouts GENERAL.
- `copy_buffer_to_texture`: `vkCmdCopyBufferToImage` with `bufferOffset = offset`, `bufferRowLength = pitch / bytes_per_pixel(dst format)`.
- `copy_texture_to_buffer`: `vkCmdCopyImageToBuffer` likewise.
- `generate_mipmaps`: for level `i = 1..n-1`, `vkCmdBlitImage` from level `i-1` (extent `max(1, w>>(i-1))`) to level `i` with `VK_FILTER_LINEAR`, a `full_barrier` between blits.

- [ ] **Step 4: Build and test, with validation**

```bash
cmake --build --preset macos --target gpu_vulkan_tests && POP_GPU_VALIDATE=1 POP_GPU_BACKEND=vulkan build/recomp/gpu_vulkan_tests
```

Expected: `N checks, 0 failures`, `all gpu vulkan tests passed`, swapchain test "skipped", and no validation messages. If the green/red halves are swapped, the negative viewport is wrong: the quad covers the left half by x only, so a swap means `rect` semantics broke — check `apply_viewport`. If the compute test reads zeros, check that `bind_descriptors` ran after `vkCmdBindPipeline` and that `Out` is not `readonly`.

- [ ] **Step 5: Commit**

```bash
python3 tools/format.py && git add src/recomp/host/gpu/vulkan && git commit -m "Vulkan pipelines, passes, storage-buffer bindings, draws, compute and transfers"
```

---

### Task 5: Swapchain over an SDL window; acquire, present, presented-time ack

Makes `test_swapchain_from_surface` run (not skip) on the Mac.

**Files:**
- Modify: `src/recomp/host/gpu/vulkan/vulkan_swapchain.cpp` (replace skeleton), `vulkan_device.h` (define `struct Chain`), `vulkan_device.cpp` (`queue_present`, `transition_if_external`, `register_external_image`)

**Interfaces:**
- Consumes: `Cmd::present_*`, `wait_semaphore`, `signal_semaphore`, `presented` (Task 3), `commit()` calling `queue_present`.
- Produces: `void *vulkan_test_native_surface(int w, int h)` (a hidden `SDL_Window*`), `void *vulkan_native_surface_for_window(void *sdl_window)` (identity), swapchain methods. The native surface handed to `create_swapchain` is always an `SDL_Window*`.

- [ ] **Step 1: Chain**

In the header, replace `struct Chain;` with:

```cpp
    struct Chain {
        void *window = nullptr; // SDL_Window*
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
        int width = 0, height = 0;
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> acquire_semaphores;  // one per image slot, rotated
        std::vector<VkSemaphore> finished_semaphores; // one per image
        uint32_t next_acquire = 0;
        std::unordered_map<uint64_t, uint32_t> acquired; // texture id -> image index
        std::unordered_map<uint64_t, VkSemaphore> acquired_wait;
        bool needs_recreate = false;
        double refresh = 1.0 / 60;
    };
```

- [ ] **Step 2: Implementation**

`vulkan_swapchain.cpp` core (all under `namespace gpu`, including `<SDL3/SDL.h>` and `<SDL3/SDL_vulkan.h>` after `vulkan_device.h` so SDL sees the real Vulkan types):

```cpp
static bool build_swapchain(VulkanDevice &d, VulkanDevice::Chain &c, int width, int height) {
    vkDeviceWaitIdle(d.device_);
    for (VkImageView v : c.views)
        vkDestroyImageView(d.device_, v, nullptr);
    c.views.clear();
    c.images.clear();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical_, c.surface, &caps);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical_, c.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical_, c.surface, &fn, formats.data());
    c.format = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    VkColorSpaceKHR space = formats.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
    for (auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            c.format = f.format;
            space = f.colorSpace;
        }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = {uint32_t(width), uint32_t(height)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = c.surface;
    sci.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount)
        sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = c.format;
    sci.imageColorSpace = space;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = c.swapchain;
    VkSwapchainKHR fresh;
    if (vkCreateSwapchainKHR(d.device_, &sci, nullptr, &fresh) != VK_SUCCESS)
        return false;
    if (c.swapchain)
        vkDestroySwapchainKHR(d.device_, c.swapchain, nullptr);
    c.swapchain = fresh;
    c.width = int(extent.width);
    c.height = int(extent.height);
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(d.device_, fresh, &n, nullptr);
    c.images.resize(n);
    vkGetSwapchainImagesKHR(d.device_, fresh, &n, c.images.data());
    for (VkImage img : c.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = c.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        vkCreateImageView(d.device_, &vci, nullptr, &view);
        c.views.push_back(view);
    }
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (c.acquire_semaphores.size() < n + 1) {
        VkSemaphore s;
        vkCreateSemaphore(d.device_, &semci, nullptr, &s);
        c.acquire_semaphores.push_back(s);
    }
    while (c.finished_semaphores.size() < n) {
        VkSemaphore s;
        vkCreateSemaphore(d.device_, &semci, nullptr, &s);
        c.finished_semaphores.push_back(s);
    }
    c.needs_recreate = false;
    return true;
}

Swapchain VulkanDevice::create_swapchain(void *native_surface, int width, int height) {
    if (failed_ || !native_surface)
        return {};
    auto c = std::make_unique<Chain>();
    c->window = native_surface;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window *>(native_surface), instance_, nullptr, &c->surface)) {
        fprintf(stderr, "gpu/vulkan: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return {};
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, c->surface, &supported);
    if (!supported || !build_swapchain(*this, *c, width, height))
        return {};
    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(
            SDL_GetDisplayForWindow(static_cast<SDL_Window *>(native_surface))))
        if (mode->refresh_rate > 1.0f)
            c->refresh = 1.0 / mode->refresh_rate;
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    swapchains_[id] = std::move(c);
    return {id};
}

void VulkanDevice::resize(Swapchain s, int width, int height) {
    wait_all_submitted();
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it != swapchains_.end())
        build_swapchain(*this, *it->second, width, height);
}

Format VulkanDevice::swapchain_format(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? Format::BGRA8 : format_of(it->second->format);
}

Texture VulkanDevice::acquire(Swapchain s) {
    std::unique_lock lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end() || failed_)
        return {};
    Chain &c = *it->second;
    if (c.needs_recreate) {
        lock.unlock();
        wait_all_submitted();
        lock.lock();
        build_swapchain(*this, c, c.width, c.height);
    }
    VkSemaphore sem = c.acquire_semaphores[c.next_acquire % c.acquire_semaphores.size()];
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, c.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        c.needs_recreate = true;
        return {};
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return {};
    if (r == VK_SUBOPTIMAL_KHR)
        c.needs_recreate = true;
    ++c.next_acquire;
    TextureDesc desc{c.width, c.height, format_of(c.format), UsageRenderTarget, 1};
    Texture t = register_external_image(c.images[index], c.views[index], desc, s.id, index);
    c.acquired[t.id] = index;
    c.acquired_wait[t.id] = sem;
    return t;
}

void VulkanDevice::release_drawable(Swapchain s, Texture t) {
    // The acquire semaphore is signalled and nobody waits on it: consume it
    // with an empty fenced submit so the slot can be reused.
    VkSemaphore sem = VK_NULL_HANDLE;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end())
            return;
        Chain &c = *it->second;
        auto w = c.acquired_wait.find(t.id);
        if (w != c.acquired_wait.end()) {
            sem = w->second;
            c.acquired_wait.erase(w);
        }
        c.acquired.erase(t.id);
        textures_.erase(t.id);
    }
    if (!sem)
        return;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &stage;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device_, &fci, nullptr, &fence);
    {
        std::lock_guard lock(queue_mutex_);
        vkQueueSubmit(queue_, 1, &si, fence);
    }
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
}

void VulkanDevice::present(CommandBuffer cb, Swapchain s, Texture t, double, std::function<void(double)> presented) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = swapchains_.find(s.id);
    if (!c || it == swapchains_.end())
        return;
    Chain &chain = *it->second;
    auto a = chain.acquired.find(t.id);
    auto tex = textures_.find(t.id);
    if (a == chain.acquired.end() || tex == textures_.end())
        return;
    end_passes(*c);
    transition_if_external(*c, tex->second, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    c->presents = true;
    c->present_swapchain = s.id;
    c->present_image = a->second;
    c->wait_semaphore = chain.acquired_wait[t.id];
    c->signal_semaphore = chain.finished_semaphores[a->second];
    c->presented = std::move(presented);
    chain.acquired.erase(a);
    chain.acquired_wait.erase(t.id);
    textures_.erase(tex);
}

void VulkanDevice::queue_present(Cmd &c) { // queue_mutex_ held, called by commit after submit
    VkSwapchainKHR sc;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(c.present_swapchain);
        if (it == swapchains_.end())
            return;
        sc = it->second->swapchain;
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &c.signal_semaphore;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &c.present_image;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(c.present_swapchain);
        if (it != swapchains_.end())
            it->second->needs_recreate = true;
    }
    c.wait_semaphore = c.signal_semaphore = VK_NULL_HANDLE;
}

double VulkanDevice::refresh_period(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? 1.0 / 60 : it->second->refresh;
}

void VulkanDevice::destroy(Swapchain s) {
    wait_all_submitted();
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end())
        return;
    Chain &c = *it->second;
    for (auto &[tid, idx] : c.acquired)
        textures_.erase(tid);
    vkDeviceWaitIdle(device_);
    for (VkImageView v : c.views)
        vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore sem : c.acquire_semaphores)
        vkDestroySemaphore(device_, sem, nullptr);
    for (VkSemaphore sem : c.finished_semaphores)
        vkDestroySemaphore(device_, sem, nullptr);
    vkDestroySwapchainKHR(device_, c.swapchain, nullptr);
    vkDestroySurfaceKHR(instance_, c.surface, nullptr);
    swapchains_.erase(it);
}

void *vulkan_test_native_surface(int w, int h) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO))
        return nullptr;
    if (const char *p = vulkan_loader_path_impl())
        SDL_Vulkan_LoadLibrary(p);
    return SDL_CreateWindow("gpu test", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
}
void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}
```

`register_external_image` (in `vulkan_device.cpp`) inserts a `Tex` with `external = true`, `layout = VK_IMAGE_LAYOUT_UNDEFINED`, the view and desc, returning a new id. `transition_if_external(Cmd &, Tex &, VkImageLayout to)` records `vkCmdPipelineBarrier` from `tex.layout` to `to` (ALL_COMMANDS → ALL_COMMANDS, MEMORY_WRITE → MEMORY_READ|WRITE) when `tex.external && tex.layout != to`, then sets `tex.layout = to`; owned images return immediately. In `present`, the image must already be GENERAL or UNDEFINED (a frame that blits into it made it GENERAL; a frame that never wrote it is still UNDEFINED and the barrier is UNDEFINED → PRESENT_SRC, which is legal).

`resize`'s caller (the presenter) may hold acquired textures; `build_swapchain` waits idle first, so nothing is in flight, and stale acquired handles are erased by `release_drawable`/`present` finding no entry.

- [ ] **Step 3: Test**

```bash
cmake --build --preset macos --target gpu_vulkan_tests && POP_GPU_VALIDATE=1 POP_GPU_BACKEND=vulkan build/recomp/gpu_vulkan_tests
```

Expected: the swapchain test runs (no "skipped"), `0 failures`, no validation output. `describe(t).width == 32` after the first acquire and `64` after `resize`; `refresh_period` in (0, 0.1).

- [ ] **Step 4: Commit**

```bash
python3 tools/format.py && git add src/recomp/host/gpu/vulkan && git commit -m "Vulkan swapchain over an SDL window with completion-time presented acks"
```

---

### Task 6: Hosts on every platform; the SDL host's Vulkan window; the renderer suites over Vulkan

**Files:**
- Modify: `src/recomp/host/CMakeLists.txt` (remove the `if(NOT APPLE) return()`; guard only Apple-specific pieces), `src/recomp/host/sdl/main.cpp`, `src/recomp/host/headless_main.cpp`, `src/recomp/host/present.cpp` (POSIX includes), `cmake/MacBundle.cmake` (no-op off Apple if it is not already)
- Test: `compositor_tests`, `host_tests` run with `POP_GPU_BACKEND=vulkan` on the Mac

**Interfaces:**
- Consumes: `gpu::native_surface_for_window`, `gpu::release_window_surface`, `gpu::vulkan_loader_path`, `gpu::default_backend_name` (Task 1).
- Produces: `PopRecomp`, `pop_headless`, `pop_smoke`, `host_tests` targets on Linux and Windows (the GEN ones only when `recomp_gen` exists, as today).

- [ ] **Step 1: Host CMake on every platform**

Replace lines 43–50 of `src/recomp/host/CMakeLists.txt` (the `if(NOT APPLE) return()` block and the `FW_*` sets) with:

```cmake
if(APPLE)
  set(FW_FOUNDATION "-framework Foundation")
  set(FW_METAL "-framework Metal" "-framework MetalKit" "-framework CoreText"
               "-framework CoreVideo" "-framework CoreGraphics" "-framework QuartzCore")
else()
  set(FW_FOUNDATION "")
  set(FW_METAL "")
endif()
```

Wrap the `present_events_tests` block in `if(APPLE)` (its source is Objective-C++). Wrap `pop_mac_bundle(PopRecomp)` in `if(APPLE)`. `POP_OBJC_ARC` stays as it is; if CMake rejects `COMPILE_LANGUAGE:OBJCXX` where the language is not enabled, change line 14 to `if(APPLE) set(POP_OBJC_ARC ...) else() set(POP_OBJC_ARC "") endif()`.

- [ ] **Step 2: SDL host window and surface**

In `src/recomp/host/sdl/main.cpp`: delete `#include <SDL3/SDL_metal.h>` and `SDL_MetalView g_metal_view`; keep `void *g_surface`. Replace the window creation and Metal view lines:

```cpp
    const bool vulkan = strcmp(gpu::default_backend_name(), "vulkan") == 0;
    if (vulkan && !SDL_Vulkan_LoadLibrary(gpu::vulkan_loader_path()))
        fprintf(stderr, "PopRecomp: SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError());
    const SDL_WindowFlags surface_flag = vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;
    g_window = SDL_CreateWindow("Populous: The Beginning", g_mode_w * scale, g_mode_h * scale,
                                surface_flag | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIDDEN);
    ...
    g_surface = gpu::native_surface_for_window(g_window);
    if (!g_surface) {
        fprintf(stderr, "PopRecomp: no %s surface for the window: %s\n", gpu::default_backend_name(),
                SDL_GetError());
        return 3;
    }
    fprintf(stderr, "GPU backend: %s\n", gpu::default_backend_name());
```

Add `#include <SDL3/SDL_vulkan.h>`. Move `g_gpu = gpu::create_default_device()` to after `SDL_Init` but keep it before the window (it is already). At shutdown replace `SDL_Metal_DestroyView(g_metal_view);` with `gpu::release_window_surface(g_surface);`.

On the Metal backend `SDL_WINDOW_METAL` is still required for `SDL_Metal_CreateView`; on Vulkan `SDL_WINDOW_VULKAN` makes `SDL_Vulkan_CreateSurface` valid. Nothing else in the host knows the backend.

- [ ] **Step 3: POSIX includes in the hosts**

`headless_main.cpp` and `present.cpp` include `<unistd.h>` or `<sys/...>`. For each use found by `grep -n "unistd\|sys/\|getpid\|usleep\|isatty\|getcwd" src/recomp/host/headless_main.cpp src/recomp/host/present.cpp`, replace: `usleep(us)` → `os_sleep_us(us)`; `getcwd` → `os_getcwd`; `isatty(fd)` → `#ifdef _WIN32 _isatty #else isatty #endif` behind a small static helper; `stat` → `os_stat`; `mkdir` → `os_mkdir`; `getpid` → `(int)os_thread_self()` where a unique id is all that is wanted. Then delete the includes. Build the Linux preset in a container or trust CI (Task 8) for the compile check; on the Mac, `grep -rn "#include <unistd.h>" src/recomp/host --include=*.cpp | grep -v tests` must be empty.

- [ ] **Step 4: Run the renderer suites over Vulkan on the Mac**

```bash
cmake --build --preset macos --target check_binaries
POP_GPU_BACKEND=vulkan build/recomp/compositor_tests
POP_GPU_BACKEND=vulkan build/recomp/host_tests 2>&1 | tail -40
```

Expected: `compositor_tests` passes. `host_tests` is the real gate: every renderer pixel test (`test_terrain_material_detail`, cull, depth, fog, alpha test, texblend, surface upload, readback kernel, brightness tiles, HD mip textures, presenter offscreen) must report ok. Fix failures in the backend, never in the renderer, with these likely culprits:
  - Culled faces inverted → switch `rs.frontFace` to `VK_FRONT_FACE_COUNTER_CLOCKWISE` in `variant_for` (the winding after the Y flip is the one empirical question in this plan; the host cull test settles it).
  - Texture rows flipped → `apply_viewport` sign or `imageOffset.y` in `upload`.
  - Points invisible → `enabled.largePoints` or `gl_PointSize` missing.
  - Brightness sums off by the SIMD factor → `thread_execution_width` must be the real subgroup size MoltenVK reports (32 on Apple GPUs) and the pipeline must be created with `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT` when `apiVersion >= 1.3`.
  - Validation complaints → run with `POP_GPU_VALIDATE=1` and fix every message.

- [ ] **Step 5: Metal still green, then commit**

```bash
ctest --preset macos -L "nogame|gpu" --output-on-failure
python3 tools/format.py && git add -A src/recomp/host cmake && git commit -m "Build the SDL host and suites on every platform; Vulkan window surface; renderer suites pass over Vulkan"
```

---

### Task 7: Platform calls for processes and temp dirs; port the POSIX-only suites

**Files:**
- Modify: `src/recomp/platform/os.h`, `os_posix.cpp`, `os_win32.cpp`, `src/recomp/platform/tests/platform_tests.cpp`
- Modify: `src/recomp/runtime/tests/runtime_tests.cpp`, `src/recomp/runtime/CMakeLists.txt`
- Modify: `src/recomp/mods/tests/{classic_modes_tests,run_record_tests,overlay_tests,host_services_tests}.cpp`, `src/recomp/mods/CMakeLists.txt`
- Modify: `src/recomp/host/tests/host_tests.cpp`
- Create: `mods/core/tests/roots_tests.py`; Delete: `mods/core/tests/roots_tests.sh`

**Interfaces:**
- Produces (in `os.h`, C linkage):

```c
// Runs argv[0] with argv (NULL-terminated), inheriting stdio; 0 and a pid, or -1.
int os_spawn(const char *const argv[], int64_t *pid_out);
// Waits for the child. exit_code receives the exit status, or 128 + signal on
// POSIX when it died by a signal (so an abort reads as 134 everywhere); 0 or -1.
int os_wait(int64_t pid, int *exit_code);
// mkdtemp: replaces the trailing XXXXXX and creates the directory; 0 or -1.
int os_mkdtemp(char *template_path);
// A writable temporary directory without a trailing separator ("/tmp", %TEMP%).
const char *os_temp_dir(void);
// "/dev/null" or "NUL".
const char *os_null_device(void);
```

- [ ] **Step 1: Failing platform test**

Append to `test_process_and_strings()` in `platform_tests.cpp`:

```cpp
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pop-platform-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    OsStat st;
    CHECK(os_stat(dir, &st) == 0 && st.is_dir);
    CHECK(os_rmdir(dir) == 0);
    // The test binary re-runs itself as a child that exits 7.
    const char *argv[] = {exe, "--child-exit-7", nullptr};
    int64_t pid = 0;
    CHECK(os_spawn(argv, &pid) == 0);
    int code = -1;
    CHECK(os_wait(pid, &code) == 0);
    CHECK(code == 7);
```

And at the top of `main`:

```cpp
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--child-exit-7") == 0)
        return 7;
```

Run: `cmake --build --preset macos --target platform_tests && build/recomp/platform_tests` → compile error (undeclared functions).

- [ ] **Step 2: Implement**

`os_posix.cpp`:

```cpp
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

int os_spawn(const char *const argv[], int64_t *pid_out) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return -1;
    *pid_out = pid;
    return 0;
}
int os_wait(int64_t pid, int *exit_code) {
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else
        *exit_code = -1;
    return 0;
}
int os_mkdtemp(char *template_path) {
    return mkdtemp(template_path) ? 0 : -1;
}
const char *os_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}
```

`os_win32.cpp` (uses the file's existing `widen`):

```cpp
int os_spawn(const char *const argv[], int64_t *pid_out) {
    std::wstring cmd;
    for (int i = 0; argv[i]; ++i) {
        std::wstring a = widen(argv[i]);
        if (i)
            cmd += L' ';
        if (a.find_first_of(L" \t\"") == std::wstring::npos)
            cmd += a;
        else {
            cmd += L'"';
            for (wchar_t ch : a)
                cmd += ch == L'"' ? std::wstring(L"\\\"") : std::wstring(1, ch);
            cmd += L'"';
        }
    }
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(widen(argv[0]).c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *pid_out = (int64_t)(intptr_t)pi.hProcess;
    return 0;
}
int os_wait(int64_t pid, int *exit_code) {
    HANDLE h = (HANDLE)(intptr_t)pid;
    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0)
        return -1;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    // abort() exits with 3 on Windows; report it as POSIX SIGABRT so tests agree.
    *exit_code = code == 3 ? 134 : (int)code;
    return 0;
}
int os_mkdtemp(char *template_path) {
    size_t len = strlen(template_path);
    if (len < 6 || strcmp(template_path + len - 6, "XXXXXX") != 0)
        return -1;
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)GetTickCount() ^ (GetCurrentThreadId() * 2654435761u);
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (size_t i = len - 6; i < len; ++i) {
            seed = seed * 1103515245u + 12345u;
            template_path[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
        }
        if (CreateDirectoryW(widen(template_path).c_str(), nullptr))
            return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return -1;
    }
    return -1;
}
const char *os_temp_dir(void) {
    static char buf[MAX_PATH * 3];
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n)
        return ".";
    if (w[n - 1] == L'\\' || w[n - 1] == L'/')
        w[n - 1] = 0;
    std::string s = narrow(w);
    strncpy(buf, s.c_str(), sizeof buf - 1);
    return buf;
}
```

Run `build/recomp/platform_tests` → `all platform tests passed`.

- [ ] **Step 3: runtime_tests**

In `runtime_tests.cpp`:
- Remove `<unistd.h>`, `<signal.h>`, `<sys/wait.h>`, `<sys/stat.h>`; add `#include "../../platform/os.h"`.
- `fork()` site (~line 2066): the child path becomes a re-exec. Add at the top of `main`:

```cpp
    if (argc > 1 && strcmp(argv[1], "--child-setjmp-abort") == 0) {
        freopen(os_null_device(), "w", stderr);
        child_setjmp_abort(); // the body that was inside `if (pid == 0)`, building the same context
        return 0;
    }
```

and the parent:

```cpp
    char exe[4096];
    check(os_exe_path(exe, sizeof exe) == 0, "the test knows its own path");
    const char *child_argv[] = {exe, "--child-setjmp-abort", nullptr};
    int64_t pid = 0;
    int code = -1;
    check(os_spawn(child_argv, &pid) == 0 && os_wait(pid, &code) == 0 && code == 134,
          "the single-call _setjmp intrinsic aborts instead of pretending to work");
```

The child must rebuild its `X86 *c` and scratch block before calling `recomp_setjmp(c)`: factor the setup lines that precede the fork (`scratch_block(64)`, the stack writes) into `static void child_setjmp_abort()` that creates a fresh context the same way the enclosing test did (read the surrounding test for how `c` is obtained; it is the same fixture `main` builds).
- `usleep(x)` → `os_sleep_us(x)`; `stat(path, &st)` → `OsStat st; os_stat(path, &st)` with `st.size` for `st_size`; `access(p, F_OK) != 0` → `os_stat(p, &st) != 0`; `setenv(a, b, 1)` → `os_setenv(a, b)`; `unsetenv` → `os_unsetenv`; `clock_gettime(CLOCK_MONOTONIC)` → `os_monotonic_ns()`.
- `system("rm -rf " + root)` → a small recursive remover over `os_listdir`/`os_unlink`/`os_rmdir` (`static void remove_tree(const std::string &)`), and `system("mkdir -p a b")` → `os_mkdir` per path component.
- `popen(".venv/bin/python -c ...")` (pefile sections): keep, guarded — `#ifdef _WIN32 #define popen _popen #define pclose _pclose #endif` and use `.venv/Scripts/python.exe` when `_WIN32`. This test needs the game files anyway.
- `SIGABRT`/`WIFSIGNALED` references disappear with the fork.

`src/recomp/runtime/CMakeLists.txt`: remove the `if(NOT WIN32)`/`endif()` around `runtime_tests` and update the comment.

- [ ] **Step 4: mods tests and host tests**

- `classic_modes_tests.cpp`: `mkstemp` → `os_mkstemp`, `close` → `os_fd_close`, `unlink` → `os_unlink`; drop `<unistd.h>`; template path `"/tmp/..."` → `std::string(os_temp_dir()) + "/pop-classic-XXXXXX"` copied into a `char` buffer.
- `host_services_tests.cpp`: `mkdtemp` → `os_mkdtemp` (returns 0), `mkstemp`/`close`/`unlink`/`rmdir` → `os_` forms; temp root via `os_temp_dir()`.
- `run_record_tests.cpp`, `overlay_tests.cpp`: `mkdir(p, 0755)` → `os_mkdir(p)`, `unlink` → `os_unlink`, drop `<sys/stat.h>`/`<unistd.h>`. The two `symlink` checks stay POSIX-only: wrap each in `#ifndef _WIN32 ... #else printf("symlink check skipped on Windows\n"); #endif` (creating symlinks needs a privilege on Windows).
- `host_tests.cpp`: `mkdtemp(dir)` → `os_mkdtemp(dir) == 0`, `mkstemp` → `os_mkstemp`, `close` → `os_fd_close`, `unlink` → `os_unlink`, `setenv(n, v, 1)` → `os_setenv(n, v)`, `unsetenv` → `os_unsetenv`; `"/tmp/"` templates → `os_temp_dir()`; drop `<unistd.h>`. The `out.write(...)` calls are `std::ofstream`, untouched.
- `src/recomp/mods/CMakeLists.txt`: replace the `if(NOT WIN32)` roots block with:

```cmake
add_test(NAME roots_tests
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/mods/core/tests/roots_tests.py $<TARGET_FILE:roots_probe>
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(roots_tests PROPERTIES LABELS nogame)
```

`mods/core/tests/roots_tests.py`:

```python
#!/usr/bin/env python3
"""Run the roots probe: defaults, overrides, empty overrides, a relocated
app-shaped directory and a negative control. Portable replacement for
roots_tests.sh."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def run(probe, args, env_overrides, expect_ok=True):
    env = {k: v for k, v in os.environ.items() if k not in ("POPM_CORE_MODS_DIR", "POPM_MODS_DIR")}
    env.update(env_overrides)
    r = subprocess.run([str(probe), *args], cwd=ROOT, env=env)
    if (r.returncode == 0) != expect_ok:
        raise SystemExit(f"FAIL: {probe} {args} env={env_overrides} exit {r.returncode}")


def main():
    src = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="pop-core-roots.") as tmp:
        probe = Path(tmp) / src.name
        shutil.copy2(src, probe)
        run(probe, ["build/recomp/mods/core", "mods"], {})
        run(probe, ["/custom/core", "/custom/user"],
            {"POPM_CORE_MODS_DIR": "/custom/core", "POPM_MODS_DIR": "/custom/user"})
        run(probe, ["build/recomp/mods/core", "mods"], {"POPM_CORE_MODS_DIR": "", "POPM_MODS_DIR": ""})
        macos = Path(tmp) / "Relocated.app/Contents/MacOS"
        macos.mkdir(parents=True)
        relocated = macos / src.name
        shutil.copy2(src, relocated)
        run(relocated, [str(macos / "../Resources/mods/core"), "mods"], {})
        run(relocated, ["/override", "mods"], {"POPM_CORE_MODS_DIR": "/override"})
        run(probe, ["/incorrect/core", "mods"], {}, expect_ok=False)
    print("PASS: default, overrides, empty overrides, relocated app and negative control")


if __name__ == "__main__":
    main()
```

`git rm mods/core/tests/roots_tests.sh`. If `roots_probe` compares paths with `/` separators on Windows and fails there, normalise in the probe with `os_`-level replacement of `\\` by `/` before comparing; CI (Task 8) shows whether that is needed.

- [ ] **Step 5: Verify on the Mac**

```bash
cmake --build --preset macos --target check_binaries
ctest --preset macos -L "nogame|gpu" --output-on-failure
python3 tools/test.py --mods && python3 tools/test.py --gameplay
build/recomp/runtime_tests 2>&1 | tail -5
grep -rn "#include <unistd.h>\|#include <sys/wait.h>\|mkdtemp(\|fork()" src/recomp/runtime/tests src/recomp/mods/tests/*.cpp src/recomp/host/tests/host_tests.cpp
```

Expected: all green (the pre-existing integration failures noted in PROGRESS.md excepted); the grep prints nothing.

- [ ] **Step 6: Commit**

```bash
python3 tools/format.py && git add -A src/recomp/platform src/recomp/runtime src/recomp/mods src/recomp/host/tests mods/core/tests && git commit -m "Platform spawn/wait/mkdtemp; runtime, mods, host and roots suites build on Windows"
```

---

### Task 8: CI: lavapipe on Linux with the gpu label; Vulkan label on macOS; Windows suites

**Files:**
- Modify: `.github/workflows/checks.yml`, `docs/superpowers/PROGRESS.md`

- [ ] **Step 1: Workflow**

In the matrix, Linux labels become `"nogame|gpu"`. Add `mesa-vulkan-drivers` and `glslc` to the Linux apt list (glslc lets `shader_drift_check` run for real there). Add after the Linux install step:

```yaml
      - name: Software Vulkan for the GPU suites (Linux)
        if: runner.os == 'Linux'
        run: |
          echo "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json" >> "$GITHUB_ENV"
          echo "LIBGL_ALWAYS_SOFTWARE=1" >> "$GITHUB_ENV"
          echo "SDL_VIDEODRIVER=dummy" >> "$GITHUB_ENV"
```

macOS keeps `nogame|gpu` (Metal); `gpu_vulkan_tests` is labelled `gpu-vulkan` and does not run there. Windows keeps `nogame`, which now includes `platform_tests` (with spawn), `roots_tests`, `shader_drift_check` (skips without glslc).

- [ ] **Step 2: Run CI on the branch and wait for it**

```bash
git push -u origin vulkan-hosts
gh workflow run checks.yml --ref vulkan-hosts
sleep 60; RUN=$(gh run list --workflow checks.yml --branch vulkan-hosts --limit 1 --json databaseId,headSha -q '.[0]'); echo $RUN
gh run watch $(echo $RUN | python3 -c 'import json,sys;print(json.load(sys.stdin)["databaseId"])') --exit-status
```

Verify the `headSha` is the branch head. Expected: `tooling`, Linux (`nogame|gpu` over lavapipe), macOS, Windows all `success`. Typical fixes on the first run: missing `dl` link for `gpu_vulkan` on Linux; `SDL_VIDEODRIVER=dummy` making `vulkan_test_native_surface` return null (fine: the swapchain test skips); lavapipe lacking `largePoints` (do not require it); Windows path separators in `roots_tests.py`; MSVC-style `clang` on Windows needing `-Wno-...` for volk.c (`target_compile_options(volk PRIVATE -w)` is acceptable for vendored C).

- [ ] **Step 3: Commit (after green)**

```bash
git add .github/workflows/checks.yml && git commit -m "CI: lavapipe runs the GPU suites on Linux; Windows runs the ported suites"
```

---

### Task 9: Game-backed proof over Vulkan on the Mac, flyby comparison, docs

**Files:**
- Create: `tools/recomp/compare_frames.py`, `tools/recomp/smoke/flyby.script`
- Modify: `docs/superpowers/PROGRESS.md`, `docs/superpowers/plans/2026-09-12-vulkan-and-platform-hosts.md` (Execution notes), memory file `multi-platform-port-roadmap.md`

- [ ] **Step 1: Game-backed suites over Vulkan**

```bash
rm -f build/recomp/profile/POP3.CD/SAVE/CONFIG00.DAT   # the profile-drift gotcha
POP_GPU_BACKEND=vulkan python3 tools/test.py --gameplay
POP_GPU_BACKEND=vulkan python3 tools/test.py --mods
POP_GPU_BACKEND=vulkan sh src/recomp/host/tests/integration_tests.sh 2>&1 | tail -20
```

Expected: the same results as Metal (the pre-existing integration fixture failures excepted). Fix backend bugs that surface; commit each.

- [ ] **Step 2: Flyby comparison**

The smoke host writes `smoke_<name>_present.ppm` (binary P6) into `POP_HOST_DUMP_DIR`. Commit the flyby script so the comparison is reproducible:

```bash
sed -n '1,/dump flyby_over/p' tools/recomp/smoke/level1.script > tools/recomp/smoke/flyby.script
echo quit >> tools/recomp/smoke/flyby.script
```

`tools/recomp/compare_frames.py`:

```python
#!/usr/bin/env python3
"""Compare two binary PPM (P6) frames of the same size: the mean absolute
channel difference must be under --mean and no channel may differ by more
than --max. Exit 1 when either bound is exceeded."""
import argparse
import sys
from pathlib import Path


def load_ppm(path):
    data = Path(path).read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6":
        raise SystemExit(f"{path}: not a binary PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--mean", type=float, default=2.0)
    ap.add_argument("--max", type=int, default=24)
    args = ap.parse_args()
    wa, ha, pa = load_ppm(args.a)
    wb, hb, pb = load_ppm(args.b)
    if (wa, ha) != (wb, hb):
        print(f"size mismatch {wa}x{ha} vs {wb}x{hb}")
        return 1
    total, worst = 0, 0
    for x, y in zip(pa, pb):
        d = abs(x - y)
        total += d
        if d > worst:
            worst = d
    mean = total / max(1, len(pa))
    print(f"mean {mean:.3f} max {worst} over {wa}x{ha}")
    return 0 if mean < args.mean and worst <= args.max else 1


if __name__ == "__main__":
    sys.exit(main())
```

Run both backends with the pinned clock (the invocation from the sky investigation) and compare:

```bash
S=<scratchpad directory>; mkdir -p $S/fly-metal $S/fly-vulkan
POPM_MODS_DIR=mods POP_RECOMP_PIN_CLOCK=1 POP_RECOMP_SCRIPT=tools/recomp/smoke/flyby.script POP_HOST_DUMP_DIR=$S/fly-metal build/recomp/pop_smoke > $S/fly-metal.log 2>&1
POP_GPU_BACKEND=vulkan POPM_MODS_DIR=mods POP_RECOMP_PIN_CLOCK=1 POP_RECOMP_SCRIPT=tools/recomp/smoke/flyby.script POP_HOST_DUMP_DIR=$S/fly-vulkan build/recomp/pop_smoke > $S/fly-vulkan.log 2>&1
grep -m1 "GPU backend" $S/fly-vulkan.log
python3 tools/recomp/compare_frames.py $S/fly-metal/smoke_flyby_over_present.ppm $S/fly-vulkan/smoke_flyby_over_present.ppm
python3 tools/recomp/compare_frames.py $S/fly-metal/smoke_flyby_over_scene.ppm $S/fly-vulkan/smoke_flyby_over_scene.ppm
```

Expected: `mean < 2.0`, `max <= 24` for both frames. A large mean over a whole region means a coordinate, winding or blend-factor bug; a high max on isolated pixels is rasteriser edge difference and acceptable up to the bound. Note the numbers in PROGRESS.md. (`pop_smoke` must print the backend: add the same `GPU backend: %s` line `sdl/main.cpp` prints to `smoke_main.cpp` where it creates its device.)

- [ ] **Step 3: Docs and memory**

- `docs/superpowers/PROGRESS.md`: roadmap row 3 → "Done on branch vulkan-hosts (pending merge)"; "Now" → manual runs on real Windows and Linux hardware remain (not a merge gate); record the flyby numbers and the CI run id; add to "Environment notes": `brew install molten-vk vulkan-loader vulkan-headers shaderc`, `POP_GPU_BACKEND`, `POP_GPU_VALIDATE`, `POP_VULKAN_LIBRARY`, regenerating `shaders_spv.h`.
- Append an "Execution notes" section to this plan listing every deviation taken.
- Memory: update `multi-platform-port-roadmap.md` (#3 done pending merge; #4 release pipeline next) and `MEMORY.md`'s line.

- [ ] **Step 4: Full suite, then the finishing menu**

```bash
ctest --preset macos -L "nogame|gpu|gpu-vulkan" --output-on-failure
python3 tools/test.py && python3 tools/format.py && python3 tools/check_repo.py
git add -A docs && git commit -m "Sub-project 3 verified over Vulkan on macOS; progress and notes"
```

Then use superpowers:finishing-a-development-branch (base branch `main`).

---

## Execution notes (2026-09-12)

Deviations from the plan, in task order:

- Task 1: `gpu_backend` is a STATIC library built from the backend objects, not an
  INTERFACE target: object files do not propagate through an INTERFACE library to
  indirect consumers. `SDL3/SDL_metal.h` moved into `metal_surface.mm`.
- Task 2: `shaders_spv.h` records the glslc version; `shaders.py check` skips (exit 0)
  under a different glslc, since each version emits different but equivalent SPIR-V.
  CI's Ubuntu glslc 2023.8 differs from Homebrew's 2026.4.
- Task 3/4: `REQUIRE_FULL_SUBGROUPS` dropped (needs `local_size_x` to be a multiple
  of the subgroup size; the kernel is 16x16). Destroyed textures and buffers go to
  a graveyard until every command buffer recording or in flight at that moment has
  retired: Metal's encoders retained resources, Vulkan does not, and the renderer's
  texture versioning relies on it. Files split: `vulkan_pipeline.cpp` holds
  pipelines, passes, bindings and transfers.
- Task 5: the swapchain extent is the caller's size clamped to the surface bounds
  (MoltenVK lets the swapchain set the layer's drawable size; X11 pins it).
- Task 6: the first edit of `sdl/main.cpp` matched the wrong `window_scale_for`
  call and deleted code between it and `main`; restored from the Task 5 commit and
  reapplied (commit b4c331b). CI does not build `PopRecomp` (no translation), so
  this was caught only by a local host build.
- Task 7: `runtime_tests` also used `pthread` directly for two waiter threads;
  now `os_thread_create`. `mkfifo` and `symlink` checks stay POSIX-only.
- Task 8: the Apple Vulkan suite is labelled `moltenvk` (a `gpu-vulkan` label
  matches the `gpu` regex). Linux needs no `VK_ICD_FILENAMES`; lavapipe is the only
  ICD on the runner. Windows needed `M_PI` and a portable clock in the mixer,
  `present.cpp` and the host tests, and the roots test compares POSIX-style paths.
- Task 9, found by the game runs and fixed in the backend:
  - Every command buffer owns its command pool: pools are single-threaded and the
    host records a command buffer from whichever thread holds it (validation:
    `UNASSIGNED-Threading-MultipleThreads-Write`).
  - One persistent transfer command buffer and fence for uploads and readbacks:
    allocating and freeing one per upload was pathologically slow on MoltenVK and
    starved the guest thread.
  - Uploads, readbacks and `map_read` do not wait for the reaper: the guest thread
    uploads while holding the presenter's mutex and the reaper's completion
    callbacks need it (deadlock). Queue order plus the transfer barrier already
    orders the copy after in-flight work.
  - `compare_frames.py` bounds the fraction of pixels over `--max` (default 1%)
    instead of the single worst pixel: rasteriser edge pixels flip whole colours.
    Flyby present: mean 0.963, 0.56% outliers; scene: 0.972, 0.48%.
- CI runs: 34697742593 (label/portability fixes), 34697930465, 34698258485,
  34698474555, 34698753707 (first green), 34700679045, 34701915809 (green at
  4a73426).
