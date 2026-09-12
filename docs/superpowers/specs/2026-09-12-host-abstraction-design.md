# Host abstraction: GPU interface, SDL3 window, portable audio

Sub-project 2 of the multi-platform port. Date: 2026-09-12.

## Roadmap context

Sub-project 1 (portable build system and runtime) landed on `main` on
2026-09-12. Decisions made while scoping this sub-project that revise the
roadmap recorded in `2026-09-12-portable-build-system-design.md`:

| Decision | Choice |
| --- | --- |
| macOS window layer | Moves to SDL3 with this sub-project. AppKit is retired; no AppKit abstraction is built. |
| Audio | One portable software mixer on every platform, output through SDL3 audio, starting with macOS in this sub-project. AVAudioEngine and the Apple DLS synth are retired. |
| MIDI | TinySoundFont, vendored, playing the game's `Sound/POPFIGHT.SF2`. No system General MIDI fallback. |
| GPU interface level | Device level: textures, buffers, pipelines, command buffers, fences, swapchain. The renderer, compositor, presenter and overlay are portable C++ over it. Metal is the first backend. |
| Sub-project 3 | Shrinks to the Vulkan backend, its shaders, and the Windows and Linux hosts' presets and CI labels. |
| Sub-project 4 | Unchanged: release pipeline with the translation embedded. |

## Goal

The macOS app runs on SDL3 for its window and input, renders through a
device-level GPU interface whose only implementation is Metal, and mixes
audio in portable C++ with SDL3 audio output and TinySoundFont MIDI. No AppKit,
AVFoundation, AudioToolbox, CoreVideo or CoreText code remains outside
`src/recomp/host/gpu/metal/`. The smoke host drives the same presenter
offscreen through the GPU interface; the headless host stays GPU-free.

## Non-goals

- No Vulkan backend, no Windows or Linux host, no iOS. Everything outside
  `gpu/metal/` compiles on Linux and Windows, and the `nogame` suites run
  there, but no window opens.
- No change to the DirectX shims, the runtime, the mod foundation or the
  translator. The `host_*` callback contract in `src/recomp/dx/host_api.h` is
  unchanged.
- No new rendering features. Classic, Enhanced, Wide view, the texture pack,
  the performance overlay and the settings page render as today.

## Facts the design rests on

From a file-by-file survey of `src/recomp/host/` on 2026-09-12:

- Already portable, no Apple API: `present.mm` (234 lines, `.mm` by convention),
  `ui_layer.mm` (335), `input.mm` (577; the only macOS-specific content is the
  key-code column of its table), `boot.cpp`, `input_gate.cpp`,
  `present_pixels.cpp`, `page_overlay.cpp`, `script.cpp`, `audio_math.cpp`,
  `audio_capture.cpp`, `report_lock.cpp`, `headless_main.cpp`.
- `main.mm` (1,379 lines) defines no `host_*` callback. It is AppKit window
  and event plumbing plus portable pieces: the pending-input queue and
  replay, pointer-capture policy, window-scale arithmetic, the run report.
- `present_thread.mm` (1,575 lines) is about 70% neutral: the sealed-frame
  mailbox, target pool, leases, retirement order, pacing and metrics. Metal
  is confined to target allocation, `nextDrawable`, compose, blit and
  `commit_present`. It reads CoreVideo host time at seven sites and owns a
  `CVDisplayLink` whose callback only records the refresh period and signals
  a tick. The fake-presenter tests already drive the state machine with no
  GPU, using an `NSObject` posing as a Metal texture.
- `compositor.mm` (510) is about 80% neutral geometry; its header already
  splits portable value types from Metal types behind `#ifdef __OBJC__`,
  with `CompositorInput` forward-declared for C++ consumers.
- `d3d_render.mm` (3,928) is the hard core: a ~60-ivar Objective-C class
  mixing Metal objects with neutral bookkeeping (vertex decode and primitive
  expansion, render-state hashing into pipeline keys, texture decode, the
  replay journal, scene slots). Three MSL sources are inline: the D3D
  vertex/fragment pair with a second coverage color attachment, three compute
  kernels for guest-resolution readback and native brightness (one uses
  `simd_sum`), the surface-upload fullscreen pass, the compositor quad pass,
  and the overlay HUD pass.
- `performance_overlay.h` imports Metal and CoreText unconditionally and is
  held by value inside the presenter; its text is rasterized with CoreText.
- `CompositorInput` and `HostSceneTarget` carry `id<MTLTexture>` across
  `d3d_render.mm`, `present_thread.mm` and `compositor.mm`; the presenter
  keys target leases and input validation on texture pointer identity.
- The renderer and presenter share one `MTLCommandQueue` so a present cannot
  run ahead of its scene. The ordering guarantee is implicit.
- `audio.mm` (2,384) implements the `host_audio_*` contract on AVAudioEngine
  with a documented two-mutex lock order, lock-free completion generations,
  gapless stream conversion, an instant-release hard clipper, offline
  rendering for tests and a capture tap. `audio_math.cpp` already holds the
  decode, gain, pan, rate and cursor arithmetic. `midi.mm` (337) drives
  Apple's MIDISynth or DLSSynth AudioUnit with a three-candidate fallback.
- The GOG installation ships `Sound/POPFIGHT.SF2`; the game plays music as
  MIDI through `midiOut*`, which the runtime routes to `host_midi_*`.
- The repository already has a 6x8 bitmap font (`src/recomp/mods/font6x8.cpp`).
- SDL3's latest release tag is `release-3.4.16`. TinySoundFont is MIT, a
  single header, last pushed July 2026.

## Design

### Layout

```
src/recomp/host/gpu/gpu.h                   GpuDevice, handles, enums, contract comments
src/recomp/host/gpu/shaders.md              the per-shader uniform/attribute contract
src/recomp/host/gpu/fake/fake_device.{h,cpp}
src/recomp/host/gpu/fake/tests/gpu_fake_tests.cpp
src/recomp/host/gpu/metal/metal_device.{h,mm}
src/recomp/host/gpu/metal/shaders_msl.h     the MSL sources, moved verbatim
src/recomp/host/gpu/metal/metal_surface.mm  CAMetalLayer swapchain
src/recomp/host/present_frame.h             the sealed Frame and CompositorInput on handles
src/recomp/host/present_thread.cpp          was present_thread.mm
src/recomp/host/compositor.cpp              was compositor.mm
src/recomp/host/d3d_render.cpp              was d3d_render.mm
src/recomp/host/performance_overlay.cpp     was header-only; bitmap-font text
src/recomp/host/present.cpp, ui_layer.cpp, input.cpp   renamed, contents unchanged
src/recomp/host/sdl/main.cpp                the SDL3 host, replaces main.mm
src/recomp/host/sdl/events.{h,cpp}          SDL events to host_input/gate calls
src/recomp/host/sdl/keymap.cpp              SDL_Scancode -> DIK/VK table
src/recomp/host/audio/mixer.{h,cpp}         every host_audio_* callback
src/recomp/host/audio/sink.h                AudioSink interface
src/recomp/host/audio/sdl_sink.cpp          SDL3 audio stream output
src/recomp/host/audio/offline_sink.cpp      synchronous render for tests and smoke
src/recomp/host/audio/midi_synth.cpp        host_midi_* over TinySoundFont
src/recomp/host/audio/tests/audio_tests.cpp
third_party/tsf/tsf.h, LICENSE              TinySoundFont
cmake/Dependencies.cmake                    SDL3 FetchContent, pinned
```

### The GPU interface

`gpu.h` declares one abstract class, `GpuDevice`, and plain handle structs.
A handle is a `uint64_t` id inside a typed struct: `GpuTexture`, `GpuBuffer`,
`GpuPipeline`, `GpuCommandBuffer`, `GpuSwapchain`. Id 0 is null. Backends
keep an id-to-native table; a handle's id is the presenter's lease key,
replacing texture pointer identity.

The device offers exactly what the four consumers use today:

- **Textures.** `create_texture(desc)` with width, height, a format enum
  (`BGRA8`, `RGBA8`, `R8`, `RGB565`, `Depth32F`) and usage flags
  (`RenderTarget`, `Sampled`, `Storage`, `Readback`); `upload(texture,
  region, bytes, pitch)`; `readback(texture, region, bytes, pitch)`, which
  waits for outstanding work on that texture; `blit(cmd, src, dst, region)`;
  `destroy(texture)`.
- **Buffers.** `create_buffer(bytes, contents)`, `update(buffer, offset,
  bytes)`, `destroy(buffer)`. Used for vertices, uniforms and compute outputs.
- **Pipelines.** `create_render_pipeline(shader_name, RenderState)`, where
  `RenderState` carries blend mode, depth compare and write, the color
  attachment formats and whether point size is written;
  `create_compute_pipeline(shader_name)`. Shader names are strings defined in
  `gpu/shaders.md`; each backend maps a name to its own source. The uniform
  and attribute layouts in that document are the contract every backend
  implements.
- **Command buffers.** `begin()`; `begin_render_pass(cmd, attachments)` with
  up to two color attachments (color and coverage) and one depth attachment,
  each with load and store actions and a clear value; `set_pipeline`,
  `set_vertex_buffer(slot)`, `set_bytes(stage, slot, bytes)` for inline
  push data, `set_texture(stage, slot)`, `set_sampler(stage, slot,
  filter, address)`, `draw(primitive, first, count)`; `end_render_pass`;
  `begin_compute_pass`, `dispatch(groups, threads_per_group)`,
  `end_compute_pass`; `on_complete(cmd, callback)`; `commit(cmd)`. The
  interface states the guarantee the code relies on today: command buffers
  committed on one device complete in commit order.
- **Swapchain.** `create_swapchain(native_surface, width, height)`, where the
  native surface is an opaque pointer the window layer supplies (a
  `CAMetalLayer *` on macOS); `resize`; `acquire(swapchain)` returns a
  texture handle for this frame or null; `present(cmd, swapchain,
  min_duration_seconds, presented_callback)`; `refresh_period(swapchain)`.
- **Clock.** `now_seconds()`. The presenter's seven CoreVideo reads become
  this call; the fake backend controls it.

### The Metal backend

`metal_device.mm` owns the `MTLDevice`, one `MTLCommandQueue`, the handle
tables and the pipeline cache keyed by `(shader_name, RenderState)`. The
three MSL sources move verbatim into `shaders_msl.h`. `metal_surface.mm`
sizes the `CAMetalLayer`, calls `nextDrawable`, and implements `present` with
`presentDrawable:afterMinimumDuration:`, `addPresentedHandler` and
`addCompletedHandler`. The readback row copies keep `dispatch_apply_f`
inside the backend. The brightness kernel keeps `simd_sum`; the contract
names the reduction, not the instruction.

### The fake backend

`fake_device.cpp` implements the interface with CPU-side storage for every
texture so `upload` and `readback` round-trip bytes, records draws and
dispatches per command buffer, runs completion and presented callbacks
immediately on `commit`, and exposes `advance_clock(seconds)`. It replaces
the `NSObject`-as-texture trick and lets the presenter, compositor and
renderer tests run as plain C++ on every platform.

### Consumers

- `d3d_render.cpp`: the neutral parts stay as they are (vertex decode,
  primitive expansion, render-state hashing, texture decode, replay journal,
  scene slots, HD cache). The Objective-C class becomes a C++ class with the
  same members typed as handles; every encoder path is rewritten over command
  buffers. The readback compute pipelines, the surface-upload pass and the
  coverage attachment map one to one onto the interface.
- `compositor.cpp`: geometry, anchors, pointer position and scene history
  unchanged; `compositor_compose` issues one render pass with the quad
  pipeline's three blend variants. `CompositorInput` moves to
  `present_frame.h` with handles.
- `present_thread.cpp`: the state machine unchanged. `Frame` moves to
  `present_frame.h` with handles, so the renderer and the tests can name it.
  `acquire` allocates targets through the device; `tick` acquires from the
  swapchain and composes; `commit_present` becomes `present`. The display link
  is replaced by `refresh_period` plus pacing on presented callbacks;
  offscreen mode keeps its synthetic 120 Hz link.
- `performance_overlay.cpp`: text rasterized with the 6x8 bitmap font into an
  `R8` texture, drawn with the HUD pipeline. CoreText goes.

### Lifetime rule

Today ARC keeps textures alive while a frame references them. Under the
interface a texture lives until `destroy`. The presenter's lease counting,
which already governs the target pool, becomes the only owner of texture
lifetime: every handle stored in a `Frame` or in the scene history is held
through a lease, and `destroy` is called when the last lease drops. The
compositor's scene-history substitution assigns handles and takes a lease
instead of assigning Objective-C references.

### The SDL3 host

`host/sdl/main.cpp` replaces `main.mm`. It creates the SDL window, the GPU
device through the backend factory, the swapchain from SDL's native surface
(`SDL_Metal_CreateView` and `SDL_Metal_GetLayer` on macOS; an SDL Vulkan
surface elsewhere later), starts the presenter, installs the `BootOptions`
hooks, runs the guest on the main thread as `main.mm` does, and pumps SDL
events from `tick` and `idle_wait`. The portable pieces of `main.mm` move
unchanged: the pending-input queue and replay, pointer-capture policy,
window-scale arithmetic, the run report.

`events.cpp` maps SDL events onto the functions `input_gate.cpp` and
`input.cpp` expose: key down and up, modifiers, buttons, motion with relative
deltas, wheel, focus lost, resize, display change, quit. `keymap.cpp` is the
`HostKeyMapping` table keyed on `SDL_Scancode` instead of the macOS key code;
SDL scancodes are positional USB HID codes, the property the macOS table was
chosen for, so the DIK and VK columns carry over one to one.

Window behaviors: `SDL_SetWindowFullscreen`; `SDL_SetWindowMouseRect` for
confinement during play and `SDL_SetWindowRelativeMouseMode` for relative
motion, replacing the private AppKit call; `SDL_ShowCursor` and
`SDL_HideCursor`; resize and display-change events drive `host_present_resize`
and swapchain re-creation through the presenter's existing message path;
`SDL_EVENT_QUIT` posts the close request, which SDL raises for Command-Q
from the default application menu it provides on macOS. Drawable size comes
from `SDL_GetWindowSizeInPixels`.

The menu-bar Settings item goes; F10 opens the settings page. Bundle name,
identity, icon, `Info.plist` and the bundle finisher are unchanged.

`smoke_main` loses its two Metal calls and creates the device through the
backend factory with no window. `headless_main` is untouched.

### Portable audio

`mixer.cpp` implements every `host_audio_*` callback. A channel holds its
source (static PCM or an appendable chunk stream), format, rate override, the
gain and the two pan gains from `audio_math.cpp`, a loop flag and a sample
cursor. The render function pulls all channels into a float stereo block at
the output rate with linear resampling, applies gains, sums, and hard-clips
at full scale with no gain reduction and no release, which is what the
existing loud-then-quiet test asserts.

One sample clock, advanced by the render callback, drives `played_bytes`,
`position` and `voice_remaining_bytes` through the cursor arithmetic
`audio_math.cpp` already defines. One-shot completion is detected on the
render thread and published through atomic generation counters; the guest
side reconciles on its next call; no render-thread code takes the channel
lock. The two-mutex lock order and `host_audio_lock_violations` stay.

`host_audio_stream` converts a looping channel into a gapless appendable
stream at its cursor, per the `host_api.h` contract; chunks are a deque
appended under the data lock and drained by the render thread.

`sink.h` is the output interface: `open(rate, channels, pull)`, `close`,
and a capture hook. `sdl_sink.cpp` wraps an SDL3 audio stream with
`SDL_SetAudioStreamGetCallback`. `offline_sink.cpp` renders a requested frame
count synchronously for tests and for the smoke host's amplitude
measurements. `audio_capture.cpp` taps the mixed output in the sink.

`midi_synth.cpp` implements `host_midi_*` over TinySoundFont: `open` loads
`Sound/POPFIGHT.SF2` and fails if absent; short messages and sysex go to the
synth; the synth renders into a dedicated mixer channel at the output rate.

### Dependencies

SDL3 at tag `release-3.4.16`, fetched by `cmake/Dependencies.cmake` with
`FetchContent` and the tag pinned, built static, linked into `PopRecomp` and
the test binaries that need audio output. Not vendored. TinySoundFont as one
header under `third_party/tsf/` with its MIT license text; `NOTICE` gains an
entry. No other new dependency.

## Testing

- `gpu_fake_tests` (nogame, all platforms): texture upload and readback
  round trip, pass recording, completion callback order, clock control.
- Presenter and compositor tests move onto the fake backend and become
  `nogame`; their readback-based variants run on macOS under `gpu` through the
  Metal backend.
- `d3d_render_tests`: the neutral half (vertex decode, render-state keys,
  texture decode, replay journal) is `nogame`; encoder output via readback is
  `gpu`.
- `audio_tests` (nogame) against the offline sink: the ported audio checks
  including the clipper's loud and quiet levels, stream conversion at the
  cursor, completion reconciliation, stop during a pending completion, and
  MIDI against a small generated SF2 fixture so no game files are needed.
- SDL host: a table test walking every `SDL_Scancode` row, plus the manual
  checklist in `docs/testing.md` (fullscreen, pointer confinement, focus,
  Command-Q, resolution cycling).
- Game-backed: `tools/test.py --native`, `--mods`, `--gameplay`, the
  integration and mods scripts, compared against the pre-existing failure
  list in the sub-project 1 plan. Frame dumps from the smoke `level1` script
  before and after step 3 are compared pixel for pixel at the probe points.

## Migration order

Tree building and suites green at every step:

1. SDL3 and TinySoundFont in the build; `gpu.h`; fake backend with tests.
2. Presenter, compositor and overlay over `gpu.h`; Metal backend for
   textures, render passes and the swapchain; `Frame` and `CompositorInput`
   on handles. The app still runs on AppKit.
3. D3D renderer over `gpu.h`, including compute readback; smoke host on the
   backend factory; frame-dump comparison.
4. SDL3 host replaces `main.mm`; scancode table; `window_presentation.h`
   retired; manual checklist.
5. Mixer, sinks and MIDI synth replace `audio.mm` and `midi.mm`; audio tests
   ported.
6. Delete every Objective-C++ file outside `gpu/metal/`; documents, labels,
   CI and `NOTICE` updated.

## Removed files

`src/recomp/host/main.mm`, `present.mm` (renamed `.cpp`), `present_thread.mm`,
`compositor.mm`, `ui_layer.mm` (renamed), `d3d_render.mm`, `input.mm`
(renamed), `audio.mm`, `midi.mm`, `midi.h`, `window_presentation.h`,
`performance_overlay.h` (becomes `.h`/`.cpp` without Metal),
`smoke_main.mm` (renamed `.cpp`), `tests/host_tests.mm` (split into portable
suites), `tests/compositor_tests.mm`, `tests/present_events_tests.mm` under
`mods/tests` (renamed), `tests/test_frame_builder.mm` (renamed).

## Accepted losses

- The native menu bar and its Settings item. F10 remains.
- Apple's General MIDI fallback. MIDI is unavailable without the SoundFont.
- The private AppKit pointer-confinement call, replaced by SDL's window mouse
  rect.

## Risks

| Risk | Mitigation |
| --- | --- |
| Frame pacing regresses without the display link | Pace on presented callbacks plus the swapchain's refresh period; compare overlay readings at 60 and 120 FPS limits before and after step 4. |
| Metal single-queue ordering is implicit today | The interface states commit-order completion; the Metal backend satisfies it with one queue; Vulkan must honor it with semaphores. |
| Audio behavior drifts from AVAudioEngine | The ported suite, the smoke host's amplitude assertions, and a listening check in a level. |
| Pointer confinement or fullscreen feels different under SDL | Manual checklist; SDL's window mouse rect is the supported replacement. |
| Renderer rewrite breaks rendering subtly | Readback-based renderer tests on Metal, the smoke probe verbs, and pre/post frame-dump comparison. |
| Scope | Six steps, each leaving a green tree; the plan can stop after any step. |
