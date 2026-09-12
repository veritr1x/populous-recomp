# Code guide

Start from the user-visible behavior, then follow the entry point below. The
native code is grouped by responsibility; the original game functions are
translated locally into `build/recomp/gen/` and are never edited in place.

## Where to make a change

| Change | Start here | Major entry points |
| --- | --- | --- |
| Options controls or labels | [options_menu.cpp](../src/recomp/mods/options_menu.cpp) | `attach`, `action`, `update`, `draw`, `mods_options_frame` |
| Live display settings | [display_settings.cpp](../src/recomp/mods/display_settings.cpp) | `mods_display_init`, `mods_display_set`, `apply_transition` |
| Saved graphics choices, new resolutions | [game_settings.cpp](../src/recomp/mods/game_settings.cpp) | `flush_changed`, `enumerate_modes`, `enumerate_display_mode`, `scale_camera_for_resolution` |
| Mouse edges, coordinate mapping | [input_gate.cpp](../src/recomp/host/input_gate.cpp) | `take_layout`, `host_gate_pointer_event`, `pointer_correction` |
| macOS window, focus, quit | [main.mm](../src/recomp/host/main.mm) | `handle_event`, `apply_focus`, `apply_window_mode`, `pump`, `applicationShouldTerminate` |
| Frame lifetime and pacing | [present_thread.cpp](../src/recomp/host/present_thread.cpp) | `acquire`, `host_frame_seal`, `sweep` |
| World rendering, materials | [d3d_render.mm](../src/recomp/host/d3d_render.mm) | `host_d3d_expand`, `uploadTexture`, `drawSnapshot`, `d3d_fragment` |
| UI separation and final composition | [ui_layer.cpp](../src/recomp/host/ui_layer.cpp), [compositor.cpp](../src/recomp/host/compositor.cpp) | `ui_layer_extract`, `replay`, `compositor_compose` |
| Audio streaming or gaps | [audio.mm](../src/recomp/host/audio.mm) | `ensure_engine`, `host_audio_stream`, `host_audio_queue`, `host_audio_queued_bytes` |
| Sound evidence | [audio_capture.cpp](../src/recomp/host/audio_capture.cpp) | `host_capture_write`, `host_capture_stats` |
| Original graphics API behavior | [DirectX adapters](../src/recomp/dx/README.md) | `Surface_Lock`, `Surface_Unlock`, `Surface_Blt`, `Surface_Flip`, `d3d_upload_texture` |
| Imports, startup, memory | [Guest runtime](../src/recomp/runtime/README.md) | `loader_load`, `patch_iat`, `imports_dispatch`, `heap_realloc` |
| Mod lifecycle and hooks | [loader.cpp](../src/recomp/mods/loader.cpp), [hooks.cpp](../src/recomp/mods/hooks.cpp) | `mods_load_all`, `build_api`, `mods_hook_dispatch`, `mods_call_next` |
| Instruction translation | [translate.py](../tools/recomp/translate.py), [x86.h](../tools/recomp/runtime/x86.h) | Instruction emitters and register/flag helpers |
| Native replacement validation | [replay.cpp](../src/recomp/native/replay.cpp), [page_track.cpp](../src/recomp/native/page_track.cpp) | `load`, `run`, `translated`, `begin`, `end` |

## Follow one live setting

A click in an added Options row calls `action`, which uses
`mods_display_nudge` / `mods_display_set`. The setter validates the choice and
persists it through the shared settings store. Simple values are published to
host readers immediately. Rendering and projection changes are committed
together by `apply_transition` at the next frame boundary.

Resolution has one visible selector in the original Graphics tab. Programmatic
requests also reach the original resolution callback through
`mods_options_frame`; that callback owns the surface teardown and rebuild.
Adding a second independent host resolution control would bypass this lifecycle.

The menu's 640×480 coordinate constants describe the original normalized layout.
They do not select the framebuffer size. Glyph measurement, glyph quads, camera
zoom and minimap storage adapt to the selected mode through documented hooks.

## Read guest-facing code

`X86* c` is an emulated register *state*, not an executing emulator. `arg(c, i)`
reads an argument from the guest stack; `rd32` and `wr32` access the guest arena.
A hexadecimal function address identifies a translated dispatch entry. A native
pointer, COM handle, surface ID and guest address are distinct values even when
all happen to fit in an integer.

COM adapters validate their `this` object, decode guest parameters, invoke host
services, and return through `com_ret`. Win32 imports use their declared calling
convention in `imports_dispatch`. New callbacks should use `guest_call` and the
existing hook APIs instead of manufacturing host calls to guest addresses.

Named symbols are useful navigation hints, but some are incomplete. Keep address
provenance for a reviewed hook. Do not rename an unknown function to a guessed
gameplay meaning or replace a routine without a behavior comparison.

## Respect ownership boundaries

- Guest memory changes happen on the scheduler baton holder. AppKit queues requests.
- The presenter receives immutable frame values and retained resources. Do not
  let it read mutable guest pointers after sealing.
- A surface revision is a content version. Preserve leased bytes before writes;
  changing palette colors can also change resolved texture content.
- Audio node operations follow the documented lock order in `audio.mm`. Its
  completion callbacks and playback clock have different timing guarantees.
- Mod teardown revokes entry before unloading code. A callback still on a worker's
  stack must finish or unwind before that module's resources are reclaimed.

Major function comments describe these contracts where they are implemented.
Keep them synchronized when changing behavior. Prefer a focused regression in
that module's tests over copying a large gameplay scenario for a small helper.
See [Testing](testing.md) for the available suites and their limits.
