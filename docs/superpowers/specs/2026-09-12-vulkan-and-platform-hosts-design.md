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
required. Instance: Vulkan 1.3 requested, 1.2 accepted with the
`VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2` and (on MoltenVK)
`VK_KHR_portability_subset` extensions. Validation layers are enabled when
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

Pipelines. `render_pipeline(name, state)` builds a graphics pipeline for the
named GLSL pair with the `RenderState` blend factors, colour write mask,
attachment formats and depth format, using dynamic rendering (no render pass
objects) and dynamic viewport, cull mode, depth test enable, depth write
enable and depth compare op, so `set_depth`, `set_cull` and `set_viewport` are
recorded commands rather than pipeline variants. Vertex input for "d3d" is
one binding of 56-byte `HostD3DVertex`; the other programs take no vertex
input. `compute_pipeline(name)` builds the named kernel;
`thread_execution_width` reports the subgroup size.

Bindings. `set_bytes(stage, 1, ...)` for the 144-byte D3D uniforms and the
other small blocks is a push-constant range (slot 0 and 1 blocks are at most
144 bytes; the surface-upload palette of 1024 bytes at slot 2 goes through
the uniform ring instead). `set_buffer` and larger `set_bytes` use a
host-visible uniform/storage ring (4 MB, wrapped per frame) and descriptor set
0. Textures and samplers are combined image samplers in set 0 at their slot
numbers; samplers are cached by `SamplerState`. Descriptor sets come from a
per-command-buffer pool reset when the buffer is recycled.

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
transition on the command buffer, submits it signalling a render-finished
semaphore, then queues the present against it. The presented time reported is
0 (no timing extension is required), so the presenter's completion fallback
paces repeats, as it does for a Metal drawable without a presented time.
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
- Presented-time-less presentation relies on the presenter's completion
  fallback, which counts frames as shown at GPU completion. FPS accounting on
  Vulkan will differ from Metal's until a timing extension is adopted.
