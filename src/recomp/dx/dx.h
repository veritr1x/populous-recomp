// dx.h - what the host application and the runtime call into for the
// graphics, audio and input shims.
//
// The runtime registers logging-only entries for DDRAW/DINPUT/DSOUND/QMIXER/
// weanetr in imports_init(). dx_register_shims() replaces them with these
// implementations, so the host must call it AFTER loader_load():
//
//     loader_load(...);        // runs imports_init(), patches the IAT
//     dx_register_shims();     // upgrades the DirectX entries in place
//
// imports_register upgrades an already-allocated trampoline in place, so the
// IAT addresses the loader patched stay valid and no runtime file needs to
// change. Registering twice is harmless.
#pragma once
#include "../runtime/guest.h"
#include <stdio.h>

// Registers every DirectDraw, Direct3D, DirectSound, DirectInput, QMixer and
// weanetr shim and builds the COM vtables in guest memory. Idempotent.
void dx_register_shims();

// Drops every COM object and rebuilds the vtables. Call after mem_init(),
// which discards the guest heap the objects and vtables live in.
void dx_reset();

// Per-module registration, exposed for the tests. dx_register_shims calls all
// of these; each is idempotent.
void ddraw_register();
void d3d_register();
void dsound_register();
void dinput_register();
void qmixer_register();
void weanetr_register();

// Per-module state resets. Each drops the cached guest addresses and handle
// tables that pointed into the arena mem_init discarded. dx_reset calls them
// all; the guest heap those records point into is gone by then.
void ddraw_reset();
void d3d_reset();
void dsound_reset();
void dinput_reset();

// The host calls this after feeding new input through host_input_state. It
// signals the notification event of every DirectInput device that registered
// one, which is what wakes a guest thread waiting on it so it can call
// GetDeviceData. Safe to call from a host thread while guest code is running.
// C linkage on purpose: this is the one symbol in this header a host calls
// from outside C++, and a host that declares it extern "C" against a mangled
// definition links to nothing. With a weak declaration on the host side that
// failure is silent - the address is null, the guarded call is skipped, and
// input stays dead with nothing to show for it.
extern "C" void dinput_host_input_changed(void);
void qmixer_reset();

// Requested against delivered for the QMixer path: how many sounds the game
// asked for, how many reached the host, and what happened to the rest.
// "Some sounds are missing" cannot be answered by a log of successes.
void qmixer_dump(FILE *out);

// Services DirectSound notification positions: signals the guest events whose
// offsets the play cursor has reached since the last look. The guest thread
// that streams into a looping buffer is parked in WaitForMultipleObjects on
// exactly those events, so nothing refills that buffer until this runs.
// QSWaveMixPump calls it because the game calls that every frame; every
// DirectSound entry point calls it too.
void dsound_pump();

// Runs on the main guest thread between frames, from the guest's own message
// loop, registered with host_set_frame_pump by dx_register_shims. It refills
// streamed QMixer waves by calling the guest back for more samples and it
// services DirectSound notification positions.
//
// It exists because the game drives neither. Real QMixer runs its own mixing
// thread, so QSWaveMixPump is called zero times in a whole run, and a streamed
// wave without a tick plays the chunks it was given at the start and then
// stops. DirectSound's streaming worker is parked in WaitForMultipleObjects on
// events nothing else would signal.
void qmixer_frame_pump(X86 *c);
// The display's frame boundary in the same shape, registered beside it: the
// display's frame ends where the guest's message loop pumps, and that fact
// lives with the display shim rather than inside the audio pump.
void ddraw_frame_pump(X86 *c);
// Test only: forget the cached POPM_QMIX_GATE reading.
void qmixer_gate_reset_for_test();

// Diagnostics: live COM objects, surfaces and audio channels.
void dx_dump(FILE *out);

// ---------------------------------------------------------------------------
// Cross-module hooks. Declared here so com.cpp's QueryInterface can hand a
// DirectDraw object the Direct3D object it owns without ddraw.cpp and d3d.cpp
// including each other.
// ---------------------------------------------------------------------------
struct ComObj;
// The IDirect3D(2) object belonging to a DirectDraw object, created on demand.
ComObj *d3d_for_ddraw(ComObj *dd);
// Presents the primary surface through host_present. Called on Flip, on
// Unlock of the primary and on a Blt whose destination is the primary.
void ddraw_present(ComObj *primary);
// The palette that governs an 8-bit surface: its own, else the primary's.
const ComObj *ddraw_effective_palette(const ComObj *surface);
// The palette that governs an 8-bit surface: its own, else the primary's.
const ComObj *ddraw_effective_palette(const ComObj *surface);
// Uploads a texture surface to the host renderer, for IDirect3DTexture2::Load
// and for the first GetHandle.
void d3d_upload_texture(ComObj *surface);
// The Direct3D device rasterizes into a DirectDraw surface, so anything it has
// drawn has to be in that surface's own memory before the guest reads it,
// writes it or presents it. ddraw.cpp calls this from Lock, Blt, BltFast,
// GetDC, Flip and the present path; it is a comparison and a return for every
// surface that is not the current render target.
// `why` names the call that asked, so one frame's worth of flushes and blits
// reads as a sequence rather than as a pile of identical lines.
void d3d_flush_surface(ComObj *surface, const char *why);
// Re-states the render target's memory to the host. Flip swaps the pixels
// behind a surface, so the host has to be told when its target moves.
void d3d_retarget_surface(ComObj *surface);
// The surface is being destroyed. If the device was rendering into it, the
// device has no target any more: the host must not be left holding a pointer
// into memory that is about to be freed.
void d3d_forget_surface(ComObj *surface);
// Allocates the next free audio channel; DirectSound buffers and QMixer
// channels share one numbering so the host mixer sees a flat channel space.
int32_t dx_alloc_audio_channel();
void dx_free_audio_channel(int32_t ch);
