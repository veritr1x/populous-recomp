# Vulkan backend and the Windows and Linux hosts (sub-project 3)

Date: 2026-09-12. Follows `2026-09-12-host-abstraction-design.md` (sub-project
2, merged). Precedes sub-project 4, the release pipeline.

## Goal

Populous runs windowed on Windows and Linux through the same SDL3 host,
presenter, compositor, renderer and mixer that macOS uses, over a Vulkan
implementation of `src/recomp/host/gpu/gpu.h`. The runtime and mods test
suites run on Windows as they do on Linux. CI proves the portable layers and,
on Linux, the GPU suites over a software Vulkan driver.

## Decisions

| Decision | Choice |
| --- | --- |
| Backend approach | A direct Vulkan implementation of `gpu.h` in `gpu/vulkan/`, mirroring the Metal backend's shape. Not SDL_GPU, not WebGPU. |
| Where Vulkan is developed | On the macOS development machine through MoltenVK (Homebrew `molten-vk`, `vulkan-loader`, `vulkan-headers`, `shaderc`), selected with `POP_GPU_BACKEND=vulkan`. |
| Shaders | Hand-written Vulkan GLSL under `gpu/vulkan/shaders/`; SPIR-V compiled with `glslc` and committed as C arrays; a `nogame` check recompiles and fails on drift when `glslc` is present. |
| Loading Vulkan | `volk` (vendored) and the Vulkan headers (vendored). No SDK needed to build or play. |
| Done means | Hosts build on all three platforms; `nogame` and Linux `gpu` (lavapipe) suites green in CI; the game-backed suites pass on macOS over Vulkan and match Metal within tolerance; one manual run each on real Windows and Linux hardware, later, when available. VM validation was considered and deferred. |
| Windows tests | The runtime and mods suites are ported off `fork`/`mkdtemp` onto `os.h` and run in Windows CI. |

## Non-goals

- No Direct3D 12 backend, no Android, no iOS, no web.
- No release packaging or installer (sub-project 4).
- No single-source shader pipeline; the MSL stays hand-written.
- No changes to the renderer, presenter, compositor or mixer beyond bugs the
  new backend exposes.
- No GPU hardware runner in CI; Windows `gpu` suites stay local.

## 1. The Vulkan backend

Files: `src/recomp/host/gpu/vulkan/vulkan_device.{h,cpp}`,
`vulkan_swapchain.cpp`, `shaders_spv.h`, `shaders/*.vert|.frag|.comp`,
`tests/` shares `gpu_contract_tests.cpp` with the Metal backend.

Loading. `volkInitialize()` at device creation; failure means the factory
returns null, as it does today off Apple. On macOS the loader finds MoltenVK
through its normal ICD search; `VK_ICD_FILENAMES` is honoured but not
required. Instance: Vulkan 1.3 requested, 1.1 accepted when the device offers
`VK_KHR_dynamic_rendering` (plus `VK_KHR_portability_subset` and the instance's
`VK_KHR_portability_enumeration` on MoltenVK). On macOS the Homebrew loader is
not on the dynamic linker's default path, so the backend also tries
`/opt/homebrew/lib/libvulkan.1.dylib` and `/usr/local/lib/libvulkan.1.dylib`,
honours `POP_VULKAN_LIBRARY`, and exposes the path it loaded so the SDL host
can hand it to `SDL_Vulkan_LoadLibrary`. Validation layers are enabled when
`POP_GPU_VALIDATE=1` and the layer is installed; messages go to stderr.

Device and queue. The first discrete physical device, else the first with a
graphics-and-compute queue family; one queue from it; one command pool with
resettable buffers. `begin()` allocates and starts a primary command buffer.
`commit()` ends it and submits it with a fresh fence. A reaper thread waits on
fences in submission order and, for each finished buffer, runs its
`on_complete` callbacks with the GPU time from a timestamp query pair written
at the buffer's start and end, then recycles the buffer, fence and queries.
In-order fence waits are what make "command buffers complete in commit order"
true here. `wait(cb)` waits on that buffer's fence; `status(cb)` reads it
without waiting; unknown ids read as Completed.

Textures. Every texture is a device-local image. `UsageCpu` textures also own
a persistently mapped host-visible staging buffer sized for level 0. `upload`
writes staging and records a buffer-to-image copy on an internal transfer
command buffer that is submitted and waited before returning, matching the
synchronous semantics `replaceRegion` had. `readback` waits for the last
submitted fence, records image-to-buffer, submits, waits, and copies out.
Private textures stage through a transient buffer for both. Image layouts are
tracked per image and transitioned with barriers on each use: colour or depth
attachment in a render pass, shader read when bound, transfer source or
destination for copies. Formats: BGRA8 `B8G8R8A8_UNORM`, RGBA8
`R8G8B8A8_UNORM`, R8 `R8_UNORM`, Depth32F `D32_SFLOAT`. `mip_levels` full
chains allocate every level; `generate_mipmaps` is a chain of blits with
barriers. `allocated_bytes` reports the memory requirement size.

Buffers. Host-visible, host-coherent, persistently mapped. `update` is a
memcpy; `map_read` waits for the last submitted fence and returns the mapping.

Pipelines. `render_pipeline(name, state)` returns a handle to a pipeline
family: the named GLSL pair plus the `RenderState` blend factors, colour write
mask, attachment formats and depth format. The `VkPipeline` itself is created
lazily at draw time for the variant the recorded `Primitive`, `Cull` and
`DepthState` need, and cached by (family, topology, cull, compare, write), so
no extended dynamic state extension is required; viewport and scissor are the
only dynamic state. Render passes use dynamic rendering
(`VK_KHR_dynamic_rendering`, core in 1.3), no render pass objects. Vertex data
is pulled: the "d3d" vertex shader indexes a storage buffer of 56-byte
`HostD3DVertex` by `gl_VertexIndex`, because the renderer supplies small draws
through `set_bytes` as well as through `set_vertex_buffer`. `compute_pipeline
(name)` builds the named kernel; `thread_execution_width` reports the subgroup
size, or the workgroup size when the shared-memory variant is chosen.

Bindings. Every buffer-like binding is a storage buffer descriptor; there are
no push constants (only 128 bytes are guaranteed, the D3D uniforms are 144).
`set_bytes` copies into a per-command-buffer host-visible ring (allocations
256-aligned) and binds that range; `set_buffer` and `set_vertex_buffer` bind
the buffer at the given offset. One descriptor set layout serves every
pipeline: bindings 0..3 are the vertex stage's buffer slots 0..3, 4..7 the
fragment stage's, 8..11 combined image samplers for texture slots 0..3
(compute uses 0..3 and 8..11). A descriptor set is written per draw or
dispatch from a per-command-buffer pool; slots the shader does not set hold a
dummy buffer and a 1x1 texture. Samplers are cached by `SamplerState`.

Layouts and barriers. Every image the backend owns lives in `VK_IMAGE_LAYOUT_
GENERAL` for its whole life, so two threads recording into different command
buffers never disagree about a texture's layout; only swapchain images move
(UNDEFINED at acquire, GENERAL on first use, PRESENT_SRC at present). A full
memory barrier is recorded before each render pass, compute pass and transfer,
which with single-queue submission order is what makes one command buffer's
writes visible to the next.

Coordinates. Metal's clip space has y up and Vulkan's y down; the backend
passes a negative-height viewport (`VK_KHR_maintenance1`, core in 1.1) so the
shared shader arithmetic is untouched, and sets front face clockwise to match
the Metal backend's winding. Depth 0..1 and the top-left texture origin are the
same in both.

Compute. `begin_compute_pass`/`end_compute_pass` bracket dispatches;
`dispatch_threads` rounds up to workgroups of the requested local size;
resources bound with `Stage::Compute` go to the compute pipeline's set 0.
`native_brightness` uses `subgroupAdd` (core since Vulkan 1.1) when the device
reports arithmetic subgroup operations for compute and a shared-memory tree
otherwise; the output layout is the one
`shaders.md` fixes, so the renderer does not know which.

Swapchain. On Vulkan platforms the SDL host passes its `SDL_Window*` as the
native surface; `create_swapchain` makes the surface with
`SDL_Vulkan_CreateSurface`, picks BGRA8 UNORM (else the first format), FIFO
present mode, three images. `acquire` waits on an acquire semaphore and
returns the image as a texture handle; `present` records the final layout
transition on the command buffer and remembers the image; `commit` then submits
waiting on the acquire semaphore, signalling a render-finished semaphore, and
queues the present against it. The presented time reported is the time the
submission's fence completed, read by the reaper thread on the backend clock:
the presenter treats a zero presented time as a missing acknowledgement and
logs a fault, so the completion time stands in for the flip time until a
timing extension is adopted. `min_duration_seconds` is ignored; FIFO paces.
`release_drawable` consumes the acquire semaphore with an empty fenced submit.
`resize` recreates the swapchain; a suboptimal or out-of-date result also
recreates it at the next acquire. `refresh_period` comes from the display
mode SDL reports for the window's display.

Clock. `now_seconds()` is the platform monotonic clock; `on_complete`
timestamps and presented times are on that clock.

Errors. Device loss or a failed submit sets the device to a failed state:
every later `begin` returns a null handle, `status` reports Error, and the
message is logged once. The presenter already treats a null command buffer as
a dropped frame.

## 2. Shaders

`gpu/vulkan/shaders/` holds `d3d.vert`, `d3d.frag`, `surface_upload.vert`,
`surface_upload.frag`, `compositor.vert`, `compositor.frag`, `hud.vert`,
`hud.frag`, `guest_readback.comp`, `guest_readback_fused.comp` and
`native_brightness.comp`, each a line-for-line translation of the MSL in
`gpu/metal/shaders_msl.h` with the bindings `shaders.md` documents. Vertex
positions follow Vulkan's clip conventions: the `d3d` vertex program flips Y
and maps depth 0..1 unchanged, so the renderer's matrices and viewport
arithmetic do not change; `compositor` and `hud` emit clip positions directly
from the same quad arithmetic with the Y flip applied.

`tools/recomp/shaders.py compile` runs `glslc -O --target-env=vulkan1.2` on
each file and writes `gpu/vulkan/shaders_spv.h`, one `static const uint32_t`
array per program plus a table of names. `tools/recomp/shaders.py check`
recompiles into a temporary file and compares; it is a `nogame` CTest that
passes with "skipped: no glslc" when the compiler is absent. The committed
header is what the backend compiles; the SDK is a developer tool only.

## 3. Hosts on Windows and Linux

The platform layer `src/recomp/platform/os.h` gains:

- `os_spawn(const char *const argv[], int *pid_out)` and
  `os_wait(pid, int *status)`, POSIX `fork`+`execv` and Win32
  `CreateProcess`, used by `runtime_tests` where it forks a child.
- `os_mkdtemp(char *template)`, `mkdtemp` and `_mktemp_s` + `CreateDirectory`.
- `os_module_suffix()` returning `.dylib`, `.so` or `.dll`, used where tests
  name plugin files.

`runtime_tests`, `mods_tests`, `host_tests` and the roots shell test move onto
those (`roots_tests.sh` becomes a Python test so Windows runs it). The
`if(NOT WIN32)` guards around those targets go. Any remaining `<unistd.h>` or
`<sys/stat.h>` use in the hosts moves behind `os.h` helpers that already
exist (`os_stat`, `os_exe_path`, `os_chdir`).

`sdl/main.cpp` creates the window with `SDL_WINDOW_METAL` on Apple and
`SDL_WINDOW_VULKAN` elsewhere, and hands the presenter the Metal layer or the
`SDL_Window*` accordingly through one `gpu::native_surface_for(SDL_Window*)`
in `gpu_factory`. The exe search and `classic-modes.json` lookup already walk
from `os_exe_path`; on the two new platforms there is no bundle, so the
checkout paths are used. `pop_headless` and `pop_smoke` need nothing new.

The file shim's case-insensitive resolution of the game's paths is exercised
for real on Linux for the first time; failures there are bugs to fix in the
shim, not in the host.

## 4. Build and dependencies

`third_party/volk/` (volk.h, volk.c, LICENSE, MIT) and
`third_party/vulkan-headers/include/` (Apache 2.0) are vendored with NOTICE
entries. `gpu/CMakeLists.txt` builds `gpu_vulkan` (an OBJECT library over
`volk` with `VK_NO_PROTOTYPES`) on every platform and `gpu_metal` on Apple;
`gpu_factory.cpp` creates Metal on Apple and Vulkan elsewhere unless
`POP_GPU_BACKEND` names the other, and reports the name through
`default_backend_name()`. The SDL host prints which backend it got.
`gpu_contract_tests.cpp` is compiled twice, as `gpu_metal_tests` (Apple,
label `gpu`) and `gpu_vulkan_tests` (everywhere, label `gpu`); each forces its
backend through the override. Presets are unchanged. The `windows` preset
keeps clang and lld.

## 5. CI, tests and verification

Linux CI installs `mesa-vulkan-drivers` and runs `-L "nogame|gpu"`:
`gpu_vulkan_tests`, `compositor_tests`, `host_tests` (renderer pixel tests
included) all run over lavapipe. Windows CI runs `-L nogame`, which now
includes `mods_tests` and `roots_tests` (ported); `runtime_tests` keeps its
`game` label and runs locally wherever game files exist; Windows `gpu` suites
are local only.
macOS CI runs `nogame|gpu` as today, with the Metal backend; `gpu_vulkan_tests`
on macOS requires MoltenVK and is labelled `gpu-vulkan` so hosted runners skip
it and the development machine runs it.

Game-backed proof on macOS with `POP_GPU_BACKEND=vulkan`: `tools/test.py
--gameplay`, `--mods`, `integration_tests.sh`, and the offline flyby scene
compared to the Metal render with a per-pixel tolerance (mean absolute
difference under 2 levels over the frame, no pixel over 24), since two drivers
rasterise differently.

Manual: one run each on real Windows and Linux hardware, recorded in
`PROGRESS.md`, when the hardware is available; not a gate for merging.

## Risks

- MoltenVK's portability subset forbids a few features and some GPUs lack
  arithmetic subgroup operations; the design keeps the shared-memory reduction
  path for `native_brightness` and uses nothing the subset forbids (no wide
  lines, no geometry shaders, no triangle fans).
- lavapipe is slow; the Linux `gpu` suites will take minutes, not seconds.
  Acceptable for CI; the renderer pixel tests are small.
- The presented time is the fence completion time, not the flip. FPS
  accounting on Vulkan will read slightly early until a timing extension is
  adopted.
- The swapchain contract test needs a native surface; the Vulkan backend makes
  one from a hidden SDL window, which a display-less Linux runner cannot give,
  so the test reports "skipped" there rather than failing.
