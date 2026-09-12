# Host Abstraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put the macOS host behind a device-level GPU interface with Metal as its only backend, move the window and input to SDL3, and replace AVAudioEngine and the Apple MIDI synth with a portable software mixer and TinySoundFont, with every existing suite still passing.

**Architecture:** `gpu.h` exposes textures, buffers, pipelines, command buffers, swapchain and a clock as opaque `uint64_t` handles; `gpu/metal/` implements it in Objective-C++, `gpu/fake/` in plain C++ for tests. The D3D renderer, compositor, presenter and overlay become portable C++ over that interface. `host/sdl/` replaces `main.mm`; `host/audio/` replaces `audio.mm` and `midi.mm` behind the unchanged `host_audio_*` and `host_midi_*` contracts.

**Tech Stack:** C++20, Objective-C++ only under `gpu/metal/`, SDL3 `release-3.4.16` via CMake FetchContent, TinySoundFont (vendored, MIT), CMake presets and CTest labels from sub-project 1.

**Spec:** `docs/superpowers/specs/2026-09-12-host-abstraction-design.md`

## Global Constraints

- No AppKit, AVFoundation, AudioToolbox, CoreVideo or CoreText code outside `src/recomp/host/gpu/metal/` when the plan is complete. During Steps 2 and 3 a temporary `gpu/metal/metal_bridge.h` lets the still-Metal renderer and the still-AppKit `main.mm` exchange native objects with the new layers; it is deleted in Task 7.
- The `host_*` callback contract in `src/recomp/dx/host_api.h` and the `host_midi_*` contract in `src/recomp/runtime/win32.h` do not change.
- SDL3 at tag `release-3.4.16`, fetched by `cmake/Dependencies.cmake` with the tag pinned, built static; not vendored. TinySoundFont vendored as `third_party/tsf/tsf.h` with its MIT license; `NOTICE` gains an entry.
- Texture lifetime: a texture lives until `destroy`; the presenter's leases are the only owner of textures stored in a `Frame` or the scene history. No texture pointer identity anywhere; handle ids are the lease keys.
- Command buffers committed on one device complete in commit order. The interface states it; the Metal backend guarantees it with one queue.
- Optimization and warning sets as sub-project 1 fixed them; new host files take `POP_WARN_HOST` unless stated.
- Format touched native code with `.venv/bin/python tools/format.py --write`; never format `third_party/`.
- Each task ends with `.venv/bin/python tools/check_repo.py`, a commit, and an update to `docs/superpowers/PROGRESS.md`'s "Now" section naming the task just finished.
- CTest labels: `nogame` (no game, no GPU), `gpu` (Metal device), `device` (real audio device too), `game`, `mods`. Tests that run on the fake GPU backend are `nogame`.

## Environment for the executor

- macOS, Xcode clang 21, CMake 4.1 and Ninja in `.venv`. The checkout links the game (`original/gog`) and listings (`analysis`), so every suite runs here: `tools/test.py --native`, `--mods`, `--gameplay`, `sh src/recomp/host/tests/integration_tests.sh`, `sh tools/recomp/mods_test.sh`. Known pre-existing failures are listed in `PROGRESS.md`; anything else is new.
- Branch `host-abstraction`, forked from `main` at 920de70. Push with `git push origin host-abstraction`; run CI with `gh workflow run checks.yml --ref host-abstraction` and confirm the run's `headSha` is `HEAD`.
- The app is launched by the user for the manual checklist; do not `open build/PopRecomp.app` from the session. The smoke host, headless host and all test binaries may be run.

## File structure

| Path | Responsibility |
| --- | --- |
| `cmake/Dependencies.cmake` | SDL3 FetchContent, pinned; `pop_link_sdl(target)` |
| `third_party/tsf/tsf.h`, `third_party/tsf/LICENSE` | TinySoundFont |
| `src/recomp/host/gpu/gpu.h` | `GpuDevice`, handles, enums, `RenderState`, attachments; the contract comments |
| `src/recomp/host/gpu/shaders.md` | per-shader uniform, attribute, texture and buffer slot contract |
| `src/recomp/host/gpu/fake/fake_device.{h,cpp}` | CPU-backed implementation for tests |
| `src/recomp/host/gpu/fake/tests/gpu_fake_tests.cpp` | its contract test |
| `src/recomp/host/gpu/metal/metal_device.{h,mm}` | Metal implementation: tables, pipelines, command buffers |
| `src/recomp/host/gpu/metal/metal_surface.mm` | `CAMetalLayer` swapchain |
| `src/recomp/host/gpu/metal/shaders_msl.h` | the three MSL sources, verbatim |
| `src/recomp/host/gpu/metal/metal_bridge.h` | temporary: native object import/export during Steps 2 and 3 |
| `src/recomp/host/gpu/gpu_factory.{h,cpp}` | `gpu_create_default_device()` selecting Metal on Apple |
| `src/recomp/host/present_frame.h` | `HostSceneTarget`, `CompositorInput`, `CompositorSceneHistory`, `Frame` on handles |
| `src/recomp/host/compositor.cpp` | was `compositor.mm`; one render pass over `gpu.h` |
| `src/recomp/host/present_thread.cpp` | was `present_thread.mm`; state machine over `gpu.h` |
| `src/recomp/host/performance_overlay.{h,cpp}` | bitmap-font HUD over `gpu.h` |
| `src/recomp/host/d3d_render.{h,cpp}` | was `d3d_render.mm`; `D3DRenderer` class over `gpu.h` |
| `src/recomp/host/present.cpp`, `ui_layer.cpp`, `input.cpp`, `smoke_main.cpp` | renamed; `input.cpp` carries the SDL scancode table |
| `src/recomp/host/sdl/main.cpp` | the SDL3 host |
| `src/recomp/host/sdl/events.{h,cpp}` | SDL events to `host_input`/`host_gate` calls |
| `src/recomp/host/audio/mixer.{h,cpp}` | every `host_audio_*` callback |
| `src/recomp/host/audio/sink.h`, `sdl_sink.cpp`, `offline_sink.cpp` | output sinks |
| `src/recomp/host/audio/midi_synth.cpp` | `host_midi_*` over TinySoundFont |
| `src/recomp/host/audio/tests/audio_tests.cpp`, `tests/fixtures/make_sf2.py` | ported audio suite and its SoundFont fixture |
| `src/recomp/host/tests/presenter_tests.cpp`, `compositor_tests.cpp`, `d3d_render_tests.cpp`, `host_tests.cpp` | the split host suite |

---

### Task 1: Dependencies and the progress tracker

**Files:**
- Create: `cmake/Dependencies.cmake`, `third_party/tsf/tsf.h`, `third_party/tsf/LICENSE`
- Modify: `CMakeLists.txt`, `NOTICE`, `.gitignore`, `docs/superpowers/PROGRESS.md`

**Interfaces:**
- Produces: CMake target `SDL3::SDL3-static` available after `include(cmake/Dependencies.cmake)`; function `pop_link_sdl(target)`; header `third_party/tsf/tsf.h` with `TSF_IMPLEMENTATION` defined in exactly one translation unit (Task 10).

- [ ] **Step 1: Write `cmake/Dependencies.cmake`**

```cmake
# Third-party code that is fetched rather than vendored. Pinned to a tag, never
# a branch, so a configure today and a configure next year build the same bytes.
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Subsystems the host does not use stay out of the binary.
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.16
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# pop_link_sdl(<target>): link SDL3 statically and give the target its headers.
function(pop_link_sdl target)
  target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
endfunction()
```

In the root `CMakeLists.txt` add `include(cmake/Dependencies.cmake)` after `include(cmake/MacBundle.cmake)`. FetchContent writes under `build/cmake/<preset>/_deps`, which is inside the ignored `build/`.

- [ ] **Step 2: Vendor TinySoundFont**

```sh
mkdir -p third_party/tsf
curl -fsSL -o third_party/tsf/tsf.h https://raw.githubusercontent.com/schellingb/TinySoundFont/master/tsf.h
curl -fsSL -o third_party/tsf/LICENSE https://raw.githubusercontent.com/schellingb/TinySoundFont/master/LICENSE
head -3 third_party/tsf/tsf.h; grep -c 'TSFDEF' third_party/tsf/tsf.h
```
Expected: the header starts with its own comment block and contains the `TSFDEF` declarations (dozens). Record the commit you fetched (`gh api repos/schellingb/TinySoundFont/commits/master --jq .sha`) in `third_party/tsf/LICENSE`'s first line as `# TinySoundFont <sha>, fetched 2026-09-12`.

Append to `NOTICE`:

```text
TinySoundFont (third_party/tsf/tsf.h) is Copyright (C) 2017-2026 Bernhard Schelling,
licensed under the MIT License; see third_party/tsf/LICENSE. It renders the game's
own SoundFont, which is not included.

SDL3 is fetched at build time from https://github.com/libsdl-org/SDL at a pinned
release tag and linked statically; it is licensed under the zlib License.
```

- [ ] **Step 3: Configure to prove the fetch works**

Run: `.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python 2>&1 | grep -E 'SDL3|Error' ; .venv/bin/cmake --build --preset macos --target SDL3-static 2>&1 | tail -2`
Expected: configure succeeds and `libSDL3.a` builds under `build/cmake/macos/_deps/sdl3-build/`. Then `.venv/bin/cmake --build --preset macos --target check_binaries && .venv/bin/ctest --preset macos -L "nogame|gpu|device"` still passes (9 entries).

- [ ] **Step 4: Update `PROGRESS.md` and commit**

In `docs/superpowers/PROGRESS.md`'s "Now" section write: "Branch `host-abstraction`. Plan `plans/2026-09-12-host-abstraction.md`. Task 1 (dependencies) done; next Task 2 (GPU interface + fake backend)." Then:

```sh
.venv/bin/python tools/check_repo.py
git add cmake/Dependencies.cmake CMakeLists.txt third_party/tsf NOTICE docs/superpowers/PROGRESS.md
git commit -m "Fetch SDL3 by pinned tag and vendor TinySoundFont"
```

---

### Task 2: The GPU interface and the fake backend

**Files:**
- Create: `src/recomp/host/gpu/gpu.h`, `src/recomp/host/gpu/shaders.md`, `src/recomp/host/gpu/fake/fake_device.h`, `src/recomp/host/gpu/fake/fake_device.cpp`, `src/recomp/host/gpu/fake/tests/gpu_fake_tests.cpp`, `src/recomp/host/gpu/CMakeLists.txt`
- Modify: `src/recomp/host/CMakeLists.txt`

**Interfaces:**
- Produces: everything in `gpu.h` below; `FakeDevice` with `advance_clock`, `texture_bytes`, `draws_recorded`; CMake object library `gpu_interface` (header-only marker) and `gpu_fake`.

- [ ] **Step 1: Write `src/recomp/host/gpu/gpu.h`**

```cpp
// gpu.h - the device-level GPU interface every host backend implements.
//
// Handles, not native objects, cross this boundary: a GpuTexture is a 64-bit
// id the backend maps to its own object, and that id is also what the
// presenter leases and compares. A texture lives until destroy(); nothing is
// freed by a handle going out of scope, so the owner of a lease is the owner
// of the texture.
//
// One queue. Command buffers committed on a device complete in the order they
// were committed. The renderer and presenter rely on that so a present can
// never run ahead of the scene it shows; a backend that cannot promise it with
// one queue must enforce it with its own synchronisation.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace gpu {

struct Texture { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct Buffer { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct Pipeline { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct CommandBuffer { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct Swapchain { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
inline bool operator==(Texture a, Texture b) { return a.id == b.id; }
inline bool operator!=(Texture a, Texture b) { return a.id != b.id; }

enum class Format { BGRA8, RGBA8, R8, Depth32F };
enum Usage : uint32_t {
    UsageSampled = 1,      // read by shaders
    UsageRenderTarget = 2, // written by render passes
    UsageStorage = 4,      // read/written by compute
    UsageCpu = 8,          // uploaded from and read back to the CPU
};
struct TextureDesc {
    int width = 0, height = 0;
    Format format = Format::RGBA8;
    uint32_t usage = UsageSampled;
    int mip_levels = 1; // 1 or the full chain
};
struct Region { int x = 0, y = 0, w = 0, h = 0; };

enum class Blend { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
                   SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor };
enum class Compare { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Cull { None, Front, Back };
enum class Filter { Nearest, Linear };
enum class MipFilter { None, Nearest, Linear };
enum class Address { Repeat, ClampToEdge, MirrorRepeat };
enum class Primitive { Points, Lines, Triangles, TriangleStrip };
enum class Load { Load, Clear, DontCare };
enum class Store { Store, DontCare };
enum class Stage { Vertex, Fragment, Compute };

// What a render pipeline needs besides its shader pair. Two colour attachments
// at most: colour and the coverage mask the D3D renderer writes.
struct RenderState {
    Format color_format[2] = {Format::BGRA8, Format::R8};
    int color_count = 1;
    bool depth_attachment = false;
    bool blend_enabled = false;
    Blend src_rgb = Blend::One, dst_rgb = Blend::Zero;
    Blend src_alpha = Blend::One, dst_alpha = Blend::Zero;
    bool write_color = true;    // colour write mask for attachment 0 and 1 together
    bool writes_point_size = false;
    uint64_t key() const; // a total order over the fields above, for caches
};
struct DepthState { Compare compare = Compare::Always; bool write = false; };
struct SamplerState {
    Address u = Address::ClampToEdge, v = Address::ClampToEdge;
    Filter mag = Filter::Nearest, min = Filter::Nearest;
    MipFilter mip = MipFilter::None;
    int anisotropy = 1;
};

struct ColorAttachment {
    Texture texture;
    Load load = Load::Load;
    Store store = Store::Store;
    float clear[4] = {0, 0, 0, 1};
};
struct DepthAttachment {
    Texture texture;
    Load load = Load::Load;
    Store store = Store::Store;
    float clear = 1.0f;
};
struct RenderPass {
    ColorAttachment color[2];
    int color_count = 1;
    DepthAttachment depth; // texture.id == 0 for none
};
struct Viewport { double x, y, w, h, near_z, far_z; };

enum class CommandStatus { Completed, Error };

class Device {
  public:
    virtual ~Device() = default;

    // --- textures ---
    virtual Texture create_texture(const TextureDesc &desc) = 0;
    // `pitch` is bytes per row of `bytes`; level 0 unless `level` says otherwise.
    virtual bool upload(Texture t, Region region, const void *bytes, int pitch, int level = 0) = 0;
    // Waits for every committed command that writes `t`, then copies out.
    virtual bool readback(Texture t, Region region, void *bytes, int pitch) = 0;
    virtual void destroy(Texture t) = 0;
    virtual TextureDesc describe(Texture t) = 0; // width 0 for an unknown handle
    // Device bytes the texture occupies, for the HD cache budget.
    virtual uint64_t allocated_bytes(Texture t) = 0;

    // --- buffers ---
    virtual Buffer create_buffer(uint64_t bytes, const void *contents) = 0;
    virtual void update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) = 0;
    // Read back a CPU-visible buffer's contents after the commands writing it completed.
    virtual const void *map_read(Buffer b) = 0;
    virtual uint64_t buffer_bytes(Buffer b) = 0;
    virtual void destroy(Buffer b) = 0;

    // --- pipelines ---
    // `shader` names a vertex/fragment pair in gpu/shaders.md ("d3d", "surface_upload",
    // "compositor", "hud"). The backend caches by (shader, state.key()).
    virtual Pipeline render_pipeline(const std::string &shader, const RenderState &state) = 0;
    // `shader` names a compute kernel ("guest_readback", "guest_readback_fused", "native_brightness").
    virtual Pipeline compute_pipeline(const std::string &shader) = 0;
    // Thread execution width of a compute pipeline, for SIMD-group sized dispatches.
    virtual int thread_execution_width(Pipeline p) = 0;

    // --- command buffers ---
    virtual CommandBuffer begin() = 0;
    virtual void begin_render_pass(CommandBuffer cb, const RenderPass &pass) = 0;
    virtual void set_pipeline(CommandBuffer cb, Pipeline p) = 0;
    virtual void set_depth(CommandBuffer cb, const DepthState &d) = 0;
    virtual void set_cull(CommandBuffer cb, Cull c) = 0;
    virtual void set_viewport(CommandBuffer cb, const Viewport &v) = 0;
    virtual void set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) = 0;
    // Inline data, at most 4 KB, copied at call time.
    virtual void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes, uint64_t count) = 0;
    virtual void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) = 0;
    virtual void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) = 0;
    virtual void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) = 0;
    virtual void draw(CommandBuffer cb, Primitive primitive, int first, int count) = 0;
    virtual void end_render_pass(CommandBuffer cb) = 0;

    virtual void begin_compute_pass(CommandBuffer cb) = 0;
    // Threads, not groups: the backend rounds up into `group` sized groups.
    virtual void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) = 0;
    virtual void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) = 0;
    virtual void end_compute_pass(CommandBuffer cb) = 0;

    virtual void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x, int dst_y) = 0;
    virtual void generate_mipmaps(CommandBuffer cb, Texture t) = 0;

    // Runs on a backend thread after the buffer finishes; `status` says how.
    virtual void on_complete(CommandBuffer cb, std::function<void(CommandStatus, double gpu_ms)> fn) = 0;
    virtual void commit(CommandBuffer cb) = 0;
    virtual void wait(CommandBuffer cb) = 0; // blocks until complete
    virtual CommandStatus status(CommandBuffer cb) = 0;

    // --- swapchain ---
    // `native_surface` is what the window layer hands over: a CAMetalLayer* on
    // macOS. Null on failure.
    virtual Swapchain create_swapchain(void *native_surface, int width, int height) = 0;
    virtual void resize(Swapchain s, int width, int height) = 0;
    virtual Format swapchain_format(Swapchain s) = 0;
    // A texture to render this frame into, or a null handle when none is available.
    // The texture is valid until present() or release_drawable().
    virtual Texture acquire(Swapchain s) = 0;
    virtual void release_drawable(Swapchain s, Texture t) = 0; // acquired but not presented
    // Schedules the present behind `cb`; `presented` runs with the display time
    // in device-clock seconds, or 0 when the display never reported it.
    virtual void present(CommandBuffer cb, Swapchain s, Texture t, double min_duration_seconds,
                         std::function<void(double presented_seconds)> presented) = 0;
    virtual double refresh_period(Swapchain s) = 0; // seconds; 1/60 when unknown
    virtual void destroy(Swapchain s) = 0;

    // --- clock ---
    virtual double now_seconds() = 0; // monotonic, the base every callback timestamp uses
};

} // namespace gpu
```

Add at the end, outside the class, the one non-virtual helper the caches use:

```cpp
namespace gpu {
inline uint64_t RenderState::key() const {
    uint64_t k = 0;
    k |= uint64_t(color_format[0]) << 0;
    k |= uint64_t(color_format[1]) << 4;
    k |= uint64_t(color_count) << 8;
    k |= uint64_t(depth_attachment) << 10;
    k |= uint64_t(blend_enabled) << 11;
    k |= uint64_t(src_rgb) << 12;
    k |= uint64_t(dst_rgb) << 16;
    k |= uint64_t(src_alpha) << 20;
    k |= uint64_t(dst_alpha) << 24;
    k |= uint64_t(write_color) << 28;
    k |= uint64_t(writes_point_size) << 29;
    return k;
}
} // namespace gpu
```

- [ ] **Step 2: Write `src/recomp/host/gpu/shaders.md`**

```markdown
# Shader contract

Every backend supplies these programs under these names. Slot numbers are the
binding indices `gpu.h` calls use; layouts are the structs the portable code
fills. A backend may implement a program in any language but must read exactly
these inputs.

## `d3d` (render pipeline)

- Vertex buffer slot 0: `HostD3DVertex` (14 floats: x y z w, u v, r g b a, sr sg sb sa), `d3d_render.h`.
- Vertex and fragment bytes slot 1: `D3DUniforms` (`d3d_render.h`): `float mvp[16]` column-major, then
  `uint32 pretransformed, textured, texblend, alphatest, alphafunc; float alpharef; uint32 specular,
  texture_has_alpha, fogmode; float fogstart, fogend, fogdensity, fogr, fogg, fogb, pointsize;
  uint32 terrain_detail`. 112 bytes, no padding.
- Fragment texture 0 + sampler 0: the draw's texture. Fragment texture 1 + sampler 1: terrain detail.
- Outputs: colour attachment 0 (BGRA8) and coverage attachment 1 (R8, 1.0 for every surviving fragment).
- Semantics: texblend cases 1/7 decal, 2 modulate, 3 decal alpha, 4 modulate alpha, 5 decal mask,
  8 add; alpha test with D3D compare functions 1..8; fog modes 1 vertex (specular alpha), 2 exp,
  3 exp2, 4 linear; terrain detail modulates by chroma-derived material weights.

## `surface_upload` (render pipeline)

- No vertex buffer: the vertex program emits a fullscreen triangle from the vertex id (0..2).
- Fragment buffer slot 0: the guest surface bytes. Fragment bytes slot 1: `uint32 p[20]`:
  `guest_w, guest_h, native_w, native_h, pitch, bpp, has_palette, 0, rmask, gmask, bmask, 0,
  rshift, gshift, bshift, 0, rmax, gmax, bmax, 0`. Fragment bytes slot 2: `uint32 palette[256]` (0x00RRGGBB).
- Output: colour attachment 0 (BGRA8): the guest pixel at `floor(x * guest / native)`.

## `compositor` (render pipeline)

- No vertex buffer: 4 vertices by id, triangle strip.
- Vertex and fragment bytes slot 0: `CompositorQuad { float rect[4]; float uv[4]; float drawable[2];
  uint32 opaque, pad; }` (48 bytes). rect in drawable pixels, y down; uv normalised.
- Fragment texture 0, nearest, clamp to edge. Output: colour attachment 0 in the target's format;
  `opaque` forces alpha to 1.

## `hud` (render pipeline)

- No vertex buffer: 4 vertices by id, triangle strip. Vertex bytes slot 0: `float rect[4]` in NDC
  (x, y, w, h with h negative for y-down). Fragment texture 0, linear sampling. Premultiplied blend.

## `guest_readback`, `guest_readback_fused` (compute)

- Texture 0 colour (read), texture 1 coverage (read). Buffer 0: `uint32 out[2 * pixels]`.
- Bytes slot 1: `uint32 p[12]`: `x0, y0, x1, y1` guest rect, `native_w, native_h, guest_w, guest_h`,
  `offset` into `out` in pixels, `edge_x1, edge_y1` native edges, `0`.
- Writes `out[2*i] = bgr | (covered << 24)`, `out[2*i+1] = lit count` (fused) or 0.
- Dispatched with `dispatch_threads(x1-x0, y1-y0, 8, 8)`.

## `native_brightness` (compute)

- Texture 0 colour. Buffer 0: `uint32 sums[]`. Bytes slot 1: `uint32 p[8]`: `x0, y0, x1, y1, offset,
  groups_x, simdgroups, 0`. Dispatched with `dispatch_groups(gx, gy, 16, 16)`; each 16x16 group
  covers a 32x32 native tile and reduces per SIMD group, lane 0 writing `sums[offset + ...]`.
  A backend without subgroup operations may reduce through shared memory; the output layout is
  what matters.
```

- [ ] **Step 3: Write the failing fake-backend test**

`src/recomp/host/gpu/fake/tests/gpu_fake_tests.cpp`:

```cpp
// gpu_fake_tests.cpp - the fake backend's own contract, so the presenter and
// compositor tests that run on it can trust what it reports.
#include "../fake_device.h"

#include <stdio.h>
#include <string.h>
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

using namespace gpu;

static void test_texture_round_trip() {
    FakeDevice d;
    Texture t = d.create_texture({4, 2, Format::RGBA8, UsageSampled | UsageCpu});
    CHECK(t);
    CHECK(d.describe(t).width == 4 && d.describe(t).height == 2);
    uint8_t in[4 * 2 * 4];
    for (int i = 0; i < 32; ++i)
        in[i] = uint8_t(i);
    CHECK(d.upload(t, {0, 0, 4, 2}, in, 16));
    uint8_t out[32] = {0};
    CHECK(d.readback(t, {0, 0, 4, 2}, out, 16));
    CHECK(memcmp(in, out, 32) == 0);
    uint8_t corner[4] = {0};
    CHECK(d.readback(t, {3, 1, 1, 1}, corner, 4));
    CHECK(corner[0] == 28 && corner[3] == 31);
    d.destroy(t);
    CHECK(d.describe(t).width == 0);
    CHECK(!d.upload(t, {0, 0, 1, 1}, in, 4));
}

static void test_commands_complete_in_commit_order() {
    FakeDevice d;
    std::vector<int> order;
    CommandBuffer a = d.begin(), b = d.begin();
    d.on_complete(b, [&](CommandStatus, double) { order.push_back(2); });
    d.on_complete(a, [&](CommandStatus, double) { order.push_back(1); });
    d.commit(a);
    d.commit(b);
    CHECK(order.size() == 2 && order[0] == 1 && order[1] == 2);
    CHECK(d.status(a) == CommandStatus::Completed);
}

static void test_render_pass_records_draws_and_clears() {
    FakeDevice d;
    Texture t = d.create_texture({8, 8, Format::BGRA8, UsageRenderTarget | UsageCpu});
    RenderPass pass;
    pass.color[0].texture = t;
    pass.color[0].load = Load::Clear;
    pass.color[0].clear[0] = 1.0f; // red
    pass.color[0].clear[1] = 0.0f;
    pass.color[0].clear[2] = 0.0f;
    CommandBuffer cb = d.begin();
    d.begin_render_pass(cb, pass);
    d.set_pipeline(cb, d.render_pipeline("compositor", RenderState{}));
    d.draw(cb, Primitive::TriangleStrip, 0, 4);
    d.end_render_pass(cb);
    d.commit(cb);
    CHECK(d.draws_recorded(cb) == 1);
    uint8_t px[4];
    CHECK(d.readback(t, {0, 0, 1, 1}, px, 4));
    CHECK(px[2] == 255 && px[1] == 0 && px[0] == 0); // BGRA: red in byte 2
}

static void test_blit_copies_bytes() {
    FakeDevice d;
    Texture a = d.create_texture({2, 2, Format::RGBA8, UsageSampled | UsageCpu});
    Texture b = d.create_texture({4, 4, Format::RGBA8, UsageRenderTarget | UsageCpu});
    uint8_t in[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    d.upload(a, {0, 0, 2, 2}, in, 8);
    CommandBuffer cb = d.begin();
    d.blit(cb, a, {0, 0, 2, 2}, b, 1, 1);
    d.commit(cb);
    uint8_t out[4];
    d.readback(b, {2, 2, 1, 1}, out, 4);
    CHECK(out[0] == 13 && out[3] == 16);
}

static void test_swapchain_and_clock() {
    FakeDevice d;
    Swapchain s = d.create_swapchain(nullptr, 16, 8);
    CHECK(s);
    Texture t = d.acquire(s);
    CHECK(t && d.describe(t).width == 16 && d.describe(t).height == 8);
    double shown = -1;
    CommandBuffer cb = d.begin();
    d.advance_clock(0.5);
    d.present(cb, s, t, 1.0 / 60, [&](double ts) { shown = ts; });
    d.commit(cb);
    CHECK(shown == 0.5);
    CHECK(d.now_seconds() == 0.5);
    CHECK(d.refresh_period(s) == 1.0 / 60);
    d.resize(s, 32, 16);
    CHECK(d.describe(d.acquire(s)).width == 32);
    d.destroy(s);
}

static void test_pipeline_cache_and_key() {
    FakeDevice d;
    RenderState a, b;
    b.blend_enabled = true;
    CHECK(a.key() != b.key());
    Pipeline p1 = d.render_pipeline("d3d", a), p2 = d.render_pipeline("d3d", a);
    CHECK(p1.id == p2.id);
    CHECK(d.render_pipeline("d3d", b).id != p1.id);
    CHECK(d.compute_pipeline("guest_readback"));
    CHECK(d.thread_execution_width(d.compute_pipeline("native_brightness")) == 32);
}

int main() {
    test_texture_round_trip();
    test_commands_complete_in_commit_order();
    test_render_pass_records_draws_and_clears();
    test_blit_copies_bytes();
    test_swapchain_and_clock();
    test_pipeline_cache_and_key();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all gpu fake tests passed\n");
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 4: Write `src/recomp/host/gpu/CMakeLists.txt` and wire it**

```cmake
# The GPU interface, its test double, and the platform backend.
add_library(gpu_fake OBJECT fake/fake_device.cpp)
target_include_directories(gpu_fake PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gpu_fake PRIVATE ${POP_WARN_STRICT})
pop_optimize(gpu_fake 1)

add_executable(gpu_fake_tests fake/tests/gpu_fake_tests.cpp)
target_link_libraries(gpu_fake_tests PRIVATE gpu_fake)
target_compile_options(gpu_fake_tests PRIVATE ${POP_WARN_STRICT})
pop_optimize(gpu_fake_tests 1)
pop_test_binary(gpu_fake_tests)
add_test(NAME gpu_fake_tests COMMAND gpu_fake_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(gpu_fake_tests PROPERTIES LABELS nogame)
```

In `src/recomp/host/CMakeLists.txt`, immediately after the `host_common` definition and before `if(NOT APPLE) return()`, add `add_subdirectory(gpu)`.

Run: `.venv/bin/cmake --preset macos && .venv/bin/cmake --build --preset macos --target gpu_fake_tests`
Expected: fails, `fake_device.h` not found.

- [ ] **Step 5: Write the fake backend**

`src/recomp/host/gpu/fake/fake_device.h`:

```cpp
// fake_device.h - gpu::Device with CPU storage and immediate completion, for
// tests that need the presenter, compositor or renderer logic and not a GPU.
// Passes execute nothing; a Clear load action fills the attachment, a blit
// copies bytes, draws and dispatches are counted. Time is whatever the test
// says it is.
#pragma once
#include "../gpu.h"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace gpu {

class FakeDevice final : public Device {
  public:
    Texture create_texture(const TextureDesc &desc) override;
    bool upload(Texture t, Region region, const void *bytes, int pitch, int level) override;
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
    void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes, uint64_t count) override;
    void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) override;
    void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) override;
    void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) override;
    void draw(CommandBuffer cb, Primitive primitive, int first, int count) override;
    void end_render_pass(CommandBuffer cb) override;
    void begin_compute_pass(CommandBuffer cb) override;
    void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) override;
    void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) override;
    void end_compute_pass(CommandBuffer cb) override;
    void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x, int dst_y) override;
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

    // --- test controls ---
    void advance_clock(double seconds);
    void set_clock(double seconds);
    int draws_recorded(CommandBuffer cb) const;
    int dispatches_recorded(CommandBuffer cb) const;
    // Whether the next N texture allocations fail, for the presenter's halving test.
    void fail_next_allocations(int n) { fail_allocations_ = n; }
    // Hold completion callbacks instead of running them at commit; `complete_all` runs them.
    void set_manual_completion(bool manual) { manual_completion_ = manual; }
    void complete_all();
    // The last present's minimum duration, for the pacing selector test.
    double last_present_min_duration() const { return last_min_duration_; }
    // Fire the presented callback of the most recent present with this time.
    void fire_presented(double presented_seconds);
    int live_textures() const;

  private:
    struct Tex { TextureDesc desc; std::vector<std::vector<uint8_t>> levels; };
    struct Buf { std::vector<uint8_t> bytes; };
    struct Cmd {
        bool committed = false;
        int draws = 0, dispatches = 0;
        std::vector<std::function<void(CommandStatus, double)>> on_complete;
        std::function<void()> deferred_present;
        RenderPass pass;
        bool in_pass = false;
    };
    struct Chain { int w, h; Format format = Format::BGRA8; std::vector<Texture> acquired; };
    static int bytes_per_pixel(Format f);
    Tex *tex(Texture t);
    mutable std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::map<uint64_t, Tex> textures_;
    std::map<uint64_t, Buf> buffers_;
    std::map<uint64_t, Cmd> commands_;
    std::map<uint64_t, Chain> swapchains_;
    std::map<std::string, std::map<uint64_t, Pipeline>> render_pipelines_;
    std::map<std::string, Pipeline> compute_pipelines_;
    std::vector<uint64_t> committed_order_;
    double clock_ = 0;
    int fail_allocations_ = 0;
    bool manual_completion_ = false;
    double last_min_duration_ = -1;
    std::function<void(double)> last_presented_;
};

} // namespace gpu
```

`src/recomp/host/gpu/fake/fake_device.cpp`:

```cpp
#include "fake_device.h"

#include <algorithm>
#include <cstring>

namespace gpu {

int FakeDevice::bytes_per_pixel(Format f) {
    switch (f) {
    case Format::R8:
        return 1;
    case Format::Depth32F:
        return 4;
    default:
        return 4;
    }
}
FakeDevice::Tex *FakeDevice::tex(Texture t) {
    auto it = textures_.find(t.id);
    return it == textures_.end() ? nullptr : &it->second;
}

Texture FakeDevice::create_texture(const TextureDesc &desc) {
    std::lock_guard lock(mutex_);
    if (desc.width <= 0 || desc.height <= 0)
        return {};
    if (fail_allocations_ > 0) {
        --fail_allocations_;
        return {};
    }
    Tex t;
    t.desc = desc;
    int w = desc.width, h = desc.height;
    for (int level = 0; level < std::max(1, desc.mip_levels); ++level) {
        t.levels.emplace_back(size_t(w) * h * bytes_per_pixel(desc.format), 0);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    uint64_t id = next_id_++;
    textures_[id] = std::move(t);
    return {id};
}
bool FakeDevice::upload(Texture tex_handle, Region r, const void *bytes, int pitch, int level) {
    std::lock_guard lock(mutex_);
    Tex *t = tex(tex_handle);
    if (!t || level < 0 || level >= int(t->levels.size()) || !bytes)
        return false;
    int w = std::max(1, t->desc.width >> level), h = std::max(1, t->desc.height >> level);
    if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h)
        return false;
    const int bpp = bytes_per_pixel(t->desc.format);
    for (int y = 0; y < r.h; ++y)
        memcpy(&t->levels[level][(size_t(r.y + y) * w + r.x) * bpp],
               static_cast<const uint8_t *>(bytes) + size_t(y) * pitch, size_t(r.w) * bpp);
    return true;
}
bool FakeDevice::readback(Texture tex_handle, Region r, void *bytes, int pitch) {
    std::lock_guard lock(mutex_);
    Tex *t = tex(tex_handle);
    if (!t || !bytes || r.x < 0 || r.y < 0 || r.x + r.w > t->desc.width || r.y + r.h > t->desc.height)
        return false;
    const int bpp = bytes_per_pixel(t->desc.format);
    for (int y = 0; y < r.h; ++y)
        memcpy(static_cast<uint8_t *>(bytes) + size_t(y) * pitch,
               &t->levels[0][(size_t(r.y + y) * t->desc.width + r.x) * bpp], size_t(r.w) * bpp);
    return true;
}
void FakeDevice::destroy(Texture t) {
    std::lock_guard lock(mutex_);
    textures_.erase(t.id);
}
TextureDesc FakeDevice::describe(Texture t) {
    std::lock_guard lock(mutex_);
    Tex *p = tex(t);
    return p ? p->desc : TextureDesc{};
}
uint64_t FakeDevice::allocated_bytes(Texture t) {
    std::lock_guard lock(mutex_);
    Tex *p = tex(t);
    uint64_t total = 0;
    if (p)
        for (auto &level : p->levels)
            total += level.size();
    return total;
}

Buffer FakeDevice::create_buffer(uint64_t bytes, const void *contents) {
    std::lock_guard lock(mutex_);
    Buf b;
    b.bytes.assign(bytes, 0);
    if (contents)
        memcpy(b.bytes.data(), contents, bytes);
    uint64_t id = next_id_++;
    buffers_[id] = std::move(b);
    return {id};
}
void FakeDevice::update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    if (it != buffers_.end() && offset + count <= it->second.bytes.size())
        memcpy(it->second.bytes.data() + offset, bytes, count);
}
const void *FakeDevice::map_read(Buffer b) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    return it == buffers_.end() ? nullptr : it->second.bytes.data();
}
uint64_t FakeDevice::buffer_bytes(Buffer b) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    return it == buffers_.end() ? 0 : it->second.bytes.size();
}
void FakeDevice::destroy(Buffer b) {
    std::lock_guard lock(mutex_);
    buffers_.erase(b.id);
}

Pipeline FakeDevice::render_pipeline(const std::string &shader, const RenderState &state) {
    std::lock_guard lock(mutex_);
    auto &by_key = render_pipelines_[shader];
    auto it = by_key.find(state.key());
    if (it != by_key.end())
        return it->second;
    Pipeline p{next_id_++};
    by_key[state.key()] = p;
    return p;
}
Pipeline FakeDevice::compute_pipeline(const std::string &shader) {
    std::lock_guard lock(mutex_);
    auto it = compute_pipelines_.find(shader);
    if (it != compute_pipelines_.end())
        return it->second;
    Pipeline p{next_id_++};
    compute_pipelines_[shader] = p;
    return p;
}
int FakeDevice::thread_execution_width(Pipeline) {
    return 32;
}

CommandBuffer FakeDevice::begin() {
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    commands_[id] = Cmd{};
    return {id};
}
void FakeDevice::begin_render_pass(CommandBuffer cb, const RenderPass &pass) {
    std::lock_guard lock(mutex_);
    auto &c = commands_[cb.id];
    c.pass = pass;
    c.in_pass = true;
    // A Clear load action is the one thing a pass does that a test can see.
    for (int i = 0; i < pass.color_count; ++i) {
        Tex *t = tex(pass.color[i].texture);
        if (!t || pass.color[i].load != Load::Clear)
            continue;
        const int bpp = bytes_per_pixel(t->desc.format);
        uint8_t px[4] = {0, 0, 0, 0};
        if (t->desc.format == Format::BGRA8) {
            px[0] = uint8_t(pass.color[i].clear[2] * 255 + 0.5f);
            px[1] = uint8_t(pass.color[i].clear[1] * 255 + 0.5f);
            px[2] = uint8_t(pass.color[i].clear[0] * 255 + 0.5f);
            px[3] = uint8_t(pass.color[i].clear[3] * 255 + 0.5f);
        } else {
            for (int k = 0; k < 4; ++k)
                px[k] = uint8_t(pass.color[i].clear[k] * 255 + 0.5f);
        }
        for (size_t at = 0; at + bpp <= t->levels[0].size(); at += bpp)
            memcpy(&t->levels[0][at], px, bpp);
    }
}
void FakeDevice::set_pipeline(CommandBuffer, Pipeline) {}
void FakeDevice::set_depth(CommandBuffer, const DepthState &) {}
void FakeDevice::set_cull(CommandBuffer, Cull) {}
void FakeDevice::set_viewport(CommandBuffer, const Viewport &) {}
void FakeDevice::set_vertex_buffer(CommandBuffer, int, Buffer, uint64_t) {}
void FakeDevice::set_bytes(CommandBuffer, Stage, int, const void *, uint64_t) {}
void FakeDevice::set_buffer(CommandBuffer, Stage, int, Buffer, uint64_t) {}
void FakeDevice::set_texture(CommandBuffer, Stage, int, Texture) {}
void FakeDevice::set_sampler(CommandBuffer, Stage, int, const SamplerState &) {}
void FakeDevice::draw(CommandBuffer cb, Primitive, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].draws;
}
void FakeDevice::end_render_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    commands_[cb.id].in_pass = false;
}
void FakeDevice::begin_compute_pass(CommandBuffer) {}
void FakeDevice::dispatch_threads(CommandBuffer cb, int, int, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].dispatches;
}
void FakeDevice::dispatch_groups(CommandBuffer cb, int, int, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].dispatches;
}
void FakeDevice::end_compute_pass(CommandBuffer) {}
void FakeDevice::blit(CommandBuffer, Texture src, Region r, Texture dst, int dx, int dy) {
    std::lock_guard lock(mutex_);
    Tex *s = tex(src), *d = tex(dst);
    if (!s || !d || s->desc.format != d->desc.format)
        return;
    const int bpp = bytes_per_pixel(s->desc.format);
    for (int y = 0; y < r.h; ++y) {
        if (r.y + y >= s->desc.height || dy + y >= d->desc.height)
            break;
        int w = std::min({r.w, s->desc.width - r.x, d->desc.width - dx});
        if (w <= 0)
            break;
        memcpy(&d->levels[0][(size_t(dy + y) * d->desc.width + dx) * bpp],
               &s->levels[0][(size_t(r.y + y) * s->desc.width + r.x) * bpp], size_t(w) * bpp);
    }
}
void FakeDevice::generate_mipmaps(CommandBuffer, Texture) {}
void FakeDevice::on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) {
    std::lock_guard lock(mutex_);
    commands_[cb.id].on_complete.push_back(std::move(fn));
}
void FakeDevice::commit(CommandBuffer cb) {
    std::vector<std::function<void(CommandStatus, double)>> run;
    std::function<void()> present;
    {
        std::lock_guard lock(mutex_);
        auto &c = commands_[cb.id];
        c.committed = true;
        committed_order_.push_back(cb.id);
        if (manual_completion_)
            return;
        run.swap(c.on_complete);
        present = std::move(c.deferred_present);
    }
    if (present)
        present();
    for (auto &fn : run)
        fn(CommandStatus::Completed, 0.0);
}
void FakeDevice::complete_all() {
    std::vector<std::function<void(CommandStatus, double)>> run;
    std::vector<std::function<void()>> presents;
    {
        std::lock_guard lock(mutex_);
        for (uint64_t id : committed_order_) {
            auto &c = commands_[id];
            for (auto &fn : c.on_complete)
                run.push_back(std::move(fn));
            c.on_complete.clear();
            if (c.deferred_present)
                presents.push_back(std::move(c.deferred_present));
        }
    }
    for (auto &p : presents)
        p();
    for (auto &fn : run)
        fn(CommandStatus::Completed, 0.0);
}
void FakeDevice::wait(CommandBuffer) {}
CommandStatus FakeDevice::status(CommandBuffer) {
    return CommandStatus::Completed;
}

Swapchain FakeDevice::create_swapchain(void *, int width, int height) {
    std::lock_guard lock(mutex_);
    if (width <= 0 || height <= 0)
        return {};
    uint64_t id = next_id_++;
    swapchains_[id] = Chain{width, height};
    return {id};
}
void FakeDevice::resize(Swapchain s, int width, int height) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it != swapchains_.end()) {
        it->second.w = width;
        it->second.h = height;
    }
}
Format FakeDevice::swapchain_format(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? Format::BGRA8 : it->second.format;
}
Texture FakeDevice::acquire(Swapchain s) {
    int w, h;
    Format f;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end())
            return {};
        w = it->second.w;
        h = it->second.h;
        f = it->second.format;
    }
    Texture t = create_texture({w, h, f, UsageRenderTarget | UsageCpu});
    std::lock_guard lock(mutex_);
    swapchains_[s.id].acquired.push_back(t);
    return t;
}
void FakeDevice::release_drawable(Swapchain, Texture t) {
    destroy(t);
}
void FakeDevice::present(CommandBuffer cb, Swapchain, Texture t, double min_duration,
                         std::function<void(double)> presented) {
    std::lock_guard lock(mutex_);
    last_min_duration_ = min_duration;
    last_presented_ = presented;
    auto self = this;
    commands_[cb.id].deferred_present = [self, presented, t] {
        double now;
        {
            std::lock_guard lock(self->mutex_);
            now = self->clock_;
        }
        if (presented)
            presented(now);
        self->destroy(t);
    };
}
void FakeDevice::fire_presented(double presented_seconds) {
    std::function<void(double)> fn;
    {
        std::lock_guard lock(mutex_);
        fn = last_presented_;
    }
    if (fn)
        fn(presented_seconds);
}
double FakeDevice::refresh_period(Swapchain) {
    return 1.0 / 60;
}
void FakeDevice::destroy(Swapchain s) {
    std::lock_guard lock(mutex_);
    swapchains_.erase(s.id);
}
double FakeDevice::now_seconds() {
    std::lock_guard lock(mutex_);
    return clock_;
}
void FakeDevice::advance_clock(double seconds) {
    std::lock_guard lock(mutex_);
    clock_ += seconds;
}
void FakeDevice::set_clock(double seconds) {
    std::lock_guard lock(mutex_);
    clock_ = seconds;
}
int FakeDevice::draws_recorded(CommandBuffer cb) const {
    std::lock_guard lock(mutex_);
    auto it = commands_.find(cb.id);
    return it == commands_.end() ? 0 : it->second.draws;
}
int FakeDevice::dispatches_recorded(CommandBuffer cb) const {
    std::lock_guard lock(mutex_);
    auto it = commands_.find(cb.id);
    return it == commands_.end() ? 0 : it->second.dispatches;
}
int FakeDevice::live_textures() const {
    std::lock_guard lock(mutex_);
    return int(textures_.size());
}

} // namespace gpu
```

- [ ] **Step 6: Build and run**

Run: `.venv/bin/cmake --build --preset macos --target gpu_fake_tests && .venv/bin/ctest --preset macos -R gpu_fake_tests --output-on-failure`
Expected: `all gpu fake tests passed`. `.venv/bin/python tools/format.py --write`.

- [ ] **Step 7: Commit**

Update `PROGRESS.md` "Now" (Task 2 done; next Task 3). Then:

```sh
.venv/bin/python tools/check_repo.py
git add src/recomp/host/gpu src/recomp/host/CMakeLists.txt docs/superpowers/PROGRESS.md
git commit -m "Add the device-level GPU interface and its fake backend"
```

---

### Task 3: The Metal backend

**Files:**
- Create: `src/recomp/host/gpu/metal/metal_device.h`, `metal_device.mm`, `metal_surface.mm`, `shaders_msl.h`, `metal_bridge.h`, `src/recomp/host/gpu/gpu_factory.h`, `gpu_factory.cpp`, `src/recomp/host/gpu/metal/tests/gpu_metal_tests.cpp`
- Modify: `src/recomp/host/gpu/CMakeLists.txt`

**Interfaces:**
- Consumes: `gpu::Device` from Task 2.
- Produces: `std::unique_ptr<gpu::Device> gpu::create_default_device();` (`gpu_factory.h`, Metal on Apple, null elsewhere); `gpu::MetalDevice` (`metal_device.h`); bridge functions in `metal_bridge.h` (temporary, deleted in Task 7):
  ```cpp
  namespace gpu::metal {
  gpu::Texture import_texture(gpu::Device *d, id<MTLTexture> t);   // registers a foreign texture; destroy() releases our reference only
  id<MTLTexture> export_texture(gpu::Device *d, gpu::Texture t);     // nil for unknown ids
  id<MTLCommandBuffer> export_command(gpu::Device *d, gpu::CommandBuffer cb);
  id<MTLCommandQueue> queue(gpu::Device *d);
  id<MTLDevice> device(gpu::Device *d);
  }
  ```

- [ ] **Step 1: Move the three MSL sources into `shaders_msl.h`**

Copy the compositor shader string from `compositor.mm` (the `Quad` struct, `quad_v`/`quad_f`), the HUD shader from `performance_overlay.h` (`hud_v`/`hud_f`) and the whole D3D library source from `d3d_render.mm:~380-600` (the `Uniforms` struct, `d3d_v`/`d3d_f`, `surface_upload_v/f`, `guest_readback`, `guest_readback_fused`, `native_brightness`) into one header:

```cpp
// shaders_msl.h - the MSL behind every program gpu/shaders.md names. Moved
// verbatim from compositor.mm, performance_overlay.h and d3d_render.mm; the
// slot numbers are the ones shaders.md documents.
#pragma once
namespace gpu::metal {
inline const char *kCompositorSource = R"MSL( ... )MSL";
inline const char *kHudSource = R"MSL( ... )MSL";
inline const char *kD3DSource = R"MSL( ... )MSL";
} // namespace gpu::metal
```

Then reconcile the slot numbers with `shaders.md`: the compositor quad is `[[buffer(0)]]` in both stages, the HUD rect `[[buffer(0)]]`, the D3D uniforms `[[buffer(1)]]` with vertices `[[buffer(0)]]`, `surface_upload` bytes `[[buffer(0)]]`, params `[[buffer(1)]]`, palette `[[buffer(2)]]`, compute params `[[buffer(1)]]` with the output `[[buffer(0)]]`. Where the existing source uses a different index, change the MSL attribute rather than the document.

- [ ] **Step 2: Write the failing Metal contract test**

`gpu/metal/tests/gpu_metal_tests.cpp` reuses the structure of `gpu_fake_tests.cpp` (same `CHECK` macro) against `gpu::create_default_device()`:

```cpp
#include "../../gpu_factory.h"
// CHECK macro as in gpu_fake_tests.cpp
using namespace gpu;

static void test_upload_readback() {
    auto d = create_default_device();
    CHECK(d != nullptr);
    Texture t = d->create_texture({4, 2, Format::RGBA8, UsageSampled | UsageCpu});
    uint8_t in[32], out[32] = {0};
    for (int i = 0; i < 32; ++i) in[i] = uint8_t(i * 3);
    CHECK(d->upload(t, {0, 0, 4, 2}, in, 16));
    CHECK(d->readback(t, {0, 0, 4, 2}, out, 16));
    CHECK(memcmp(in, out, 32) == 0);
    d->destroy(t);
}
static void test_clear_and_compositor_draw() {
    auto d = create_default_device();
    Texture target = d->create_texture({8, 8, Format::BGRA8, UsageRenderTarget | UsageCpu});
    Texture src = d->create_texture({2, 2, Format::BGRA8, UsageSampled | UsageCpu});
    uint8_t green[16];
    for (int i = 0; i < 4; ++i) { green[i*4] = 0; green[i*4+1] = 255; green[i*4+2] = 0; green[i*4+3] = 255; }
    d->upload(src, {0, 0, 2, 2}, green, 8);
    RenderPass pass;
    pass.color[0] = {target, Load::Clear, Store::Store, {1, 0, 0, 1}};
    RenderState state; state.color_format[0] = Format::BGRA8;
    struct Quad { float rect[4]; float uv[4]; float drawable[2]; uint32_t opaque, pad; } q =
        {{0, 0, 4, 8}, {0, 0, 1, 1}, {8, 8}, 1, 0}; // left half
    CommandBuffer cb = d->begin();
    d->begin_render_pass(cb, pass);
    d->set_pipeline(cb, d->render_pipeline("compositor", state));
    d->set_bytes(cb, Stage::Vertex, 0, &q, sizeof q);
    d->set_bytes(cb, Stage::Fragment, 0, &q, sizeof q);
    d->set_texture(cb, Stage::Fragment, 0, src);
    d->set_sampler(cb, Stage::Fragment, 0, SamplerState{});
    d->draw(cb, Primitive::TriangleStrip, 0, 4);
    d->end_render_pass(cb);
    bool completed = false; d->on_complete(cb, [&](CommandStatus s, double) { completed = s == CommandStatus::Completed; });
    d->commit(cb); d->wait(cb);
    CHECK(completed);
    uint8_t left[4], right[4];
    d->readback(target, {1, 4, 1, 1}, left, 4);
    d->readback(target, {6, 4, 1, 1}, right, 4);
    CHECK(left[1] == 255 && left[2] == 0);   // green quad
    CHECK(right[2] == 255 && right[1] == 0); // red clear
}
static void test_commit_order() {
    auto d = create_default_device();
    std::vector<int> order; std::mutex m;
    CommandBuffer a = d->begin(), b = d->begin();
    d->on_complete(a, [&](CommandStatus, double) { std::lock_guard l(m); order.push_back(1); });
    d->on_complete(b, [&](CommandStatus, double) { std::lock_guard l(m); order.push_back(2); });
    d->commit(a); d->commit(b); d->wait(b);
    std::lock_guard l(m);
    CHECK(order.size() == 2 && order[0] == 1 && order[1] == 2);
}
static void test_compute_readback_kernel() {
    auto d = create_default_device();
    Texture color = d->create_texture({16, 16, Format::BGRA8, UsageSampled | UsageCpu | UsageRenderTarget});
    Texture cover = d->create_texture({16, 16, Format::R8, UsageSampled | UsageCpu | UsageRenderTarget});
    std::vector<uint8_t> px(16 * 16 * 4, 0x40), cv(16 * 16, 0xff);
    d->upload(color, {0, 0, 16, 16}, px.data(), 64); d->upload(cover, {0, 0, 16, 16}, cv.data(), 16);
    Buffer out = d->create_buffer(16 * 16 * 8, nullptr);
    uint32_t p[12] = {0, 0, 16, 16, 16, 16, 16, 16, 0, 16, 16, 0};
    CommandBuffer cb = d->begin();
    d->begin_compute_pass(cb);
    d->set_pipeline(cb, d->compute_pipeline("guest_readback"));
    d->set_texture(cb, Stage::Compute, 0, color); d->set_texture(cb, Stage::Compute, 1, cover);
    d->set_buffer(cb, Stage::Compute, 0, out, 0); d->set_bytes(cb, Stage::Compute, 1, p, sizeof p);
    d->dispatch_threads(cb, 16, 16, 8, 8);
    d->end_compute_pass(cb); d->commit(cb); d->wait(cb);
    const uint32_t *words = static_cast<const uint32_t *>(d->map_read(out));
    CHECK(words && (words[0] & 0xffffff) == 0x404040 && (words[0] >> 24) != 0);
}
static void test_swapchain_from_layer() {
    auto d = create_default_device();
    void *layer = gpu_test_make_native_surface(32, 16); // declared in metal_bridge.h: a CAMetalLayer
    Swapchain s = d->create_swapchain(layer, 32, 16);
    CHECK(s && d->swapchain_format(s) == Format::BGRA8);
    Texture t = d->acquire(s);
    CHECK(t && d->describe(t).width == 32);
    d->release_drawable(s, t);
    CHECK(d->refresh_period(s) > 0.0 && d->refresh_period(s) < 0.1);
    d->destroy(s);
}
int main() {
    test_upload_readback();
    test_clear_and_compositor_draw();
    test_commit_order();
    test_compute_readback_kernel();
    test_swapchain_from_layer();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all gpu metal tests passed\n");
    return g_failures ? 1 : 0;
}
```

Add to `metal_bridge.h`: `void *gpu_test_make_native_surface(int w, int h);` implemented in `metal_surface.mm` as a retained `CAMetalLayer` with `drawableSize` set.

- [ ] **Step 3: Wire the test and build to see it fail**

Append to `gpu/CMakeLists.txt`:

```cmake
if(APPLE)
  add_library(gpu_metal OBJECT metal/metal_device.mm metal/metal_surface.mm gpu_factory.cpp)
  target_include_directories(gpu_metal PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(gpu_metal PRIVATE ${POP_WARN_HOST} ${POP_OBJC_ARC})
  target_link_libraries(gpu_metal PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo" "-framework Foundation")
  pop_optimize(gpu_metal 2)
  add_executable(gpu_metal_tests metal/tests/gpu_metal_tests.cpp)
  target_link_libraries(gpu_metal_tests PRIVATE gpu_metal)
  pop_optimize(gpu_metal_tests 1)
  pop_test_binary(gpu_metal_tests)
  add_test(NAME gpu_metal_tests COMMAND gpu_metal_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(gpu_metal_tests PROPERTIES LABELS gpu)
else()
  add_library(gpu_metal OBJECT gpu_factory.cpp) # returns nullptr until sub-project 3 adds Vulkan
  target_include_directories(gpu_metal PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
endif()
```

Run: `.venv/bin/cmake --preset macos && .venv/bin/cmake --build --preset macos --target gpu_metal_tests`. Expected: fails on missing headers.

- [ ] **Step 4: Write `metal_device.h` and `metal_device.mm`**

`metal_device.h` (Objective-C++ header, included only by `.mm` files and the bridge):

```cpp
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../gpu.h"
#include <mutex>
#include <unordered_map>

namespace gpu {
class MetalDevice final : public Device {
  public:
    explicit MetalDevice(id<MTLDevice> device);
    // every Device method declared override, as in FakeDevice
    // --- native access for the bridge ---
    id<MTLDevice> native() const { return device_; }
    id<MTLCommandQueue> native_queue() const { return queue_; }
    Texture import_texture(id<MTLTexture> t);
    id<MTLTexture> native_texture(Texture t);
    id<MTLCommandBuffer> native_command(CommandBuffer cb);
  private:
    struct Tex { id<MTLTexture> texture; TextureDesc desc; bool foreign; };
    struct Cmd {
        id<MTLCommandBuffer> buffer;
        id<MTLRenderCommandEncoder> render;
        id<MTLComputeCommandEncoder> compute;
        id<MTLBlitCommandEncoder> blit;
        Pipeline pipeline;
        std::vector<std::function<void(CommandStatus, double)>> on_complete;
        std::vector<std::pair<Swapchain, Texture>> presents;
    };
    struct Chain { CAMetalLayer *layer; int w, h; std::unordered_map<uint64_t, id<CAMetalDrawable>> drawables; double refresh; };
    struct Pipe { id<MTLRenderPipelineState> render; id<MTLComputePipelineState> compute; };
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> library_; // compiled from the three shaders_msl.h sources joined
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, id<MTLBuffer>> buffers_;
    std::unordered_map<uint64_t, Cmd> commands_;
    std::unordered_map<uint64_t, Chain> swapchains_;
    std::unordered_map<uint64_t, Pipe> pipelines_;
    std::unordered_map<std::string, std::unordered_map<uint64_t, Pipeline>> render_cache_;
    std::unordered_map<std::string, Pipeline> compute_cache_;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_cache_; // key: compare<<1 | write
    std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache_;
    id<MTLBlitCommandEncoder> blit_encoder(Cmd &c); // ends render/compute encoders first
    void end_encoders(Cmd &c);
};
} // namespace gpu
```

Implementation rules for `metal_device.mm`:
- Formats: BGRA8→`MTLPixelFormatBGRA8Unorm`, RGBA8→`RGBA8Unorm`, R8→`R8Unorm`, Depth32F→`Depth32Float`. Usage: Sampled→`ShaderRead`, RenderTarget→`RenderTarget`, Storage→`ShaderRead|ShaderWrite`; storage mode `MTLStorageModePrivate` unless `UsageCpu`, then `Shared`; depth textures always Private. `mip_levels` > 1 sets `mipmapLevelCount` and `allowGPUOptimizedContents`.
- `upload`: Shared textures `replaceRegion:mipmapLevel:withBytes:bytesPerRow:`; Private textures stage through a transient Shared `MTLBuffer` and a blit on a fresh command buffer that is committed and waited.
- `readback`: Shared textures `getBytes:` after `wait` on the last command buffer that rendered into the texture (track `last_writer_` per texture, set at `end_render_pass`/`end_compute_pass`/`blit`); Private textures blit into a Shared staging texture first.
- Buffers: `MTLResourceStorageModeShared`; `update` is `memcpy` into `contents`; `map_read` returns `contents` after waiting on the last writer.
- `render_pipeline`: descriptor from `shader` name (`"d3d"`→`d3d_v`/`d3d_f`, `"surface_upload"`, `"compositor"`→`quad_v`/`quad_f`, `"hud"`→`hud_v`/`hud_f`), `color_count` attachments with the formats, `depthAttachmentPixelFormat` when `depth_attachment`, blend factors mapped one to one, write mask `MTLColorWriteMaskAll` or `None` on every attachment, `vertexDescriptor` only for `"d3d"` (three float4 attributes and one float2 at the offsets `HostD3DVertex` defines, stride 56). Cached by `(shader, state.key())`.
- `compute_pipeline`: `newComputePipelineStateWithFunction:`; `thread_execution_width` reads the state's `threadExecutionWidth`.
- Command buffers: `begin` → `[queue_ commandBuffer]`. `begin_render_pass` → `MTLRenderPassDescriptor` from the attachments, `Load::Clear`→`MTLLoadActionClear` with `clearColor`/`clearDepth`; `set_*` → the matching encoder call (`setVertexBytes`/`setFragmentBytes`/`setBytes`, `setVertexBuffer`, `setFragmentTexture`, `setTexture`, samplers via `sampler_cache_`, `setDepthStencilState` via `depth_cache_`, `setCullMode` with `setFrontFacingWinding:MTLWindingClockwise` once per pass as `d3d_render.mm` does today); `draw` → `drawPrimitives:vertexStart:vertexCount:`. `dispatch_threads` → `dispatchThreads:threadsPerThreadgroup:`; `dispatch_groups` → `dispatchThreadgroups:threadsPerThreadgroup:`. `blit`/`generate_mipmaps` use `blit_encoder`. `on_complete` is stored; `commit` ends open encoders, registers one `addCompletedHandler:` that computes `gpu_ms = (GPUEndTime - GPUStartTime) * 1000`, maps `status == MTLCommandBufferStatusCompleted` to `Completed`, runs the stored callbacks in order, then `commit`s. Presents registered through `present()` are `[buffer presentDrawable:afterMinimumDuration:]` (or `presentDrawable:` when the duration is 0) issued before commit, with `addPresentedHandler:` forwarding `presentedTime`.
- `wait` → `waitUntilCompleted`; `status` maps the Metal status.
- Swapchains: see Step 5. `now_seconds` → `double(CVGetCurrentHostTime()) / CVGetHostClockFrequency()`, the same base `presentedTime` uses.
- Every table access under `mutex_`; encoder calls outside it (one thread records a command buffer at a time, as today).

- [ ] **Step 5: Write `metal_surface.mm`**

`create_swapchain` takes a `CAMetalLayer *` (via `(__bridge CAMetalLayer *)native_surface`), sets `device`, `pixelFormat` BGRA8, `framebufferOnly = NO`, `maximumDrawableCount = 3`, `drawableSize`. `resize` updates `drawableSize`. `acquire` → `[layer nextDrawable]`; nil → null handle; else registers `drawable.texture` as a foreign texture and remembers the drawable by that id. `release_drawable` forgets the drawable and the texture. `present` looks the drawable up by texture id and records it on the command buffer (Step 4). `refresh_period` reads the refresh rate of the display the layer is on: `CGDisplayModeGetRefreshRate(CGDisplayCopyDisplayMode(display))` where `display` comes from the layer's window screen `NSScreenNumber` when the layer has one, else `CGMainDisplayID()`; 0 Hz (Apple laptops report 0 for ProMotion) → `1.0/60`. `destroy` drops the layer reference. `gpu_test_make_native_surface` returns `(__bridge_retained void *)[CAMetalLayer layer]` with `drawableSize` set.

- [ ] **Step 6: `gpu_factory`**

```cpp
// gpu_factory.h
#pragma once
#include <memory>
#include "gpu.h"
namespace gpu { std::unique_ptr<Device> create_default_device(); const char *default_backend_name(); }
```
`gpu_factory.cpp`: `#ifdef __APPLE__` → `std::make_unique<MetalDevice>(MTLCreateSystemDefaultDevice())` through a small `.mm` helper `metal_create_device()` declared in `metal_bridge.h`; else `return nullptr` and `"none"`.

- [ ] **Step 7: Run the Metal tests**

Run: `.venv/bin/cmake --build --preset macos --target gpu_metal_tests gpu_fake_tests && .venv/bin/ctest --preset macos -R 'gpu_' --output-on-failure`
Expected: both pass. Format, `check_repo.py`, update `PROGRESS.md` (Task 3 done; next Task 4).

```sh
git add src/recomp/host/gpu docs/superpowers/PROGRESS.md
git commit -m "Add the Metal backend behind the GPU interface"
```

---

### Task 4: Presenter, compositor and overlay over the interface

**Files:**
- Create: `src/recomp/host/present_frame.h`, `src/recomp/host/performance_overlay.cpp`, `src/recomp/host/tests/presenter_tests.cpp`
- Rename: `compositor.mm`→`compositor.cpp`, `present_thread.mm`→`present_thread.cpp`, `ui_layer.mm`→`ui_layer.cpp`, `tests/compositor_tests.mm`→`.cpp`, `tests/ui_layer_tests.mm`→`.cpp`, `tests/test_frame_builder.mm`→`.cpp`
- Modify: `compositor.h`, `present.h`, `present_test.h`, `performance_overlay.h`, `d3d_render.mm` (bridge calls only), `smoke_main.mm`, `main.mm`, `tests/host_tests.mm`, `src/recomp/mods/tests/present_events_tests.mm`, `src/recomp/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `gpu::Device`, `gpu::FakeDevice`, `gpu::metal::*` bridge.
- Produces (in `present.h`, replacing the Metal-typed declarations):
  ```cpp
  void host_present_set_device(gpu::Device *device);          // before start; the renderer's device
  gpu::Device *host_present_device();
  void host_present_start(void *native_surface, int w, int h);  // was (CAMetalLayer*, CGDirectDisplayID)
  void host_present_start_offscreen(int w, int h);              // was (id<MTLCommandQueue>, w, h)
  void host_present_resize(int drawable_w, int drawable_h);
  void host_present_install_surface(void *native_surface, int w, int h);
  void host_present_track_command(gpu::CommandBuffer command);
  struct HostSceneTarget { gpu::Texture color, coverage, depth; int width, height; uint64_t lease; };
  ```
  `compositor.h`: `CompositorInput` holds `gpu::Texture world, overlay, legacy_frame, settings_page`; `void compositor_compose(gpu::Device *d, const CompositorInput *in, gpu::Texture out, gpu::CommandBuffer cb);`. `present_test.h`: `void host_present_test_commit_layer(id, id)` is replaced by `void host_present_test_use_device(gpu::Device *d);` plus `FakeDevice`'s controls.
  `performance_overlay.h`: `class PerformanceOverlay { void configure(gpu::Device*); void note_frame(...unchanged...); void draw(gpu::CommandBuffer cb, gpu::Texture target, int w, int h); }`.

- [ ] **Step 1: Write `present_frame.h`**

Move `Fence`, `Target`, `Frame`, `Message` and `HostSceneTarget` out of `present_thread.mm` so tests and the renderer share one definition, with every `id<MTLTexture>` replaced by `gpu::Texture`:

```cpp
#pragma once
#include "compositor.h"
#include "gpu/gpu.h"
#include "present.h"
#include "ui_layer.h"
#include <memory>
#include <vector>

struct Fence { bool done = true, success = true; };
struct Target { HostSceneTarget scene; gpu::Texture pixels; std::vector<uint8_t> test_pixels; };
struct Frame {
    uint64_t frame_id = 0, epoch = 0;
    HostScreenClass cls = HOST_SCREEN_UNKNOWN;
    bool had_draws = false, supplied = false, gpu = false, shown = false, released = false, dropped = false,
         success = true, repeat = false, staged_pixels = false, completion_fallback = false;
    double presented_ts = 0, gpu_ts = 0, submitted_ts = 0, sealed_ts = 0, gpu_ms = 0, acknowledgement_deadline = 0;
    unsigned pending_prefixes = 0;
    std::shared_ptr<Target> target, scene_lease;
    std::shared_ptr<Fence> prefix;
    UiFrame ui;
    CompositorInput input;
    LayoutSnapshot layout;
    HostFrameCapture capture;
    gpu::Texture composed;
};
struct Message { void *surface = nullptr; int w = 0, h = 0; bool install = false; };
```

- [ ] **Step 2: Convert `compositor.mm` to `compositor.cpp`**

Mapping, each one a mechanical edit:

| Today | After |
| --- | --- |
| `id<MTLTexture>` fields in `CompositorInput`/`CompositorSceneHistory` | `gpu::Texture` |
| `compositor_compose(in, out, cb)` builds `MTLRenderPassDescriptor` + encoder | `gpu::RenderPass pass; pass.color[0] = {out, Load::Clear, Store::Store, {0,0,0,1}}; d->begin_render_pass(cb, pass);` |
| `g_pipeline` / `g_pipeline_premul` built from the MSL | `d->render_pipeline("compositor", state)` with `state.blend_enabled` + factors per blend mode (Opaque: none; Straight: SrcAlpha/OneMinusSrcAlpha; Premultiplied: One/OneMinusSrcAlpha), `color_format[0] = d->describe(out).format` |
| `setVertexBytes/setFragmentBytes(&quad, sizeof quad, 0)` | `d->set_bytes(cb, Stage::Vertex, 0, ...)`, `Stage::Fragment` |
| `setFragmentTexture:atIndex:0` + nearest sampler | `d->set_texture(cb, Stage::Fragment, 0, t); d->set_sampler(cb, Stage::Fragment, 0, {})` |
| `drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4` | `d->draw(cb, Primitive::TriangleStrip, 0, 4)` |
| `[texture width]`/`height` | `d->describe(t).width` (cache per compose call; `describe` takes the device mutex) |
| `texture == nil` / pointer compares in `compositor_resolve_scene` | `!texture` / `operator==` on handles |

The quad layout and the scene/cursor/pointer arithmetic do not change. The UI double in `tests/compositor_ui_double.h` drops its ObjC texture stand-in for `gpu::Texture{id}` values; `compositor_tests.cpp` runs on `FakeDevice` and becomes label `nogame`.

- [ ] **Step 3: Convert `present_thread.mm` to `present_thread.cpp`**

| Today | After |
| --- | --- |
| `CAMetalLayer *layer; CGDirectDisplayID display; CVDisplayLinkRef link` | `gpu::Swapchain chain; void *surface;` pacing thread: a `std::thread` that sleeps to the next `refresh_period()` boundary and calls the display-tick path the CVDisplayLink callback called (the same `window_wake(ts, display_tick=true)`), exiting when the service stops |
| `id<MTLCommandQueue> queue` / `host_present_set_shared_queue` | `gpu::Device *device` / `host_present_set_device` |
| `[queue commandBuffer]` | `device->begin()` |
| `[cb addCompletedHandler:^(id<MTLCommandBuffer> c){...}]` | `device->on_complete(cb, [...](gpu::CommandStatus s, double ms){...})`; `status == MTLCommandBufferStatusCompleted` → `s == Completed`; `GPUEndTime - GPUStartTime` → `ms` |
| `[layer nextDrawable]` / `drawable.texture` | `device->acquire(chain)` / the returned texture |
| `[cb presentDrawable:d afterMinimumDuration:t]` + `addPresentedHandler` | `device->present(cb, chain, tex, t, [...](double presented){...})`; the `completion_fallback` path stays: when `presented == 0` the frame takes the completion timestamp |
| `double(CVGetCurrentHostTime()) / CVGetHostClockFrequency()` (7 sites) | `device->now_seconds()` |
| `newTextureWithDescriptor:` for `Target` pools | `device->create_texture({w, h, Format::BGRA8, UsageRenderTarget|UsageSampled|UsageCpu})` (color), `R8` coverage, `Depth32F` depth; null handle where nil was tested (the halving-on-failure path keeps its logic) |
| texture release on `Target` drop | `device->destroy(...)` in `Target`'s destructor, guarded by a `gpu::Device *` member |
| `layer.drawableSize` change | `device->resize(chain, w, h)` |
| `[drawable.texture getBytes:...]` for test pixels | `device->readback(...)` |
| `host_present_test_commit_layer(layer, command)` | removed; tests inject a `FakeDevice` with `host_present_test_use_device` |
| `CVDisplayLinkGetActualOutputVideoRefreshPeriod` | `device->refresh_period(chain)` |

Keep every state-machine decision, counter and log line as it is. The file compiles as C++ with no `#import`.

- [ ] **Step 4: Port the overlay to `performance_overlay.cpp`**

CoreText goes; `mods_font6x8_glyph` (from `src/recomp/mods/mods_internal.h`, 6x8 glyphs, bit 5 leftmost) renders the same lines into the 330x136 RGBA buffer at 2x scale (12x16 cells, 27 columns), uploaded with `device->upload` each 0.25 s refresh, drawn with the `hud` pipeline (premultiplied blend) via `set_bytes(cb, Stage::Vertex, 0, rect, 16)`. The graph in mode 2 draws 1-pixel columns into the same buffer. Line content and thresholds unchanged.

- [ ] **Step 5: Bridge the renderer and `main.mm` for now**

In `d3d_render.mm`: `host_present_acquire_target` now returns `gpu::Texture`s; convert with `gpu::metal::export_texture(host_present_device(), scene.color)` at the two sites that bind the scene target as a render attachment and `import_texture` where the renderer hands the presenter its world/overlay textures in `CompositorInput`. In `main.mm`: `host_present_set_device(device)` where `host_present_set_shared_queue` was, `host_present_start((__bridge void *)layer, w, h)`, `host_present_resize(w, h)`. In `smoke_main.mm` and `host_tests.mm`: `host_present_start_offscreen(w, h)` after `host_present_set_device`. `present_events_tests.mm` uses `host_present_test_use_device(&fake)`.

- [ ] **Step 6: Write `tests/presenter_tests.cpp` from the fake-presenter half of `host_tests.mm`**

Every test in `host_tests.mm` that uses `FakePresentLayer`/`FakePresentDrawable`/`FakePresentCommand` moves here, rewritten on `gpu::FakeDevice`: `[command complete]` → `fake.complete_all()`, `presentedTime = t` → `fake.fire_presented(t)`, `minimumDuration` → `fake.last_present_min_duration()`, `host_present_test_fail_allocations(n)` → `fake.fail_next_allocations(n)`. Same `CHECK` macro and test table shape as `host_tests.mm`. Label `nogame`.

- [ ] **Step 7: CMake**

`POP_PRESENT_SOURCES` becomes `present_pixels.cpp present_thread.cpp compositor.cpp ui_layer.cpp performance_overlay.cpp d3d_render.mm indexed_frame.cpp`; every host target links `gpu_metal` (and `gpu_fake` for tests); `compositor_tests` and `ui_layer_tests` lose the Metal framework and `POP_OBJC_ARC`; add:

```cmake
add_executable(presenter_tests ${HOST}/tests/presenter_tests.cpp ${HOST}/present_thread.cpp ${HOST}/compositor.cpp
               ${HOST}/present_pixels.cpp ${HOST}/ui_layer.cpp ${HOST}/performance_overlay.cpp ${HOST}/page_overlay.cpp
               ${BACKENDS}/indexed_frame.cpp ${HOST}/tests/test_frame_builder.cpp)
target_link_libraries(presenter_tests PRIVATE gpu_fake recomp_platform recomp_mods_testing)
# include dirs, warnings, pop_optimize, pop_test_binary as compositor_tests
add_test(NAME presenter_tests COMMAND presenter_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(presenter_tests PROPERTIES LABELS nogame)
```

- [ ] **Step 8: Verify**

Run: `.venv/bin/python tools/build.py --target app --target smoke && .venv/bin/python tools/test.py --native && .venv/bin/python tools/test.py --mods && sh src/recomp/host/tests/integration_tests.sh`
Expected: all pass except the pre-existing failures listed in `PROGRESS.md`. Then `build/recomp/pop_smoke --frames 120` (the smoke command line the integration script uses) dumps a frame; compare its PPM against one from `main` with `tools/recomp/compare_frames.py` if present, else `cmp` the two files after converting both with the same `host_write_ppm` path. Expected: identical bytes, since the compositor math and shaders did not change.

Format, `check_repo.py`, `PROGRESS.md` (Task 4 done; next Task 5), commit `"Run the presenter, compositor and overlay over the GPU interface"`.

---

### Task 5: The D3D renderer over the interface

**Files:**
- Rename: `d3d_render.mm`→`d3d_render.cpp`, `smoke_main.mm`→`smoke_main.cpp`
- Modify: `d3d_render.h`, `tests/host_tests.mm`
- Create: `tests/d3d_render_tests.cpp` (the renderer tests from `host_tests.mm`, on the real device, label `gpu`)

**Interfaces:**
- Consumes: `gpu::Device`, `HostSceneTarget` with handles, `host_present_device()`.
- Produces: `struct D3DRenderer` (C++ class replacing the `PopD3DRenderer` ObjC class) with `explicit D3DRenderer(gpu::Device *d)` and the same method set (`drawCommands`, `uploadSurface`, `readbackRect`, `brightness`, `presentTargets`, HD pack `loadPack`/`evict`, statistics), exported through the existing C entry points in `d3d_render.h` (`host_d3d_*`) which do not change; `shaders.md` `D3DUniforms` struct declared in `d3d_render.h`.

- [ ] **Step 1: Split the 3928-line file while converting**

Keep `d3d_render.cpp` for the class, and move two self-contained pieces into their own files as they are converted: `d3d_textures.cpp` (texture revisions, leases, HD pack uploads and mip generation, the byte budget via `allocated_bytes`) and `d3d_readback.cpp` (the `guest_readback`/`native_brightness` dispatches, the `dispatch_apply_f` copy-out and the readback params arrays). Each exposes a small header (`d3d_textures.h`, `d3d_readback.h`) with the functions `d3d_render.cpp` calls.

- [ ] **Step 2: Conversion mapping**

| Today | After |
| --- | --- |
| `newTextureWithDescriptor:` (10) | `create_texture` with the usage: render targets `RenderTarget|Sampled`, guest textures `Sampled|UsageCpu`, mip chains `mip_levels = full` |
| `replaceRegion:` (3) | `upload(t, region, bytes, pitch, level)` |
| `copyFromTexture:` (6) / `copyFromBuffer:` (3) / blit encoders (9) | `blit(cb, src, region, dst, x, y)`; buffer→texture copies become `upload` on Shared textures |
| `getBytes:` (3) | `readback` |
| `newBufferWithLength:` (5) | `create_buffer`; the argument-bytes pool keeps its status-keyed recycling, status via `device->status(cb)` |
| `setVertexBytes` ≤4096 else buffer (`bindVertices`) | `set_bytes` / `set_vertex_buffer` with the same 4096 threshold |
| `setFragmentBytes`/`setVertexBytes` for `Uniforms` | `set_bytes(cb, Stage::Vertex|Fragment, 1, &uniforms, 112)` |
| pipeline cache keyed `(overlay<<41)|(enabled<<40)|(writeColor<<32)|(src<<16)|dst` | `RenderState` filled from the same inputs (`color_count = 2`, `color_format = {BGRA8, R8}`, `depth_attachment = true`, `writes_point_size` for point sprites); the backend caches |
| `MTLDepthStencilState` cache | `set_depth({compare, write})` |
| `renderCommandEncoderWithDescriptor:` (3) + 23 `endEncoding` | `begin_render_pass`/`end_render_pass`, `begin_compute_pass`/`end_compute_pass` |
| `dispatchThreads:` / `dispatchThreadgroups:` | `dispatch_threads(cb, w, h, 8, 8)` / `dispatch_groups(cb, gx, gy, 16, 16)`; thread width from `thread_execution_width` |
| `generateMipmapsForTexture:` | `generate_mipmaps` |
| `waitUntilCompleted` (5) | `wait` |
| `MTLCounterSampleBuffer` GPU timings | `gpu_ms` from `on_complete`; the per-phase counter split is dropped and `host_stats_note_phase` receives the whole-buffer time under `HOST_PHASE_GPU` |
| `dispatch_apply_f` readback copies | `std::thread` pool of `std::thread::hardware_concurrency()` in `d3d_readback.cpp`, same slicing |
| `host_d3d_*` C entry points | unchanged signatures; body calls the `D3DRenderer` singleton |

- [ ] **Step 3: Move the renderer tests**

Tests in `host_tests.mm` that create `MTLCreateSystemDefaultDevice` and a `PopD3DRenderer` move to `tests/d3d_render_tests.cpp` using `gpu::create_default_device()` and `D3DRenderer`; `readPixels`/`colorTarget` helpers become `device->readback` on the scene target's `color`. Label `gpu`. What remains of `host_tests.mm` is the CPU table plus audio; it is renamed `host_tests.cpp` in Task 8 when audio goes portable.

- [ ] **Step 4: Smoke on the factory**

`smoke_main.cpp`: `auto device = gpu::create_default_device(); D3DRenderer renderer(device.get()); host_present_set_device(device.get()); host_present_start_offscreen(w, h);`. `capture_at_seal` reads back with `device->readback`. No `#import` remains in the file.

- [ ] **Step 5: Verify with a frame comparison**

Run the smoke script the integration suite uses on both `main` (`git stash`-free: use `git worktree add /tmp/pop-main main`, build there with `tools/build.py --target smoke`) and this branch with the same `--frames` and `POP_SMOKE_DRAWABLE` settings; compare the dumped PPMs byte for byte. Expected: identical. Then `tools/test.py --native`, `--mods`, `--gameplay` (pre-existing failures only), `ctest -L gpu`.

Format, `check_repo.py`, `PROGRESS.md` (Task 5 done; next Task 6), commit `"Run the D3D renderer over the GPU interface"`.

---

### Task 6: The SDL3 host

**Files:**
- Create: `src/recomp/host/sdl/main.cpp`, `sdl/events.h`, `sdl/events.cpp`
- Rename: `input.mm`→`input.cpp`, `present.mm`→`present.cpp`
- Modify: `input.h`, `main.mm` (deleted at the end of this task), `window_presentation.h` (deleted), `CMakeLists.txt`, `cmake/MacBundle.cmake`

**Interfaces:**
- Consumes: `gpu::create_default_device`, `host_present_start(void*, w, h)`, `host_input_*`, `host_gate_*`, `BootOptions`, `mods_display_load_modes`.
- Produces: `input.h`: `struct HostKeyMapping { uint16_t scancode; uint8_t dik; uint8_t vk; };` (was `mac`), `void host_input_key(uint16_t scancode, bool down);`, `void host_input_modifiers(uint32_t sdl_keymod, uint32_t sides);` where `sides` is the host's own left/right bitmask (`HOST_MOD_LSHIFT=1, RSHIFT=2, LCTRL=4, RCTRL=8, LALT=0x10, RALT=0x20, LGUI=0x40, RGUI=0x80, CAPS=0x100`). `events.h`:
  ```cpp
  struct SdlHostWindow { SDL_Window *window; void *surface; int drawable_w, drawable_h; int mode; bool captured; };
  bool sdl_host_handle_event(SdlHostWindow *w, const SDL_Event &e); // false on quit
  void sdl_host_apply_window_mode(SdlHostWindow *w, int mode);      // 0 windowed, 1 borderless, 2 fullscreen
  void sdl_host_set_capture(SdlHostWindow *w, bool on);
  ```

- [ ] **Step 1: Write the failing keymap test**

In `tests/host_tests.mm`'s CPU table the existing mapping tests (`kKeys` covers every DIK the game reads, modifiers resolve sides) are rewritten against scancodes; add:

```cpp
static void input_scancode_table_covers_game_keys() {
    // Every DIK the guest's dinput path reads has exactly one scancode.
    static const uint8_t needed[] = {0x01 /*ESC*/, 0x1c /*RETURN*/, 0x39 /*SPACE*/, 0x0f /*TAB*/,
        0x2a, 0x36 /*shifts*/, 0x1d, 0x9d /*ctrls*/, 0x38, 0xb8 /*alts*/, 0xc8, 0xd0, 0xcb, 0xcd /*arrows*/,
        0x3b, 0x44, 0x57, 0x58 /*F1 F10 F11 F12*/, 0x47, 0x4f, 0xc7, 0xcf /*kp7 kp1 home end*/};
    for (uint8_t dik : needed) {
        int hits = 0;
        for (int i = 0; i < host_key_mapping_count(); ++i)
            if (host_key_mapping(i).dik == dik) ++hits;
        CHECK(hits == 1);
    }
    CHECK(host_key_mapping_for_scancode(SDL_SCANCODE_A).dik == 0x1e && host_key_mapping_for_scancode(SDL_SCANCODE_A).vk == 'A');
}
static void input_modifier_sides_follow_host_bits() {
    host_input_reset();
    host_input_modifiers(SDL_KMOD_LSHIFT, HOST_MOD_LSHIFT);
    uint8_t keys[256]; host_input_peek_keys(keys);
    CHECK(keys[0x2a] && !keys[0x36]);
    host_input_modifiers(SDL_KMOD_RSHIFT, HOST_MOD_RSHIFT);
    host_input_peek_keys(keys);
    CHECK(!keys[0x2a] && keys[0x36]);
}
```

Run the `host_tests` build. Expected: compile failure on `host_key_mapping_for_scancode`.

- [ ] **Step 2: Rewrite the table in `input.cpp`**

The `kKeys[]` rows keep their DIK and VK columns; the first column becomes `SDL_SCANCODE_*`. The full row set: letters A–Z → DIK 0x1e.. per the standard layout, digits, `GRAVE`, `MINUS`, `EQUALS`, `BACKSPACE`, `TAB`, `LEFTBRACKET`, `RIGHTBRACKET`, `RETURN`, `SEMICOLON`, `APOSTROPHE`, `BACKSLASH`, `COMMA`, `PERIOD`, `SLASH`, `SPACE`, `ESCAPE`, `F1..F15`, `CAPSLOCK`, `NUMLOCKCLEAR`, `SCROLLLOCK`, `KP_0..9`, `KP_DIVIDE`, `KP_MULTIPLY`, `KP_MINUS`, `KP_PLUS`, `KP_ENTER`, `KP_PERIOD`, `KP_EQUALS`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `UP/DOWN/LEFT/RIGHT`, `LSHIFT/RSHIFT/LCTRL/RCTRL/LALT/RALT/LGUI/RGUI`, `APPLICATION`, `PRINTSCREEN`, `PAUSE`. Every existing Mac row has a scancode equivalent; F13–F15 stand-ins stay. `kModifierSides` pairs `HOST_MOD_*` bits with scancodes. Add `HostKeyMapping host_key_mapping(int i)`, `HostKeyMapping host_key_mapping_for_scancode(uint16_t)` and `void host_input_peek_keys(uint8_t out[256])` (exposes the DIK key array the existing peek already owns). The escape-releases-capture check moves to `events.cpp`.

- [ ] **Step 3: `sdl/events.cpp`**

Translate `main.mm`'s `PendingInput` handling one to one; the queue and `queue_or_apply` (with `sched_in_idle_slice`/`sched_input_arrived`) move here unchanged:

| NSEvent path in `main.mm` | SDL event |
| --- | --- |
| `mouseMoved`/`mouseDragged` → MOTION with `inside`, `edge` | `SDL_EVENT_MOUSE_MOTION`: `x,y` in points × `SDL_GetWindowPixelDensity`, `xrel,yrel` scaled by `host_input_scale_delta`; `inside` from window bounds; `edge` unchanged |
| `mouseDown/Up`, `rightMouse*`, `otherMouse*` → BUTTON | `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` (`button` 1 left 2 middle 3 right → host 0/2/1 as today) |
| `scrollWheel` → WHEEL dz | `SDL_EVENT_MOUSE_WHEEL` `y` × 120 (WHEEL_DELTA), `SDL_MOUSEWHEEL_FLIPPED` negates |
| `keyDown/keyUp` → KEY (mac code, character) | `SDL_EVENT_KEY_DOWN/UP` (`scancode`, `repeat` ignored for KEY as today); `SDL_EVENT_TEXT_INPUT` supplies the character for WM_CHAR |
| `flagsChanged` → MODIFIERS | `SDL_EVENT_KEY_DOWN/UP` on modifier scancodes → `host_input_modifiers(e.key.mod, sides)` with `sides` tracked from the scancodes seen |
| `windowDidBecomeKey/ResignKey` → FOCUS | `SDL_EVENT_WINDOW_FOCUS_GAINED/LOST` → `apply_focus` posting WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS/WM_PAINT |
| `windowDidResize`/backing change → `post_drawable_size` | `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` → `host_present_resize(w, h)` |
| `windowWillClose` → close unwind | `SDL_EVENT_QUIT` / `SDL_EVENT_WINDOW_CLOSE_REQUESTED` → return false |
| escape keyCode 53 → RELEASE_CAPTURE | `SDL_SCANCODE_ESCAPE` while captured |
| `mouseConfinementRect` private API | `SDL_SetWindowMouseRect` to the game rect plus `SDL_SetWindowRelativeMouseMode` when `host_gate` asks for relative motion |
| `host_display_take_window()` modes | `sdl_host_apply_window_mode`: 0 `SDL_SetWindowFullscreen(w, false)` + bordered; 1 borderless `SDL_SetWindowBordered(false)` sized to the display; 2 `SDL_SetWindowFullscreen(w, true)` with `SDL_SetWindowFullscreenMode(w, nullptr)` (borderless desktop mode) |

- [ ] **Step 4: `sdl/main.cpp`**

Same sequence as `main.mm`'s `main()` with SDL in place of AppKit:

```cpp
int main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return report_fatal("SDL_Init", SDL_GetError());
    auto device = gpu::create_default_device();
    if (!device) return report_fatal("gpu", "no GPU backend for this platform");
    D3DRenderer renderer(device.get());
    host_present_set_device(device.get());
    int w, h; window_scale_for(&w, &h);                     // unchanged logic from main.mm
    SDL_Window *window = SDL_CreateWindow("Populous: The Beginning", w, h,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
    void *surface = host_native_surface(window);             // gpu/metal: SDL_Metal_CreateView + SDL_Metal_GetLayer
    SDL_GetWindowSizeInPixels(window, &w, &h);
    mods_display_load_modes("classic-modes.json");
    host_present_on_mode_change(on_mode_change);
    host_input_set_notify(dinput_host_input_changed);
    host_present_start(surface, w, h);
    SdlHostWindow hw{window, surface, w, h, 0, false};
    BootOptions opts = default_boot_options();                 // tick=pump, idle_wait, report, graces 15/30
    sched_set_input_queue(input_pending, drain_input);
    host_midi_startup(win32_midi_soundfont_path());
    SDL_ShowWindow(window);
    int rc = boot_run(argc, argv, &opts);
    host_present_stop();
    SDL_DestroyWindow(window); SDL_Quit();
    return rc;
}
```
`pump()` = `while (SDL_PollEvent(&e)) if (!sdl_host_handle_event(&hw, e)) request_close();` then `deliver_pending_input`, `apply_window_mode`, `apply_mode_change`, `after_events`, `host_gate_pointer_tick`, as today. `idle_wait(seconds)` → `SDL_WaitEventTimeout(nullptr, int(seconds * 1000))`. `host_native_surface` lives in `gpu/metal/metal_surface.mm` behind `void *gpu_native_surface_for_sdl_window(SDL_Window *)` declared in `gpu_factory.h`; a non-Apple backend supplies its own in sub-project 3.

- [ ] **Step 5: CMake and bundle**

`PopRecomp` sources: `sdl/main.cpp sdl/events.cpp present.cpp page_overlay.cpp ${POP_PRESENT_SOURCES} input.cpp input_gate.cpp audio.mm audio_math.cpp audio_capture.cpp midi.mm`; `pop_link_sdl(PopRecomp)`; frameworks drop `Cocoa` (SDL links what it needs). `pop_mac_bundle` keeps `Info.plist`; remove the menu-bar Settings item code path (`window_presentation.h` deleted; its `host_display_take_window` users move to `events.cpp`). `pop_smoke`, `present_events_tests`, `host_tests` list `input.cpp` and gain `pop_link_sdl` (for the scancode constants only).

- [ ] **Step 6: Verify**

`tools/build.py --target app`, `tools/test.py --native`, `--mods`, `integration_tests.sh`. Then the manual checklist, to be run by the user and recorded in `PROGRESS.md`: launch, front end renders, mouse and keyboard reach the game, Escape releases capture, window modes 0/1/2 via the existing keybinding, resize, focus loss pauses input, Cmd-Q quits cleanly with the close-unwind report. Before asking, run `build/recomp/pop_smoke` through the gameplay suite to confirm the non-window path.

Format, `check_repo.py`, `PROGRESS.md` (Task 6 done, manual checklist pending; next Task 7), commit `"Replace the AppKit host with SDL3"`.

---

### Task 7: Delete the Metal bridge

**Files:**
- Delete: `src/recomp/host/gpu/metal/metal_bridge.h`
- Modify: anything that still includes it (`grep -rn metal_bridge src/`)

- [ ] **Step 1:** `grep -rln 'metal_bridge\|#import' src/recomp/host --include='*.cpp' --include='*.h'`. Expected after Tasks 4–6: only `gpu_metal_tests.cpp` (for `gpu_test_make_native_surface`). Move that declaration to `gpu_factory.h` as `void *gpu_test_native_surface(int w, int h)` (Apple returns a layer; others return nullptr and the swapchain test is skipped), delete the bridge header, rebuild everything: `tools/build.py --target app --target smoke --target headless`, `ctest --preset macos -L "nogame|gpu|device|mods"`.
- [ ] **Step 2:** Commit `"Remove the temporary Metal bridge"`; `PROGRESS.md` (Task 7 done; next Task 8).

---

### Task 8: The portable mixer and sinks

**Files:**
- Create: `src/recomp/host/audio/mixer.h`, `mixer.cpp`, `sink.h`, `sdl_sink.cpp`, `offline_sink.cpp`, `audio/tests/audio_tests.cpp`
- Delete: `audio.mm` (end of task)
- Modify: `audio.h`, `tests/host_tests.mm`→`host_tests.cpp`, `smoke_main.cpp`, `headless_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `host_audio_*` contract in `dx/host_api.h`, `audio_math.cpp` helpers (`host_audio_pan_gains`, gain/rate/decode/wall_clock_bytes/position_bytes), `audio_capture.cpp`.
- Produces: `sink.h`:
  ```cpp
  struct AudioSink {
      virtual ~AudioSink() = default;
      virtual bool start(double sample_rate, int channels, std::function<void(float *interleaved, int frames)> render) = 0;
      virtual void stop() = 0;
      virtual double sample_rate() const = 0;
      virtual uint64_t frames_rendered() const = 0; // monotonic count the mixer clock reads
  };
  std::unique_ptr<AudioSink> make_sdl_sink();            // SDL_OpenAudioDeviceStream + SDL_SetAudioStreamGetCallback
  std::unique_ptr<AudioSink> make_offline_sink(double sample_rate, uint32_t max_frames); // render on demand
  ```
  `mixer.h`: `class Mixer` holding the `Channel` table; `void mixer_install(Mixer *, AudioSink *)`; every `host_audio_*` callback in `mixer.cpp` forwards to the installed mixer; `host_audio_engine()` returns the `Mixer *`; `host_audio_offline_begin/skip/end` drive the offline sink; `host_audio_set_node_ops` and `host_audio_engine_run` remain for `host_tests`.

- [ ] **Step 1: Port `Channel`, plan, reconcile, cursor**

`Channel` keeps every field (`pcm, bits, channels, base_rate, rate, rate_overridden, loop, volume_mb, pan_mb, start_offset, cursor, started, generation, playing, silent, streaming, stream_started, stream_base, stream_head, ring fields, stream_skew`). `plan_locked`, `reconcile_locked`, `stream_cursor_locked`, the `g_api_mutex`→`DataLock` lock order, atomic completion generations and `g_queued_total/g_queued_played` carry over as they are. What changes: the AVAudioEngine source node's render block becomes `Mixer::render(float *out, int frames)`, which for each playing channel decodes `pcm` with `audio_math`'s decode at `rate/sample_rate` using linear interpolation, applies `gain`/`pan`, sums into `out`, then runs the instant-release hard clipper and the capture tap (`audio_capture.cpp`). The node sample time the cursor math reads becomes `sink->frames_rendered()`; the wall-clock fallback stays. Completion (`host_audio_completed`, `host_audio_queue_completed`) fires from the render thread exactly where the node's completion callbacks fired.

- [ ] **Step 2: Write the failing `audio_tests.cpp`**

Move every audio test from `host_tests.mm` (they run `host_audio_offline_begin` then assert levels, cursors, queue health, clipping, pan) into `audio/tests/audio_tests.cpp` unchanged except for setup: `Mixer mixer; auto sink = make_offline_sink(48000, 1 << 20); mixer_install(&mixer, sink.get());`. Add one new test:

```cpp
static void mixer_render_is_deterministic_across_block_sizes() {
    auto a = render_tone_blocks(512), b = render_tone_blocks(64); // same Channel, same total frames
    CHECK(a.size() == b.size() && memcmp(a.data(), b.data(), a.size() * 4) == 0);
}
```
Label: `nogame` (the offline sink needs no device). The `peak > 0.095f` level test that failed on the hosted runner now runs on the offline sink and is expected to pass everywhere; if it does not, the mixer has a bug, not the machine.

- [ ] **Step 3: Sinks**

`sdl_sink.cpp`: `SDL_Init(SDL_INIT_AUDIO)`, `SDL_AudioSpec spec{SDL_AUDIO_F32, channels, int(sample_rate)}`, `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr)`, `SDL_SetAudioStreamGetCallback(stream, get_cb, this)` where `get_cb` renders `additional_amount / (channels * 4)` frames into a scratch vector and `SDL_PutAudioStreamData`; `SDL_ResumeAudioStreamDevice`. `sample_rate()` reports the device's actual rate from `SDL_GetAudioDeviceFormat`. `offline_sink.cpp`: `render(frames)` called by `host_audio_offline_skip`; stores nothing, just advances `frames_rendered_`.

- [ ] **Step 4: Wire and verify**

`host_tests.mm` → `host_tests.cpp` (no ObjC left once the audio tests moved; the CPU table stays). `PopRecomp`/`pop_headless` list `audio/mixer.cpp audio/sdl_sink.cpp audio/offline_sink.cpp audio_math.cpp audio_capture.cpp`; `pop_link_sdl(pop_headless)`; `FW_AUDIO` removed. `audio_tests` target like `gpu_fake_tests` with label `nogame`; `host_tests` is labelled `nogame` once the audio and renderer tests have moved out of it.

Run: `ctest --preset macos -L "nogame|gpu|mods"`, `tools/test.py --gameplay`, `build/recomp/pop_headless` with `POP_HOST_AUDIO_CAPTURE=/tmp/cap.wav` for 600 frames and check the WAV is non-silent (`python -c` reading RMS > 0.01). Delete `audio.mm`.

Format, `check_repo.py`, `PROGRESS.md` (Task 8 done; next Task 9), commit `"Replace AVAudioEngine with the portable mixer and SDL audio"`.

---

### Task 9: MIDI over TinySoundFont

**Files:**
- Create: `src/recomp/host/audio/midi_synth.cpp`, `tests/fixtures/make_sf2.py`
- Delete: `midi.mm`
- Modify: `midi.h` (comments only), `audio/tests/audio_tests.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `tsf.h` (`tsf_load_filename`, `tsf_set_output(TSF_STEREO_INTERLEAVED, rate, gain_db)`, `tsf_channel_note_on/off`, `tsf_channel_set_presetnumber`, `tsf_channel_midi_control`, `tsf_channel_set_pitchwheel`, `tsf_render_float`, `tsf_note_off_all`, `tsf_reset`); `Mixer` (Task 8) gains `void set_music_source(std::function<void(float*, int)>)` mixed after the channels, before the clipper.
- Produces: the unchanged `host_midi_*` functions; `host_midi_synth_name()` returns `"TinySoundFont"`.

- [ ] **Step 1: Fixture**

`tests/fixtures/make_sf2.py` writes a minimal valid SF2 (one preset, one instrument, one 64-sample sine sample, RIFF `sfbk` with `INFO`, `sdta`, `pdta` chunks) to `build/tests/fixtures/sine.sf2`; called from CMake as a custom command the `audio_tests` target depends on. Keep it under 120 lines; the struct layouts are in the SF2 2.04 spec §7.

- [ ] **Step 2: Failing tests**

```cpp
static void midi_note_on_renders_sound() {
    CHECK(host_midi_open(fixture_path("sine.sf2")) == 0);
    CHECK(host_midi_is_open());
    host_midi_short(0x007f3c90); // note on ch0, C4, vel 127  (data2<<16 | data1<<8 | status)
    std::vector<float> buf(2 * 4800);
    midi_render_for_test(buf.data(), 4800);
    float peak = 0; for (float s : buf) peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.05f);
    CHECK(host_midi_notes_started() == 1);
    host_midi_short(0x00003c80);
    midi_render_for_test(buf.data(), 4800); // release
    host_midi_reset();
}
static void midi_gm_reset_sysex_silences() {
    static const uint8_t gm[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    host_midi_short(0x007f3c90);
    host_midi_sysex(gm, sizeof gm);
    std::vector<float> buf(2 * 4800); midi_render_for_test(buf.data(), 4800);
    float peak = 0; for (float s : buf) peak = std::max(peak, std::fabs(s));
    CHECK(peak < 0.01f);
}
static void midi_close_keeps_synth() { host_midi_close(); CHECK(!host_midi_is_open()); CHECK(host_midi_open(fixture_path("sine.sf2")) == 0); }
```
`midi_render_for_test(float*, int)` is declared in `midi.h` under a `// test seam` comment.

- [ ] **Step 3: `midi_synth.cpp`**

`#define TSF_IMPLEMENTATION` then `#include "third_party/tsf/tsf.h"`. `host_midi_startup` tries the same candidate table `midi.mm` has (the game's `Sound/POPFIGHT.SF2` via `win32_midi_soundfont_path()` first); `host_midi_open` loads with `tsf_load_filename`, sets output to the mixer's rate, registers `set_music_source`. `host_midi_short` parses `status = msg & 0xff`, `data1 = (msg >> 8) & 0x7f`, `data2 = (msg >> 16) & 0x7f`: `0x90` with `data2 > 0` → `tsf_channel_note_on(ch, key, vel/127.f)` and `++notes_started`; `0x90` vel 0 and `0x80` → note off; `0xb0` → `tsf_channel_midi_control`; `0xc0` → `tsf_channel_set_presetnumber(ch, data1, ch == 9)`; `0xe0` → `tsf_channel_set_pitchwheel(ch, data1 | data2 << 7)`. Sysex: the GM reset bytes → `tsf_note_off_all` + `tsf_reset`-equivalent channel reset; anything else dropped, as today. `host_midi_close` stops feeding the mixer but keeps the `tsf*`. Rendering under its own mutex; `tsf_render_float(synth, out, frames, 0)`.

- [ ] **Step 4: Verify**

`audio_tests` passes; `PopRecomp` links `audio/midi_synth.cpp` and not `midi.mm`; `host_tests` likewise. Run the gameplay suite and the headless host with capture; front-end music is present in the WAV (RMS check as in Task 8). Delete `midi.mm`.

Format, `check_repo.py`, `PROGRESS.md` (Task 9 done; next Task 10), commit `"Synthesize MIDI with TinySoundFont"`.

---

### Task 10: Cleanup, labels, CI and docs

**Files:**
- Modify: `src/recomp/host/CMakeLists.txt`, `src/recomp/host/gpu/CMakeLists.txt`, `.github/workflows/checks.yml`, `tools/test.py`, `src/recomp/host/README.md`, `docs/` build and test pages from sub-project 1, `docs/superpowers/PROGRESS.md`, `NOTICE`

- [ ] **Step 1: Prove the boundary**

```sh
find src/recomp/host -name '*.mm' -not -path '*/gpu/metal/*'     # expected: nothing
grep -rln '#import\|<Metal/\|<AppKit/\|<AVFoundation/\|<CoreText/\|<CoreVideo/' src/recomp/host --include='*.cpp' --include='*.h' | grep -v gpu/metal  # expected: nothing
```
Add that pair as `tools/recomp/tests/test_host_boundary.py` (two asserts over `pathlib` globs) and register it in CTest as `host_boundary_check`, label `nogame`.

- [ ] **Step 2: CMake**

`if(NOT APPLE) return()` in `host/CMakeLists.txt` becomes: build `gpu_fake`, `gpu_fake_tests`, `compositor_tests`, `ui_layer_tests`, `presenter_tests`, `audio_tests`, `host_tests` on every platform; `gpu_metal`, `gpu_metal_tests`, `d3d_render_tests`, the hosts and `present_events_tests` (needs a device factory) stay `if(APPLE)` until sub-project 3 supplies Vulkan. `FW_AUDIO` and `"-framework Cocoa"` deleted; `FW_METAL` shrinks to what `gpu_metal` links. CI: `ctest -L nogame` on Linux and Windows now includes the new portable suites; `gpu` and `device` stay macOS-local. Run `gh workflow run checks.yml --ref host-abstraction` and confirm the `headSha`.

- [ ] **Step 3: Docs**

`src/recomp/host/README.md`: new layout tree (the File structure table of this plan), the `gpu.h` contract in one paragraph, the shader contract pointer, the SDL host, the mixer, how to run each suite. Sub-project 1 docs: replace mentions of AppKit/AVAudioEngine/CoreVideo. `NOTICE` already updated in Task 1. `PROGRESS.md`: roadmap row for sub-project 2 set to done, "Now" points at sub-project 3 brainstorming, log entry.

- [ ] **Step 4: Full verification and finish**

```sh
.venv/bin/python tools/build.py --target app --target smoke --target headless
.venv/bin/python tools/test.py --native && .venv/bin/python tools/test.py --mods && .venv/bin/python tools/test.py --gameplay
sh src/recomp/host/tests/integration_tests.sh && sh tools/recomp/mods_test.sh
.venv/bin/python tools/check_repo.py
```
Expected: only the pre-existing failures from `PROGRESS.md`. Commit `"Finish the host abstraction: portable suites, CI labels, docs"`, then use the finishing-a-development-branch skill.

---

## Self-review notes

- Spec coverage: GPU interface (T2), Metal backend (T3), presenter/compositor/overlay (T4), renderer (T5), SDL3 window/input (T6), bridge removal (T7), mixer/sinks (T8), TinySoundFont MIDI (T9), `.mm` boundary, labels, CI, docs (T10). Accepted losses from the spec (menu-bar Settings item, Apple GM fallback, private confinement call) are each named in T6 and T9.
- Type consistency: `gpu::Texture`/`Device`/`FakeDevice` names are identical across tasks; `host_present_set_device`, `host_present_start(void*,int,int)`, `host_present_start_offscreen(int,int)` in T4 are what T5/T6 call; `HostKeyMapping{scancode,dik,vk}` in T6 matches its tests; `AudioSink`/`Mixer`/`mixer_install` in T8 are what T9 extends.
- Formats: `RGB565` from the spec's list is not in `gpu::Format`; the presenter expands 565 on the CPU today (`host_present_expand_rgb565`) and that stays, so no backend needs the format.
