#include "../../mods/sprite_view.h"
#include "../passes.h"
// dx_tests.cpp - headless tests for the DirectX, audio and input shims.
//
// Nothing here opens a window, a device or an audio stream: the host
// callbacks are captured by strong definitions in this file, which override
// the weak no-ops in host_api.cpp.
//
// Every call goes through the real guest path: a guest stack is built, the
// arguments are pushed, and the shim is reached through
// imports_dispatch(recomp_call) exactly as recompiled code would reach it.
// That means the tests also check the stack discipline, which is where a
// wrong argc in a vtable shows up.
#include "../com.h"
#include "../dx.h"
#include "../host_api.h"
#include "../ddraw.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"

#include <stdio.h>
#include <string.h>
#include <array>
#include <type_traits>
#include <time.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Captured host callbacks
// ---------------------------------------------------------------------------
struct Present {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0, bpp = 0, pitch = 0;
    bool had_palette = false;
    uint32_t palette[256] = {0};
};
static std::vector<Present> g_presents;
static int g_display_w = 0, g_display_h = 0, g_display_bpp = 0;

struct DrawRecord {
    uint32_t primitive_type = 0, vertex_type = 0, vertex_count = 0, index_count = 0;
    uint32_t texture_handle = 0;
    std::vector<uint8_t> vertices;
    std::vector<uint16_t> indices;
    uint32_t cull = 0;
    int32_t viewport[4] = {0, 0, 0, 0};
    bool had_projection = false;
};
static std::vector<DrawRecord> g_draws;
static int g_begin_scene = 0, g_end_scene = 0;
static std::vector<uint32_t> g_textures;
// The whole of the last texture upload, so a test can assert that the pixels
// and the palette really reached the renderer rather than only the handle.
struct TextureUpload {
    uint32_t handle = 0;
    uint32_t revision = 0;
    int32_t width = 0, height = 0, pitch = 0, bpp = 0;
    uint32_t rmask = 0, gmask = 0, bmask = 0, amask = 0;
    bool has_palette = false;
    uint32_t palette[256] = {0};
    std::vector<uint8_t> pixels;
};
static std::vector<TextureUpload> g_uploads;
static std::vector<uint32_t> g_clears;

// The mod foundation's texture seam, recorded.
//
// src/recomp/runtime/mods_seam.cpp defines this weakly and a strong definition
// here wins for the whole binary, so it stays inert until a test switches it
// on. What it is for: the shim hashes a texture's CONTENT at upload, because
// the DirectDraw handle is a slot number the game reuses and is not an
// identity a mod could key an override on across runs.
static bool g_tex_hook_on = false;
static std::vector<uint64_t> g_tex_hashes;
static std::vector<uint8_t> g_tex_override;
static uint64_t g_tex_override_for = 0;

extern "C" int mods_texture_override(uint64_t hash64, int32_t w, int32_t h, int32_t,
                                     uint8_t **out_rgba8, uint32_t *out_bytes) {
    if (!g_tex_hook_on)
        return 0;
    g_tex_hashes.push_back(hash64);
    if (g_tex_override.empty() || hash64 != g_tex_override_for)
        return 0;
    (void)w;
    (void)h;
    *out_rgba8 = g_tex_override.data();
    *out_bytes = (uint32_t)g_tex_override.size();
    return 1;
}

struct PlayRecord {
    int32_t channel = 0, rate = 0, channels = 0, bits = 0, loop = 0, volume = 0, pan = 0;
    uint32_t bytes = 0;
    uint32_t start_offset = 0;
    std::vector<uint8_t> pcm;
};
static std::vector<PlayRecord> g_plays;
static std::vector<int32_t> g_stops;

static HostInputState g_input;

extern "C" {

void host_present(const void *pixels, int w, int h, int bpp, const uint32_t *palette, int pitch) {
    Present p;
    p.w = w;
    p.h = h;
    p.bpp = bpp;
    p.pitch = pitch;
    p.pixels.assign((const uint8_t *)pixels, (const uint8_t *)pixels + (size_t)pitch * h);
    if (palette) {
        p.had_palette = true;
        memcpy(p.palette, palette, sizeof p.palette);
    }
    g_presents.push_back(std::move(p));
}

void host_set_display_mode(int w, int h, int bpp) {
    g_display_w = w;
    g_display_h = h;
    g_display_bpp = bpp;
}

static std::vector<HostDirtyRect> g_t4_read_rects;
int host_d3d_readback_rects(const HostD3DSurface *, uint32_t, const HostDirtyRect *r, uint32_t n) {
    g_t4_read_rects.assign(r, r + n);
    return 1;
}
void host_d3d_begin_scene() {
    ++g_begin_scene;
}
void host_d3d_end_scene() {
    ++g_end_scene;
}

void host_d3d_draw(const HostD3DDrawSnapshot *cmd) {
    if (cmd->kind == HOST_DRAW_CLEAR) {
        g_clears.push_back(cmd->clear_flags);
        return;
    }
    DrawRecord d;
    d.primitive_type = cmd->primitive_type;
    d.vertex_type = cmd->fvf;
    d.vertex_count = cmd->vertex_count;
    d.index_count = cmd->index_count;
    d.texture_handle = cmd->texture_handle;
    size_t n = (size_t)cmd->vertex_stride * cmd->vertex_count;
    d.vertices.assign((const uint8_t *)cmd->vertices, (const uint8_t *)cmd->vertices + n);
    if (cmd->indices)
        d.indices.assign(cmd->indices, cmd->indices + cmd->index_count);
    d.cull = cmd->state.render_state[D3DRENDERSTATE_CULLMODE];
    memcpy(d.viewport, cmd->state.viewport, sizeof d.viewport);
    d.had_projection = cmd->state.transform_set[D3DTRANSFORMSTATE_PROJECTION] != 0;
    g_draws.push_back(std::move(d));
}

// The renderer's texture leases, modelled the way the real one implements
// them: a set of live (handle, revision) pairs with counts, so a test can ask
// what the frame is holding.
static std::map<uint64_t, uint32_t> g_tex_leases;
static uint64_t tex_key_for_test(uint32_t h, uint32_t r) {
    return ((uint64_t)h << 32) | r;
}
// Reads without inserting: map::operator[] would create the very entry a test
// is asking about, so "nobody holds this" would answer itself.
static uint32_t leases_for_test(uint32_t h, uint32_t r) {
    auto it = g_tex_leases.find(tex_key_for_test(h, r));
    return it == g_tex_leases.end() ? 0u : it->second;
}
// The double models the renderer: a revision it never received cannot be held,
// and the shim is told so. g_tex_uploaded is what "received" means here.
static std::set<uint64_t> g_tex_uploaded;
int host_d3d_texture_retain(uint32_t handle, uint32_t revision) {
    if (!handle)
        return 1;
    if (!g_tex_uploaded.count(tex_key_for_test(handle, revision)))
        return 0;
    ++g_tex_leases[tex_key_for_test(handle, revision)];
    return 1;
}
void host_d3d_texture_release(uint32_t handle, uint32_t revision) {
    auto it = g_tex_leases.find(tex_key_for_test(handle, revision));
    if (it == g_tex_leases.end())
        return;
    if (it->second)
        --it->second;
    if (!it->second)
        g_tex_leases.erase(it);
}

void host_d3d_clear(uint32_t flags, const int32_t *, uint32_t, uint32_t, float) {
    g_clears.push_back(flags);
}

void host_d3d_texture(const HostD3DTexture *t) {
    g_textures.push_back(t->handle);
    g_tex_uploaded.insert(((uint64_t)t->handle << 32) | t->revision);
    TextureUpload u;
    u.handle = t->handle;
    u.revision = t->revision;
    u.width = t->width;
    u.height = t->height;
    u.pitch = t->pitch;
    u.bpp = t->bpp;
    u.rmask = t->rmask;
    u.gmask = t->gmask;
    u.bmask = t->bmask;
    u.amask = t->amask;
    u.has_palette = t->palette != nullptr;
    if (t->palette)
        memcpy(u.palette, t->palette, sizeof u.palette);
    if (t->pixels && t->height > 0 && t->pitch > 0)
        u.pixels.assign((const uint8_t *)t->pixels,
                        (const uint8_t *)t->pixels + (size_t)t->pitch * (size_t)t->height);
    g_uploads.push_back(std::move(u));
}
void host_d3d_texture_destroyed(uint32_t) {}

// The play cursor a test wants the mixer to believe in, so a streaming refill
// can be driven deterministically instead of by waiting.
static uint32_t g_test_audio_pos = 0;
// The stream contract, modelled the way the real host implements it:
// host_audio_stream converts a looping channel at its cursor and reports the
// offset it resumed from, and host_audio_played_bytes counts on from there and
// never goes backwards. Tests drive the second by hand.
static bool g_ch_streaming = false;
static uint32_t g_stream_base = 0;
static uint32_t g_stream_played = 0;
// A host that can continue a sound. Off by default, so the tests that do not
// care exercise the re-submitting path a host without it forces.
static bool g_queue_enabled = false;
static uint32_t g_queued_bytes = 0;
static std::vector<PlayRecord> g_queues;
// Whether each channel's current sound was submitted as a loop. The real host
// refuses a queue behind a looping buffer, because a buffer scheduled with the
// loop option plays for ever and nothing appended behind it is ever reached.
// A test host that accepted one would hide the ordering the shim depends on.
static std::map<int32_t, int32_t> g_ch_loop;
// Set to make a queue refuse the way the real host does once the buffer that
// began a stream has played out: its completion arrives while appended
// buffers are still scheduled, and the channel is marked as no longer
// playing. Cleared by the next play, which is what un-retires it.
static bool g_queue_retired = false;
// Everything ever accepted on any channel, so a test can model the host's own
// two counters: what has been appended and what of it has been played.
static uint64_t g_queued_accepted = 0;
// Models host_audio_voice_remaining_bytes: what the VOICE still has to play,
// the sound it is playing included. A play SETS this to that sound's length
// where it zeroes g_queued_bytes, and that difference is the whole point of
// the second query - the shim's refill gate reads this one, because QMixer
// plays a sound on a named channel and refills behind it rather than
// appending into a ring.
static uint32_t g_voice_remaining = 0;

void host_audio_play(const HostAudioPlay *p) {
    PlayRecord r;
    r.channel = p->channel;
    r.rate = p->sample_rate;
    r.channels = p->channels;
    r.bits = p->bits;
    r.loop = p->loop;
    r.volume = p->volume;
    r.pan = p->pan;
    r.bytes = p->bytes;
    r.start_offset = p->start_offset;
    r.pcm.assign((const uint8_t *)p->pcm, (const uint8_t *)p->pcm + p->bytes);
    g_plays.push_back(std::move(r));
    // A fresh play replaces the stream: it stops the node, which discards
    // everything scheduled behind it, and the queue accounting starts again.
    // The real host does exactly this and the shim's cursor arithmetic for a
    // streamed DirectSound ring depends on it.
    g_queued_bytes = 0;
    g_queues.clear();
    // Not zero: the voice is now playing this sound, and all of it is ahead.
    g_voice_remaining = p->bytes;
    g_ch_loop[p->channel] = p->loop;
    g_queued_accepted = 0;
    // A fresh play makes the channel playing again, so a voice that had been
    // retired accepts appends once more. The real host does the same.
    g_queue_retired = false;
    g_ch_streaming = false;
    g_stream_base = 0;
    g_stream_played = 0;
}
int32_t host_audio_stream(int32_t ch) {
    if (!g_queue_enabled)
        return -1; // a host without the contract
    if (!g_ch_loop.count(ch))
        return -1; // never played
    if (g_ch_streaming)
        return (int32_t)(g_stream_base + g_stream_played);
    g_ch_streaming = true;
    g_ch_loop[ch] = 0;                // no longer a loop
    g_stream_base = g_test_audio_pos; // resumed at the play cursor
    g_stream_played = 0;
    return (int32_t)g_stream_base;
}

uint32_t host_audio_played_bytes(int32_t ch) {
    (void)ch;
    return g_ch_streaming ? g_stream_base + g_stream_played : 0;
}

int32_t host_audio_queue(int32_t ch, const void *pcm, uint32_t bytes) {
    if (!g_queue_enabled || !pcm || !bytes)
        return 0;
    if (g_ch_loop.count(ch) && g_ch_loop[ch])
        return 0;
    if (g_queue_retired)
        return 0;
    PlayRecord q;
    q.channel = ch;
    q.bytes = bytes;
    q.pcm.assign((const uint8_t *)pcm, (const uint8_t *)pcm + bytes);
    g_queues.push_back(std::move(q));
    g_queued_bytes += bytes;
    g_queued_accepted += bytes;
    g_voice_remaining += bytes;
    return (int32_t)bytes;
}
uint32_t host_audio_queued_bytes(int32_t) {
    return g_queued_bytes;
}
uint32_t host_audio_voice_remaining_bytes(int32_t) {
    return g_voice_remaining;
}

void host_audio_stop(int32_t ch) {
    g_stops.push_back(ch);
    g_queued_bytes = 0;
    g_voice_remaining = 0;
}
void host_audio_set_volume(int32_t, int32_t) {}
void host_audio_set_pan(int32_t, int32_t) {}
void host_audio_set_frequency(int32_t, uint32_t) {}
uint32_t host_audio_position(int32_t) {
    return g_test_audio_pos;
}
int32_t host_audio_is_playing(int32_t) {
    return 1;
}

// The contract in host_api.h says the deltas are consumed on read, so this
// test host clears them exactly as the real one must.
void host_input_state(HostInputState *out) {
    *out = g_input;
    g_input.mouse_dx = g_input.mouse_dy = g_input.mouse_dz = 0;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------
static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        uint64_t va = (uint64_t)(a), vb = (uint64_t)(b);                                           \
        ++g_checks;                                                                                \
        if (va != vb) {                                                                            \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b,   \
                    (unsigned long long)va, (unsigned long long)vb);                               \
        }                                                                                          \
    } while (0)

// The guest CPU the tests drive shims with.
static X86 g_cpu;
static uint32_t g_stack_top = 0;

static void cpu_reset() {
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.eflags_misc = 0x202;
    g_cpu.fpu_cw = 0x037f;
    g_cpu.fpu_tag = 0xffff;
    g_cpu.r[R_ESP] = g_stack_top;
}

// Calls a shim or a COM vtable slot exactly as recompiled code does: push the
// arguments right to left, push a return address, then dispatch. Returns EAX
// and asserts the callee popped precisely its own arguments.
static uint32_t call_shim(uint32_t target, std::initializer_list<uint32_t> args) {
    uint32_t esp = g_cpu.r[R_ESP];
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, a[i]);
    }
    esp -= 4;
    const uint32_t RET = 0x00401000u;
    wr32(esp, RET);
    g_cpu.r[R_ESP] = esp;
    uint32_t before = esp;

    if (!imports_dispatch(&g_cpu, target)) {
        fprintf(stderr, "FAIL: %08x is not a shim trampoline\n", target);
        ++g_failures;
        g_cpu.r[R_ESP] = before + 4 + 4 * (uint32_t)a.size();
        return 0;
    }
    ++g_checks;
    uint32_t expected = before + 4 + 4 * (uint32_t)a.size();
    if (g_cpu.r[R_ESP] != expected) {
        ++g_failures;
        fprintf(stderr,
                "FAIL: %s left ESP at %08x, expected %08x "
                "(a wrong argc in the vtable)\n",
                imports_describe(target) ? imports_describe(target) : "shim", g_cpu.r[R_ESP],
                expected);
        g_cpu.r[R_ESP] = expected;
    }
    ++g_checks;
    if (g_cpu.eip != RET) {
        ++g_failures;
        fprintf(stderr, "FAIL: shim did not return to the pushed address\n");
    }
    return g_cpu.r[R_EAX];
}

// A COM method: `this` is the first argument and the slot comes from the
// object's own vtable, so this exercises the real guest indirection.
static uint32_t call_method(uint32_t iface_ptr, uint32_t slot,
                            std::initializer_list<uint32_t> rest = {}) {
    uint32_t vtbl = rd32(iface_ptr + COM_OFF_vtbl);
    uint32_t target = rd32(vtbl + slot * 4);
    std::vector<uint32_t> a;
    a.push_back(iface_ptr);
    for (uint32_t v : rest)
        a.push_back(v);
    uint32_t esp = g_cpu.r[R_ESP];
    for (size_t i = a.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, a[i]);
    }
    esp -= 4;
    const uint32_t RET = 0x00401000u;
    wr32(esp, RET);
    g_cpu.r[R_ESP] = esp;
    uint32_t before = esp;
    if (!imports_dispatch(&g_cpu, target)) {
        fprintf(stderr, "FAIL: vtable slot %u holds %08x, not a trampoline\n", slot, target);
        ++g_failures;
        return 0;
    }
    ++g_checks;
    uint32_t expected = before + 4 + 4 * (uint32_t)a.size();
    if (g_cpu.r[R_ESP] != expected) {
        ++g_failures;
        fprintf(stderr, "FAIL: %s left ESP at %08x, expected %08x\n",
                imports_describe(target) ? imports_describe(target) : "method", g_cpu.r[R_ESP],
                expected);
        g_cpu.r[R_ESP] = expected;
    }
    return g_cpu.r[R_EAX];
}

// The trampoline for a DLL export. imports_resolve allocates one on demand
// for any registered shim, which is what the loader would otherwise do when it
// patched the IAT; these tests never load the PE.
static uint32_t tramp(const char *dll, const char *name) {
    return imports_resolve(dll, name);
}

// Scratch guest memory for out-parameters.
static uint32_t g_scratch = 0;
static uint32_t sc(uint32_t off) {
    return g_scratch + off;
}

// ---------------------------------------------------------------------------
// SDK record sizes, derived here from the published field lists rather than
// read from dxtypes.h. Sharing the implementation's constants would make an
// ABI mistake invisible: the shim and the test would simply agree on the
// wrong number. test_sdk_abi checks these against what the shims use.
// ---------------------------------------------------------------------------
enum {
    // DDSURFACEDESC: dwSize dwFlags dwHeight dwWidth lPitch dwBackBufferCount
    // dwMipMapCount dwAlphaBitDepth dwReserved lpSurface (10 dwords = 40),
    // 4 DDCOLORKEYs (32), DDPIXELFORMAT (32), DDSCAPS (4) = 108.
    SDK_DDSURFACEDESC = 108,
    // DDSURFACEDESC2 is the same but with DDSCAPS2 (16) and dwTextureStage
    // (4): 40 + 32 + 32 + 16 + 4 = 124.
    SDK_DDSURFACEDESC2 = 124,
    // DDPIXELFORMAT: dwSize dwFlags dwFourCC dwRGBBitCount and four masks.
    SDK_DDPIXELFORMAT = 32,
    // DDBLTFX: 25 dwords through dwFillColor plus two DDCOLORKEYs = 100.
    SDK_DDBLTFX = 100,
    // DDDEVICEIDENTIFIER: two 512-byte strings, LARGE_INTEGER, four dwords
    // and a GUID = 512+512+8+16+16 = 1064.
    SDK_DDDEVICEIDENTIFIER = 1064,
    // D3DPRIMCAPS: 14 dwords.
    SDK_D3DPRIMCAPS = 56,
    // D3DDEVICEDESC (DirectX 6): 252, with dpcTriCaps at 0x64.
    SDK_D3DDEVICEDESC = 252,
    SDK_D3DDD_dpcTriCaps_OFF = 0x64,
    // D3DFINDDEVICESEARCH: dwSize dwFlags bHardware dcmColorModel (16),
    // GUID (16), dwCaps (4), D3DPRIMCAPS (56) = 92.
    SDK_D3DFINDDEVICESEARCH = 92,
    // D3DFINDDEVICERESULT: dwSize (4) + GUID (16) + two D3DDEVICEDESCs.
    SDK_D3DFINDDEVICERESULT = 4 + 16 + 252 + 252,
    // D3DVIEWPORT and D3DVIEWPORT2 are both 11 dwords.
    SDK_D3DVIEWPORT = 44,
    SDK_D3DVIEWPORT2 = 44,
    // D3DCLIPSTATUS: dwFlags dwStatus and six D3DVALUEs.
    SDK_D3DCLIPSTATUS = 32,
    // DSBUFFERDESC: dwSize dwFlags dwBufferBytes dwReserved lpwfxFormat.
    SDK_DSBUFFERDESC = 20,
    // WAVEFORMATEX: 2+2+4+4+2+2+2.
    SDK_WAVEFORMATEX = 18,
    // DIDEVICEOBJECTDATA through DirectInput 7: four dwords.
    SDK_DIDEVICEOBJECTDATA = 16,
    // DIPROPDWORD: a 16-byte DIPROPHEADER plus dwData.
    SDK_DIPROPDWORD = 20,
    // DIMOUSESTATE: lX lY lZ and four buttons.
    SDK_DIMOUSESTATE = 16,
    // The D3DFINDDEVICESEARCH flag bits, from d3dcaps.h.
    SDK_D3DFDS_COLORMODEL = 0x01,
    SDK_D3DFDS_GUID = 0x02,
    SDK_D3DFDS_HARDWARE = 0x04,
};

// ---------------------------------------------------------------------------
// Interface slot numbers, spelled out so a vtable reordering fails loudly
// here rather than silently in the game.
// ---------------------------------------------------------------------------
enum {
    DD_QueryInterface = 0,
    DD_AddRef = 1,
    DD_Release = 2,
    DD_CreatePalette = 5,
    DD_CreateSurface = 6,
    DD_EnumDisplayModes = 8,
    DD_GetDisplayMode = 12,
    DD_GetFourCCCodes = 13,
    DD_SetCooperativeLevel = 20,
    DD_SetDisplayMode = 21,
    DD_GetAvailableVidMem = 23,
    DD_GetDeviceIdentifier = 27,
};
enum {
    S_QueryInterface = 0,
    S_Release = 2,
    S_Blt = 5,
    S_BltFast = 7,
    S_AddAttachedSurface = 3,
    S_DeleteAttachedSurface = 8,
    S_Flip = 11,
    S_GetAttachedSurface = 12,
    S_GetPixelFormat = 21,
    S_SetColorKey = 29,
    S_GetSurfaceDesc = 22,
    S_IsLost = 24,
    S_Lock = 25,
    S_SetPalette = 31,
    S_Unlock = 32,
};
enum { P_SetEntries = 6 };
enum {
    D3D_EnumDevices = 3,
    D3D_CreateMaterial = 5,
    D3D_CreateViewport = 6,
    D3D_FindDevice = 7,
    D3D_CreateDevice = 8,
};
enum {
    DEV_GetCaps = 3,
    DEV_AddViewport = 6,
    DEV_EnumTextureFormats = 9,
    DEV_BeginScene = 10,
    DEV_EndScene = 11,
    DEV_SetCurrentViewport = 13,
    DEV_SetRenderState = 23,
    DEV_SetTransform = 26,
    DEV_DrawPrimitive = 29,
    DEV_DrawIndexedPrimitive = 30,
    DEV_SwapTextureHandles = 4,
    DEV_SetRenderTarget = 15,
    DEV_GetClipStatus = 32,
};
enum { VP_SetViewport2 = 17, VP_Clear = 12, VP_SetBackground = 8 };
enum { MAT_GetHandle = 5 };
enum { TEX_GetHandle = 3, TEX_PaletteChanged = 4, TEX_Load = 5 };
enum {
    DS_CreateSoundBuffer = 3,
    DS_SetCooperativeLevel = 6,
    B_QueryInterface = 0,
    B_GetCaps = 3,
    B_GetCurrentPosition = 4,
    B_GetFormat = 5,
    B_GetStatus = 9,
    B_Lock = 11,
    B_Play = 12,
    B_SetVolume = 15,
    B_SetFrequency = 17,
    B_Stop = 18,
    B_Unlock = 19,
    DS_DuplicateSoundBuffer = 5,
    N_SetNotificationPositions = 3,
};
enum {
    DI_CreateDevice = 3,
    DID_SetProperty = 6,
    DID_Acquire = 7,
    DID_GetDeviceState = 9,
    DID_GetDeviceData = 10,
    DID_SetDataFormat = 11,
    DID_SetEventNotification = 12,
};

// Builds a DirectDraw object, a Direct3D2, a 3D-capable render target and a
// device on it, returning the device interface pointer.
static uint32_t make_d3d_device() {
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    if (!dd)
        return 0;
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});
    uint32_t iid = sc(0x40);
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    uint32_t d3d = rd32(sc(0x60));
    if (!d3d)
        return 0;
    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
    wr32(desc + DDSD_OFF_dwWidth, 640);
    wr32(desc + DDSD_OFF_dwHeight, 480);
    call_method(dd, DD_CreateSurface, {desc, sc(8), 0});
    uint32_t target = rd32(sc(8));
    if (!target)
        return 0;
    const uint8_t hal[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                             0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};
    uint32_t guid = sc(0x500);
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, hal[i]);
    call_method(d3d, D3D_CreateDevice, {guid, target, sc(12)});
    return rd32(sc(12));
}

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------

// Resolution changes destroy the old render target and its full-size depth
// buffer. Both explicit detachment and destruction must drop the attachment's
// reference, while preserving references still owned by the caller.
static void test_resolution_depth_lifetime() {
    cpu_reset();
    CHECK_EQ(call_shim(tramp("DDRAW.dll", "DirectDrawCreate"), {0, sc(0), 0}), DD_OK);
    const uint32_t dd = rd32(sc(0));
    const HeapStats baseline = heap_stats();
    const uint32_t live = com_live_count();
    const uint32_t sizes[][2] = {{640, 480},   {800, 600},   {1024, 768}, {1920, 1080},
                                 {2560, 1440}, {3840, 2160}, {800, 600},  {640, 480}};
    const uint32_t desc = sc(0x100), caps = sc(0x200), out = sc(0x204);
    for (int ownership = 0; ownership < 6; ++ownership) {
        for (int cycle = 0; cycle < 24; ++cycle) {
            const auto &size = sizes[cycle % 8];
            CHECK_EQ(call_method(dd, DD_SetDisplayMode, {size[0], size[1], 16}), DD_OK);
            auto surface = [&](uint32_t surface_caps) {
                gm_zero(desc, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
                wr32(desc + DDSD_OFF_dwWidth, size[0]);
                wr32(desc + DDSD_OFF_dwHeight, size[1]);
                wr32(desc + DDSD_OFF_ddsCaps, surface_caps);
                CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, out, 0}), DD_OK);
                return rd32(out);
            };
            uint32_t primary = 0, target = 0;
            if (ownership < 3 || ownership == 5) {
                target = surface(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
                if (ownership == 5) {
                    primary = surface(DDSCAPS_OFFSCREENPLAIN);
                    CHECK_EQ(call_method(primary, S_AddAttachedSurface, {target}), DD_OK);
                }
            } else {
                gm_zero(desc, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
                wr32(desc + DDSD_OFF_ddsCaps,
                     DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE);
                wr32(desc + DDSD_OFF_dwBackBufferCount, ownership == 3 ? 1 : 2);
                CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, out, 0}), DD_OK);
                primary = rd32(out);
                wr32(caps, DDSCAPS_BACKBUFFER);
                CHECK_EQ(call_method(primary, S_GetAttachedSurface, {caps, out}), DD_OK);
                target = rd32(out);
            }
            const uint32_t depth = surface(DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY);
            if (!target || !depth)
                return; // a failed allocation is already reported
            const uint32_t depth_id = com_this(depth)->id;
            const uint32_t pixels = com_this(depth)->pixels;
            CHECK_EQ(call_method(target, S_AddAttachedSurface, {depth}), DD_OK);
            wr32(caps, DDSCAPS_ZBUFFER);
            CHECK_EQ(call_method(target, S_GetAttachedSurface, {caps, out}), DD_OK);
            CHECK_EQ(rd32(out), depth);
            CHECK_EQ(call_method(rd32(out), S_Release, {}), 2u);
            if (ownership == 0) {
                CHECK_EQ(call_method(target, S_DeleteAttachedSurface, {0, depth}), DD_OK);
                CHECK_EQ(com_this(depth)->refs, 1);
                CHECK_EQ(call_method(target, S_GetAttachedSurface, {caps, out}), DDERR_NOTFOUND);
                CHECK_EQ(rd32(out), 0u);
                CHECK_EQ(call_method(depth, S_Release, {}), 0u);
                CHECK_EQ(call_method(target, S_Release, {}), 0u);
            } else if (ownership == 1) {
                CHECK_EQ(call_method(target, S_Release, {}), 0u);
                CHECK(heap_owns(pixels)); // the caller still owns its depth reference
                CHECK_EQ(call_method(depth, S_Release, {}), 0u);
            } else if (ownership == 2) {
                CHECK_EQ(call_method(depth, S_Release, {}), 1u);
                CHECK_EQ(call_method(target, S_Release, {}), 0u);
            } else if (ownership == 5) {
                // An explicit attachment is independently owned: destroying
                // its parent must leave the caller's target/depth usable.
                CHECK_EQ(call_method(primary, S_Release, {}), 0u);
                CHECK(com_this(target) != nullptr);
                CHECK_EQ(com_this(target)->refs, 1);
                CHECK_EQ(call_method(depth, S_Release, {}), 1u);
                CHECK_EQ(call_method(target, S_Release, {}), 0u);
            } else {
                // Populous keeps the GetAttachedSurface reference when it
                // destroys a flip chain. An implicit back buffer has the
                // primary's lifetime even when its interface was retained.
                const uint32_t target_id = com_this(target)->id;
                const uint32_t target_pixels = com_this(target)->pixels;
                CHECK_EQ(call_method(primary, S_Release, {}), 0u);
                CHECK(com_get(target_id) == nullptr);
                CHECK(!heap_owns(target_pixels));
                CHECK_EQ(call_method(depth, S_Release, {}), 0u);
            }
            CHECK(com_get(depth_id) == nullptr);
            CHECK(!heap_owns(pixels));
            CHECK_EQ(com_live_count(), live);
            CHECK_EQ(heap_stats().used_bytes, baseline.used_bytes);
            CHECK_EQ(heap_stats().used_blocks, baseline.used_blocks);
            CHECK(heap_check().empty());
            // Keep a broken implementation from exhausting the test process.
            if (heap_stats().used_bytes != baseline.used_bytes)
                return;
        }
    }
    CHECK_EQ(call_method(dd, DD_Release, {}), 0u);
}

// The test the brief specifies: create DirectDraw through the shim, set
// 640x480x8, create a primary with a back buffer, lock the back buffer, write
// a gradient, flip, and check host_present received those pixels and the
// palette.
static void test_gradient_flip() {
    g_presents.clear();
    cpu_reset();

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    CHECK(create != 0);
    uint32_t hr = call_shim(create, {0, sc(0), 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t dd = rd32(sc(0));
    CHECK(dd != 0);

    hr = call_method(dd, DD_SetCooperativeLevel, {0x20004, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    CHECK_EQ(hr, DD_OK);

    hr = call_method(dd, DD_SetDisplayMode, {640, 480, 8});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(g_display_w, 640);
    CHECK_EQ(g_display_h, 480);
    CHECK_EQ(g_display_bpp, 8);

    // A complex flip chain: primary plus one back buffer.
    uint32_t desc = sc(0x100);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    wr32(desc + DDSD_OFF_dwBackBufferCount, 1);
    hr = call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t primary = rd32(sc(4));
    CHECK(primary != 0);

    // The primary must report the display mode it was created against.
    uint32_t sd = sc(0x200);
    wr32(sd + DDSD_OFF_dwSize, DDSD_SIZE);
    hr = call_method(primary, S_GetSurfaceDesc, {sd});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(rd32(sd + DDSD_OFF_dwWidth), 640);
    CHECK_EQ(rd32(sd + DDSD_OFF_dwHeight), 480);
    CHECK_EQ(rd32(sd + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwRGBBitCount), 8);

    // Fetch the back buffer.
    uint32_t caps = sc(0x300);
    wr32(caps, DDSCAPS_BACKBUFFER);
    hr = call_method(primary, S_GetAttachedSurface, {caps, sc(8)});
    CHECK_EQ(hr, DD_OK);
    uint32_t back = rd32(sc(8));
    CHECK(back != 0);
    CHECK(back != primary);

    // A palette: a 256-entry ramp, attached to the primary.
    uint32_t entries = sc(0x400);
    for (uint32_t i = 0; i < 256; ++i) {
        wr8(entries + i * 4 + 0, (uint8_t)i);         // red
        wr8(entries + i * 4 + 1, (uint8_t)(255 - i)); // green
        wr8(entries + i * 4 + 2, (uint8_t)(i / 2));   // blue
        wr8(entries + i * 4 + 3, 0);
    }
    hr = call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_INITIALIZE, entries, sc(12), 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t pal = rd32(sc(12));
    CHECK(pal != 0);
    hr = call_method(primary, S_SetPalette, {pal});
    CHECK_EQ(hr, DD_OK);

    // Attaching a palette to the visible surface is itself a presentation
    // change, so the count restarts here, after the setup.
    g_presents.clear();

    // Lock the back buffer and write a gradient through the pointer the shim
    // hands back, exactly as the game would.
    uint32_t lockdesc = sc(0x800);
    gm_zero(lockdesc, DDSD_SIZE);
    wr32(lockdesc + DDSD_OFF_dwSize, DDSD_SIZE);
    hr = call_method(back, S_Lock, {0, lockdesc, DDLOCK_WAIT, 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t surface = rd32(lockdesc + DDSD_OFF_lpSurface);
    uint32_t pitch = rd32(lockdesc + DDSD_OFF_lPitch);
    CHECK(surface != 0);
    CHECK(pitch >= 640);
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 640; ++x)
            wr8(surface + y * pitch + x, (uint8_t)((x + y) & 0xff));

    hr = call_method(back, S_Unlock, {0});
    CHECK_EQ(hr, DD_OK);
    // Unlocking a back buffer must not present: only the visible surface does.
    CHECK_EQ(g_presents.size(), 0);

    hr = call_method(primary, S_Flip, {0, DDFLIP_WAIT});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(g_presents.size(), 1);

    if (!g_presents.empty()) {
        const Present &p = g_presents[0];
        CHECK_EQ(p.w, 640);
        CHECK_EQ(p.h, 480);
        CHECK_EQ(p.bpp, 8);
        CHECK(p.had_palette);
        // The gradient the test wrote is what reached the host.
        bool pixels_match = true;
        for (uint32_t y = 0; y < 480 && pixels_match; ++y)
            for (uint32_t x = 0; x < 640; ++x)
                if (p.pixels[(size_t)y * p.pitch + x] != (uint8_t)((x + y) & 0xff)) {
                    pixels_match = false;
                    break;
                }
        CHECK(pixels_match);
        // And so is the palette, in 0x00RRGGBB.
        CHECK_EQ(p.palette[0], 0x0000ff00u);
        CHECK_EQ(p.palette[255], 0x00ff007fu);
        bool palette_match = true;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t want = (i << 16) | ((255 - i) << 8) | (i / 2);
            if (p.palette[i] != want) {
                palette_match = false;
                break;
            }
        }
        CHECK(palette_match);
    }

    // After the flip the front buffer holds what the back one did, which is
    // what the guest's next Lock of the primary must see.
    uint32_t d2 = sc(0x900);
    gm_zero(d2, DDSD_SIZE);
    wr32(d2 + DDSD_OFF_dwSize, DDSD_SIZE);
    hr = call_method(primary, S_Lock, {0, d2, DDLOCK_WAIT, 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t fp = rd32(d2 + DDSD_OFF_lpSurface);
    uint32_t fpitch = rd32(d2 + DDSD_OFF_lPitch);
    CHECK_EQ(rd8(fp + 3 * fpitch + 5), (uint8_t)8);
    call_method(primary, S_Unlock, {0});

    // Releasing the primary releases the flip chain with it.
    uint32_t live_before = com_live_count();
    call_method(primary, S_Release, {});
    CHECK(com_live_count() < live_before);
}

// Blt colour fill and BltFast with a source colour key, both onto the primary,
// which must present each time.
static void test_blt_and_colorkey() {
    g_presents.clear();
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE);
    call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    uint32_t primary = rd32(sc(4));
    CHECK(primary != 0);

    // An offscreen 16x16 source.
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    wr32(desc + DDSD_OFF_dwWidth, 16);
    wr32(desc + DDSD_OFF_dwHeight, 16);
    uint32_t hr = call_method(dd, DD_CreateSurface, {desc, sc(8), 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t src = rd32(sc(8));
    CHECK(src != 0);

    // Fill the source: half index 7, half index 3 (which will be keyed out).
    uint32_t ld = sc(0x200);
    gm_zero(ld, DDSD_SIZE);
    wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(src, S_Lock, {0, ld, DDLOCK_WAIT, 0});
    uint32_t sp = rd32(ld + DDSD_OFF_lpSurface);
    uint32_t spitch = rd32(ld + DDSD_OFF_lPitch);
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            wr8(sp + y * spitch + x, (uint8_t)(x < 8 ? 7 : 3));
    call_method(src, S_Unlock, {0});

    // Colour-fill the primary with index 1.
    uint32_t fx = sc(0x300);
    gm_zero(fx, DDBLTFX_SIZE);
    wr32(fx + 0, DDBLTFX_SIZE);
    wr32(fx + DDBLTFX_OFF_dwFillColor, 1);
    size_t before = g_presents.size();
    hr = call_method(primary, S_Blt, {0, 0, 0, DDBLT_COLORFILL | DDBLT_WAIT, fx});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(g_presents.size(), before + 1);
    CHECK_EQ(g_presents.back().pixels[0], 1);

    // The flag values are the contract, not just the names: SetColorKey picks
    // source or destination out of these bits, and with the wrong numbers a
    // source key is silently filed as a destination key and every keyed blit
    // copies the whole sprite.
    CHECK_EQ(DDCKEY_COLORSPACE, 0x00000001u);
    CHECK_EQ(DDCKEY_DESTBLT, 0x00000002u);
    CHECK_EQ(DDCKEY_DESTOVERLAY, 0x00000004u);
    CHECK_EQ(DDCKEY_SRCBLT, 0x00000008u);
    CHECK_EQ(DDCKEY_SRCOVERLAY, 0x00000010u);
    CHECK_EQ(DDBLT_KEYDEST, 0x00002000u);
    CHECK_EQ(DDBLT_KEYDESTOVERRIDE, 0x00004000u);
    CHECK_EQ(DDBLT_KEYSRC, 0x00008000u);
    CHECK_EQ(DDBLT_KEYSRCOVERRIDE, 0x00010000u);
    CHECK_EQ(DDSD_CKDESTBLT, 0x00004000u);
    CHECK_EQ(DDSD_CKSRCBLT, 0x00010000u);

    // Setting a source key must not be readable as a destination key.
    {
        uint32_t probe = sc(0x3a0);
        wr32(probe + DDCK_OFF_lo, 5);
        wr32(probe + DDCK_OFF_hi, 5);
        CHECK_EQ(call_method(src, 29 /* SetColorKey */, {DDCKEY_SRCBLT, probe}), DD_OK);
        uint32_t out = sc(0x3b0);
        CHECK_EQ(call_method(src, 16 /* GetColorKey */, {DDCKEY_SRCBLT, out}), DD_OK);
        CHECK_EQ(rd32(out + DDCK_OFF_lo), 5u);
        CHECK_EQ(call_method(src, 16 /* GetColorKey */, {DDCKEY_DESTBLT, out}), DDERR_NOCOLORKEY);
    }

    // A key given at CreateSurface time counts too, not only one set later.
    {
        uint32_t kdesc = sc(0x400);
        gm_zero(kdesc, DDSD_SIZE);
        wr32(kdesc + DDSD_OFF_dwSize, DDSD_SIZE);
        wr32(kdesc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_CKSRCBLT);
        wr32(kdesc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
        wr32(kdesc + DDSD_OFF_dwWidth, 4);
        wr32(kdesc + DDSD_OFF_dwHeight, 4);
        wr32(kdesc + DDSD_OFF_ckSrcBlt + DDCK_OFF_lo, 6);
        wr32(kdesc + DDSD_OFF_ckSrcBlt + DDCK_OFF_hi, 6);
        CHECK_EQ(call_method(dd, DD_CreateSurface, {kdesc, sc(0x40), 0}), DD_OK);
        uint32_t keyed = rd32(sc(0x40));
        uint32_t out = sc(0x3b0);
        CHECK_EQ(call_method(keyed, 16 /* GetColorKey */, {DDCKEY_SRCBLT, out}), DD_OK);
        CHECK_EQ(rd32(out + DDCK_OFF_lo), 6u);
        call_method(keyed, S_Release, {});
    }

    // Key out index 3, then BltFast the source to 100,50.
    uint32_t ck = sc(0x380);
    wr32(ck + DDCK_OFF_lo, 3);
    wr32(ck + DDCK_OFF_hi, 3);
    hr = call_method(src, 29 /* SetColorKey */, {DDCKEY_SRCBLT, ck});
    CHECK_EQ(hr, DD_OK);

    hr = call_method(primary, S_BltFast, {100, 50, src, 0, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT});
    CHECK_EQ(hr, DD_OK);
    const Present &p = g_presents.back();
    // The unkeyed left half landed; the keyed right half left the fill intact.
    CHECK_EQ(p.pixels[(size_t)50 * p.pitch + 100], 7);
    CHECK_EQ(p.pixels[(size_t)50 * p.pitch + 107], 7);
    CHECK_EQ(p.pixels[(size_t)50 * p.pitch + 108], 1);
    CHECK_EQ(p.pixels[(size_t)50 * p.pitch + 115], 1);

    // Blt honours the same key through DDBLT_KEYSRC. A cursor sprite is
    // blitted this way, and an unkeyed copy puts an opaque block around it.
    uint32_t drect = sc(0x3c0);
    wr32(drect + 0, 200);
    wr32(drect + 4, 60);
    wr32(drect + 8, 216);
    wr32(drect + 12, 76);
    hr = call_method(primary, S_Blt, {drect, src, 0, DDBLT_KEYSRC | DDBLT_WAIT, 0});
    CHECK_EQ(hr, DD_OK);
    {
        const Present &q = g_presents.back();
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 200], 7); // copied
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 208], 1); // keyed out, fill intact
    }

    // DDBLT_KEYSRCOVERRIDE takes the key from the DDBLTFX instead of the
    // surface, so a caller can key one blit without touching the surface. Key
    // out 7 this time, which is the half the surface's own key keeps.
    gm_zero(fx, DDBLTFX_SIZE);
    wr32(fx + 0, DDBLTFX_SIZE);
    wr32(fx + DDBLTFX_OFF_ddckSrcColorkey + DDCK_OFF_lo, 7);
    wr32(fx + DDBLTFX_OFF_ddckSrcColorkey + DDCK_OFF_hi, 7);
    wr32(drect + 0, 300);
    wr32(drect + 4, 60);
    wr32(drect + 8, 316);
    wr32(drect + 12, 76);
    hr = call_method(primary, S_Blt, {drect, src, 0, DDBLT_KEYSRCOVERRIDE | DDBLT_WAIT, fx});
    CHECK_EQ(hr, DD_OK);
    {
        const Present &q = g_presents.back();
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 300], 1); // 7 keyed out by the override
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 308], 3); // 3 copied
    }

    // A colour key is a range, not one value: keying 2..4 must take index 3.
    wr32(ck + DDCK_OFF_lo, 2);
    wr32(ck + DDCK_OFF_hi, 4);
    call_method(src, 29 /* SetColorKey */, {DDCKEY_SRCBLT, ck});
    wr32(drect + 0, 400);
    wr32(drect + 4, 60);
    wr32(drect + 8, 416);
    wr32(drect + 12, 76);
    call_method(primary, S_Blt, {drect, src, 0, DDBLT_KEYSRC | DDBLT_WAIT, 0});
    {
        const Present &q = g_presents.back();
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 400], 7);
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 408], 1); // 3 is inside 2..4
    }

    // A destination key writes only where the destination matches. The primary
    // is filled with 1, so keying the destination on 1 lets the copy through,
    // and keying it on 9 keeps every destination pixel.
    wr32(ck + DDCK_OFF_lo, 1);
    wr32(ck + DDCK_OFF_hi, 1);
    call_method(primary, 29 /* SetColorKey */, {DDCKEY_DESTBLT, ck});
    wr32(drect + 0, 500);
    wr32(drect + 4, 60);
    wr32(drect + 8, 516);
    wr32(drect + 12, 76);
    call_method(primary, S_Blt, {drect, src, 0, DDBLT_KEYDEST | DDBLT_WAIT, 0});
    {
        const Present &q = g_presents.back();
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 500], 7); // destination was 1: written
    }
    wr32(ck + DDCK_OFF_lo, 9);
    wr32(ck + DDCK_OFF_hi, 9);
    call_method(primary, 29 /* SetColorKey */, {DDCKEY_DESTBLT, ck});
    wr32(drect + 0, 520);
    wr32(drect + 4, 60);
    wr32(drect + 8, 536);
    wr32(drect + 12, 76);
    call_method(primary, S_Blt, {drect, src, 0, DDBLT_KEYDEST | DDBLT_WAIT, 0});
    {
        const Present &q = g_presents.back();
        CHECK_EQ(q.pixels[(size_t)60 * q.pitch + 520], 1); // destination was not 9: kept
    }
}

// The same keyed blit at 16 bpp: a key is compared against whatever the
// surface's pixels are, so the 5-6-5 path must key on the 16-bit value.
static void test_colorkey_16bpp() {
    g_presents.clear();
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    uint32_t desc = sc(0x100);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE);
    call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    uint32_t primary = rd32(sc(4));

    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    wr32(desc + DDSD_OFF_dwWidth, 8);
    wr32(desc + DDSD_OFF_dwHeight, 8);
    call_method(dd, DD_CreateSurface, {desc, sc(8), 0});
    uint32_t src = rd32(sc(8));
    CHECK(src != 0);

    const uint16_t MAGENTA = 0xf81f, GREEN = 0x07e0;
    uint32_t ld = sc(0x200);
    gm_zero(ld, DDSD_SIZE);
    wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(src, S_Lock, {0, ld, DDLOCK_WAIT, 0});
    uint32_t sp = rd32(ld + DDSD_OFF_lpSurface);
    uint32_t spitch = rd32(ld + DDSD_OFF_lPitch);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            wr16(sp + y * spitch + x * 2, x < 4 ? GREEN : MAGENTA);
    call_method(src, S_Unlock, {0});

    uint32_t fx = sc(0x300);
    gm_zero(fx, DDBLTFX_SIZE);
    wr32(fx + 0, DDBLTFX_SIZE);
    wr32(fx + DDBLTFX_OFF_dwFillColor, 0x001f); // blue
    call_method(primary, S_Blt, {0, 0, 0, DDBLT_COLORFILL | DDBLT_WAIT, fx});

    uint32_t ck = sc(0x380);
    wr32(ck + DDCK_OFF_lo, MAGENTA);
    wr32(ck + DDCK_OFF_hi, MAGENTA);
    call_method(src, 29 /* SetColorKey */, {DDCKEY_SRCBLT, ck});
    uint32_t hr =
        call_method(primary, S_BltFast, {20, 30, src, 0, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT});
    CHECK_EQ(hr, DD_OK);
    const Present &p = g_presents.back();
    const uint16_t *row = (const uint16_t *)(p.pixels.data() + (size_t)30 * p.pitch);
    CHECK_EQ(row[20], GREEN); // copied
    CHECK_EQ(row[23], GREEN);
    CHECK_EQ(row[24], 0x001f); // magenta keyed out, the fill shows through
    CHECK_EQ(row[27], 0x001f);
}

// Re-attaching the palette a surface already has must not destroy it. The game
// does exactly this: its WNDPROC at 004b0870 re-attaches the primary's palette
// on every WM_ACTIVATEAPP, and by then the surface can be holding the last
// reference to it.
static void test_setpalette_self() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE);
    call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    uint32_t primary = rd32(sc(4));
    CHECK(primary != 0);

    uint32_t table = sc(0x800);
    for (uint32_t i = 0; i < 256; ++i)
        wr32(table + 4 * i, 0x00010203u * i);
    CHECK_EQ(call_method(dd, DD_CreatePalette, {0x08 /* 8 BIT */, table, sc(0x20), 0}), DD_OK);
    uint32_t pal = rd32(sc(0x20));
    CHECK(pal != 0);

    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);
    // Two references now: the one CreatePalette handed the guest and the one
    // the surface took. Give the guest's back, so the surface holds the last
    // one - which is the case that breaks.
    CHECK_EQ(call_method(pal, 2 /* Release */, {}), 1u);
    ComObj *obj = com_this(pal);
    CHECK(obj != nullptr);
    if (!obj)
        return;
    CHECK_EQ(obj->refs, 1);

    // Attach the same palette again. It must survive, with the count unchanged:
    // releasing the outgoing one before retaining the incoming one would
    // destroy it here and everything after would work on a dead object.
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);
    obj = com_this(pal);
    CHECK(obj != nullptr);
    if (!obj)
        return;
    CHECK_EQ(obj->refs, 1);

    // And it is still the surface's palette, not a dangling id: a present of
    // the primary reads it, so this would fault or come back unpalettised.
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);
    CHECK(com_this(pal) != nullptr);

    call_method(primary, S_Release, {});
}

// SetEventNotification is what the game's two DirectInput service threads at
// 0052c880 and 0052ceda register before waiting; a shim that accepts the call
// and never signals anything leaves the mouse and keyboard dead.
// ---------------------------------------------------------------------------
// The display interface's ABI.
//
// Every later task in this plan writes against these types, and several of
// them copy records into a frame arena and read them back on another thread.
// So the layout is pinned here as a FACT, member by member, rather than left
// to whoever next edits the struct: inserting a field in the middle is a
// change every consumer has to be recompiled for, and a table that fails says
// so at once instead of at the far end of a frame.
//
// The numbers come from the compiler, not from arithmetic done by hand.
// ---------------------------------------------------------------------------
static void test_display_abi() {
    struct Member {
        const char *name;
        size_t offset;
    };
    static const Member kBlit[] = {
        {"seq", 0},
        {"dst", 4},
        {"dst_generation", 8},
        {"dst_x", 12},
        {"dst_y", 16},
        {"w", 20},
        {"h", 24},
        {"src", 28},
        {"src_x", 36},
        {"src_y", 40},
        {"fill_value", 44},
        {"src_key_lo", 48},
        {"src_key_hi", 52},
        {"dst_key_lo", 56},
        {"dst_key_hi", 60},
        {"has_srckey", 64},
        {"has_dstkey", 65},
        {"is_upload", 66},
        {"after_first_draw", 67},
        {"after_first_hud", 68},
        {"palette_version", 72},
        {"coverage", 80},
        {"cpu_pixels", 88},
        {"cpu_bpp", 96},
        {"cpu_pitch", 100},
    };
    CHECK_EQ(sizeof(HostBlitRecord), 104u);
    CHECK_EQ(alignof(HostBlitRecord), 8u);
    for (const Member &m : kBlit) {
        size_t got = 0;
        // Looked up by name so a failure names the member that moved.
        if (!strcmp(m.name, "seq"))
            got = offsetof(HostBlitRecord, seq);
        else if (!strcmp(m.name, "dst"))
            got = offsetof(HostBlitRecord, dst);
        else if (!strcmp(m.name, "dst_generation"))
            got = offsetof(HostBlitRecord, dst_generation);
        else if (!strcmp(m.name, "dst_x"))
            got = offsetof(HostBlitRecord, dst_x);
        else if (!strcmp(m.name, "dst_y"))
            got = offsetof(HostBlitRecord, dst_y);
        else if (!strcmp(m.name, "w"))
            got = offsetof(HostBlitRecord, w);
        else if (!strcmp(m.name, "h"))
            got = offsetof(HostBlitRecord, h);
        else if (!strcmp(m.name, "src"))
            got = offsetof(HostBlitRecord, src);
        else if (!strcmp(m.name, "src_x"))
            got = offsetof(HostBlitRecord, src_x);
        else if (!strcmp(m.name, "src_y"))
            got = offsetof(HostBlitRecord, src_y);
        else if (!strcmp(m.name, "fill_value"))
            got = offsetof(HostBlitRecord, fill_value);
        else if (!strcmp(m.name, "src_key_lo"))
            got = offsetof(HostBlitRecord, src_key_lo);
        else if (!strcmp(m.name, "src_key_hi"))
            got = offsetof(HostBlitRecord, src_key_hi);
        else if (!strcmp(m.name, "dst_key_lo"))
            got = offsetof(HostBlitRecord, dst_key_lo);
        else if (!strcmp(m.name, "dst_key_hi"))
            got = offsetof(HostBlitRecord, dst_key_hi);
        else if (!strcmp(m.name, "has_srckey"))
            got = offsetof(HostBlitRecord, has_srckey);
        else if (!strcmp(m.name, "has_dstkey"))
            got = offsetof(HostBlitRecord, has_dstkey);
        else if (!strcmp(m.name, "is_upload"))
            got = offsetof(HostBlitRecord, is_upload);
        else if (!strcmp(m.name, "after_first_draw"))
            got = offsetof(HostBlitRecord, after_first_draw);
        else if (!strcmp(m.name, "after_first_hud"))
            got = offsetof(HostBlitRecord, after_first_hud);
        else if (!strcmp(m.name, "palette_version"))
            got = offsetof(HostBlitRecord, palette_version);
        else if (!strcmp(m.name, "coverage"))
            got = offsetof(HostBlitRecord, coverage);
        else if (!strcmp(m.name, "cpu_pixels"))
            got = offsetof(HostBlitRecord, cpu_pixels);
        else if (!strcmp(m.name, "cpu_bpp"))
            got = offsetof(HostBlitRecord, cpu_bpp);
        else if (!strcmp(m.name, "cpu_pitch"))
            got = offsetof(HostBlitRecord, cpu_pitch);
        else {
            CHECK(!"the table names a member this test does not read");
            continue;
        }
        if (got != m.offset) {
            printf("  [FAIL] HostBlitRecord::%s moved: %zu, expected %zu\n", m.name, got, m.offset);
            ++g_failures;
        } else {
            ++g_checks;
        }
    }

    // Amendment 3's additions are present and are the types it names.
    CHECK_EQ(sizeof(((HostBlitRecord *)0)->cpu_bpp), 1u);
    CHECK_EQ(sizeof(((HostBlitRecord *)0)->cpu_pitch), 4u);

    // The state a draw carries is copied into a frame arena and read from
    // another thread, so it has to be copyable by memcpy and nothing else.
    static_assert(std::is_trivially_copyable<HostD3DRenderState>::value,
                  "HostD3DRenderState is copied by value into the frame arena");
    static_assert(std::is_trivially_copyable<HostD3DDrawSnapshot>::value,
                  "HostD3DDrawSnapshot is copied by value into the frame arena");
    static_assert(std::is_trivially_copyable<HostBlitRecord>::value,
                  "HostBlitRecord is copied by value into the frame arena");
    CHECK_EQ(sizeof(HostD3DRenderState), 1472u);
    CHECK_EQ(sizeof(HostD3DDrawSnapshot), 1576u);
    CHECK_EQ(sizeof(HostD3DLightValue), 16u);

    // The draw's own members, pinned like the record's. primitive_type is the
    // topology: without it a vertex buffer is a list of points and the
    // renderer has to guess how to join them.
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, seq), 0u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, kind), 5u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, primitive_type), 8u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, vertices), 16u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, state), 64u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, clear_flags), 1552u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, clear_rect_count), 1556u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, clear_rects), 1560u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, clear_color), 1568u);
    CHECK_EQ(offsetof(HostD3DDrawSnapshot, clear_z), 1572u);

    // Material and lights travel as arena-owned VALUES, not as handles that
    // could be read at composite time, and the light array has no cap: the
    // guest may attach as many as it likes and a frame that dropped the
    // seventeenth would light differently from the one it asked for.
    static_assert(std::is_trivially_copyable<HostD3DLightValue>::value,
                  "HostD3DLightValue is copied into the frame arena");
    static_assert(std::is_pointer<decltype(HostD3DRenderState().lights)>::value,
                  "lights is an arena-owned array, not a fixed one");
    static_assert(std::is_pointer<decltype(HostD3DRenderState().material)>::value,
                  "the material is copied, not referred to by handle");
    static_assert(std::is_pointer<decltype(HostD3DDrawSnapshot().clear_rects)>::value,
                  "clear_rects is arena-owned: the shim accepts up to 4096");

    // The state block must be able to hold everything the device keeps, or a
    // draw would be replayed against a truncated copy of its own state.
    CHECK_EQ(HOST_D3D_RENDERSTATE_MAX, D3D_RENDERSTATE_MAX);
    CHECK_EQ(HOST_D3D_LIGHTSTATE_MAX, D3D_LIGHTSTATE_MAX);
    CHECK_EQ(HOST_D3D_TRANSFORM_MAX, D3DTRANSFORMSTATE_MAX);

    // The identities, whose sizes cross the arena too.
    CHECK_EQ(sizeof(HostSurfaceKey), 8u);
    CHECK_EQ(sizeof(HostPixels), 24u);
    CHECK_EQ(sizeof(HostFrameHandle), 8u);
    CHECK_EQ(HOST_SURFACE_NONE, 0u);
    CHECK_EQ(HOST_SRC_CPU, 0xffffffffu);
    CHECK_EQ(HOST_DRAW_PRIMITIVE, 0u);
    CHECK_EQ(HOST_DRAW_CLEAR, 1u);
    CHECK_EQ((uint32_t)HOST_SCREEN_MENU, 0u);
    CHECK_EQ((uint32_t)HOST_SCREEN_FMV, 1u);
    CHECK_EQ((uint32_t)HOST_SCREEN_GAMEPLAY, 2u);

    // A fresh frame answers consistently for every accessor: no records, no
    // draws, no HUD, nothing drawn. Before DISP-T2 these were stub answers;
    // now they are a real empty frame, which is the same contract from the
    // caller's side and the one that has to keep holding.
    reset_ddraw_for_test();
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 0u);
    CHECK(host_frame_record(f, 0) == nullptr);
    CHECK_EQ(host_frame_draw_count(f), 0u);
    CHECK(host_frame_draw(f, 0) == nullptr);
    CHECK_EQ(host_frame_first_hud_seq(f), 0xffffffffu);
    CHECK_EQ(host_frame_had_draws(f), 0);
    CHECK_EQ(host_frame_render_surface(f), HOST_SURFACE_NONE);
    CHECK_EQ(host_cursor_surface(), HOST_SURFACE_NONE);
    // A key nobody leased cannot be leased, and the failure clears the out.
    HostSurfaceKey key = {1u, 1u};
    HostPixels px;
    memset(&px, 0xcd, sizeof px);
    CHECK(host_revision_lease(key, &px) != 0);
    CHECK(px.data == nullptr);

    // The counters are process-wide and cumulative, so this asserts what it
    // can about the ACCESSOR rather than about the numbers: it fills every
    // field, and a reset leaves them all zero. What each reader increments is
    // the "access counts" suite's business.
    HostAccessCounts counts;
    memset(&counts, 0xcd, sizeof counts);
    ddraw_reset_access_counts();
    host_access_counts(&counts);
    CHECK_EQ(counts.lock_read, 0u);
    CHECK_EQ(counts.lock_write, 0u);
    CHECK_EQ(counts.getdc, 0u);
    CHECK_EQ(counts.blt_source, 0u);
    CHECK_EQ(counts.dstkey_read, 0u);
    CHECK_EQ(counts.duplicate, 0u);
    CHECK_EQ(counts.texture_load, 0u);
    CHECK_EQ(counts.flip, 0u);
    CHECK_EQ(counts.clean_reads, 0u);
}

// ===========================================================================
// The frame recorder (DISP-T2).
//
// The helpers below drive the shim through its real vtables - the same path
// the guest takes - because the recorder hangs off those entry points and a
// test that called the recorder directly would not be testing the wiring.
// ===========================================================================
static uint32_t g_rec_dd = 0;

// cpu_reset() throws every COM object away, so a cached interface pointer from
// a previous test is a dangling guest address. Reset it there, not here.
static void rec_reset() {
    cpu_reset();
    g_rec_dd = 0;
    reset_ddraw_for_test();
}

static uint32_t rec_dd() {
    if (g_rec_dd)
        return g_rec_dd;
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0x800), 0});
    g_rec_dd = rd32(sc(0x800));
    // A mode first, as every guest does before it asks for a surface: without
    // one the shim has no pixel format to give an offscreen surface.
    if (g_rec_dd)
        call_method(g_rec_dd, DD_SetDisplayMode, {640, 480, 8});
    return g_rec_dd;
}

// A surface through DD_CreateSurface, with the caps the caller asks for.
static uint32_t rec_make_surface(uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps) {
    uint32_t dd = rec_dd();
    if (!dd)
        return 0;
    uint32_t desc = sc(0x900);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, caps);
    wr32(desc + DDSD_OFF_dwWidth, w);
    wr32(desc + DDSD_OFF_dwHeight, h);
    (void)bpp;
    // The out pointer is kept well clear of the descriptor. It was inside it
    // once - sc(desc + 8) - and CreateSurface's own com_out_ptr(out, 0) then
    // zeroed dwHeight before reading it, so every create failed with
    // DDERR_INVALIDPARAMS and the descriptor looked perfect afterwards.
    call_method(dd, DD_CreateSurface, {desc, sc(0xa00), 0});
    return rd32(sc(0xa00));
}
static uint32_t make_render_target_for_test(uint32_t w, uint32_t h, uint32_t bpp) {
    return rec_make_surface(w, h, bpp, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
}
static uint32_t make_offscreen_for_test(uint32_t w, uint32_t h, uint32_t bpp) {
    return rec_make_surface(w, h, bpp, DDSCAPS_OFFSCREENPLAIN);
}

// A primary with a back buffer, which is what makes it flippable: Flip is one
// of the two events that seal a frame, and a chain with no back buffer answers
// DDERR_NOTFLIPPABLE and seals nothing.
static uint32_t make_primary_chain_for_test() {
    uint32_t dd = rec_dd();
    if (!dd)
        return 0;
    uint32_t desc = sc(0x900);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    wr32(desc + DDSD_OFF_dwBackBufferCount, 1);
    call_method(dd, DD_CreateSurface, {desc, sc(0xa00), 0});
    return rd32(sc(0xa00));
}

static ComObj *rec_obj(uint32_t iface) {
    return iface ? com_this(iface, IF_DDSURFACE) : nullptr;
}

// Writes through the shim's own Lock/Unlock, so the recorder sees a CPU write
// exactly as it would from the guest.
static void fill_for_test(uint32_t surface, uint32_t value) {
    ComObj *o = rec_obj(surface);
    if (!o)
        return;
    uint32_t desc = sc(0xa80);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(surface, S_Lock, {0, desc, DDLOCK_WAIT, 0});
    for (uint32_t y = 0; y < o->height; ++y)
        for (uint32_t x = 0; x < o->width; ++x)
            wr8(o->pixels + y * o->pitch + x, (uint8_t)value);
    call_method(surface, S_Unlock, {0});
}

// Half `a`, half `b`, in vertical stripes: 128 of 256 pixels on a 16x16.
static void checker_fill_for_test(uint32_t surface, uint32_t a, uint32_t b) {
    ComObj *o = rec_obj(surface);
    if (!o)
        return;
    uint32_t desc = sc(0xa80);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(surface, S_Lock, {0, desc, DDLOCK_WAIT, 0});
    for (uint32_t y = 0; y < o->height; ++y)
        for (uint32_t x = 0; x < o->width; ++x)
            wr8(o->pixels + y * o->pitch + x, (uint8_t)((x & 1) ? b : a));
    call_method(surface, S_Unlock, {0});
}

static void lock_write_poke_for_test(uint32_t surface, uint32_t x, uint32_t y, uint32_t v) {
    ComObj *o = rec_obj(surface);
    if (!o)
        return;
    uint32_t desc = sc(0xa80);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(surface, S_Lock, {0, desc, DDLOCK_WAIT, 0});
    wr8(o->pixels + y * o->pitch + x, (uint8_t)v);
    call_method(surface, S_Unlock, {0});
}

static void set_srckey_for_test(uint32_t surface, uint32_t lo, uint32_t hi) {
    uint32_t key = sc(0xb80);
    wr32(key + 0, lo);
    wr32(key + 4, hi);
    call_method(surface, S_SetColorKey, {DDCKEY_SRCBLT, key});
}

static uint32_t blt_for_test(uint32_t dst, uint32_t src, int32_t dx, int32_t dy, int32_t w,
                             int32_t h, uint32_t flags) {
    uint32_t dr = sc(0xc00), sr = sc(0xc40);
    wr32(dr + 0, (uint32_t)dx);
    wr32(dr + 4, (uint32_t)dy);
    wr32(dr + 8, (uint32_t)(dx + w));
    wr32(dr + 12, (uint32_t)(dy + h));
    wr32(sr + 0, 0);
    wr32(sr + 4, 0);
    wr32(sr + 8, (uint32_t)w);
    wr32(sr + 12, (uint32_t)h);
    return call_method(dst, S_Blt, {dr, src, src ? sr : 0u, flags, 0});
}

static uint32_t fill_blt_for_test(uint32_t dst, int32_t dx, int32_t dy, int32_t w, int32_t h,
                                  uint32_t value) {
    uint32_t dr = sc(0xc00), fx = sc(0xc80);
    wr32(dr + 0, (uint32_t)dx);
    wr32(dr + 4, (uint32_t)dy);
    wr32(dr + 8, (uint32_t)(dx + w));
    wr32(dr + 12, (uint32_t)(dy + h));
    gm_zero(fx, DDBLTFX_SIZE);
    wr32(fx + DDBLTFX_OFF_dwFillColor, value);
    return call_method(dst, S_Blt, {dr, 0, 0, DDBLT_COLORFILL, fx});
}

static int coverage_sum(const HostBlitRecord *r) {
    int n = 0;
    for (int32_t i = 0; i < r->w * r->h; ++i)
        n += r->coverage[i];
    return n;
}

static void test_record_basic_and_coverage() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(16, 16, 8);
    CHECK(rt != 0 && sp != 0);
    fill_for_test(sp, 5);
    uint32_t rev = host_surface_revision_for_test(rec_obj(sp)->id);
    reset_ddraw_for_test();
    // Re-read the revision after the reset, so the record and the expectation
    // are taken against the same state.
    rev = host_surface_revision_for_test(rec_obj(sp)->id);
    blt_for_test(rt, sp, 100, 200, 16, 16, 0);
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 1u);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    CHECK_EQ(r->dst_x, 100);
    CHECK_EQ(r->dst_y, 200);
    CHECK_EQ(r->w, 16);
    CHECK_EQ(r->h, 16);
    CHECK_EQ(r->src.surface, rec_obj(sp)->id);
    CHECK_EQ(r->src.revision, rev);
    CHECK_EQ(coverage_sum(r), 256);
}

static void test_keyed_blit_coverage_and_key_values() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(16, 16, 8);
    checker_fill_for_test(sp, 0, 7);
    set_srckey_for_test(sp, 0, 0);
    reset_ddraw_for_test();
    blt_for_test(rt, sp, 0, 0, 16, 16, DDBLT_KEYSRC);
    const HostBlitRecord *r = host_frame_record(host_frame_current(), 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    CHECK_EQ(r->has_srckey, 1);
    CHECK_EQ(r->src_key_lo, 0u);
    CHECK_EQ(r->src_key_hi, 0u);
    // Half the source is the key colour, so half the destination is spared.
    CHECK_EQ(coverage_sum(r), 128);
}

static void test_fill_upload_and_flags() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    fill_for_test(sp, 3);
    reset_ddraw_for_test();

    // A fill has no source and carries the value it filled with.
    fill_blt_for_test(rt, 0, 0, 8, 8, 9);
    const HostBlitRecord *r0 = host_frame_record(host_frame_current(), 0);
    CHECK(r0 != nullptr);
    if (!r0)
        return;
    CHECK_EQ(r0->src.surface, HOST_SURFACE_NONE);
    CHECK_EQ(r0->fill_value, 9u);
    CHECK_EQ(r0->after_first_draw, 0);
    CHECK_EQ(r0->after_first_hud, 0);

    // A draw, then a blit: the blit is after the first draw.
    ddraw_note_draw();
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    const HostBlitRecord *r1 = host_frame_record(host_frame_current(), 1);
    CHECK(r1 != nullptr);
    if (!r1)
        return;
    CHECK_EQ(r1->after_first_draw, 1);
    CHECK_EQ(r1->after_first_hud, 0);
    CHECK_EQ(host_frame_had_draws(host_frame_current()), 1);

    // A HUD blit, then a blit: the later one is after the first HUD, and the
    // frame remembers where the HUD began.
    ddraw_note_hud();
    blt_for_test(rt, sp, 8, 8, 8, 8, 0);
    const HostBlitRecord *r2 = host_frame_record(host_frame_current(), 2);
    CHECK(r2 != nullptr);
    if (!r2)
        return;
    CHECK_EQ(r2->after_first_hud, 1);
    CHECK(host_frame_first_hud_seq(host_frame_current()) != 0xffffffffu);

    // A blit into a surface the device has a texture handle for is an upload.
    ComObj *spo = rec_obj(sp);
    uint32_t saved = spo->texture_handle;
    spo->texture_handle = 0x1234u;
    blt_for_test(sp, rt, 0, 0, 8, 8, 0);
    const HostBlitRecord *r3 = host_frame_record(host_frame_current(), 3);
    CHECK(r3 != nullptr);
    if (r3)
        CHECK_EQ(r3->is_upload, 1);
    spo->texture_handle = saved;

    // A texture surface with no handle yet is still an upload: the handle
    // arrives at GetHandle, and a write before that is not screen content.
    uint32_t tex = rec_make_surface(8, 8, 8, DDSCAPS_TEXTURE);
    CHECK(tex != 0);
    if (!tex)
        return;
    ComObj *to = rec_obj(tex);
    CHECK(to != nullptr);
    if (to)
        CHECK_EQ(to->texture_handle, 0u);
    uint32_t before_n = host_frame_record_count(host_frame_current());
    blt_for_test(tex, sp, 0, 0, 8, 8, 0);
    uint32_t after_n = host_frame_record_count(host_frame_current());
    CHECK_EQ(after_n, before_n + 1);
    const HostBlitRecord *rtex = host_frame_record(host_frame_current(), after_n - 1);
    CHECK(rtex != nullptr);
    if (rtex)
        CHECK_EQ(rtex->is_upload, 1);
}

static void test_revision_bumps_and_retained_lease() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    fill_for_test(sp, 5);
    reset_ddraw_for_test();
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    const HostBlitRecord *r = host_frame_record(host_frame_current(), 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    HostSurfaceKey k = r->src;

    // The guest overwrites the source. The frame still refers to what it
    // recorded, so the old contents have to survive - copied at the moment
    // they were about to be lost, not when they were recorded.
    fill_for_test(sp, 6);
    CHECK(host_surface_revision_for_test(k.surface) != k.revision);
    HostPixels px;
    CHECK_EQ(host_revision_lease(k, &px), 0);
    CHECK(px.data != nullptr);
    if (px.data)
        CHECK_EQ(px.data[0], 5);
    host_revision_release(k);
}

static void test_seal_events() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    CHECK(primary != 0);
    if (!primary)
        return;
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    uint32_t sp = make_offscreen_for_test(4, 4, 8);
    CHECK(rt != 0);
    CHECK(sp != 0);
    if (!rt || !sp)
        return;
    fill_for_test(sp, 7);

    uint32_t caps = sc(0x1408);
    wr32(caps, DDSCAPS_BACKBUFFER);
    CHECK_EQ(call_method(primary, S_GetAttachedSurface, {caps, sc(0x140c)}), DD_OK);
    uint32_t back = rd32(sc(0x140c));
    CHECK(back != 0);
    if (!back)
        return;

    uint32_t entries = sc(0x1000);
    gm_zero(entries, 256 * 4);
    wr32(entries + 1 * 4, 0x000000FFu);
    uint32_t dd = rec_dd();
    CHECK_EQ(call_method(dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x1400), 0}),
             DD_OK);
    uint32_t pal = rd32(sc(0x1400));
    CHECK(pal != 0);
    if (!pal)
        return;
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);

    reset_ddraw_for_test();
    HostFrameHandle f0 = host_frame_current();

    // A frame ends in exactly two places: a Flip of the primary chain, and the
    // production pump's tick. Nothing else does, however much of the screen it
    // changes. These four are the ones that used to.
    blt_for_test(rt, sp, 0, 0, 4, 4, 0); // an offscreen write
    CHECK_EQ(host_frame_current().id, f0.id);
    blt_for_test(primary, sp, 0, 0, 4, 4, 0); // a write to the PRIMARY
    CHECK_EQ(host_frame_current().id, f0.id);
    lock_write_poke_for_test(primary, 1, 1, 3); // and an Unlock on it
    CHECK_EQ(host_frame_current().id, f0.id);
    wr32(entries + 1 * 4, 0x0000FF00u); // and a palette change
    CHECK_EQ(call_method(pal, P_SetEntries, {0, 0, 256, entries}), DD_OK);
    CHECK_EQ(host_frame_current().id, f0.id);

    // The palette change really happened, which is what makes the assertion
    // above about sealing rather than about a call that did nothing.
    const HostBlitRecord *r0 = host_frame_record(f0, 0);
    CHECK(r0 != nullptr);
    blt_for_test(rt, sp, 0, 0, 4, 4, 0);
    const HostBlitRecord *rlast = host_frame_record(f0, host_frame_record_count(f0) - 1);
    CHECK(rlast != nullptr);
    if (r0 && rlast)
        CHECK(rlast->palette_version != r0->palette_version);

    // One frame, not five: the ordering state a HUD rule reads is still the
    // one this frame started with.
    CHECK(host_frame_current().id == f0.id);

    // The pump ends it.
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, f0.id + 1);

    // A pump with no records seals nothing: an idle game must not produce an
    // unbounded stream of empty frames.
    HostFrameHandle f1 = host_frame_current();
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, f1.id);

    // And a Flip of the primary chain ends one, with the content in the back
    // buffer, which is how the game draws.
    blt_for_test(back, sp, 0, 0, 4, 4, 0);
    CHECK_EQ(host_frame_current().id, f1.id);
    call_method(primary, S_Flip, {0, DDFLIP_WAIT});
    CHECK_EQ(host_frame_current().id, f1.id + 1);

    // A draw with no blit behind it is content as well: a gameplay frame that
    // only renders geometry still has a picture to present, and a recorder
    // that counted records alone could never seal one.
    reset_ddraw_for_test();
    HostFrameHandle f2 = host_frame_current();
    ddraw_note_draw();
    ddraw_pump_present();
    CHECK(host_frame_current().id != f2.id);
    ddraw_note_device(0);
}

// Four simultaneously live arenas must stay independent, then reuse all
// chunks (including an oversized chunk) after retirement for 100 frames.
static void test_unchanged_texture_uploads() {
    rec_reset();
    uint32_t id = make_offscreen_for_test(8, 8, 16);
    ComObj *surface = com_this(id);
    CHECK(surface != nullptr);
    if (!surface)
        return;
    surface->texture_handle = 0x170001;
    d3d_upload_texture(surface);
    size_t uploads = g_uploads.size();
    CHECK(uploads > 0);
    uint32_t revision = ddraw_surface_revision(surface->id);
    for (int frame = 0; frame < 100; ++frame)
        d3d_upload_texture(surface);
    printf("T17 unchanged texture fixture phases: upload=%zu/%zu\n", g_uploads.size() - uploads,
           (g_uploads.size() - uploads) * 8 * 8);
    CHECK_EQ(g_uploads.size(), uploads);
    CHECK_EQ(ddraw_surface_revision(surface->id), revision);
    fill_for_test(id, 1);
    d3d_upload_texture(surface);
    CHECK_EQ(g_uploads.size(), uploads + 1);
    CHECK(ddraw_surface_revision(surface->id) != revision);
}

static void test_frame_command_pool() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    CHECK(primary != 0);
    HostFrameHandle held[4]{};
    uint8_t *payload[4]{};
    for (int batch = 0; batch < 26; ++batch) {
        uint64_t before = ddraw_frame_chunk_allocations_for_test();
        for (int i = 0; i < 4; ++i) {
            held[i] = host_frame_current();
            payload[i] = (uint8_t *)ddraw_frame_alloc(100000, 16);
            memset(payload[i], i + 1, 100000);
            blt_for_test(primary, sp, 0, 0, 8, 8, 0);
            pump_present_for_test();
            CHECK(host_frame_current().id != held[i].id);
        }
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(payload[i][0], i + 1);
            CHECK_EQ(payload[i][99999], i + 1);
            host_frame_release(held[i]);
        }
        if (batch)
            CHECK_EQ(ddraw_frame_chunk_allocations_for_test(), before);
    }
}

static void test_frame_release_frees_leases() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    fill_for_test(sp, 5);
    reset_ddraw_for_test();
    uint64_t before = host_retained_bytes_for_test();

    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    HostFrameHandle f = host_frame_current();
    // Overwriting the source forces the copy the frame's lease is holding.
    fill_for_test(sp, 6);
    CHECK(host_retained_bytes_for_test() > before);

    // Sealed first, because a frame is handed over finished and released when
    // the compositor is done with it. A release test that never sealed was
    // releasing a frame still being recorded into.
    //
    // The pump ends a frame that reached the SCREEN, so the frame needs a
    // write to the primary in it: a frame whose content never got there is a
    // picture identical to the one before it.
    uint32_t primary = make_primary_chain_for_test();
    CHECK(primary != 0);
    if (!primary)
        return;
    blt_for_test(primary, sp, 0, 0, 8, 8, 0);
    pump_present_for_test();
    CHECK(host_frame_current().id != f.id);

    // Releasing the frame releases the lease, and the copy goes with it.
    host_frame_release(f);
    CHECK_EQ(host_retained_bytes_for_test(), before);
}

static unsigned presenter_writes = 0, presenter_seals = 0;
static HostFrameHandle presenter_sealed{};
static void recorder_present_write() {
    ++presenter_writes;
}
static void recorder_present_seal() {
    ++presenter_seals;
    presenter_sealed = host_frame_current();
    CHECK(host_frame_palette_version(presenter_sealed) != 0);
}
static void test_presenter_seal_hook_and_retirement_queue() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    uint32_t sprite = make_offscreen_for_test(8, 8, 8);
    CHECK(primary != 0 && sprite != 0);
    if (!primary || !sprite)
        return;
    fill_for_test(sprite, 3);
    reset_ddraw_for_test();
    presenter_writes = presenter_seals = 0;
    ddraw_set_present_callbacks(recorder_present_write, recorder_present_seal);
    pump_present_for_test();
    CHECK_EQ(presenter_seals, 0u);
    CHECK_EQ(presenter_writes, 0u);
    uint32_t desc = sc(0xa80);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc, DDSD_SIZE);
    CHECK_EQ(call_method(sprite, S_Lock, {0, desc, DDLOCK_READONLY | DDLOCK_WAIT, 0}), DD_OK);
    call_method(sprite, S_Unlock, {0});
    CHECK_EQ(presenter_writes, 0u);
    blt_for_test(primary, sprite, 0, 0, 8, 8, 0);
    fill_for_test(sprite, 4);
    auto f = host_frame_current();
    CHECK(host_retained_bytes_for_test() > 0);
    pump_present_for_test();
    CHECK_EQ(presenter_seals, 1u);
    CHECK_EQ(presenter_sealed.id, f.id);
    CHECK(host_frame_current().id != f.id);
    CHECK(presenter_writes > 0);
    pump_present_for_test();
    CHECK_EQ(presenter_seals, 1u);
    std::thread completion([f] { ddraw_present_release(f); });
    completion.join();
    CHECK(host_frame_record_count(f) > 0); // worker never touches guest-owned stores
    ddraw_drain_present_releases();
    CHECK_EQ(host_frame_record_count(f), 0u);
    CHECK_EQ(host_retained_bytes_for_test(), 0u);
    // Primary Flip is the second seal path, with the same callback ordering.
    fill_for_test(primary, 9);
    auto flip_frame = host_frame_current();
    CHECK_EQ(call_method(primary, S_Flip, {0, 0}), DD_OK);
    CHECK_EQ(presenter_seals, 2u);
    CHECK_EQ(presenter_sealed.id, flip_frame.id);
    ddraw_set_present_callbacks(nullptr, nullptr);
    host_frame_release(flip_frame);
}

static void test_screen_class() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    fill_for_test(sp, 1);
    reset_ddraw_for_test();

    // No device and no video: a menu.
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    CHECK_EQ((uint32_t)host_frame_class(host_frame_current()), (uint32_t)HOST_SCREEN_MENU);

    // A device exists: gameplay, whether or not anything was drawn this frame.
    reset_ddraw_for_test();
    ddraw_note_device(1);
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    CHECK_EQ((uint32_t)host_frame_class(host_frame_current()), (uint32_t)HOST_SCREEN_GAMEPLAY);
    CHECK_EQ(host_frame_had_draws(host_frame_current()), 0);
    ddraw_note_draw();
    CHECK_EQ(host_frame_had_draws(host_frame_current()), 1);

    // The movie: the game destroys its device, sets 640x480x16 and the decoder
    // writes the primary through Lock/Unlock. That combination, and only that
    // one, is a video frame.
    reset_ddraw_for_test();
    ddraw_note_device(0);
    uint32_t dd = rec_dd();
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});
    uint32_t primary = make_primary_chain_for_test();
    CHECK(primary != 0);
    if (!primary)
        return;
    // The decoder's write does not end the frame - only a Flip or the pump
    // does - so the frame it landed in is still the current one.
    HostFrameHandle movie = host_frame_current();
    lock_write_poke_for_test(primary, 0, 0, 0x1f);
    CHECK_EQ(host_frame_current().id, movie.id);
    CHECK_EQ((uint32_t)host_frame_class(movie), (uint32_t)HOST_SCREEN_FMV);
    // And the frame AFTER it is not a movie frame. FMV is a property of the
    // frame that carried a decoder write, and carrying it forward called every
    // frame after a movie a movie until something else reclassified it.
    ddraw_pump_present();
    CHECK(host_frame_current().id != movie.id);
    CHECK_EQ((uint32_t)host_frame_class(host_frame_current()), (uint32_t)HOST_SCREEN_MENU);
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});
}

// An 8-bit fade is a run of frames whose only change is a palette write. The
// pixels never move, so a recorder that counted only records and draws would
// seal nothing at all, the compositor would repeat its last frame under the
// palette version it leased, and the fade would never appear on screen.
static void test_palette_only_frames_seal() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    CHECK(primary != 0);
    if (!primary)
        return;
    uint32_t dd = rec_dd();

    uint32_t entries = sc(0x1000);
    gm_zero(entries, 256 * 4);
    wr32(entries + 1 * 4, 0x00000010u);
    CHECK_EQ(call_method(dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x1400), 0}),
             DD_OK);
    uint32_t pal = rd32(sc(0x1400));
    CHECK(pal != 0);
    if (!pal)
        return;
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);

    reset_ddraw_for_test();
    // An idle pump still seals nothing: this is a fade, not a spin.
    HostFrameHandle start = host_frame_current();
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, start.id);

    // Three steps of a fade, each one a SetEntries and a pump tick, and
    // nothing else. Three frames, three palette versions.
    uint32_t ids[3] = {0, 0, 0};
    uint32_t versions[3] = {0, 0, 0};
    for (int step = 0; step < 3; ++step) {
        wr32(entries + 1 * 4, (uint32_t)(0x20 + step * 0x20));
        CHECK_EQ(call_method(pal, P_SetEntries, {0, 0, 256, entries}), DD_OK);
        HostFrameHandle f = host_frame_current();
        ids[step] = (uint32_t)f.id;
        CHECK_EQ(host_frame_record_count(f), 0u); // no pixel moved
        ddraw_pump_present();
        CHECK(host_frame_current().id != f.id); // and yet the frame ended
        versions[step] = host_frame_palette_version(f);
        CHECK(versions[step] != 0u);
        // The colours it sealed under, not the ones the guest moves on to.
        // Released straight away: a lease left open here would hold the
        // version through the next step's write and hide whether the SEAL is
        // holding it.
        const uint8_t *rgb = host_palette_lease(versions[step]);
        CHECK(rgb != nullptr);
        if (rgb) {
            CHECK_EQ(rgb[3], (uint32_t)(0x20 + step * 0x20));
            host_palette_release(versions[step]);
        }
    }
    // The second step's write must not prune the first frame's version. In a
    // fade the guest writes again long before the compositor looks, and a
    // version pruned there would leave the frame naming colours nobody can
    // fetch.
    {
        const uint8_t *first = host_palette_lease(versions[0]);
        CHECK(first != nullptr);
        if (first) {
            CHECK_EQ(first[3], 0x20u);
            host_palette_release(versions[0]);
        }
    }
    CHECK(ids[0] != ids[1]);
    CHECK(ids[1] != ids[2]);
    CHECK(versions[0] != versions[1]);
    CHECK(versions[1] != versions[2]);
    for (int step = 0; step < 3; ++step) {
        HostFrameHandle f = {ids[step]};
        host_frame_release(f);
    }

    // Attaching the palette that is ALREADY attached changes no colour, so it
    // is not a change to the picture and ends no frame. The game re-attaches
    // its palette to the primary as a matter of course.
    reset_ddraw_for_test();
    HostFrameHandle same = host_frame_current();
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, same.id);

    // A palette that governs nothing on screen is not a change to the picture.
    // A texture's palette is the case: the renderer is told, and no frame ends.
    uint32_t tex = rec_make_surface(16, 16, 8, DDSCAPS_TEXTURE);
    CHECK(tex != 0);
    if (!tex)
        return;
    uint32_t entries2 = sc(0x1500); // 0x400 bytes: 0x1500 through 0x18ff
    gm_zero(entries2, 256 * 4);
    CHECK_EQ(call_method(dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries2, sc(0x1900), 0}),
             DD_OK);
    uint32_t texpal = rd32(sc(0x1900));
    CHECK(texpal != 0);
    if (!texpal)
        return;
    CHECK_EQ(call_method(tex, S_SetPalette, {texpal}), DD_OK);
    reset_ddraw_for_test();
    HostFrameHandle quiet = host_frame_current();
    wr32(entries2 + 1 * 4, 0x00000077u);
    CHECK_EQ(call_method(texpal, P_SetEntries, {0, 0, 256, entries2}), DD_OK);
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, quiet.id);
}

// Only the PRIMARY chain's Flip is a frame boundary. An offscreen flip chain is
// a private double buffer of the guest's, and flipping it says nothing about
// what is on screen.
static void test_offscreen_flip_does_not_seal() {
    rec_reset();
    uint32_t dd = rec_dd();
    uint32_t desc = sc(0x900);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_BACKBUFFERCOUNT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    wr32(desc + DDSD_OFF_dwWidth, 64);
    wr32(desc + DDSD_OFF_dwHeight, 64);
    wr32(desc + DDSD_OFF_dwBackBufferCount, 1);
    call_method(dd, DD_CreateSurface, {desc, sc(0xa00), 0});
    uint32_t chain = rd32(sc(0xa00));
    CHECK(chain != 0);
    if (!chain)
        return;
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    CHECK(sp != 0);
    if (!sp)
        return;
    fill_for_test(sp, 4);

    reset_ddraw_for_test();
    HostFrameHandle f = host_frame_current();
    blt_for_test(chain, sp, 0, 0, 8, 8, 0);
    CHECK_EQ(host_frame_current().id, f.id);
    CHECK_EQ(call_method(chain, S_Flip, {0, DDFLIP_WAIT}), DD_OK);
    // Still the same frame: nothing about the screen has happened.
    CHECK_EQ(host_frame_current().id, f.id);

    // Nor does the pump end it. The pump fires on every PeekMessageA and this
    // game pumps in more than one place, so it seals at most once per thing
    // presented or drawn - and an offscreen blit is neither. A frame whose
    // only content never reached the screen would be a frame identical to the
    // one before it.
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, f.id);

    // Nor does a write to the chain's BACK BUFFER. It has a front_obj, like
    // the primary's back buffer does, and nothing about it reaches the screen:
    // the head of its chain is not the primary.
    uint32_t caps = sc(0x1408);
    wr32(caps, DDSCAPS_BACKBUFFER);
    CHECK_EQ(call_method(chain, S_GetAttachedSurface, {caps, sc(0x140c)}), DD_OK);
    uint32_t chain_back = rd32(sc(0x140c));
    CHECK(chain_back != 0);
    if (!chain_back)
        return;
    blt_for_test(chain_back, sp, 0, 0, 8, 8, 0);
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, f.id);

    // A write to the PRIMARY is a presentation, and then the pump ends it.
    uint32_t primary = make_primary_chain_for_test();
    CHECK(primary != 0);
    if (!primary)
        return;
    blt_for_test(primary, sp, 0, 0, 8, 8, 0);
    CHECK_EQ(host_frame_current().id, f.id); // presenting still is not sealing
    ddraw_pump_present();
    CHECK(host_frame_current().id != f.id);

    // And a second pump straight after seals nothing: one seal per picture,
    // which is what keeps a mid-frame input drain from ending a frame twice.
    HostFrameHandle g = host_frame_current();
    ddraw_pump_present();
    CHECK_EQ(host_frame_current().id, g.id);
}

// IDirectDrawSurface4::Unlock is the one that takes a RECT, and the shim reads
// its argument that way only for that interface. The game holds the v1 view, so
// this path has no other cover.
static void test_v4_unlock_takes_a_rect() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    CHECK(rt != 0);
    if (!rt)
        return;
    fill_for_test(rt, 0);
    ComObj *o = rec_obj(rt);
    CHECK(o != nullptr);
    if (!o)
        return;

    // IID_IDirectDrawSurface4 = 0B2B8630-AD35-11D0-8EA6-00609797EA5B.
    uint8_t s4[16] = {0x30, 0x86, 0x2B, 0x0B, 0x35, 0xAD, 0xD0, 0x11,
                      0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, s4[i]);
    CHECK_EQ(call_method(rt, S_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t v4 = rd32(sc(0x60));
    CHECK(v4 != 0);
    if (!v4)
        return;

    reset_ddraw_for_test();
    uint32_t desc = sc(0xa80);
    uint32_t left = sc(0x1d80), right = sc(0x1da0);
    wr32(left + 0, 0);
    wr32(left + 4, 0);
    wr32(left + 8, 16);
    wr32(left + 12, 64);
    wr32(right + 0, 32);
    wr32(right + 4, 0);
    wr32(right + 8, 64);
    wr32(right + 12, 64);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    CHECK_EQ(call_method(v4, S_Lock, {left, desc, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(v4, S_Lock, {right, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 9 * o->pitch + 3, 55); // inside the LEFT lock

    // Closing the left lock by naming its rectangle records the write. Popping
    // the newest instead would look at the right lock, which that pixel is not
    // in, and record nothing.
    CHECK_EQ(call_method(v4, S_Unlock, {left}), DD_OK);
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 1u);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (r) {
        CHECK_EQ(r->dst_x, 3);
        CHECK_EQ(r->dst_y, 9);
    }
    CHECK_EQ(call_method(v4, S_Unlock, {right}), DD_OK);
}

// The game hides the Windows cursor and draws its own, so the shim has to know
// which surface the pointer comes from. It reads the game's own globals:
// create_mouse_surface (004fccb0) puts its two 32x32 pointer surfaces there,
// and the cursor draw at 004fd370 is the only code that uses them.
static void test_cursor_surface_learned() {
    rec_reset();
    const uint32_t kPtrA = 0x005d5718u, kPtrB = 0x005d571cu;
    wr32(kPtrA, 0);
    wr32(kPtrB, 0);
    CHECK_EQ(host_cursor_surface(), HOST_SURFACE_NONE);

    uint32_t pointer = rec_make_surface(32, 32, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    uint32_t sprite = rec_make_surface(32, 32, 8, DDSCAPS_OFFSCREENPLAIN);
    uint32_t primary = make_primary_chain_for_test();
    CHECK(pointer != 0);
    CHECK(sprite != 0);
    CHECK(primary != 0);
    if (!pointer || !sprite || !primary)
        return;
    ComObj *po = rec_obj(pointer);
    CHECK(po != nullptr);
    if (!po)
        return;

    // A small keyed sprite blitted onto the visible chain teaches nothing:
    // that is what a HUD sprite looks like too, and the rule that learned from
    // it could be taught the wrong surface by any frame.
    fill_for_test(sprite, 3);
    set_srckey_for_test(sprite, 0, 0);
    blt_for_test(primary, sprite, 10, 10, 32, 32, DDBLT_KEYSRC);
    CHECK_EQ(host_cursor_surface(), HOST_SURFACE_NONE);

    // The game's own global is what says it.
    wr32(kPtrA, pointer);
    CHECK_EQ(host_cursor_surface(), po->id);

    // Kept while the game rebuilds them: it clears the globals first, and a
    // frame in between still has a pointer in it.
    wr32(kPtrA, 0);
    CHECK_EQ(host_cursor_surface(), po->id);

    // The second global serves as well as the first.
    wr32(kPtrB, pointer);
    CHECK_EQ(host_cursor_surface(), po->id);

    // A value that is not an interface pointer at all is an ordinary answer of
    // "not learned yet". Reading a guest global is a data read, so a number
    // that happens to be there teaches nothing and complains about nothing.
    wr32(kPtrA, 0x12345678u);
    wr32(kPtrB, 0);
    CHECK_EQ(host_cursor_surface(), po->id);

    // A value that names something that is not a 32x32 surface teaches
    // nothing, so a build whose data lies elsewhere learns nothing rather than
    // learning something wrong.
    uint32_t big = make_offscreen_for_test(64, 64, 8);
    CHECK(big != 0);
    wr32(kPtrA, big);
    wr32(kPtrB, 0);
    CHECK_EQ(host_cursor_surface(), po->id); // still the last good answer

    // A REFUSED mode change is a no-op, so it must not throw the answer away:
    // the game goes on drawing in the mode it still has, and it does not check
    // the return.
    uint32_t dd = rec_dd();
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {123, 456, 8}), DDERR_INVALIDPARAMS);
    CHECK_EQ(host_cursor_surface(), po->id);

    // An accepted one rebuilds the pointer surfaces, so the answer goes stale.
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {800, 600, 8}), DD_OK);
    CHECK_EQ(host_cursor_surface(), HOST_SURFACE_NONE);
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});
    wr32(kPtrA, 0);
    wr32(kPtrB, 0);
}

// A Lock hands the guest a raw pointer and the shim sees none of the stores,
// so what a lock wrote can only be known by comparing before with after. Every
// such write is a record, with the payload and coverage a compositor needs to
// replay it - not one synthetic record for the first movie frame.
static void test_lock_write_records() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    CHECK(rt != 0);
    if (!rt)
        return;
    fill_for_test(rt, 0);
    ComObj *o = rec_obj(rt);
    CHECK(o != nullptr);
    if (!o)
        return;
    reset_ddraw_for_test();

    // Three pixels, in a line, in the middle of a full-surface lock.
    uint32_t desc = sc(0xa80);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 20 * o->pitch + 10, 5);
    wr8(o->pixels + 20 * o->pitch + 11, 6);
    wr8(o->pixels + 20 * o->pitch + 12, 7);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);

    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 1u);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    // The write, not the lock: the record is the bounding box of what changed.
    CHECK_EQ(r->src.surface, HOST_SRC_CPU);
    CHECK_EQ(r->dst_x, 10);
    CHECK_EQ(r->dst_y, 20);
    CHECK_EQ(r->w, 3);
    CHECK_EQ(r->h, 1);
    CHECK_EQ(coverage_sum(r), 3);
    // The payload is in the surface's own format, tightly packed.
    CHECK_EQ(r->cpu_bpp, 8);
    CHECK_EQ(r->cpu_pitch, 3);
    CHECK(r->cpu_pixels != nullptr);
    if (r->cpu_pixels) {
        CHECK_EQ(r->cpu_pixels[0], 5);
        CHECK_EQ(r->cpu_pixels[1], 6);
        CHECK_EQ(r->cpu_pixels[2], 7);
    }

    // A lock that wrote nothing is not a write and records nothing.
    reset_ddraw_for_test();
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    CHECK_EQ(host_frame_record_count(host_frame_current()), 0u);

    // A read-only lock is not a write either, whatever happens through the
    // pointer: the guest promised not to write, and the shim takes the promise
    // rather than paying for a snapshot on every read.
    reset_ddraw_for_test();
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT | DDLOCK_READONLY, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    CHECK_EQ(host_frame_record_count(host_frame_current()), 0u);

    // A NESTED write lock is its own write. DirectDraw allows locks to nest,
    // and an inner one can name a rectangle the outer one never covered, so a
    // single outermost shadow loses whatever it wrote.
    reset_ddraw_for_test();
    uint32_t inner = sc(0x1d00);
    wr32(inner + 0, 40);
    wr32(inner + 4, 40);
    wr32(inner + 8, 44);
    wr32(inner + 12, 41);
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Lock, {inner, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 40 * o->pitch + 41, 9);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK); // closes the inner lock
    HostFrameHandle nested = host_frame_current();
    CHECK_EQ(host_frame_record_count(nested), 1u);
    const HostBlitRecord *ri = host_frame_record(nested, 0);
    CHECK(ri != nullptr);
    if (ri) {
        CHECK_EQ(ri->dst_x, 41);
        CHECK_EQ(ri->dst_y, 40);
        CHECK_EQ(ri->w, 1);
        CHECK_EQ(ri->h, 1);
    }
    // The outer lock saw those pixels change too - it was open while they were
    // written - but they are already recorded, and sending them twice would
    // hand the compositor the same payload twice. It adds nothing.
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    CHECK_EQ(host_frame_record_count(nested), 1u);

    // The outer lock still records what IT wrote. Same nesting, but this time
    // the outer lock writes a pixel of its own outside the inner rectangle.
    reset_ddraw_for_test();
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Lock, {inner, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 40 * o->pitch + 41, 21);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    wr8(o->pixels + 50 * o->pitch + 3, 22);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    HostFrameHandle both = host_frame_current();
    CHECK_EQ(host_frame_record_count(both), 2u);
    const HostBlitRecord *outer = host_frame_record(both, 1);
    CHECK(outer != nullptr);
    if (outer) {
        CHECK_EQ(outer->dst_x, 3);
        CHECK_EQ(outer->dst_y, 50);
        CHECK_EQ(outer->w, 1);
        CHECK_EQ(outer->h, 1);
    }

    // Two locks open on rectangles that do not touch. The re-baseline that
    // keeps an outer Unlock from repeating an inner write has to notice that
    // there is nothing in common between them: the intersection is empty, its
    // width is negative, and a memcpy of a negative size is the end of the
    // process.
    reset_ddraw_for_test();
    uint32_t left = sc(0x1d40), right = sc(0x1d60);
    wr32(left + 0, 0);
    wr32(left + 4, 0);
    wr32(left + 8, 16);
    wr32(left + 12, 64);
    wr32(right + 0, 32);
    wr32(right + 4, 0);
    wr32(right + 8, 64);
    wr32(right + 12, 64);
    CHECK_EQ(call_method(rt, S_Lock, {left, desc, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Lock, {right, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 5 * o->pitch + 40, 44); // inside the right lock only
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    HostFrameHandle sib = host_frame_current();
    CHECK_EQ(host_frame_record_count(sib), 1u);
    const HostBlitRecord *rs = host_frame_record(sib, 0);
    CHECK(rs != nullptr);
    if (rs) {
        CHECK_EQ(rs->dst_x, 40);
        CHECK_EQ(rs->dst_y, 5);
    }
    // The left lock closes over a region that was never written, and nothing
    // was re-baselined into it.
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    CHECK_EQ(host_frame_record_count(sib), 1u);

    // A guest that closes its locks OUT OF ORDER says which one it means with
    // the rectangle. Closing the outer lock first must not diff the inner
    // shadow, or whatever the inner lock writes afterwards is lost.
    reset_ddraw_for_test();
    uint32_t whole = sc(0x1d20);
    wr32(whole + 0, 0);
    wr32(whole + 4, 0);
    wr32(whole + 8, 64);
    wr32(whole + 12, 64);
    // IDirectDrawSurface::Unlock takes the pointer Lock returned, not a
    // rectangle, and that is what says which lock is being closed. Two locks
    // side by side, and the guest closes the FIRST one: matching by pointer
    // records the write that landed in it, while popping the newest shadow
    // instead would find a region the write never touched and record nothing.
    reset_ddraw_for_test();
    uint32_t la = sc(0x1d40), lb = sc(0x1d60);
    wr32(la + 0, 0);
    wr32(la + 4, 0);
    wr32(la + 8, 16);
    wr32(la + 12, 64);
    wr32(lb + 0, 32);
    wr32(lb + 4, 0);
    wr32(lb + 8, 64);
    wr32(lb + 12, 64);
    CHECK_EQ(call_method(rt, S_Lock, {la, desc, DDLOCK_WAIT, 0}), DD_OK);
    uint32_t ptr_a = rd32(desc + DDSD_OFF_lpSurface);
    CHECK_EQ(call_method(rt, S_Lock, {lb, desc, DDLOCK_WAIT, 0}), DD_OK);
    uint32_t ptr_b = rd32(desc + DDSD_OFF_lpSurface);
    CHECK(ptr_a != 0);
    CHECK(ptr_b != 0);
    CHECK(ptr_a != ptr_b);
    wr8(o->pixels + 7 * o->pitch + 5, 33); // inside A, not inside B
    CHECK_EQ(call_method(rt, S_Unlock, {ptr_a}), DD_OK);
    HostFrameHandle ooo = host_frame_current();
    CHECK_EQ(host_frame_record_count(ooo), 1u);
    const HostBlitRecord *last = host_frame_record(ooo, 0);
    CHECK(last != nullptr);
    if (last) {
        CHECK_EQ(last->dst_x, 5);
        CHECK_EQ(last->dst_y, 7);
    }
    CHECK_EQ(call_method(rt, S_Unlock, {ptr_b}), DD_OK);

    // And a write lock UNDER a read-only outer lock is still a write. The
    // outer lock shadows nothing, so without a stack this write had nowhere to
    // be recorded at all.
    reset_ddraw_for_test();
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT | DDLOCK_READONLY, 0}), DD_OK);
    CHECK_EQ(call_method(rt, S_Lock, {inner, desc, DDLOCK_WAIT, 0}), DD_OK);
    wr8(o->pixels + 40 * o->pitch + 42, 11);
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    HostFrameHandle under = host_frame_current();
    CHECK_EQ(host_frame_record_count(under), 1u);
    const HostBlitRecord *ru = host_frame_record(under, 0);
    CHECK(ru != nullptr);
    if (ru) {
        CHECK_EQ(ru->dst_x, 42);
        CHECK_EQ(ru->dst_y, 40);
    }
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    // The read-only outer lock adds nothing when it closes.
    CHECK_EQ(host_frame_record_count(under), 1u);
}

// A record names the palette version it was drawn against, and the version's
// colours outlive the guest's next palette write for as long as somebody holds
// them. Without that, a frame composited two writes later repaints in whatever
// colours the guest has by then.
static void test_lock_diff_partial_records_and_payload() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    ComObj *o = com_this(rt);
    CHECK(o != nullptr);
    if (!o)
        return;
    reset_ddraw_for_test();
    uint32_t desc = sc(0x1c00);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc, DDSD_SIZE);
    CHECK_EQ(call_method(rt, S_Lock, {0, desc, DDLOCK_WAIT, 0}), DD_OK);
    for (int cluster = 0; cluster < 2; ++cluster)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                wr8(o->pixels + (10 + cluster * 20 + y) * o->pitch + 10 + x,
                    (uint8_t)(40 + cluster));
    CHECK_EQ(call_method(rt, S_Unlock, {0}), DD_OK);
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 2u);
    for (uint32_t i = 0; i < host_frame_record_count(f); ++i) {
        const HostBlitRecord *r = host_frame_record(f, i);
        CHECK_EQ(r->src.surface, HOST_SRC_CPU);
        CHECK_EQ(r->w, 3);
        CHECK_EQ(r->h, 3);
        CHECK_EQ(coverage_sum(r), 9);
        CHECK(r->cpu_pixels != nullptr);
        if (r->cpu_pixels)
            for (int y = 0; y < r->h; ++y)
                for (int x = 0; x < r->w; ++x)
                    CHECK_EQ(r->cpu_pixels[y * r->cpu_pitch + x], 40u + i);
    }
}

static void test_getdc_releasedc() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    ComObj *o = com_this(rt);
    CHECK(o != nullptr);
    if (!o)
        return;
    reset_ddraw_for_test();
    ddraw_reset_access_counts();
    host_d3d_mark_dirty(o->id, ddraw_surface_generation(o->id), {10, 11, 11, 12});
    CHECK_EQ(call_method(rt, 17, {sc(0x1c00)}), DD_OK); // GetDC
    wr8(o->pixels + 11 * o->pitch + 10, 77);
    CHECK_EQ(call_method(rt, 26, {rd32(sc(0x1c00))}), DD_OK); // ReleaseDC
    HostAccessCounts a{};
    host_access_counts(&a);
    CHECK_EQ(a.getdc, 1u);
    CHECK_EQ(host_readback_reason_count(HOST_READ_GETDC), 1u);
    CHECK_EQ(host_frame_record_count(host_frame_current()), 1u);
    const HostBlitRecord *r = host_frame_record(host_frame_current(), 0);
    CHECK(r != nullptr);
    if (r) {
        CHECK_EQ(r->src.surface, HOST_SRC_CPU);
        CHECK_EQ(r->cpu_pixels[0], 77);
    }
}

static void test_palette_versions() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    uint32_t primary = make_primary_chain_for_test();
    CHECK(rt != 0);
    CHECK(sp != 0);
    CHECK(primary != 0);
    if (!rt || !sp || !primary)
        return;
    fill_for_test(sp, 1);

    uint32_t entries = sc(0x1000);
    gm_zero(entries, 256 * 4);
    wr32(entries + 1 * 4, 0x00000042u); // R,G,B,flags: R = 0x42
    uint32_t dd = rec_dd();
    CHECK_EQ(call_method(dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x1400), 0}),
             DD_OK);
    uint32_t pal = rd32(sc(0x1400));
    CHECK(pal != 0);
    if (!pal)
        return;
    // Attached to the DESTINATION this test records into. A record is drawn in
    // the colours of its own destination chain, not of whatever surface the
    // shim happens to be presenting.
    CHECK_EQ(call_method(rt, S_SetPalette, {pal}), DD_OK);
    CHECK_EQ(call_method(primary, S_SetPalette, {pal}), DD_OK);

    reset_ddraw_for_test();
    // Named before the write. The palette change below does not seal by
    // itself, but it is a change to this frame, so the pump that follows any
    // of this would end it and "the current frame" would be a different one.
    HostFrameHandle f = host_frame_current();
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    uint32_t v = r->palette_version;
    CHECK_EQ(host_palette_version(), v);

    // The guest repaints the palette. The version the record names must still
    // answer with the colours that were live when it was recorded.
    wr32(entries + 1 * 4, 0x00000099u);
    CHECK_EQ(call_method(pal, P_SetEntries, {0, 0, 256, entries}), DD_OK);
    CHECK(host_palette_version() != v);

    const uint8_t *old_rgb = host_palette_lease(v);
    CHECK(old_rgb != nullptr);
    if (old_rgb)
        CHECK_EQ(old_rgb[3], 0x42); // entry 1, red
    const uint8_t *new_rgb = host_palette_lease(host_palette_version());
    CHECK(new_rgb != nullptr);
    if (new_rgb)
        CHECK_EQ(new_rgb[3], 0x99);
    host_palette_release(v);
    host_palette_release(host_palette_version());

    // A version nobody holds is dropped, which is what keeps a level of
    // palette animation from accumulating a snapshot per frame.
    host_frame_release(f);
    CHECK(host_palette_lease(v) == nullptr);

    // And a lease READS. It never decides what the current version is, which
    // it would do by resolving identity: with the live version belonging to an
    // offscreen surface's own palette and the primary on another, re-resolving
    // inside the lease would bump the counter and prune the very version
    // host_palette_version had just reported.
    reset_ddraw_for_test();
    uint32_t own = make_offscreen_for_test(8, 8, 8);
    CHECK(own != 0);
    if (!own)
        return;
    uint32_t entries3 = sc(0x1500); // and its out pointer is clear of it
    gm_zero(entries3, 256 * 4);
    wr32(entries3 + 1 * 4, 0x000000abu);
    CHECK_EQ(call_method(dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries3, sc(0x1904), 0}),
             DD_OK);
    uint32_t otherpal = rd32(sc(0x1904));
    CHECK(otherpal != 0);
    if (!otherpal)
        return;
    CHECK_EQ(call_method(own, S_SetPalette, {otherpal}), DD_OK);
    blt_for_test(own, sp, 0, 0, 8, 8, 0); // a record in own's colours
    HostFrameHandle g = host_frame_current();
    const HostBlitRecord *rg = host_frame_record(g, 0);
    CHECK(rg != nullptr);
    if (!rg)
        return;
    // The premise: the live version belongs to the offscreen surface's own
    // palette, which is not the one the primary is on.
    uint32_t live = host_palette_version();
    CHECK_EQ(rg->palette_version, live);
    host_frame_release(g); // nothing holds it now
    const uint8_t *live_rgb = host_palette_lease(live);
    CHECK(live_rgb != nullptr); // the version just reported
    if (live_rgb)
        CHECK_EQ(live_rgb[3], 0xab); // and in ITS colours

    // Releasing the last hold on the LIVE version must not throw it away. Its
    // identity was fixed when it was made; dropping it lets the next lease of
    // the same number resolve it against whatever is on screen by then, and
    // one version number would report two different palettes.
    if (live_rgb)
        host_palette_release(live);
    CHECK_EQ(host_palette_version(), live);
    const uint8_t *again = host_palette_lease(live);
    CHECK(again != nullptr);
    if (again)
        CHECK_EQ(again[3], 0xab);
    if (again)
        host_palette_release(live);
}

// Content revisions and backing-storage generations are different things: a
// Flip does not change what a surface contains, it changes WHERE the contents
// live, and a record that carried a revision where the generation belongs
// would claim the picture changed when only the address did.
static void test_storage_generations() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    uint32_t rt = make_render_target_for_test(64, 64, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    CHECK(primary != 0);
    CHECK(rt != 0);
    CHECK(sp != 0);
    if (!primary || !rt || !sp)
        return;
    ComObj *po = rec_obj(primary);
    CHECK(po != nullptr);
    if (!po)
        return;

    // The back buffer holds something else, so a swap that lost the leased
    // contents would be visible as the wrong pixels rather than as nothing.
    uint32_t caps = sc(0x1408);
    wr32(caps, DDSCAPS_BACKBUFFER);
    CHECK_EQ(call_method(primary, S_GetAttachedSurface, {caps, sc(0x140c)}), DD_OK);
    uint32_t back = rd32(sc(0x140c));
    CHECK(back != 0);
    if (!back)
        return;
    fill_for_test(back, 9);
    fill_for_test(sp, 5);
    blt_for_test(primary, sp, 0, 0, 8, 8, 0); // the primary now holds 5s
    reset_ddraw_for_test();

    // A frame reads the primary, which leases the primary's own contents.
    HostFrameHandle f = host_frame_current();
    blt_for_test(rt, primary, 0, 0, 8, 8, 0);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    HostSurfaceKey k = r->src;
    CHECK_EQ(k.surface, po->id);
    uint32_t gen = host_surface_generation_for_test(po->id);

    // A Flip moves the generation because the memory changed address, AND the
    // revision because the contents changed with it: afterwards the primary
    // holds what the back buffer held. A revision that stood still here would
    // let a record made after the swap key to the revision whose bytes the
    // retained store is holding from before it.
    uint32_t rev = host_surface_revision_for_test(po->id);
    call_method(primary, S_Flip, {0, DDFLIP_WAIT});
    CHECK(host_surface_generation_for_test(po->id) != gen);
    CHECK(host_surface_revision_for_test(po->id) != rev);

    // And the frame still refers to the pixels it recorded, leased after the
    // swap that moved them. Preserving them is what makes this answerable:
    // without it the lease reads the back buffer's 9s, or nothing at all.
    HostPixels px;
    CHECK_EQ(host_revision_lease(k, &px), 0);
    CHECK(px.data != nullptr);
    if (px.data)
        CHECK_EQ(px.data[0], 5);
    host_revision_release(k);

    // And a record made AFTER the swap names a different revision, so it
    // cannot be handed the bytes the retained store is holding from before it.
    // The primary now holds the back buffer's 9s.
    HostFrameHandle f2 = host_frame_current();
    blt_for_test(rt, primary, 0, 0, 8, 8, 0);
    const HostBlitRecord *r2 = host_frame_record(f2, host_frame_record_count(f2) - 1);
    CHECK(r2 != nullptr);
    if (r2) {
        CHECK(r2->src.revision != k.revision);
        HostPixels px2;
        CHECK_EQ(host_revision_lease(r2->src, &px2), 0);
        CHECK(px2.data != nullptr);
        if (px2.data)
            CHECK_EQ(px2.data[0], 9);
        host_revision_release(r2->src);
    }
    host_frame_release(f);
    host_frame_release(f2);
}

// ===========================================================================
// Draw snapshots (DISP-T3).
//
// A draw is composited long after DrawPrimitive returned, by which time the
// guest has reused its vertex buffer and moved its matrices on. Everything the
// renderer needs is therefore COPIED into the frame's arena at submission, and
// these tests are about that copy being real: the assertions all change the
// guest's own memory after the draw and demand the old values back.
// ===========================================================================
static uint32_t g_t3_dd = 0, g_t3_d3d = 0, g_t3_dev = 0, g_t3_vp = 0, g_t3_target = 0;

// A device with a viewport, built through the real vtables the way the game
// builds one.
static bool make_device_for_test() {
    cpu_reset();
    reset_ddraw_for_test();
    g_t3_dd = g_t3_d3d = g_t3_dev = g_t3_vp = g_t3_target = 0;

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    g_t3_dd = rd32(sc(0));
    if (!g_t3_dd)
        return false;
    call_method(g_t3_dd, DD_SetDisplayMode, {640, 480, 16});

    uint32_t iid = sc(0x40);
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    if (call_method(g_t3_dd, DD_QueryInterface, {iid, sc(0x60)}) != S_OK)
        return false;
    g_t3_d3d = rd32(sc(0x60));

    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
    wr32(desc + DDSD_OFF_dwWidth, 640);
    wr32(desc + DDSD_OFF_dwHeight, 480);
    call_method(g_t3_dd, DD_CreateSurface, {desc, sc(8), 0});
    g_t3_target = rd32(sc(8));
    if (!g_t3_target)
        return false;

    uint8_t hal[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                       0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};
    uint32_t guid = sc(0x500);
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, hal[i]);
    if (call_method(g_t3_d3d, D3D_CreateDevice, {guid, g_t3_target, sc(12)}) != D3D_OK_)
        return false;
    g_t3_dev = rd32(sc(12));
    if (!g_t3_dev)
        return false;

    if (call_method(g_t3_d3d, D3D_CreateViewport, {sc(16), 0}) != D3D_OK_)
        return false;
    g_t3_vp = rd32(sc(16));
    if (!g_t3_vp)
        return false;
    call_method(g_t3_dev, DEV_AddViewport, {g_t3_vp});
    uint32_t vpdata = sc(0x1800);
    gm_zero(vpdata, D3DVIEWPORT2_SIZE);
    wr32(vpdata + D3DVP_OFF_dwSize, D3DVIEWPORT2_SIZE);
    wr32(vpdata + D3DVP_OFF_dwWidth, 640);
    wr32(vpdata + D3DVP_OFF_dwHeight, 480);
    wrf32(vpdata + D3DVP_OFF_dvMinZ, 0.0f);
    wrf32(vpdata + D3DVP_OFF_dvMaxZ, 1.0f);
    call_method(g_t3_vp, VP_SetViewport2, {vpdata});
    call_method(g_t3_dev, DEV_SetCurrentViewport, {g_t3_vp});
    return true;
}

// `n` D3DVT_TLVERTEX vertices whose x is all the same float, so a test can
// say which generation of the buffer it is looking at by reading one number.
static void write_vertices_for_test(uint32_t addr, uint32_t n, float x) {
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t b = addr + v * 32;
        wrf32(b + 0, x);
        wrf32(b + 4, (float)v);
        wrf32(b + 8, 0.5f);
        wrf32(b + 12, 1.0f);
        wr32(b + 16, 0xffffffffu);
        wr32(b + 20, 0);
        wrf32(b + 24, 0.0f);
        wrf32(b + 28, 0.0f);
    }
}

static void set_matrix_for_test(uint32_t dev, uint32_t which, float scale) {
    uint32_t m = sc(0x1900);
    for (uint32_t i = 0; i < 16; ++i)
        wrf32(m + 4u * i, (i == 0 || i == 5 || i == 10) ? scale : (i == 15 ? 1.0f : 0.0f));
    call_method(dev, DEV_SetTransform, {which, m});
}

static void t5_draw(float x0, float y0, float x1, float y1) {
    uint32_t v = sc(0x3000);
    write_vertices_for_test(v, 3, x0);
    wrf32(v + 4, y0);
    wrf32(v + 32, x1);
    wrf32(v + 36, y0);
    wrf32(v + 64, x0);
    wrf32(v + 68, y1);
    CHECK_EQ(call_method(g_t3_dev, DEV_DrawPrimitive, {4, 3, v, 3, 0}), D3D_OK_);
}
static uint32_t t5_hud(int x = 0, int y = 0) {
    uint32_t s = make_offscreen_for_test(64, 64, 16);
    CHECK(s != 0);
    blt_for_test(g_t3_target, s, x, y, 64, 64, 0);
    return s;
}
static void test_first_hud_boundary_and_overlay_pass() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(100, 100, 150, 150);
    t5_hud();
    t5_draw(2, 2, 30, 30);
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_record_count(f), 1u);
    CHECK_EQ(host_frame_draw_count(f), 2u);
    const auto *r = host_frame_record(f, 0);
    CHECK(r != nullptr);
    if (!r)
        return;
    CHECK_EQ(r->after_first_draw, 1);
    CHECK_EQ(host_frame_first_hud_seq(f), r->seq);
    CHECK_EQ(host_frame_draw(f, 0)->in_overlay_pass, 0);
    CHECK_EQ(host_frame_draw(f, 1)->in_overlay_pass, 1);
    CHECK(!host_frame_legacy(f));
}
static void test_overlay_mapping_rule() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(100, 100, 150, 150);
    t5_hud();
    t5_draw(2, 2, 30, 30);
    HostFrameHandle f = host_frame_current();
    CHECK(!strcmp(host_overlay_mapping_for_test(f, host_frame_draw(f, 1)->seq), "ui"));
    CHECK(!host_frame_legacy(f));
    t5_draw(2, 2, 64.1f, 30);
    CHECK(!strcmp(host_overlay_mapping_for_test(f, host_frame_draw(f, 2)->seq), "scene"));
    CHECK(host_frame_legacy(f));
}
static void test_inexpressible_interleave_triggers_legacy_replay() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(100, 100, 150, 150);
    t5_hud();
    t5_draw(32, 32, 90, 90);
    HostFrameHandle f = host_frame_current();
    CHECK(host_frame_legacy(f));
    CHECK_EQ(host_legacy_fallback_count(), 1u);
    t5_draw(40, 40, 100, 100);
    CHECK_EQ(host_legacy_fallback_count(), 1u);
    pump_present_for_test();
    CHECK(host_frame_current().id != f.id);
    CHECK(host_frame_legacy(host_frame_current()));
    t5_draw(200, 200, 220, 220);
    pump_present_for_test();
    CHECK_EQ(host_legacy_fallback_count(), 1u);
    ddraw_note_device(0);
    CHECK(!host_frame_legacy(host_frame_current()));
    ddraw_note_device(1);
    CHECK(!host_frame_legacy(host_frame_current()));
    CHECK(host_frame_legacy(f)); // a sealed frame keeps its decision
}
static void test_hud_rule_exclusions_and_grouping() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(200, 200, 240, 240);
    uint32_t sp = make_offscreen_for_test(64, 64, 16);
    uint32_t other = make_render_target_for_test(64, 64, 16);
    blt_for_test(other, sp, 0, 0, 64, 64, 0);                // not this device's render surface
    blt_for_test(g_t3_target, g_t3_target, 0, 0, 64, 64, 0); // self blit
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_first_hud_seq(f), UINT32_MAX);
    blt_for_test(g_t3_target, sp, 0, 0, 64, 64, 0);
    CHECK_EQ(host_frame_first_hud_seq(f), host_frame_record(f, 2)->seq);
    t5_draw(2, 2, 63.9f, 30); // rounded bounds cross the edge, actual vertices do not
    CHECK_EQ(host_frame_draw_mapping(f, host_frame_draw(f, 1)->seq), HOST_MAPPING_UI);
    CHECK(!host_frame_legacy(f));
    blt_for_test(g_t3_target, sp, 64, 0, 64, 64, 0);
    t5_draw(32, 2, 96, 30); // inside the connected element, crossing its record boundary
    CHECK_EQ(host_frame_draw_mapping(f, host_frame_draw(f, 2)->seq), HOST_MAPPING_UI);
    CHECK(!host_frame_legacy(f));
    // Texture destinations do not become HUD, even with 3DDEVICE caps.
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    ComObj *target = com_this(g_t3_target);
    target->caps |= DDSCAPS_TEXTURE;
    t5_draw(200, 200, 240, 240);
    t5_hud();
    CHECK_EQ(host_frame_first_hud_seq(host_frame_current()), UINT32_MAX);
    target->caps &= ~DDSCAPS_TEXTURE;
}
static void test_transformed_overlay_containment() {
    float v[3][8] = {{-0.9f, 0.9f, 0}, {-0.85f, 0.9f, 0}, {-0.9f, 0.85f, 0}};
    HostD3DDrawSnapshot d{};
    d.vertices = v;
    d.vertex_count = 3;
    d.vertex_stride = 32;
    d.fvf = 2;
    d.state.viewport[2] = 640;
    d.state.viewport[3] = 480;
    CHECK(d3d_draw_inside_rect(&d, 0, 0, 64, 64));
    v[1][0] = -0.79f;
    CHECK(!d3d_draw_inside_rect(&d, 0, 0, 64, 64));
    v[1][0] = -0.85f;
    d.state.transform_set[3] = 1; // w=0 has no finite screen position
    CHECK(!d3d_draw_inside_rect(&d, 0, 0, 640, 480));
}

static void test_later_scene_overlay_intersects_hud() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(100, 100, 150, 150);
    t5_hud();
    t5_draw(200, 200, 220, 220); // clean scene overlay is not a trigger
    HostFrameHandle f = host_frame_current();
    CHECK_EQ(host_frame_draw_mapping(f, host_frame_draw(f, 1)->seq), HOST_MAPPING_SCENE);
    CHECK(!host_frame_legacy(f));
    t5_hud(180, 180); // a later HUD must not move above the already encoded scene draw
    CHECK(host_frame_legacy(f));
    CHECK_EQ(host_legacy_fallback_count(), 1u);
    // The amendment's forward ordering: a HUD destination intersects a later
    // scene-mapped overlay (all vertices do not fit the element).
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    t5_draw(100, 100, 150, 150);
    t5_hud();
    t5_hud(180, 180);
    t5_draw(220, 220, 300, 300);
    f = host_frame_current();
    CHECK(host_frame_record(f, 1)->seq < host_frame_draw(f, 1)->seq);
    CHECK_EQ(host_frame_draw_mapping(f, host_frame_draw(f, 1)->seq), HOST_MAPPING_SCENE);
    CHECK(host_frame_legacy(f));
    CHECK_EQ(host_legacy_fallback_count(), 1u);
}

static void test_no_reader_no_readback() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    ComObj *target = com_this(g_t3_target);
    HostFrameHandle f = host_frame_current();
    HostDirtyRect expected{640, 480, 0, 0};
    for (int i = 0; i < 10; ++i) {
        uint32_t vb = sc(0x1a00);
        write_vertices_for_test(vb, 3, 10.0f + i * 5);
        wrf32(vb + 32, 14.0f + i * 5);
        wrf32(vb + 64 + 4, 8.0f + i);
        CHECK_EQ(call_method(g_t3_dev, DEV_DrawPrimitive,
                             {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0}),
                 D3D_OK_);
        const auto *d = host_frame_draw(f, i);
        CHECK(d != nullptr);
        if (!d)
            continue;
        expected.x0 = std::min(expected.x0, d->screen_min_x);
        expected.y0 = std::min(expected.y0, d->screen_min_y);
        expected.x1 = std::max(expected.x1, d->screen_max_x);
        expected.y1 = std::max(expected.y1, d->screen_max_y);
    }
    pump_present_for_test();
    CHECK(host_frame_current().id != f.id);
    CHECK_EQ(host_readback_count_for_test(), 0u);
    HostDirtyRect actual{};
    CHECK(host_d3d_dirty_rect(target->id, ddraw_surface_generation(target->id), &actual));
    CHECK_EQ(actual.x0, expected.x0);
    CHECK_EQ(actual.y0, expected.y0);
    CHECK_EQ(actual.x1, expected.x1);
    CHECK_EQ(actual.y1, expected.y1);
}

static void test_each_reader_reads_back_only_dirty() {
    for (int reason = 0; reason < HOST_READ_REASON_COUNT; ++reason) {
        CHECK(make_device_for_test());
        if (!g_t3_dev)
            return;
        g_rec_dd = g_t3_dd;
        uint32_t src = g_t3_target, other = make_offscreen_for_test(640, 480, 16);
        ComObj *o = com_this(src);
        CHECK(o != nullptr);
        if (!o)
            return;
        auto read = [&] {
            uint32_t desc = sc(0x1c00);
            gm_zero(desc, DDSD_SIZE);
            wr32(desc, DDSD_SIZE);
            switch (reason) {
            case HOST_READ_LOCK:
            case HOST_READ_LOCK_WRITE:
                CHECK_EQ(
                    call_method(src, S_Lock,
                                {0, desc,
                                 DDLOCK_WAIT | (reason == HOST_READ_LOCK_WRITE ? DDLOCK_WRITEONLY
                                                                               : DDLOCK_READONLY),
                                 0}),
                    DD_OK);
                CHECK_EQ(call_method(src, S_Unlock, {0}), DD_OK);
                break;
            case HOST_READ_GETDC:
                CHECK_EQ(call_method(src, 17, {sc(0x1c00)}), DD_OK);
                CHECK_EQ(call_method(src, 26, {rd32(sc(0x1c00))}), DD_OK);
                break;
            case HOST_READ_BLT_SOURCE:
                blt_for_test(other, src, 0, 0, 32, 32, 0);
                break;
            case HOST_READ_DSTKEY: {
                wr32(sc(0x1c00), 0);
                wr32(sc(0x1c04), 0xffff);
                CHECK_EQ(call_method(src, S_SetColorKey, {DDCKEY_DESTBLT, sc(0x1c00)}), DD_OK);
                blt_for_test(src, other, 0, 0, 32, 32, DDBLT_KEYDEST);
                break;
            }
            case HOST_READ_DUPLICATE:
                CHECK_EQ(call_method(g_t3_dd, 7, {src, sc(0x1c00)}), DD_OK);
                call_method(rd32(sc(0x1c00)), 2, {});
                break;
            case HOST_READ_TEXTURE_LOAD: {
                uint32_t st = com_view(o, IF_D3DTEXTURE2),
                         dt = com_view(com_this(other), IF_D3DTEXTURE2);
                CHECK_EQ(call_method(dt, TEX_Load, {st}), D3D_OK_);
                break;
            }
            }
        };
        host_d3d_mark_dirty(o->id, ddraw_surface_generation(o->id), {2, 3, 12, 13});
        read();
        CHECK_EQ(host_readback_count_for_test(), 1u);
        CHECK_EQ(g_t4_read_rects.size(), 1u);
        if (g_t4_read_rects.size() == 1) {
            const auto &r = g_t4_read_rects.front();
            CHECK_EQ(r.x0, 2);
            CHECK_EQ(r.y0, 3);
            CHECK_EQ(r.x1, 12);
            CHECK_EQ(r.y1, 13);
        }
        CHECK_EQ(host_readback_reason_count((HostReadReason)reason), 1u);
        HostAccessCounts before{}, after{};
        host_access_counts(&before);
        read();
        host_access_counts(&after);
        CHECK_EQ(host_readback_count_for_test(), 1u);
        // A keyed blit has two readers; both are now clean.
        CHECK_EQ(after.clean_reads, before.clean_reads + (reason == HOST_READ_DSTKEY ? 2u : 1u));
    }
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    g_rec_dd = g_t3_dd;
    uint32_t fast_dst = make_offscreen_for_test(640, 480, 16);
    ComObj *fast_src = com_this(g_t3_target);
    host_d3d_mark_dirty(fast_src->id, ddraw_surface_generation(fast_src->id), {1, 2, 3, 4});
    CHECK_EQ(call_method(fast_dst, S_BltFast, {0, 0, g_t3_target, 0, DDBLTFAST_WAIT}), DD_OK);
    CHECK_EQ(host_readback_reason_count(HOST_READ_BLT_SOURCE), 1u);
    HostAccessCounts fast_before{}, fast_after{};
    host_access_counts(&fast_before);
    CHECK_EQ(call_method(fast_dst, S_BltFast, {0, 0, g_t3_target, 0, DDBLTFAST_WAIT}), DD_OK);
    host_access_counts(&fast_after);
    CHECK_EQ(host_readback_count_for_test(), 1u);
    CHECK_EQ(fast_after.clean_reads, fast_before.clean_reads + 1);
    // A partial read must not clean an unread island or reread a clean hole.
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    ComObj *o = com_this(g_t3_target);
    uint32_t g = ddraw_surface_generation(o->id);
    host_d3d_mark_dirty(o->id, g, {0, 0, 30, 30});
    int32_t middle[4] = {10, 10, 20, 20};
    d3d_read_surface(o, middle, HOST_READ_LOCK);
    d3d_read_surface(o, middle, HOST_READ_GETDC);
    CHECK_EQ(host_readback_count_for_test(), 1u);
    CHECK(host_d3d_dirty_rect(o->id, g, nullptr));
    d3d_read_surface(o, nullptr, HOST_READ_DUPLICATE);
    CHECK_EQ(host_readback_count_for_test(), 2u);
    CHECK(!host_d3d_dirty_rect(o->id, g, nullptr));
}

// Repeated contained draws must stay compact, and preserving that union
// must not turn a previously read clean hole back into dirty pixels.
static void test_contained_dirty_draws() {
    host_d3d_reset_coherence();
    uint16_t pixels[32 * 24] = {};
    HostD3DSurface surface{};
    surface.id = 12345;
    surface.width = 32;
    surface.height = 24;
    surface.pixels = pixels;
    surface.pitch = 64;
    surface.bpp = 16;
    constexpr uint32_t generation = 7;
    HostDirtyRect full{0, 0, 32, 24};
    host_d3d_mark_dirty(surface.id, generation, full);
    for (int i = 0; i < 1000; ++i) {
        int x = i % 30, y = (i * 7) % 22;
        host_d3d_mark_dirty(surface.id, generation, {x, y, x + 2, y + 2});
    }
    CHECK_EQ(host_d3d_make_coherent(&surface, generation, nullptr, HOST_READ_LOCK), 1);
    CHECK_EQ(g_t4_read_rects.size(), 1u);
    if (g_t4_read_rects.size() == 1) {
        auto r = g_t4_read_rects[0];
        CHECK_EQ(r.x0, 0);
        CHECK_EQ(r.y0, 0);
        CHECK_EQ(r.x1, 32);
        CHECK_EQ(r.y1, 24);
    }
    // Compare exact dirty coverage to a small bitmap oracle across overlapping
    // writes, CPU cleaning, partial readers, and repeated marks. Every emitted
    // pixel must occur once and belong to the requested dirty set.
    bool dirty[24][32] = {};
    uint32_t random = 0x4137;
    auto next = [&]() {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    auto mark = [&](HostDirtyRect r) {
        host_d3d_mark_dirty(surface.id, generation, r);
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
                dirty[y][x] = true;
    };
    auto read = [&](HostDirtyRect r) {
        g_t4_read_rects.clear();
        int status = host_d3d_make_coherent(&surface, generation, &r, HOST_READ_LOCK);
        unsigned seen[24][32] = {};
        unsigned expected = 0;
        for (auto q : g_t4_read_rects) {
            bool valid = q.x0 >= r.x0 && q.y0 >= r.y0 && q.x1 <= r.x1 && q.y1 <= r.y1 &&
                         q.x0 < q.x1 && q.y0 < q.y1;
            CHECK(valid);
            if (!valid)
                continue;
            for (int y = q.y0; y < q.y1; ++y)
                for (int x = q.x0; x < q.x1; ++x)
                    ++seen[y][x];
        }
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x) {
                bool want = dirty[y][x] && x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
                CHECK_EQ(seen[y][x], unsigned(want));
                expected += want;
                if (want)
                    dirty[y][x] = false;
            }
        CHECK_EQ(status, expected ? 1 : 0);
    };
    mark(full);
    read({8, 6, 24, 18});
    mark({1, 1, 7, 5});
    read({8, 6, 24, 18}); // still a clean hole
    mark({6, 4, 10, 8});
    read(full); // a new draw bridges into the hole
    for (int i = 0; i < 600; ++i) {
        int x = next() % 32, y = next() % 24;
        HostDirtyRect r{x, y, x + 1 + int(next() % (32 - x)), y + 1 + int(next() % (24 - y))};
        if (i % 4 == 0)
            read(r);
        else if (i % 4 == 1) {
            host_d3d_clean_pixels(surface.id, generation, r);
            for (int yy = r.y0; yy < r.y1; ++yy)
                for (int xx = r.x0; xx < r.x1; ++xx)
                    dirty[yy][xx] = false;
        } else {
            mark(r);
            mark(r);
        }
    }
    read(full);
    host_d3d_reset_coherence();
}

static void test_flip_transfers_dirty_region() {
    rec_reset();
    uint32_t primary = make_primary_chain_for_test();
    ComObj *front = com_this(primary);
    CHECK(front != nullptr);
    if (!front)
        return;
    ComObj *back = com_get(front->back_obj);
    CHECK(back != nullptr);
    if (!back)
        return;
    uint32_t fg = ddraw_surface_generation(front->id), bg = ddraw_surface_generation(back->id);
    HostDirtyRect dirty{7, 9, 21, 23}, actual{};
    host_d3d_mark_dirty(back->id, bg, dirty);
    CHECK_EQ(call_method(primary, S_Flip, {0, DDFLIP_WAIT}), DD_OK);
    CHECK(host_d3d_dirty_rect(front->id, fg + 1, &actual));
    CHECK_EQ(actual.x0, dirty.x0);
    CHECK_EQ(actual.y0, dirty.y0);
    CHECK_EQ(actual.x1, dirty.x1);
    CHECK_EQ(actual.y1, dirty.y1);
    CHECK(!host_d3d_dirty_rect(back->id, bg, nullptr));
    CHECK(!host_d3d_dirty_rect(front->id, fg, nullptr));
    CHECK_EQ(host_readback_count_for_test(), 0u);
}

static void test_draw_snapshot_is_deep() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    HostFrameHandle f = host_frame_current();

    uint32_t vb = sc(0x1a00);
    write_vertices_for_test(vb, 16, 1.0f);
    set_matrix_for_test(g_t3_dev, D3DTRANSFORMSTATE_WORLD, 2.0f);
    CHECK_EQ(
        call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 16, 0}),
        D3D_OK_);

    // The guest reuses the buffer and moves the matrix on, which is exactly
    // what it does between one draw and the next.
    write_vertices_for_test(vb, 16, 9.0f);
    set_matrix_for_test(g_t3_dev, D3DTRANSFORMSTATE_WORLD, 3.0f);

    CHECK_EQ(host_frame_draw_count(f), 1u);
    const HostD3DDrawSnapshot *d = host_frame_draw(f, 0);
    CHECK(d != nullptr);
    if (!d)
        return;
    CHECK_EQ(d->kind, HOST_DRAW_PRIMITIVE);
    CHECK_EQ(d->vertex_count, 16u);
    CHECK_EQ(d->vertex_stride, 32u);
    CHECK_EQ(d->primitive_type, (uint32_t)D3DPT_TRIANGLELIST);
    CHECK_EQ(d->fvf, (uint32_t)D3DVT_TLVERTEX);
    CHECK(d->vertices != nullptr);
    if (d->vertices)
        CHECK(((const float *)d->vertices)[0] == 1.0f);
    CHECK_EQ(d->state.transform_set[D3DTRANSFORMSTATE_WORLD], 1);
    CHECK(d->state.transform[D3DTRANSFORMSTATE_WORLD][0] == 2.0f);

    // The copy is the frame's, not the guest's. This is the assertion the
    // whole task is about: a pointer into guest memory here would read the
    // 9.0f above by the time the frame is composited.
    const uint8_t *base = gm_ptr(0);
    const uint8_t *v = (const uint8_t *)d->vertices;
    CHECK(v < base || v >= base + GUEST_SIZE);
}

// A frame's draws are replayed in one order with its blits, so a draw's
// sequence number comes from the same series a blit record's does.
static void test_draw_and_blit_share_one_order() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    HostFrameHandle f = host_frame_current();
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    CHECK(sp != 0);
    if (!sp)
        return;

    uint32_t vb = sc(0x1a00);
    write_vertices_for_test(vb, 3, 1.0f);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});
    blt_for_test(g_t3_target, sp, 0, 0, 8, 8, 0);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});

    CHECK_EQ(host_frame_draw_count(f), 2u);
    CHECK_EQ(host_frame_record_count(f), 1u);
    const HostD3DDrawSnapshot *d0 = host_frame_draw(f, 0);
    const HostD3DDrawSnapshot *d1 = host_frame_draw(f, 1);
    const HostBlitRecord *r = host_frame_record(f, 0);
    CHECK(d0 && d1 && r);
    if (!d0 || !d1 || !r)
        return;
    CHECK(d0->seq < r->seq);
    CHECK(r->seq < d1->seq);
    // And the blit is after the first draw, which is what the HUD rule reads.
    CHECK_EQ(r->after_first_draw, 1);
}

// Clear is part of the draw list, in its place in the order, with its
// rectangles copied: the guest's array is its own the moment Clear returns.
static void test_clear_is_recorded_with_its_rects() {
    CHECK(make_device_for_test());
    if (!g_t3_vp)
        return;
    HostFrameHandle f = host_frame_current();

    uint32_t rects = sc(0x1c00);
    wr32(rects + 0, 10);
    wr32(rects + 4, 20);
    wr32(rects + 8, 110);
    wr32(rects + 12, 220);
    wr32(rects + 16, 300);
    wr32(rects + 20, 5);
    wr32(rects + 24, 400);
    wr32(rects + 28, 15);
    // Viewport::Clear takes (count, rects, flags): two rectangles, target
    // and Z, which is what the game clears at the top of a frame.
    CHECK_EQ(call_method(g_t3_vp, VP_Clear, {2, rects, 3}), D3D_OK_);

    // The guest overwrites its own rectangle array straight afterwards.
    wr32(rects + 0, 999);
    wr32(rects + 4, 999);

    CHECK_EQ(host_frame_draw_count(f), 1u);
    const HostD3DDrawSnapshot *d = host_frame_draw(f, 0);
    CHECK(d != nullptr);
    if (!d)
        return;
    CHECK_EQ(d->kind, HOST_DRAW_CLEAR);
    CHECK_EQ(d->clear_flags, 3u);
    CHECK_EQ(d->clear_rect_count, 2u);
    CHECK(d->clear_rects != nullptr);
    if (d->clear_rects) {
        CHECK_EQ(d->clear_rects[0], 10);
        CHECK_EQ(d->clear_rects[1], 20);
        CHECK_EQ(d->clear_rects[6], 400);
    }
    // Its bounds are what it cleared, which is what the interleaving rule asks.
    CHECK_EQ(d->screen_min_x, 10);
    CHECK_EQ(d->screen_min_y, 5);
    CHECK_EQ(d->screen_max_x, 400);
    CHECK_EQ(d->screen_max_y, 220);
}

// Where a draw lands, which the HUD and interleaving rules are both asked
// about later. A pre-transformed draw is already in screen space.
static void test_draw_screen_bounds() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    HostFrameHandle f = host_frame_current();

    uint32_t vb = sc(0x1a00);
    // Three vertices at (100,10), (140,10), (100,60).
    wrf32(vb + 0, 100.0f);
    wrf32(vb + 4, 10.0f);
    wrf32(vb + 8, 0.5f);
    wrf32(vb + 12, 1.0f);
    wr32(vb + 16, 0xffffffffu);
    wr32(vb + 20, 0);
    wrf32(vb + 24, 0);
    wrf32(vb + 28, 0);
    wrf32(vb + 32, 140.0f);
    wrf32(vb + 36, 10.0f);
    wrf32(vb + 40, 0.5f);
    wrf32(vb + 44, 1.0f);
    wr32(vb + 48, 0xffffffffu);
    wr32(vb + 52, 0);
    wrf32(vb + 56, 0);
    wrf32(vb + 60, 0);
    wrf32(vb + 64, 100.0f);
    wrf32(vb + 68, 60.0f);
    wrf32(vb + 72, 0.5f);
    wrf32(vb + 76, 1.0f);
    wr32(vb + 80, 0xffffffffu);
    wr32(vb + 84, 0);
    wrf32(vb + 88, 0);
    wrf32(vb + 92, 0);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});

    const HostD3DDrawSnapshot *d = host_frame_draw(f, 0);
    CHECK(d != nullptr);
    if (!d)
        return;
    // max is EXCLUSIVE, the same convention as Clear's rectangles: the vertex
    // at 140 is covered, so the bound past it is 141.
    CHECK_EQ(d->screen_min_x, 100);
    CHECK_EQ(d->screen_min_y, 10);
    CHECK_EQ(d->screen_max_x, 141);
    CHECK_EQ(d->screen_max_y, 61);
}

// The frame holds the texture revision its draw named, and lets go when the
// frame retires. Without the lease the guest's next upload replaces the pixels
// under a frame nobody has composited yet.
static void test_draw_leases_its_texture_revision() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    g_tex_leases.clear();

    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 16);
    wr32(desc + DDSD_OFF_dwHeight, 16);
    call_method(g_t3_dd, DD_CreateSurface, {desc, sc(20), 0});
    uint32_t texsurf = rd32(sc(20));
    CHECK(texsurf != 0);
    if (!texsurf)
        return;
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(texsurf, S_QueryInterface, {iid, sc(24)}), S_OK);
    uint32_t tex = rd32(sc(24));
    CHECK(tex != 0);
    if (!tex)
        return;
    CHECK_EQ(call_method(tex, TEX_GetHandle, {g_t3_dev, sc(28)}), D3D_OK_);
    uint32_t handle = rd32(sc(28));
    CHECK(handle != 0);
    if (!handle)
        return;

    ComObj *to = rec_obj(texsurf);
    CHECK(to != nullptr);
    if (!to)
        return;
    uint32_t rev = host_surface_revision_for_test(to->id);

    call_method(g_t3_dev, DEV_SetRenderState, {D3DRENDERSTATE_TEXTUREHANDLE, handle});
    HostFrameHandle f = host_frame_current();
    uint32_t vb = sc(0x1a00);
    write_vertices_for_test(vb, 3, 1.0f);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});

    const HostD3DDrawSnapshot *d = host_frame_draw(f, 0);
    CHECK(d != nullptr);
    if (!d)
        return;
    CHECK_EQ(d->texture_handle, handle);
    CHECK_EQ(d->texture_revision, rev);
    CHECK_EQ(host_sprite_frame_id(), f.id);
    CHECK_EQ(host_sprite_texture_revision(handle), d->texture_revision);
    CHECK_EQ(host_sprite_texture_revision(0xffffffffu), 0u);
    CHECK_EQ(leases_for_test(handle, rev), 1u);

    // The guest writes the texture again: a new revision, and the frame is
    // still holding the old one.
    fill_for_test(texsurf, 7);
    CHECK(host_surface_revision_for_test(to->id) != rev);
    CHECK_EQ(host_sprite_texture_revision(handle), host_surface_revision_for_test(to->id));
    CHECK_EQ(d->texture_revision, rev); // the older draw is still its own revision
    CHECK_EQ(leases_for_test(handle, rev), 1u);

    // Retiring the frame is what lets go.
    host_frame_release(f);
    CHECK_EQ(leases_for_test(handle, rev), 0u);
}

// Every path that changes what a texture SAMPLES has to move its revision, or
// the upload that follows replaces a texture some frame is still holding under
// the same key - which is the one thing the lease exists to prevent.
static void test_every_upload_path_bumps_the_revision() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    uint32_t desc = sc(0x400);
    auto make_tex = [&](uint32_t out) {
        gm_zero(desc, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
        wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
        wr32(desc + DDSD_OFF_dwWidth, 16);
        wr32(desc + DDSD_OFF_dwHeight, 16);
        call_method(g_t3_dd, DD_CreateSurface, {desc, out, 0});
        return rd32(out);
    };
    uint32_t a_surf = make_tex(sc(20)), b_surf = make_tex(sc(0x30));
    CHECK(a_surf != 0);
    CHECK(b_surf != 0);
    if (!a_surf || !b_surf)
        return;
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(a_surf, S_QueryInterface, {iid, sc(24)}), S_OK);
    CHECK_EQ(call_method(b_surf, S_QueryInterface, {iid, sc(0x34)}), S_OK);
    uint32_t a = rd32(sc(24)), b = rd32(sc(0x34));
    CHECK(a != 0);
    CHECK(b != 0);
    if (!a || !b)
        return;
    CHECK_EQ(call_method(a, TEX_GetHandle, {g_t3_dev, sc(28)}), D3D_OK_);
    CHECK_EQ(call_method(b, TEX_GetHandle, {g_t3_dev, sc(0x38)}), D3D_OK_);
    ComObj *ao = rec_obj(a_surf);
    ComObj *bo = rec_obj(b_surf);
    CHECK(ao != nullptr);
    CHECK(bo != nullptr);
    if (!ao || !bo)
        return;

    // Load copies new pixels in.
    uint32_t before = host_surface_revision_for_test(ao->id);
    CHECK_EQ(call_method(a, TEX_Load, {b}), D3D_OK_);
    CHECK(host_surface_revision_for_test(ao->id) != before);

    // A palette re-resolve changes what the same indices mean.
    before = host_surface_revision_for_test(ao->id);
    CHECK_EQ(call_method(a, TEX_PaletteChanged, {0, 0}), D3D_OK_);
    CHECK(host_surface_revision_for_test(ao->id) != before);

    // And a handle swap makes each handle name the other surface's pixels.
    uint32_t ra = host_surface_revision_for_test(ao->id);
    uint32_t rb = host_surface_revision_for_test(bo->id);
    uint32_t ha = ao->texture_handle, hb = bo->texture_handle;
    CHECK_EQ(call_method(g_t3_dev, DEV_SwapTextureHandles, {a, b}), D3D_OK_);
    uint32_t ra2 = host_surface_revision_for_test(ao->id);
    uint32_t rb2 = host_surface_revision_for_test(bo->id);
    CHECK(ra2 != ra);
    CHECK(rb2 != rb);
    // And not merely different from their own previous values: different from
    // every revision EITHER surface has held. Per-surface counters leave both
    // at low numbers, so after a swap "handle A revision 3" can mean one
    // surface's pixels or the other's, and a frame holding the first is handed
    // the second.
    CHECK(ra2 != rb);
    CHECK(rb2 != ra);
    CHECK(ra2 != rb2);
    CHECK_EQ(host_sprite_texture_revision(ha), rb2);
    CHECK_EQ(host_sprite_texture_revision(hb), ra2);
}

// Lookup must retain object identity as the table grows, rejects released
// objects, and clears every old mapping before handles restart after reset.
static void test_texture_handle_lifetime() {
    rec_reset();
    d3d_reset();
    struct Texture {
        uint32_t surface, view, handle, revision;
    };
    std::vector<Texture> textures;
    for (unsigned i = 0; i < 512; ++i) {
        uint32_t surface = rec_make_surface(1, 1, 8, DDSCAPS_TEXTURE);
        ComObj *o = rec_obj(surface);
        CHECK(o != nullptr);
        if (!o)
            return;
        uint32_t view = com_view(o, IF_D3DTEXTURE2);
        CHECK_EQ(call_method(view, TEX_GetHandle, {0, sc(28)}), D3D_OK_);
        textures.push_back({surface, view, rd32(sc(28)), host_surface_revision_for_test(o->id)});
    }
    for (const auto &t : textures) {
        CHECK(t.revision != 0);
        CHECK_EQ(host_sprite_texture_revision(t.handle), t.revision);
        CHECK_EQ(call_method(t.view, TEX_GetHandle, {0, sc(28)}), D3D_OK_);
        CHECK_EQ(rd32(sc(28)), t.handle);
    }
    for (size_t i = 0; i < textures.size(); i += 2) {
        CHECK_EQ(call_method(textures[i].surface, S_Release, {}), 0u);
        CHECK_EQ(host_sprite_texture_revision(textures[i].handle), 0u);
    }
    for (size_t i = 1; i < textures.size(); i += 2)
        CHECK_EQ(host_sprite_texture_revision(textures[i].handle), textures[i].revision);
    CHECK_EQ(host_sprite_texture_revision(0), 0u);
    CHECK_EQ(host_sprite_texture_revision(0xffffffffu), 0u);
    d3d_reset();
    for (const auto &t : textures)
        CHECK_EQ(host_sprite_texture_revision(t.handle), 0u);
    uint32_t surface = rec_make_surface(1, 1, 8, DDSCAPS_TEXTURE);
    ComObj *o = rec_obj(surface);
    CHECK(o != nullptr);
    if (!o)
        return;
    uint32_t view = com_view(o, IF_D3DTEXTURE2);
    CHECK_EQ(call_method(view, TEX_GetHandle, {0, sc(28)}), D3D_OK_);
    CHECK_EQ(rd32(sc(28)), textures.front().handle);
    CHECK_EQ(host_sprite_texture_revision(rd32(sc(28))), host_surface_revision_for_test(o->id));
}

// Revisions come from one counter for the whole process, so no two surfaces
// ever share one. Per-surface counters put two freshly written surfaces at the
// same number, and a handle swap then makes one (handle, revision) pair mean
// two different pictures - a frame holding the first is handed the second.
static void test_revisions_are_unique_across_surfaces() {
    rec_reset();
    uint32_t one = make_offscreen_for_test(8, 8, 8);
    uint32_t two = make_offscreen_for_test(8, 8, 8);
    CHECK(one != 0);
    CHECK(two != 0);
    if (!one || !two)
        return;
    ComObj *o1 = rec_obj(one);
    ComObj *o2 = rec_obj(two);
    CHECK(o1 != nullptr);
    CHECK(o2 != nullptr);
    if (!o1 || !o2)
        return;

    // The same history on each: created, then written once.
    fill_for_test(one, 1);
    fill_for_test(two, 1);
    CHECK(host_surface_revision_for_test(o1->id) != host_surface_revision_for_test(o2->id));

    // And again after a second write apiece.
    fill_for_test(one, 2);
    fill_for_test(two, 2);
    CHECK(host_surface_revision_for_test(o1->id) != host_surface_revision_for_test(o2->id));
}

// A palette write changes what an 8-bit texture's indices resolve to. The
// renderer holds expanded pixels and refuses to overwrite a revision a frame
// is holding, so without a new revision the upload is dropped and the texture
// never fades.
static void test_palette_write_bumps_a_texture_revision() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    // An 8-bit mode, so the texture is palettised: a palette on a 16-bit
    // surface is refused, and it is the palettised ones that fade.
    CHECK_EQ(call_method(g_t3_dd, DD_SetDisplayMode, {640, 480, 8}), DD_OK);
    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 16);
    wr32(desc + DDSD_OFF_dwHeight, 16);
    call_method(g_t3_dd, DD_CreateSurface, {desc, sc(20), 0});
    uint32_t texsurf = rd32(sc(20));
    CHECK(texsurf != 0);
    if (!texsurf)
        return;

    uint32_t entries = sc(0x1000);
    gm_zero(entries, 256 * 4);
    wr32(entries + 1 * 4, 0x00000030u);
    CHECK_EQ(call_method(g_t3_dd, DD_CreatePalette,
                         {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x1400), 0}),
             DD_OK);
    uint32_t pal = rd32(sc(0x1400));
    CHECK(pal != 0);
    if (!pal)
        return;
    CHECK_EQ(call_method(texsurf, S_SetPalette, {pal}), DD_OK);

    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(texsurf, S_QueryInterface, {iid, sc(24)}), S_OK);
    uint32_t tex = rd32(sc(24));
    CHECK(tex != 0);
    if (!tex)
        return;
    CHECK_EQ(call_method(tex, TEX_GetHandle, {g_t3_dev, sc(28)}), D3D_OK_);
    uint32_t handle = rd32(sc(28));
    ComObj *to = rec_obj(texsurf);
    CHECK(handle != 0);
    CHECK(to != nullptr);
    if (!handle || !to)
        return;

    // A frame draws with it, so the revision it uses is held.
    g_tex_leases.clear();
    call_method(g_t3_dev, DEV_SetRenderState, {D3DRENDERSTATE_TEXTUREHANDLE, handle});
    uint32_t vb = sc(0x1a00);
    write_vertices_for_test(vb, 3, 1.0f);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});
    uint32_t held = host_surface_revision_for_test(to->id);
    CHECK_EQ(leases_for_test(handle, held), 1u);

    // The guest fades the palette. The texture's revision has to move, and the
    // upload that follows has to carry the new one.
    size_t uploads_before = g_uploads.size();
    wr32(entries + 1 * 4, 0x00000090u);
    CHECK_EQ(call_method(pal, P_SetEntries, {0, 0, 256, entries}), DD_OK);
    uint32_t after = host_surface_revision_for_test(to->id);
    CHECK(after != held);
    CHECK(g_uploads.size() > uploads_before);
    CHECK_EQ(g_uploads.back().handle, handle);
    CHECK_EQ(g_uploads.back().revision, after);
}

// A draw can name a revision the renderer never received without anything
// having written the texture: a Flip or a SetSurfaceDesc moves a surface's
// revision on its own. The draw uploads and leases rather than sampling
// nothing.
static void test_draw_uploads_a_revision_the_renderer_lacks() {
    CHECK(make_device_for_test());
    if (!g_t3_dev)
        return;
    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 16);
    wr32(desc + DDSD_OFF_dwHeight, 16);
    call_method(g_t3_dd, DD_CreateSurface, {desc, sc(20), 0});
    uint32_t texsurf = rd32(sc(20));
    CHECK(texsurf != 0);
    if (!texsurf)
        return;
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(texsurf, S_QueryInterface, {iid, sc(24)}), S_OK);
    uint32_t tex = rd32(sc(24));
    CHECK(tex != 0);
    if (!tex)
        return;
    CHECK_EQ(call_method(tex, TEX_GetHandle, {g_t3_dev, sc(28)}), D3D_OK_);
    uint32_t handle = rd32(sc(28));
    CHECK(handle != 0);
    ComObj *to = rec_obj(texsurf);
    CHECK(to != nullptr);
    if (!handle || !to)
        return;

    // The revision moves with no upload behind it, which is what a Flip or a
    // SetSurfaceDesc does to a surface.
    ddraw_storage_changed_for_test(to->id);
    uint32_t rev = host_surface_revision_for_test(to->id);

    g_tex_leases.clear();
    call_method(g_t3_dev, DEV_SetRenderState, {D3DRENDERSTATE_TEXTUREHANDLE, handle});
    HostFrameHandle f = host_frame_current();
    uint32_t vb = sc(0x1a00);
    write_vertices_for_test(vb, 3, 1.0f);
    call_method(g_t3_dev, DEV_DrawPrimitive, {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, vb, 3, 0});

    const HostD3DDrawSnapshot *d = host_frame_draw(f, 0);
    CHECK(d != nullptr);
    if (!d)
        return;
    // The draw holds a revision the renderer has, rather than one it does not.
    CHECK_EQ(d->texture_revision, rev);
    CHECK_EQ(leases_for_test(handle, rev), 1u);
}

// Load reads the source surface's pixels. It is one of the readers the
// coherence contract names, and it was the only one with no counter.
static void test_texture_load_is_counted() {
    CHECK(make_device_for_test());
    if (!g_t3_dd)
        return;
    uint32_t desc = sc(0x400);
    auto make_tex = [&](uint32_t out) {
        gm_zero(desc, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
        wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
        wr32(desc + DDSD_OFF_dwWidth, 16);
        wr32(desc + DDSD_OFF_dwHeight, 16);
        call_method(g_t3_dd, DD_CreateSurface, {desc, out, 0});
        return rd32(out);
    };
    uint32_t dst_surf = make_tex(sc(20));
    uint32_t src_surf = make_tex(sc(0x30));
    CHECK(dst_surf != 0);
    CHECK(src_surf != 0);
    if (!dst_surf || !src_surf)
        return;

    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(dst_surf, S_QueryInterface, {iid, sc(24)}), S_OK);
    CHECK_EQ(call_method(src_surf, S_QueryInterface, {iid, sc(0x34)}), S_OK);
    uint32_t dst = rd32(sc(24)), src = rd32(sc(0x34));
    CHECK(dst != 0);
    CHECK(src != 0);
    if (!dst || !src)
        return;

    ddraw_reset_access_counts();
    HostAccessCounts a;
    CHECK_EQ(call_method(dst, TEX_Load, {src}), D3D_OK_);
    host_access_counts(&a);
    CHECK_EQ(a.texture_load, 1u);
    // And nothing else claims the read.
    CHECK_EQ(a.blt_source, 0u);
    CHECK_EQ(a.lock_read, 0u);
}

// Every reader named by the coherence contract increments exactly its own
// counter and no other. The point is the "and no other": a single readback
// total would say a frame was expensive, and these say which path made it so.
static void test_access_counts_by_reason() {
    rec_reset();
    uint32_t rt = make_render_target_for_test(640, 480, 8);
    uint32_t sp = make_offscreen_for_test(8, 8, 8);
    CHECK(rt != 0 && sp != 0);
    if (!rt || !sp)
        return;
    uint32_t desc = sc(0xa80);

    // A read lock, and only a read lock.
    ddraw_reset_access_counts();
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(sp, S_Lock, {0, desc, DDLOCK_WAIT | DDLOCK_READONLY, 0});
    call_method(sp, S_Unlock, {0});
    HostAccessCounts a;
    host_access_counts(&a);
    CHECK_EQ(a.lock_read, 1u);
    CHECK_EQ(a.lock_write, 0u);
    CHECK_EQ(a.getdc, 0u);
    CHECK_EQ(a.blt_source, 0u);
    CHECK_EQ(a.dstkey_read, 0u);
    CHECK_EQ(a.duplicate, 0u);
    CHECK_EQ(a.flip, 0u);

    // A write lock, and only a write lock.
    ddraw_reset_access_counts();
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(sp, S_Lock, {0, desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, 0});
    call_method(sp, S_Unlock, {0});
    host_access_counts(&a);
    CHECK_EQ(a.lock_write, 1u);
    CHECK_EQ(a.lock_read, 0u);

    // A lock with neither hint can read, so it counts as a read.
    ddraw_reset_access_counts();
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(sp, S_Lock, {0, desc, DDLOCK_WAIT, 0});
    call_method(sp, S_Unlock, {0});
    host_access_counts(&a);
    CHECK_EQ(a.lock_read, 1u);
    CHECK_EQ(a.lock_write, 0u);

    // A blit reads its source. No destination key here, so no key read.
    ddraw_reset_access_counts();
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    host_access_counts(&a);
    CHECK_EQ(a.blt_source, 1u);
    CHECK_EQ(a.dstkey_read, 0u);
    CHECK_EQ(a.lock_read, 0u);
    CHECK_EQ(a.lock_write, 0u);

    // A fill has no source at all: nothing is read.
    ddraw_reset_access_counts();
    fill_blt_for_test(rt, 0, 0, 8, 8, 4);
    host_access_counts(&a);
    CHECK_EQ(a.blt_source, 0u);
    CHECK_EQ(a.dstkey_read, 0u);

    // A destination-keyed blit reads BOTH: the source for its pixels and the
    // destination to decide which of its own pixels may be written.
    ddraw_reset_access_counts();
    uint32_t key = sc(0xb80);
    wr32(key + 0, 0);
    wr32(key + 4, 0);
    call_method(rt, S_SetColorKey, {DDCKEY_DESTBLT, key});
    blt_for_test(rt, sp, 0, 0, 8, 8, DDBLT_KEYDEST);
    host_access_counts(&a);
    CHECK_EQ(a.blt_source, 1u);
    CHECK_EQ(a.dstkey_read, 1u);

    // A flip reads both buffers.
    ddraw_reset_access_counts();
    uint32_t primary = make_primary_chain_for_test();
    if (primary) {
        call_method(primary, S_Flip, {0, DDFLIP_WAIT});
        host_access_counts(&a);
        CHECK_EQ(a.flip, 1u);
        CHECK_EQ(a.blt_source, 0u);
    }

    // A source without GPU-dirty pixels is a clean reader.
    ddraw_reset_access_counts();
    blt_for_test(rt, sp, 0, 0, 8, 8, 0);
    host_access_counts(&a);
    CHECK_EQ(a.blt_source, 1u);
    CHECK_EQ(a.clean_reads, 1u);

    // Lock flags are hints: even WRITEONLY exposes a readable guest pointer.
    ddraw_reset_access_counts();
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    call_method(sp, S_Lock, {0, desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, 0});
    call_method(sp, S_Unlock, {0});
    host_access_counts(&a);
    CHECK_EQ(a.lock_write, 1u);
    CHECK_EQ(a.lock_read, 0u);
    CHECK_EQ(a.clean_reads, 1u);

    // texture_load belongs to d3d.cpp, where Load lives, and is Task 3's.
    CHECK_EQ(a.texture_load, 0u);
}

static void test_dinput_event_notification() {
    cpu_reset();
    uint32_t create = tramp("DINPUT.dll", "DirectInputCreateA");
    CHECK_EQ(call_shim(create, {0x400000, 0x0500, sc(0), 0}), DI_OK);
    uint32_t di = rd32(sc(0));
    CHECK(di != 0);

    // GUID_SysKeyboard, as the game asks for it.
    uint32_t guid = sc(0x40);
    static const uint8_t KBD[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                    0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, KBD[i]);
    CHECK_EQ(call_method(di, DI_CreateDevice, {guid, sc(0x50), 0}), DI_OK);
    uint32_t dev = rd32(sc(0x50));
    CHECK(dev != 0);

    // An auto-reset event, the kind the game registers.
    uint32_t createev = tramp("KERNEL32.dll", "CreateEventA");
    uint32_t ev = call_shim(createev, {0, 0, 0, 0});
    CHECK(ev != 0);
    CHECK_EQ(call_method(dev, DID_SetEventNotification, {ev}), DI_OK);

    // Taking the event with a zero timeout is how the test asks "is it
    // signalled", and it consumes it exactly as a waiting guest would. Every
    // one of these goes through imports_dispatch, whose scheduling checkpoint
    // is where a signal queued from a host thread is applied.
    uint32_t wait = tramp("KERNEL32.dll", "WaitForSingleObject");
    CHECK_EQ(call_shim(wait, {ev, 0}), 0x102u); // nothing yet

    dinput_host_input_changed();
    CHECK_EQ(call_shim(wait, {ev, 0}), 0u);     // woken
    CHECK_EQ(call_shim(wait, {ev, 0}), 0x102u); // and consumed by the wait

    // The next change signals it again rather than leaving the guest asleep.
    dinput_host_input_changed();
    CHECK_EQ(call_shim(wait, {ev, 0}), 0u);

    // INVALID_HANDLE_VALUE clears the registration, and then nothing is
    // signalled however much input arrives.
    CHECK_EQ(call_method(dev, DID_SetEventNotification, {0xffffffffu}), DI_OK);
    dinput_host_input_changed();
    CHECK_EQ(call_shim(wait, {ev, 0}), 0x102u);

    // Re-registering works, and a reset forgets it: after one the handle would
    // name a kernel object that no longer exists.
    CHECK_EQ(call_method(dev, DID_SetEventNotification, {ev}), DI_OK);
    dinput_host_input_changed();
    CHECK_EQ(call_shim(wait, {ev, 0}), 0u);
    dinput_reset();
    dinput_host_input_changed();
    CHECK_EQ(call_shim(wait, {ev, 0}), 0x102u);
}

// Two devices share one host input state, and host_input_state consumes the
// mouse deltas. So whichever device polls first takes them, and with both
// woken by the same notification the keyboard usually gets there first. That
// is a mouse whose buttons work and whose movement does not: buttons are a
// level the host keeps reporting, motion is a delta reported once.
// An unchanged host state yields no buffered event.
//
// This is a property of the shim, not of the gate: it builds its events by
// diffing the host state against what it saw last time. It is stated here
// because this is where the buffered path lives and this file has no host
// input layer - it feeds the shim through its own g_input.
//
// CONSUMPTION ITSELF IS NOT PROVED HERE, and this test should not be read as
// proving it. That is host_tests.mm's "input gate", which drives the real
// host_key_event and checks all four guest channels including this one, with
// the DirectInput shim linked in.
static void test_unchanged_state_produces_no_buffered_event() {
    cpu_reset();
    dinput_reset();
    memset(&g_input, 0, sizeof g_input);

    uint32_t create = tramp("DINPUT.dll", "DirectInputCreateA");
    CHECK_EQ(call_shim(create, {0x400000, 0x0500, sc(0), 0}), DI_OK);
    uint32_t di = rd32(sc(0));

    uint32_t guid = sc(0x40);
    static const uint8_t KBD[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                    0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, KBD[i]);
    CHECK_EQ(call_method(di, DI_CreateDevice, {guid, sc(0x50), 0}), DI_OK);
    uint32_t kb = rd32(sc(0x50));
    CHECK(kb != 0);

    uint32_t df = sc(0x70);
    gm_zero(df, 24);
    wr32(df + 0, 24);
    wr32(df + 4, 16);
    wr32(df + 12, 256);
    CHECK_EQ(call_method(kb, DID_SetDataFormat, {df}), DI_OK);
    uint32_t prop = sc(0x80);
    gm_zero(prop, 20);
    wr32(prop + 0, 20);
    wr32(prop + 4, 16);
    wr32(prop + 12, 0);
    wr32(prop + 16, 32);
    CHECK_EQ(call_method(kb, DID_SetProperty, {1 /* DIPROP_BUFFERSIZE */, prop}), DI_OK);
    CHECK_EQ(call_method(kb, DID_Acquire, {}), DI_OK);

    const uint32_t kDikF10 = 0x44;
    uint32_t count = sc(0x90), buf = sc(0xa0);

    // Drain whatever acquiring left, so what follows is only this key.
    wr32(count, 8);
    call_method(kb, DID_GetDeviceData, {16, buf, count, 0});

    // Delivered: the host set the state, so the shim derives one event for it.
    g_input.keys[kDikF10] = 0x80;
    dinput_host_input_changed();
    wr32(count, 8);
    CHECK_EQ(call_method(kb, DID_GetDeviceData, {16, buf, count, 0}), DI_OK);
    CHECK_EQ(rd32(count), 1u);
    CHECK_EQ(rd32(buf + 0), kDikF10);
    CHECK_EQ(rd32(buf + 4), 0x80u);

    // The state is exactly what it was, which is what a consumed press leaves
    // behind upstream: host_input_key was never called. Nothing to derive.
    dinput_host_input_changed();
    wr32(count, 8);
    CHECK_EQ(call_method(kb, DID_GetDeviceData, {16, buf, count, 0}), DI_OK);
    CHECK_EQ(rd32(count), 0u);
}

static void test_mouse_motion_survives_keyboard_poll() {
    cpu_reset();
    dinput_reset();
    uint32_t create = tramp("DINPUT.dll", "DirectInputCreateA");
    CHECK_EQ(call_shim(create, {0x400000, 0x0500, sc(0), 0}), DI_OK);
    uint32_t di = rd32(sc(0));

    uint32_t guid = sc(0x40);
    static const uint8_t KBD[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                    0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, KBD[i]);
    CHECK_EQ(call_method(di, DI_CreateDevice, {guid, sc(0x50), 0}), DI_OK);
    uint32_t kb = rd32(sc(0x50));

    static const uint8_t MOU[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                    0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, MOU[i]);
    CHECK_EQ(call_method(di, DI_CreateDevice, {guid, sc(0x58), 0}), DI_OK);
    uint32_t ms = rd32(sc(0x58));
    CHECK(kb != 0 && ms != 0);

    // Both formatted, buffered and acquired, as the game sets them up.
    uint32_t df = sc(0x70);
    gm_zero(df, 24);
    wr32(df + 0, 24);   // dwSize
    wr32(df + 4, 16);   // dwObjSize
    wr32(df + 12, 256); // c_dfDIKeyboard: 256 bytes
    CHECK_EQ(call_method(kb, DID_SetDataFormat, {df}), DI_OK);
    wr32(df + 12, DIMOUSESTATE_SIZE); // c_dfDIMouse: 16 bytes
    CHECK_EQ(call_method(ms, DID_SetDataFormat, {df}), DI_OK);

    uint32_t prop = sc(0x80);
    gm_zero(prop, 20);
    wr32(prop + 0, 20);
    wr32(prop + 4, 16);
    wr32(prop + 12, 0);
    wr32(prop + 16, 32);
    CHECK_EQ(call_method(kb, DID_SetProperty, {1 /* DIPROP_BUFFERSIZE */, prop}), DI_OK);
    CHECK_EQ(call_method(ms, DID_SetProperty, {1, prop}), DI_OK);
    CHECK_EQ(call_method(kb, DID_Acquire, {}), DI_OK);
    CHECK_EQ(call_method(ms, DID_Acquire, {}), DI_OK);

    // The host reports motion and a button down.
    g_input.mouse_dx = 7;
    g_input.mouse_dy = -3;
    g_input.mouse_buttons[0] = 0x80;

    // The keyboard polls FIRST, which is what the shared notification causes.
    uint32_t count = sc(0x90);
    uint32_t buf = sc(0xa0);
    wr32(count, 8);
    CHECK_EQ(call_method(kb, DID_GetDeviceData, {16, buf, count, 0}), DI_OK);

    // The mouse must still see the motion. Before the fix the keyboard's poll
    // had consumed and discarded it, and this came back with only the button.
    wr32(count, 8);
    CHECK_EQ(call_method(ms, DID_GetDeviceData, {16, buf, count, 0}), DI_OK);
    uint32_t n = rd32(count);
    bool saw_x = false, saw_y = false, saw_button = false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t ofs = rd32(buf + i * 16 + 0);
        int32_t dat = (int32_t)rd32(buf + i * 16 + 4);
        if (ofs == 0 && dat == 7)
            saw_x = true;
        if (ofs == 4 && dat == -3)
            saw_y = true; // negative deltas sign-extend
        if (ofs == 12 && dat == 0x80)
            saw_button = true;
    }
    CHECK(saw_x);
    CHECK(saw_y);
    CHECK(saw_button);

    // The immediate state reports motion since the last call and clears it.
    // Read once first to clear what the buffered block above accumulated: both
    // views are fed by the same motion, and the immediate one holds it until
    // somebody reads it.
    uint32_t st = sc(0xc0);
    CHECK_EQ(call_method(ms, DID_GetDeviceState, {16, st}), DI_OK);
    g_input.mouse_dx = 5;
    call_method(kb, DID_GetDeviceData, {16, buf, count, 0}); // keyboard polls again
    g_input.mouse_dx = 4;                                    // and more arrives
    CHECK_EQ(call_method(ms, DID_GetDeviceState, {16, st}), DI_OK);
    CHECK_EQ((int32_t)rd32(st + 0), 9); // 5 accumulated + 4, none lost
    CHECK_EQ(call_method(ms, DID_GetDeviceState, {16, st}), DI_OK);
    CHECK_EQ((int32_t)rd32(st + 0), 0); // and reading cleared it
    CHECK_EQ(rd8(st + 12), 0x80u);      // the button is a level, still down

    g_input.mouse_buttons[0] = 0;
    dinput_reset();
}

// QueryInterface: the DirectDraw object hands out IDirectDraw2 and 4, refuses
// an interface it does not implement, and reaches Direct3D2.
static void test_query_interface() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));

    // The IIDs the game actually asks for, from cross-referencing the EXE.
    struct {
        const char *name;
        uint8_t iid[16];
        bool expect;
    } cases[] = {
        {"IDirectDraw2",
         {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11, 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33,
          0x56},
         true},
        {"IDirectDraw4",
         {0x9A, 0x50, 0x59, 0x9C, 0xBD, 0x39, 0xD1, 0x11, 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30,
          0xC5},
         true},
        {"IDirect3D2",
         {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11, 0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7,
          0x6A},
         true},
        // A DirectDraw object is not a sound buffer.
        {"IDirectSoundBuffer",
         {0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5,
          0x60},
         false},
    };
    for (auto &t : cases) {
        uint32_t iid = sc(0x40);
        for (int i = 0; i < 16; ++i)
            wr8(iid + (uint32_t)i, t.iid[i]);
        wr32(sc(0x60), 0xdeadbeef);
        uint32_t hr = call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
        if (t.expect) {
            CHECK_EQ(hr, S_OK);
            CHECK(rd32(sc(0x60)) != 0);
            CHECK(rd32(sc(0x60)) != 0xdeadbeef);
        } else {
            CHECK_EQ(hr, E_NOINTERFACE);
            CHECK_EQ(rd32(sc(0x60)), 0);
        }
    }

    // An unregistered IID is refused rather than matched by accident.
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, (uint8_t)(0x11 + i));
    uint32_t hr = call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    CHECK_EQ(hr, E_NOINTERFACE);

    // Two QueryInterface calls for the same interface return the same pointer,
    // as COM requires for a stable identity.
    uint8_t dd2[16] = {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11,
                       0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, dd2[i]);
    call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    uint32_t first = rd32(sc(0x60));
    call_method(dd, DD_QueryInterface, {iid, sc(0x64)});
    CHECK_EQ(rd32(sc(0x64)), first);
    // and IDirectDraw2's vtable is a different one from IDirectDraw's.
    CHECK(rd32(first + COM_OFF_vtbl) != rd32(dd + COM_OFF_vtbl));
}

// EnumDisplayModes offers the native modes, through a real guest
// callback.
static uint32_t g_enum_count = 0;
static uint32_t g_enum_modes[16][3];

static void test_enum_display_modes() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));

    // The guest callback is a trampoline of our own: recomp_call routes any
    // trampoline address here, so guest_call reaches it exactly as it would
    // reach translated code.
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "EnumModesCallback",
        [](X86 *c) {
            uint32_t desc = arg(c, 0);
            if (g_enum_count < 16) {
                g_enum_modes[g_enum_count][0] = rd32(desc + DDSD_OFF_dwWidth);
                g_enum_modes[g_enum_count][1] = rd32(desc + DDSD_OFF_dwHeight);
                g_enum_modes[g_enum_count][2] =
                    rd32(desc + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwRGBBitCount);
            }
            ++g_enum_count;
            set_eax(c, DDENUMRET_OK);
        },
        2);

    g_enum_count = 0;
    uint32_t hr = call_method(dd, DD_EnumDisplayModes, {0, 0, 0, cb});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(g_enum_count, 10);
    CHECK_EQ(g_enum_modes[0][0], 640);
    CHECK_EQ(g_enum_modes[0][1], 480);
    CHECK_EQ(g_enum_modes[0][2], 8);
    CHECK_EQ(g_enum_modes[5][0], 1024);
    CHECK_EQ(g_enum_modes[5][2], 16);
    CHECK_EQ(g_enum_modes[9][0], 3840);
    CHECK_EQ(g_enum_modes[9][1], 2160);
    CHECK_EQ(g_enum_modes[9][2], 16);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {3840, 2160, 16}), DD_OK);

    // A restricted enumeration returns only the matching modes.
    uint32_t match = sc(0x100);
    gm_zero(match, DDSD_SIZE);
    wr32(match + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(match + DDSD_OFF_dwFlags, DDSD_WIDTH | DDSD_HEIGHT);
    wr32(match + DDSD_OFF_dwWidth, 800);
    wr32(match + DDSD_OFF_dwHeight, 600);
    g_enum_count = 0;
    call_method(dd, DD_EnumDisplayModes, {0, match, 0, cb});
    CHECK_EQ(g_enum_count, 2);

    // An unoffered mode is refused rather than silently accepted.
    hr = call_method(dd, DD_SetDisplayMode, {1600, 1200, 32});
    CHECK_EQ(hr, DDERR_INVALIDPARAMS);
}

// POPM_DDRAW_MODES replaces the offered set, and the SAME table decides what
// SetDisplayMode accepts. That is the whole point of the amendment: a mode the
// game was offered and then refused, or refused and then offered, is a
// contradiction the guest cannot recover from, and one table is what makes it
// impossible.
static void enum_modes_now(uint32_t dd, uint32_t cb) {
    g_enum_count = 0;
    memset(g_enum_modes, 0, sizeof g_enum_modes);
    CHECK_EQ(call_method(dd, DD_EnumDisplayModes, {0, 0, 0, cb}), DD_OK);
}

static void test_classic_probe_surface_creation() {
    // Match mode_probe.py: scrub inherited POP tuning, preserve runtime paths,
    // offer both boot depths plus the candidate, then reapply after Classic init.
    struct ProbeEnvironment {
        std::map<std::string, std::string> saved;
        static std::map<std::string, std::string> take() {
            extern char **environ;
            std::map<std::string, std::string> values;
            for (char **p = environ; *p; ++p) {
                std::string entry(*p);
                if (entry.starts_with("POPM_") || entry.starts_with("POP_SMOKE_") ||
                    entry.starts_with("POP_HOST_") || entry.starts_with("POP_RECOMP_")) {
                    auto equal = entry.find('=');
                    values.emplace(entry.substr(0, equal), entry.substr(equal + 1));
                }
            }
            for (const auto &[key, value] : values)
                unsetenv(key.c_str());
            return values;
        }
        ProbeEnvironment() : saved(take()) {}
        ~ProbeEnvironment() {
            take();
            for (const auto &[key, value] : saved)
                setenv(key.c_str(), value.c_str(), 1);
            ddraw_reset_modes();
        }
    } environment;
    char root[4096];
    CHECK(os_getcwd(root, sizeof root) == 0);
    const uint32_t sizes[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 960},
                                 {1600, 1200}, {1920, 1440}, {2560, 1920}, {3840, 2880},
                                 {1280, 720},  {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (const auto &size : sizes)
        for (uint32_t depth : {8u, 16u}) {
            std::string target = std::to_string(size[0]) + "x" + std::to_string(size[1]) + "x" +
                                 std::to_string(depth);
            std::string list = "640x480x8,640x480x16";
            if (size[0] != 640 || size[1] != 480)
                list += "," + target;
            std::string path = std::string(root) + "/build/recomp/mode-probe/" + target;
            setenv("POPM_DDRAW_MODES", list.c_str(), 1);
            setenv("POPM_NO_MODS", "1", 1);
            setenv("POP_RECOMP_PIN_CLOCK", "1", 1);
            setenv("POP_RECOMP_SCRIPT", (path + "/probe.script").c_str(), 1);
            setenv("POP_HOST_DUMP_DIR", path.c_str(), 1);
            setenv("POP_SMOKE_CLASSIC_PROBE", target.c_str(), 1);
            ddraw_reset_modes();
            CHECK_EQ(ddraw_set_modes(getenv("POPM_DDRAW_MODES")), 1);
            cpu_reset();
            CHECK_EQ(call_shim(tramp("DDRAW.dll", "DirectDrawCreate"), {0, sc(0), 0}), DD_OK);
            uint32_t dd = rd32(sc(0));
            CHECK_EQ(call_method(dd, DD_SetCooperativeLevel,
                                 {0x20004, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN}),
                     DD_OK);
            // The real log first creates a desktop primary before SetDisplayMode,
            // then boots through both depths. No requested pixel format is supplied.
            for (uint32_t boot_depth : {0u, 8u, 16u}) {
                if (boot_depth)
                    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {640, 480, boot_depth}), DD_OK);
                uint32_t desc = sc(0x200);
                gm_zero(desc, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
                wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
                wr32(desc + DDSD_OFF_ddsCaps,
                     DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP);
                wr32(desc + DDSD_OFF_dwBackBufferCount, 1);
                // An invalid out-pointer must be reported as a primary request,
                // and the next valid request must still succeed.
                if (!boot_depth)
                    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, 0, 0}), DDERR_INVALIDPARAMS);
                CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
                CHECK(rd32(sc(0x10)) != 0);
                CHECK_EQ(rd32(desc + DDSD_OFF_dwWidth), 640u);
                CHECK_EQ(rd32(desc + DDSD_OFF_dwHeight), 480u);
                CHECK_EQ(rd32(desc + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwRGBBitCount),
                         boot_depth ? boot_depth : 8u);
                CHECK_EQ(call_method(rd32(sc(0x10)), S_Release, {}), 0u);
            }
            // PVRC is an optional PowerVR texture candidate, not a primary format.
            // Its refusal must leave ordinary RGB/offscreen creation working.
            uint32_t desc = sc(0x200), pf = desc + DDSD_OFF_ddpfPixelFormat;
            gm_zero(desc, DDSD_SIZE);
            wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
            wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
            wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
            wr32(desc + DDSD_OFF_dwWidth, 64);
            wr32(desc + DDSD_OFF_dwHeight, 64);
            wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
            wr32(pf + DDPF_OFF_dwFlags, DDPF_FOURCC);
            wr32(pf + DDPF_OFF_dwFourCC, 0x43525650u);
            CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}),
                     DDERR_INVALIDPIXELFORMAT);
            CHECK_EQ(rd32(sc(0x10)), 0u);
            wr32(pf + DDPF_OFF_dwFlags, DDPF_RGB);
            wr32(pf + DDPF_OFF_dwFourCC, 0);
            wr32(pf + DDPF_OFF_dwRGBBitCount, 16);
            CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
            CHECK_EQ(rd32(desc + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwRBitMask), 0xf800u);
            CHECK_EQ(call_method(rd32(sc(0x10)), S_Release, {}), 0u);
            CHECK_EQ(call_method(dd, DD_SetDisplayMode, {size[0], size[1], depth}), DD_OK);
            CHECK_EQ(call_method(dd, 2 /* Release */, {}), 0u);
        }
}

static void test_configurable_display_modes() {
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "EnumModesCallback2",
        [](X86 *c) {
            uint32_t desc = arg(c, 0);
            if (g_enum_count < 16) {
                g_enum_modes[g_enum_count][0] = rd32(desc + DDSD_OFF_dwWidth);
                g_enum_modes[g_enum_count][1] = rd32(desc + DDSD_OFF_dwHeight);
                g_enum_modes[g_enum_count][2] =
                    rd32(desc + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwRGBBitCount);
            }
            ++g_enum_count;
            set_eax(c, DDENUMRET_OK);
        },
        2);

    auto fresh_dd = [&]() {
        cpu_reset();
        uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
        call_shim(create, {0, sc(0), 0});
        return rd32(sc(0));
    };

    // A list of two, one of which the built-in table does not contain.
    setenv("POPM_DDRAW_MODES", "640x480x8,1280x960x16", 1);
    ddraw_reset_modes();
    uint32_t dd = fresh_dd();
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 2u);
    CHECK_EQ(g_enum_modes[0][0], 640u);
    CHECK_EQ(g_enum_modes[0][1], 480u);
    CHECK_EQ(g_enum_modes[0][2], 8u);
    CHECK_EQ(g_enum_modes[1][0], 1280u);
    CHECK_EQ(g_enum_modes[1][1], 960u);
    CHECK_EQ(g_enum_modes[1][2], 16u);
    // Accepted because it is offered...
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1280, 960, 16}), DD_OK);
    // ...and 800x600, which the built-in table has and this list does not, is
    // now refused. This is the assertion that proves one table and not two.
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {800, 600, 16}), DDERR_INVALIDPARAMS);

    // A single mode, which is how a display test forces the game's hand.
    setenv("POPM_DDRAW_MODES", "1920x1080x16", 1);
    ddraw_reset_modes();
    dd = fresh_dd();
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 1u);
    CHECK_EQ(g_enum_modes[0][0], 1920u);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1920, 1080, 16}), DD_OK);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {640, 480, 8}), DDERR_INVALIDPARAMS);

    // Every way of being malformed keeps the built-in list, whole. A list
    // honoured up to its first mistake would offer a set nobody wrote down.
    const char *bad[] = {
        "",                      // set to nothing is the same as unset
        "640x480",               // a mode needs a depth
        "640x480x8,",            // a trailing comma is a missing mode
        "640x480x8 1280x960x16", // a space is not a separator
        "640x480x24",            // a depth the enumeration cannot describe
        "640x480x0",             // nor can it describe none
        "0x480x8",               // a mode with no width is not a mode
        "abcx480x8",             // and neither is a word
        "640x480x8,junk",        // one bad entry rejects the whole list
    };
    for (const char *spec : bad) {
        setenv("POPM_DDRAW_MODES", spec, 1);
        ddraw_reset_modes();
        dd = fresh_dd();
        enum_modes_now(dd, cb);
        if (g_enum_count != 10)
            printf("  POPM_DDRAW_MODES=\"%s\" gave %u modes, wanted the built-in 10\n", spec,
                   g_enum_count);
        CHECK_EQ(g_enum_count, 10u);
        CHECK_EQ(call_method(dd, DD_SetDisplayMode, {800, 600, 16}), DD_OK);
    }

    // ddraw_set_modes does the same job at runtime, which is what a display
    // script uses. It exists because the variable cannot do this safely: the
    // game selects 640x480x8 at startup without asking what is available, so
    // a variable that omits it has that call refused and the guest walks into
    // a SIGBUS. Setting the table after boot has no such problem.
    unsetenv("POPM_DDRAW_MODES");
    ddraw_reset_modes();
    dd = fresh_dd();
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {640, 480, 8}), DD_OK);
    CHECK_EQ(ddraw_set_modes("1280x960x16"), 1);
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 1u);
    CHECK_EQ(g_enum_modes[0][0], 1280u);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1280, 960, 16}), DD_OK);

    // A malformed runtime spec changes nothing at all, so a typo cannot
    // narrow the offered set behind the script's back.
    CHECK_EQ(ddraw_set_modes("1280x960"), 0);
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 1u);
    CHECK_EQ(g_enum_modes[0][0], 1280u);

    // And an empty spec puts the built-in list back.
    CHECK_EQ(ddraw_set_modes(nullptr), 1);
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 10u);

    // Unset is the same list as well, which is the state every other test runs in -
    // so this one has to leave it that way.
    unsetenv("POPM_DDRAW_MODES");
    ddraw_reset_modes();
    dd = fresh_dd();
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 10u);
    CHECK_EQ(g_enum_modes[5][0], 1024u);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1280, 960, 16}), DDERR_INVALIDPARAMS);
    CHECK_EQ(ddraw_add_mode(1280, 960, 16), 1);
    CHECK_EQ(ddraw_add_mode(1280, 960, 16), 1); // duplicate request adds no duplicate row
    CHECK_EQ(ddraw_add_mode(1920, 1080, 32), 0);
    enum_modes_now(dd, cb);
    CHECK_EQ(g_enum_count, 11u);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1280, 960, 16}), DD_OK);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {3840, 2160, 16}), DD_OK);
    ddraw_reset_modes();
}

// The Direct3D path the game takes: QueryInterface for IDirect3D2, FindDevice
// for the HAL device, CreateDevice on a 3D back buffer, a viewport, then a
// scene with a textured indexed draw.
static void test_d3d_pipeline() {
    cpu_reset();
    g_draws.clear();
    g_textures.clear();
    g_begin_scene = g_end_scene = 0;

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    // IDirect3D2 off the DirectDraw object.
    uint32_t iid = sc(0x40);
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    uint32_t hr = call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    CHECK_EQ(hr, S_OK);
    uint32_t d3d = rd32(sc(0x60));
    CHECK(d3d != 0);

    // FindDevice for the HAL device, the way init_d3d does it.
    uint32_t search = sc(0x100);
    uint32_t result = sc(0x200);
    gm_zero(search, D3DFDS_SIZE);
    wr32(search + D3DFDS_OFF_dwSize, D3DFDS_SIZE);
    wr32(search + D3DFDS_OFF_dwFlags, D3DFDS_GUID);
    uint8_t hal[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                       0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};
    for (int i = 0; i < 16; ++i)
        wr8(search + D3DFDS_OFF_guid + (uint32_t)i, hal[i]);
    gm_zero(result, D3DFDR_SIZE);
    wr32(result + D3DFDR_OFF_dwSize, D3DFDR_SIZE);
    hr = call_method(d3d, D3D_FindDevice, {search, result});
    CHECK_EQ(hr, D3D_OK_);
    // The game checks the returned GUID is non-zero and that the triangle
    // caps advertise the blend and alpha-compare bits it needs.
    CHECK(rd32(result + D3DFDR_OFF_guid) != 0);
    uint32_t tri = result + D3DFDR_OFF_ddHwDesc + D3DDD_OFF_dpcTriCaps;
    CHECK((rd32(tri + D3DPC_OFF_dwDestBlendCaps) & 1u) != 0);
    CHECK((rd32(tri + D3DPC_OFF_dwSrcBlendCaps) & 0x1000u) != 0);
    CHECK((rd32(tri + D3DPC_OFF_dwAlphaCmpCaps) & 2u) != 0);

    // A back buffer marked as a 3D device target.
    uint32_t desc = sc(0x400);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
    wr32(desc + DDSD_OFF_dwWidth, 640);
    wr32(desc + DDSD_OFF_dwHeight, 480);
    call_method(dd, DD_CreateSurface, {desc, sc(8), 0});
    uint32_t target = rd32(sc(8));
    CHECK(target != 0);

    uint32_t guid = sc(0x500);
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, hal[i]);
    hr = call_method(d3d, D3D_CreateDevice, {guid, target, sc(12)});
    CHECK_EQ(hr, D3D_OK_);
    uint32_t dev = rd32(sc(12));
    CHECK(dev != 0);

    // A viewport, added to the device and made current.
    hr = call_method(d3d, D3D_CreateViewport, {sc(16), 0});
    CHECK_EQ(hr, D3D_OK_);
    uint32_t vp = rd32(sc(16));
    CHECK(vp != 0);
    hr = call_method(dev, DEV_AddViewport, {vp});
    CHECK_EQ(hr, D3D_OK_);

    uint32_t vpdata = sc(0x800);
    gm_zero(vpdata, D3DVIEWPORT2_SIZE);
    wr32(vpdata + D3DVP_OFF_dwSize, D3DVIEWPORT2_SIZE);
    wr32(vpdata + D3DVP_OFF_dwX, 0);
    wr32(vpdata + D3DVP_OFF_dwY, 0);
    wr32(vpdata + D3DVP_OFF_dwWidth, 640);
    wr32(vpdata + D3DVP_OFF_dwHeight, 480);
    wrf32(vpdata + D3DVP_OFF_dvMinZ, 0.0f);
    wrf32(vpdata + D3DVP_OFF_dvMaxZ, 1.0f);
    hr = call_method(vp, VP_SetViewport2, {vpdata});
    CHECK_EQ(hr, D3D_OK_);
    hr = call_method(dev, DEV_SetCurrentViewport, {vp});
    CHECK_EQ(hr, D3D_OK_);

    // A texture: a 16x16 surface, QueryInterface'd to IDirect3DTexture2.
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 16);
    wr32(desc + DDSD_OFF_dwHeight, 16);
    call_method(dd, DD_CreateSurface, {desc, sc(20), 0});
    uint32_t texsurf = rd32(sc(20));
    CHECK(texsurf != 0);

    // IID_IDirect3DTexture2 = 93281502-8CF8-11D0-89AB-00A0C9054129. The four
    // 9328150x IIDs are not in interface-declaration order, so this is the
    // SDK's literal rather than a value derived from the neighbouring ones.
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    hr = call_method(texsurf, S_QueryInterface, {iid, sc(24)});
    CHECK_EQ(hr, S_OK);
    uint32_t tex = rd32(sc(24));
    CHECK(tex != 0);

    hr = call_method(tex, TEX_GetHandle, {dev, sc(28)});
    CHECK_EQ(hr, D3D_OK_);
    uint32_t thandle = rd32(sc(28));
    CHECK(thandle != 0);
    CHECK_EQ(g_textures.size(), 1);

    // Bind the texture and set a render state the host must see.
    hr = call_method(dev, DEV_SetRenderState, {D3DRENDERSTATE_TEXTUREHANDLE, thandle});
    CHECK_EQ(hr, D3D_OK_);
    hr = call_method(dev, DEV_SetRenderState, {D3DRENDERSTATE_CULLMODE, 2});
    CHECK_EQ(hr, D3D_OK_);

    // A projection matrix, so the host sees it as set.
    uint32_t mat = sc(0xc00);
    for (int i = 0; i < 16; ++i)
        wrf32(mat + 4u * (uint32_t)i, i == 0 || i == 5 || i == 10 || i == 15 ? 1.0f : 0.0f);
    hr = call_method(dev, DEV_SetTransform, {D3DTRANSFORMSTATE_PROJECTION, mat});
    CHECK_EQ(hr, D3D_OK_);

    hr = call_method(dev, DEV_BeginScene, {});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_begin_scene, 1);
    // A second BeginScene is an error, as D3D specifies.
    hr = call_method(dev, DEV_BeginScene, {});
    CHECK_EQ(hr, D3DERR_SCENEINSCENE);

    // Four TL vertices and six indices: two triangles.
    uint32_t verts = sc(0x800);
    for (uint32_t v = 0; v < 4; ++v) {
        uint32_t b = verts + v * 32;
        wrf32(b + 0, (float)(v & 1 ? 100 : 0));
        wrf32(b + 4, (float)(v & 2 ? 100 : 0));
        wrf32(b + 8, 0.5f);
        wrf32(b + 12, 1.0f);
        wr32(b + 16, 0xffffffffu);
        wr32(b + 20, 0);
        wrf32(b + 24, (float)(v & 1));
        wrf32(b + 28, (float)((v >> 1) & 1));
    }
    uint32_t idx = sc(0xa00);
    const uint16_t order[6] = {0, 1, 2, 2, 1, 3};
    for (uint32_t i = 0; i < 6; ++i)
        wr16(idx + i * 2, order[i]);

    hr = call_method(dev, DEV_DrawIndexedPrimitive,
                     {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, verts, 4, idx, 6, 0});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_draws.size(), 1);
    if (!g_draws.empty()) {
        const DrawRecord &d = g_draws[0];
        CHECK_EQ(d.primitive_type, D3DPT_TRIANGLELIST);
        CHECK_EQ(d.vertex_type, D3DVT_TLVERTEX);
        CHECK_EQ(d.vertex_count, 4);
        CHECK_EQ(d.index_count, 6);
        CHECK_EQ(d.texture_handle, thandle);
        CHECK_EQ(d.cull, 2);
        CHECK_EQ(d.viewport[2], 640);
        CHECK_EQ(d.viewport[3], 480);
        CHECK(d.had_projection);
        CHECK_EQ(d.vertices.size(), 4 * 32);
        CHECK_EQ(d.indices.size(), 6);
        bool idx_ok = true;
        for (uint32_t i = 0; i < 6; ++i)
            if (d.indices[i] != order[i])
                idx_ok = false;
        CHECK(idx_ok);
        // The first vertex's x really is the float the guest wrote.
        float x0;
        memcpy(&x0, d.vertices.data(), 4);
        CHECK(x0 == 0.0f);
    }

    // A non-indexed draw is forwarded with no index list.
    hr = call_method(dev, DEV_DrawPrimitive, {D3DPT_TRIANGLESTRIP, D3DVT_TLVERTEX, verts, 4, 0});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_draws.size(), 2);
    CHECK_EQ(g_draws[1].index_count, 0);
    CHECK_EQ(g_draws[1].primitive_type, D3DPT_TRIANGLESTRIP);

    hr = call_method(dev, DEV_EndScene, {});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_end_scene, 1);
    hr = call_method(dev, DEV_EndScene, {});
    CHECK_EQ(hr, D3DERR_SCENENOTINSCENE);

    // GetCaps reports a hardware device with the texture limits the renderer
    // is promised.
    uint32_t caps = sc(0xc00);
    gm_zero(caps, D3DDEVICEDESC_SIZE);
    wr32(caps + D3DDD_OFF_dwSize, D3DDEVICEDESC_SIZE);
    hr = call_method(dev, DEV_GetCaps, {caps, 0});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(rd32(caps + D3DDD_OFF_dcmColorModel), D3DCOLOR_RGB);
    CHECK_EQ(rd32(caps + D3DDD_OFF_dwMaxTextureWidth), 2048);
}

// DirectSound: create a buffer, fill it through Lock, play it, and check the
// PCM and format reached the host.
static void test_dsound() {
    cpu_reset();
    g_plays.clear();
    g_stops.clear();

    uint32_t create = tramp("DSOUND.dll", "ord1");
    CHECK(create != 0);
    uint32_t hr = call_shim(create, {0, sc(0), 0});
    CHECK_EQ(hr, DS_OK);
    uint32_t ds = rd32(sc(0));
    CHECK(ds != 0);

    hr = call_method(ds, DS_SetCooperativeLevel, {0x20004, 3});
    CHECK_EQ(hr, DS_OK);

    // A 22050 Hz, mono, 16-bit secondary buffer of 1024 bytes.
    uint32_t wfx = sc(0x100);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 1);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 22050);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 44100);
    wr16(wfx + WFX_OFF_nBlockAlign, 2);
    wr16(wfx + WFX_OFF_wBitsPerSample, 16);
    wr16(wfx + WFX_OFF_cbSize, 0);

    uint32_t bd = sc(0x200);
    gm_zero(bd, DSBUFFERDESC_SIZE);
    wr32(bd + DSBD_OFF_dwSize, DSBUFFERDESC_SIZE);
    wr32(bd + DSBD_OFF_dwFlags, DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC);
    wr32(bd + DSBD_OFF_dwBufferBytes, 1024);
    wr32(bd + DSBD_OFF_lpwfxFormat, wfx);
    hr = call_method(ds, DS_CreateSoundBuffer, {bd, sc(4), 0});
    CHECK_EQ(hr, DS_OK);
    uint32_t buf = rd32(sc(4));
    CHECK(buf != 0);

    // A buffer with no format is refused.
    wr32(bd + DSBD_OFF_lpwfxFormat, 0);
    hr = call_method(ds, DS_CreateSoundBuffer, {bd, sc(8), 0});
    CHECK_EQ(hr, DSERR_INVALIDPARAM);
    wr32(bd + DSBD_OFF_lpwfxFormat, wfx);

    // Lock the whole buffer and write a ramp.
    hr = call_method(buf, B_Lock, {0, 1024, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0});
    CHECK_EQ(hr, DS_OK);
    uint32_t p1 = rd32(sc(0x300));
    uint32_t n1 = rd32(sc(0x304));
    CHECK(p1 != 0);
    CHECK_EQ(n1, 1024);
    CHECK_EQ(rd32(sc(0x30c)), 0); // no wrap
    for (uint32_t i = 0; i < 1024; ++i)
        wr8(p1 + i, (uint8_t)(i & 0xff));
    hr = call_method(buf, B_Unlock, {p1, 1024, 0, 0});
    CHECK_EQ(hr, DS_OK);

    hr = call_method(buf, B_SetVolume, {(uint32_t)(int32_t)-2000});
    CHECK_EQ(hr, DS_OK);

    hr = call_method(buf, B_Play, {0, 0, 0});
    CHECK_EQ(hr, DS_OK);
    CHECK_EQ(g_plays.size(), 1);
    if (!g_plays.empty()) {
        const PlayRecord &r = g_plays[0];
        CHECK_EQ(r.rate, 22050);
        CHECK_EQ(r.channels, 1);
        CHECK_EQ(r.bits, 16);
        CHECK_EQ(r.bytes, 1024);
        CHECK_EQ(r.loop, 0);
        CHECK_EQ(r.volume, (uint64_t)(int64_t)-2000);
        bool pcm_ok = true;
        for (uint32_t i = 0; i < 1024; ++i)
            if (r.pcm[i] != (uint8_t)(i & 0xff)) {
                pcm_ok = false;
                break;
            }
        CHECK(pcm_ok);
    }

    uint32_t st = sc(0x400);
    hr = call_method(buf, B_GetStatus, {st});
    CHECK_EQ(hr, DS_OK);
    CHECK((rd32(st) & DSBSTATUS_PLAYING) != 0);

    hr = call_method(buf, B_Stop, {});
    CHECK_EQ(hr, DS_OK);
    CHECK_EQ(g_stops.size(), 1);

    // A lock that runs past the end wraps into the second region.
    hr = call_method(buf, B_Lock, {1000, 100, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0});
    CHECK_EQ(hr, DS_OK);
    CHECK_EQ(rd32(sc(0x304)), 24);
    CHECK_EQ(rd32(sc(0x30c)), 76);
    CHECK(rd32(sc(0x308)) != 0);
    call_method(buf, B_Unlock, {0, 0, 0, 0});

    // The 3D buffer interface is the same object seen another way.
    uint32_t iid = sc(0x40);
    uint8_t b3d[16] = {0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                       0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, b3d[i]);
    hr = call_method(buf, 0 /* QueryInterface */, {iid, sc(0x60)});
    CHECK_EQ(hr, S_OK);
    uint32_t b3 = rd32(sc(0x60));
    CHECK(b3 != 0);
    CHECK(b3 != buf);
    // SetPosition then GetPosition round-trips the floats.
    float pos[3] = {1.5f, -2.5f, 3.25f};
    uint32_t a0, a1, a2;
    memcpy(&a0, &pos[0], 4);
    memcpy(&a1, &pos[1], 4);
    memcpy(&a2, &pos[2], 4);
    hr = call_method(b3, 19 /* SetPosition */, {a0, a1, a2, 1});
    CHECK_EQ(hr, DS_OK);
    hr = call_method(b3, 10 /* GetPosition */, {sc(0x500)});
    CHECK_EQ(hr, DS_OK);
    CHECK(rdf32(sc(0x500)) == 1.5f);
    CHECK(rdf32(sc(0x504)) == -2.5f);
    CHECK(rdf32(sc(0x508)) == 3.25f);
}

// DirectInput: the keyboard reports the host's key state, and the mouse
// delivers buffered events built by diffing successive host states.
static void test_dinput() {
    cpu_reset();
    memset(&g_input, 0, sizeof g_input);

    uint32_t create = tramp("DINPUT.dll", "DirectInputCreateA");
    CHECK(create != 0);
    uint32_t hr = call_shim(create, {0x400000, 0x0500, sc(0), 0});
    CHECK_EQ(hr, DI_OK);
    uint32_t di = rd32(sc(0));
    CHECK(di != 0);

    // The keyboard.
    uint32_t guid = sc(0x40);
    uint8_t kbd[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                       0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, kbd[i]);
    hr = call_method(di, DI_CreateDevice, {guid, sc(4), 0});
    CHECK_EQ(hr, DI_OK);
    uint32_t kb = rd32(sc(4));
    CHECK(kb != 0);

    // Acquire before a data format is set must fail.
    hr = call_method(kb, DID_Acquire, {});
    CHECK_EQ(hr, DIERR_NOTINITIALIZED);

    uint32_t df = sc(0x100);
    gm_zero(df, 24);
    wr32(df + 0, 24);   // dwSize
    wr32(df + 4, 16);   // dwObjSize
    wr32(df + 12, 256); // dwDataSize
    hr = call_method(kb, DID_SetDataFormat, {df});
    CHECK_EQ(hr, DI_OK);
    hr = call_method(kb, DID_Acquire, {});
    CHECK_EQ(hr, DI_OK);

    // Two keys down at the host.
    g_input.keys[0x1e] = 0x80; // DIK_A
    g_input.keys[0x11] = 0x80; // DIK_W
    uint32_t state = sc(0x200);
    gm_zero(state, 256);
    hr = call_method(kb, DID_GetDeviceState, {256, state});
    CHECK_EQ(hr, DI_OK);
    CHECK_EQ(rd8(state + 0x1e), 0x80);
    CHECK_EQ(rd8(state + 0x11), 0x80);
    CHECK_EQ(rd8(state + 0x20), 0);

    // The mouse, with a buffer, reporting relative motion.
    uint8_t mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                         0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, mouse[i]);
    hr = call_method(di, DI_CreateDevice, {guid, sc(8), 0});
    CHECK_EQ(hr, DI_OK);
    uint32_t ms = rd32(sc(8));
    CHECK(ms != 0);

    wr32(df + 12, DIMOUSESTATE_SIZE);
    call_method(ms, DID_SetDataFormat, {df});

    uint32_t prop = sc(0x300);
    gm_zero(prop, 20);
    wr32(prop + DIPH_OFF_dwSize, 20);
    wr32(prop + DIPH_OFF_dwHeaderSize, 16);
    wr32(prop + DIPROPDWORD_OFF_dwData, 32);
    hr = call_method(ms, DID_SetProperty, {1 /* DIPROP_BUFFERSIZE */, prop});
    CHECK_EQ(hr, DI_OK);
    hr = call_method(ms, DID_Acquire, {});
    CHECK_EQ(hr, DI_OK);

    g_input.mouse_dx = 7;
    g_input.mouse_dy = -3;
    g_input.mouse_buttons[0] = 0x80;

    uint32_t mstate = sc(0x400);
    gm_zero(mstate, DIMOUSESTATE_SIZE);
    hr = call_method(ms, DID_GetDeviceState, {DIMOUSESTATE_SIZE, mstate});
    CHECK_EQ(hr, DI_OK);
    CHECK_EQ((int32_t)rd32(mstate + DIMS_OFF_lX), 7);
    CHECK_EQ((int32_t)rd32(mstate + DIMS_OFF_lY), (uint64_t)(int64_t)-3);
    CHECK_EQ(rd8(mstate + DIMS_OFF_rgbButtons + 0), 0x80);

    // The same motion arrived as buffered events. GetDeviceState polled once,
    // so those events are already queued.
    uint32_t inout = sc(0x500);
    wr32(inout, 16);
    uint32_t data = sc(0x800);
    hr = call_method(ms, DID_GetDeviceData, {DIDEVICEOBJECTDATA_SIZE, data, inout, 0});
    CHECK_EQ(hr, DI_OK);
    uint32_t n = rd32(inout);
    CHECK_EQ(n, 3); // x, y and the button
    if (n == 3) {
        CHECK_EQ(rd32(data + 0 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwOfs), 0);
        CHECK_EQ((int32_t)rd32(data + 0 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwData), 7);
        CHECK_EQ(rd32(data + 1 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwOfs), 4);
        // The negative delta survives the round trip through the packed event.
        CHECK_EQ((int32_t)rd32(data + 1 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwData),
                 (uint64_t)(int64_t)-3);
        CHECK_EQ(rd32(data + 2 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwOfs), 12);
        CHECK_EQ(rd32(data + 2 * DIDEVICEOBJECTDATA_SIZE + DIDOD_OFF_dwData), 0x80);
    }
    // The events were consumed.
    wr32(inout, 16);
    call_method(ms, DID_GetDeviceData, {DIDEVICEOBJECTDATA_SIZE, data, inout, 0});
    CHECK_EQ(rd32(inout), 0);

    // A joystick GUID is refused rather than half-supported.
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, (uint8_t)(0x40 + i));
    hr = call_method(di, DI_CreateDevice, {guid, sc(12), 0});
    CHECK_EQ(hr, DIERR_DEVICENOTREG);
    CHECK_EQ(rd32(sc(12)), 0);
}

// QMixer: a session, a channel, and a wave supplied the way the game supplies
// one, as raw PCM plus an explicit WAVEFORMATEX in a five-dword record. There
// is no RIFF container on this path.
//
// Record layout, from the disassembly (see dxtypes.h):
//   +0x00 LPWAVEFORMATEX   +0x04 sample data   +0x08 byte count   +0x0c,+0x10 zero
static void test_qmixer() {
    cpu_reset();
    g_plays.clear();

    uint32_t init = tramp("QMIXER.dll", "QSWaveMixInitEx");
    CHECK(init != 0);
    uint32_t initdata = sc(0x100);
    gm_zero(initdata, 0x40);
    wr32(initdata + 0, 0x40); // dwSize, as the game passes
    wr32(initdata + 4, 0x12);
    wr32(initdata + 8, 0x5622); // 22050, as the game passes
    uint32_t hmix = call_shim(init, {initdata});
    CHECK(hmix != 0);

    uint32_t activate = tramp("QMIXER.dll", "QSWaveMixActivate");
    CHECK_EQ(call_shim(activate, {hmix, 1}), 0u);

    uint32_t open_ch = tramp("QMIXER.dll", "QSWaveMixOpenChannel");
    CHECK_EQ(call_shim(open_ch, {hmix, 3, 2}), 0u);

    // A WAVEFORMATEX exactly as the game's own slot 20 (0x5711c0) writes one:
    // PCM, 1 channel, 22050 Hz, 8 bit, nBlockAlign = (bits*ch+7)/8.
    uint32_t wfx = sc(0x200);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + 0x00, 1);
    wr16(wfx + 0x02, 1);
    wr32(wfx + 0x04, 22050);
    wr32(wfx + 0x08, 22050);
    wr16(wfx + 0x0c, 1);
    wr16(wfx + 0x0e, 8);
    wr16(wfx + 0x10, 0);

    const uint32_t data_bytes = 64;
    uint32_t pcm = sc(0x400);
    for (uint32_t i = 0; i < data_bytes; ++i)
        wr8(pcm + i, (uint8_t)(i * 3));

    uint32_t owd = sc(0x300);
    gm_zero(owd, 20);
    wr32(owd + 0x00, wfx);
    wr32(owd + 0x04, pcm);
    wr32(owd + 0x08, data_bytes);

    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    uint32_t hwave = call_shim(open_wave, {hmix, owd, 8});
    CHECK(hwave != 0);

    // QMixer's volume is a linear amplitude on 0..32767, not decibels: its own
    // parameter handler multiplies the value by 1/32767 and uses that as the
    // gain. Half scale is therefore about -6 dB.
    uint32_t set_vol = tramp("QMIXER.dll", "QSWaveMixSetVolume");
    CHECK_EQ(call_shim(set_vol, {hmix, 3, 0, 16384}), 0u);

    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");
    CHECK_EQ(call_shim(play, {hmix, 3, 0x20, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1);
    if (!g_plays.empty()) {
        const PlayRecord &r = g_plays[0];
        CHECK_EQ(r.rate, 22050);
        CHECK_EQ(r.channels, 1);
        CHECK_EQ(r.bits, 8);
        CHECK_EQ(r.bytes, data_bytes);
        CHECK_EQ(r.volume, -602); // 2000 * log10(16384/32767)
        bool ok = true;
        for (uint32_t i = 0; i < data_bytes; ++i)
            if (r.pcm[i] != (uint8_t)(i * 3)) {
                ok = false;
                break;
            }
        CHECK(ok);
    }

    // Pump is called every frame and must stay quiet and cheap.
    uint32_t pump = tramp("QMIXER.dll", "QSWaveMixPump");
    CHECK_EQ(call_shim(pump, {}), 0u);

    uint32_t stop = tramp("QMIXER.dll", "QSWaveMixStopChannel");
    CHECK_EQ(call_shim(stop, {hmix, 3, 0}), 0u);

    uint32_t freew = tramp("QMIXER.dll", "QSWaveMixFreeWave");
    CHECK_EQ(call_shim(freew, {hmix, hwave}), 0u);

    uint32_t close = tramp("QMIXER.dll", "QSWaveMixCloseSession");
    CHECK_EQ(call_shim(close, {hmix}), 0u);
}

// weanetr: both network bring-up paths report unavailable so the game runs
// single-player, and GetCurrentMs is a real clock.
static void test_weanetr() {
    cpu_reset();
    uint32_t startup = tramp("weanetr.dll", "?StartupNetwork@MLDPlay@@QAEHP6GXKPAXKK0@Z@Z");
    CHECK(startup != 0);
    // A thiscall method: `this` is in ECX, not on the stack.
    g_cpu.r[R_ECX] = 0x01000000;
    CHECK_EQ(call_shim(startup, {0x401000}), 0u);

    // The other bring-up path. Its caller at 00413db0 compares the result
    // against 0x20, not against zero, so zero would read as "MLDPlay is up and
    // we were not lobbied" and send the game into the multiplayer lobby.
    uint32_t lobbied = tramp(
        "weanetr.dll",
        "?AreWeLobbied@MLDPlay@@QAEKP6GXKPAXKK0@ZPAU_GUID@@PAUMLDPLAY_LOBBYINFO@@PAKPAG5KK@Z");
    CHECK(lobbied != 0);
    g_cpu.r[R_ECX] = 0x01000000;
    CHECK_EQ(call_shim(lobbied, {0x401000, 0, 0, 0, 0, 0, 4, 0}), 0x20u);

    uint32_t ms = tramp("weanetr.dll", "?GetCurrentMs@MLDPlay@@QAEKXZ");
    CHECK(ms != 0);
    uint32_t t = call_shim(ms, {});
    CHECK_EQ(t, host_millis());

    uint32_t enum_sessions = tramp(
        "weanetr.dll", "?EnumerateSessions@MLDPlay@@QAEHKP6GXPAUMLDPLAY_SESSIONDESC@@PAX@ZK1@Z");
    CHECK(enum_sessions != 0);
    CHECK_EQ(call_shim(enum_sessions, {0, 0, 0, 0}), 0u);

    uint32_t shutdown = tramp("weanetr.dll", "?ShutdownNetwork@MLDPlay@@QAEHXZ");
    CHECK_EQ(call_shim(shutdown, {}), 0u);
}

// Every vtable slot must hold a distinct, allocated trampoline: a duplicate
// would mean two methods sharing one implementation by accident.
static void test_vtable_integrity() {
    struct {
        ComIface iface;
        uint32_t slots;
        const char *name;
    } ifaces[] = {
        {IF_DIRECTDRAW, 23, "IDirectDraw"},
        {IF_DIRECTDRAW2, 24, "IDirectDraw2"},
        {IF_DIRECTDRAW4, 28, "IDirectDraw4"},
        {IF_DDSURFACE, 36, "IDirectDrawSurface"},
        {IF_DDSURFACE2, 39, "IDirectDrawSurface2"},
        {IF_DDSURFACE3, 40, "IDirectDrawSurface3"},
        {IF_DDSURFACE4, 45, "IDirectDrawSurface4"},
        {IF_DDPALETTE, 7, "IDirectDrawPalette"},
        {IF_DDCLIPPER, 9, "IDirectDrawClipper"},
        {IF_D3D, 9, "IDirect3D"},
        {IF_D3D2, 9, "IDirect3D2"},
        {IF_D3DDEVICE2, 33, "IDirect3DDevice2"},
        {IF_D3DVIEWPORT2, 18, "IDirect3DViewport2"},
        {IF_D3DMATERIAL2, 6, "IDirect3DMaterial2"},
        {IF_D3DLIGHT, 6, "IDirect3DLight"},
        {IF_D3DTEXTURE2, 6, "IDirect3DTexture2"},
        {IF_DSOUND, 11, "IDirectSound"},
        {IF_DSBUFFER, 21, "IDirectSoundBuffer"},
        {IF_DS3DBUFFER, 21, "IDirectSound3DBuffer"},
        {IF_DS3DLISTENER, 18, "IDirectSound3DListener"},
        {IF_DSNOTIFY, 4, "IDirectSoundNotify"},
        {IF_DINPUT, 8, "IDirectInputA"},
        {IF_DINPUTDEVICE, 27, "IDirectInputDeviceA"},
    };
    for (auto &e : ifaces) {
        uint32_t vt = com_vtable_of(e.iface);
        if (!vt) {
            ++g_checks;
            ++g_failures;
            fprintf(stderr, "FAIL: %s has no vtable\n", e.name);
            continue;
        }
        std::vector<uint32_t> seen;
        for (uint32_t i = 0; i < e.slots; ++i) {
            uint32_t t = rd32(vt + i * 4);
            ++g_checks;
            if (!imports_is_trampoline(t)) {
                ++g_failures;
                fprintf(stderr, "FAIL: %s slot %u holds %08x, not a trampoline\n", e.name, i, t);
                continue;
            }
            for (uint32_t s : seen) {
                if (s == t) {
                    ++g_failures;
                    fprintf(stderr, "FAIL: %s slot %u repeats an earlier slot\n", e.name, i);
                    break;
                }
            }
            seen.push_back(t);
        }
        // The slot immediately past the interface must belong to a different
        // interface or be unallocated: catching an off-by-one in the count.
        ++g_checks;
        if (rd32(vt + e.slots * 4) != 0 && imports_is_trampoline(rd32(vt + e.slots * 4))) {
            // Reading past the allocation is not itself an error, since the
            // heap may hold another vtable there; only report a slot that is
            // one of this interface's own.
            for (uint32_t s : seen) {
                if (s == rd32(vt + e.slots * 4)) {
                    ++g_failures;
                    fprintf(stderr, "FAIL: %s appears to have more than %u slots\n", e.name,
                            e.slots);
                    break;
                }
            }
        }
    }
}

// Releasing every interface really destroys the object, and the guest memory
// the surfaces held goes back to the heap.
static void test_refcounts() {
    cpu_reset();
    uint32_t before_live = com_live_count();
    HeapStats hs_before = heap_stats();

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN);
    wr32(desc + DDSD_OFF_dwWidth, 256);
    wr32(desc + DDSD_OFF_dwHeight, 256);
    call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    uint32_t s = rd32(sc(4));
    CHECK(s != 0);

    // AddRef then two Releases: the object survives the first.
    uint32_t r = call_method(s, 1 /* AddRef */, {});
    CHECK_EQ(r, 2u);
    r = call_method(s, S_Release, {});
    CHECK_EQ(r, 1u);
    CHECK(com_this(s) != nullptr);
    r = call_method(s, S_Release, {});
    CHECK_EQ(r, 0u);
    CHECK(com_this(s) == nullptr);

    call_method(dd, DD_Release, {});
    CHECK_EQ(com_live_count(), before_live);

    HeapStats hs_after = heap_stats();
    // The surface's pixels and the object's views went back to the heap. The
    // vtables and the scratch buffers stay, so this compares live blocks
    // against the count before this test rather than against zero.
    CHECK_EQ(hs_after.used_blocks, hs_before.used_blocks);
}

// ---------------------------------------------------------------------------
// Fix-round tests. Each one covers a defect the review found; they use the
// independently derived SDK_ sizes above, so an implementation constant that
// drifts back to a wrong value fails here.
// ---------------------------------------------------------------------------

// SetRenderTarget on the surface the device is already rendering into must not
// destroy it. Releasing the outgoing target before retaining the incoming one
// is fine while they differ and fatal when they are the same object and the
// device holds its last reference: the release destroys it, and the addref,
// the flush and the host's new pointer all then work on a dead object.
static void test_setrendertarget_same_surface() {
    cpu_reset();
    uint32_t before_live = com_live_count();

    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    if (!dev)
        return;

    // The surface the device was created with, as the guest would fetch it.
    CHECK_EQ(call_method(dev, 16 /* GetRenderTarget */, {sc(0x20)}), 0u);
    uint32_t target = rd32(sc(0x20));
    CHECK(target != 0);
    // Three references now: the one CreateSurface handed the guest, the one
    // CreateDevice took, and the one GetRenderTarget just took. Give back
    // GetRenderTarget's and the guest's own, so the device holds the last
    // reference - which is the case that breaks.
    CHECK_EQ(call_method(target, S_Release, {}), 2u);
    CHECK_EQ(call_method(target, S_Release, {}), 1u);
    ComObj *obj = com_this(target);
    CHECK(obj != nullptr);
    if (!obj)
        return;
    CHECK_EQ(obj->refs, 1);

    // Set the same surface again. It has to survive, with the count unchanged:
    // a device that already holds a reference for a target does not need a
    // second one for the same surface.
    CHECK_EQ(call_method(dev, DEV_SetRenderTarget, {target, 0}), 0u);
    obj = com_this(target);
    CHECK(obj != nullptr);
    if (!obj)
        return;
    CHECK_EQ(obj->refs, 1);

    // And it is still usable afterwards, which a destroyed object would not be.
    CHECK_EQ(call_method(dev, 16 /* GetRenderTarget */, {sc(0x24)}), 0u);
    CHECK_EQ(rd32(sc(0x24)), target);
    call_method(target, S_Release, {});

    // Releasing the device releases the target with it.
    call_method(dev, 2 /* Release */, {});
    CHECK(com_this(target) == nullptr);
    CHECK(com_live_count() >= before_live);
}

// The record sizes and flag bits the shims use must match the SDK layouts.
static void test_sdk_abi() {
    CHECK_EQ(DDSD_SIZE, SDK_DDSURFACEDESC);
    CHECK_EQ(DDSD2_SIZE, SDK_DDSURFACEDESC2);
    CHECK_EQ(DDPF_SIZE, SDK_DDPIXELFORMAT);
    CHECK_EQ(DDBLTFX_SIZE, SDK_DDBLTFX);
    CHECK_EQ(DDDEVID_SIZE, SDK_DDDEVICEIDENTIFIER);
    CHECK_EQ(D3DPRIMCAPS_SIZE, SDK_D3DPRIMCAPS);
    CHECK_EQ(D3DDEVICEDESC_SIZE, SDK_D3DDEVICEDESC);
    CHECK_EQ(D3DDD_OFF_dpcTriCaps, SDK_D3DDD_dpcTriCaps_OFF);
    CHECK_EQ(D3DFDS_SIZE, SDK_D3DFINDDEVICESEARCH);
    CHECK_EQ(D3DFDR_SIZE, SDK_D3DFINDDEVICERESULT);
    CHECK_EQ(D3DVIEWPORT_SIZE, SDK_D3DVIEWPORT);
    CHECK_EQ(D3DVIEWPORT2_SIZE, SDK_D3DVIEWPORT2);
    CHECK_EQ(D3DCLIPSTATUS_SIZE, SDK_D3DCLIPSTATUS);
    CHECK_EQ(DSBUFFERDESC_SIZE, SDK_DSBUFFERDESC);
    CHECK_EQ(WFX_SIZE, SDK_WAVEFORMATEX);
    CHECK_EQ(DIDEVICEOBJECTDATA_SIZE, SDK_DIDEVICEOBJECTDATA);
    CHECK_EQ(DIPROPDWORD_SIZE, SDK_DIPROPDWORD);
    CHECK_EQ(DIMOUSESTATE_SIZE, SDK_DIMOUSESTATE);
    CHECK_EQ(D3DFDS_COLORMODEL, SDK_D3DFDS_COLORMODEL);
    CHECK_EQ(D3DFDS_GUID, SDK_D3DFDS_GUID);
    CHECK_EQ(D3DFDS_HARDWARE, SDK_D3DFDS_HARDWARE);
}

// Reproduces init_d3d's search byte for byte: dwSize 0x5c, dwFlags 2, the
// rest of the record zero except the HAL GUID at offset 0x10. With the flag
// bits wrong this matched a software device instead.
static void test_find_device_literal() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));

    uint32_t iid = sc(0x40);
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    uint32_t d3d = rd32(sc(0x60));
    CHECK(d3d != 0);

    const uint8_t hal[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                             0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};
    uint32_t search = sc(0x100), result = sc(0x200);
    // The game zeroes 0x17 dwords, then writes dwSize and dwFlags and the
    // GUID; bHardware and dcmColorModel stay zero.
    gm_zero(search, SDK_D3DFINDDEVICESEARCH);
    wr32(search + 0x00, 0x5c);
    wr32(search + 0x04, 2);
    for (int i = 0; i < 16; ++i)
        wr8(search + 0x10 + (uint32_t)i, hal[i]);
    gm_zero(result, SDK_D3DFINDDEVICERESULT);
    wr32(result + 0x00, 0x20c);

    uint32_t hr = call_method(d3d, D3D_FindDevice, {search, result});
    CHECK_EQ(hr, D3D_OK_);
    // It must be the HAL device that came back, not a software one.
    bool guid_is_hal = true;
    for (int i = 0; i < 16; ++i)
        if (rd8(result + 0x04 + (uint32_t)i) != hal[i])
            guid_is_hal = false;
    CHECK(guid_is_hal);
    // And the suitability gate init_d3d applies to dpcTriCaps must pass.
    uint32_t tri = result + 0x14 + SDK_D3DDD_dpcTriCaps_OFF;
    CHECK((rd32(tri + 0x14) & 1u) != 0);      // dwDestBlendCaps
    CHECK((rd32(tri + 0x10) & 0x1000u) != 0); // dwSrcBlendCaps
    CHECK((rd32(tri + 0x18) & 2u) != 0);      // dwAlphaCmpCaps
    CHECK(rd32(result + 0x10) != 0);          // last GUID dword non-zero

    // A hardware-only search that contradicts a software GUID finds nothing.
    gm_zero(search, SDK_D3DFINDDEVICESEARCH);
    wr32(search + 0x00, 0x5c);
    wr32(search + 0x04, SDK_D3DFDS_GUID | SDK_D3DFDS_HARDWARE);
    const uint8_t rgb[16] = {0x60, 0x5C, 0x66, 0xA4, 0x73, 0x26, 0xCF, 0x11,
                             0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
    for (int i = 0; i < 16; ++i)
        wr8(search + 0x10 + (uint32_t)i, rgb[i]);
    wr32(search + 0x08, 1); // bHardware
    gm_zero(result, SDK_D3DFINDDEVICERESULT);
    wr32(result, 0x20c);
    CHECK_EQ(call_method(d3d, D3D_FindDevice, {search, result}), DDERR_NOTFOUND);
}

// The game's viewport record is 11 dwords and reaches SetViewport2 at +0x44.
static void test_viewport_record() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    uint32_t iid = sc(0x40);
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    call_method(dd, DD_QueryInterface, {iid, sc(0x60)});
    uint32_t d3d = rd32(sc(0x60));
    call_method(d3d, D3D_CreateViewport, {sc(16), 0});
    uint32_t vp = rd32(sc(16));
    CHECK(vp != 0);

    // A canary immediately after an 11-dword record must survive.
    uint32_t v = sc(0x800);
    gm_zero(v, SDK_D3DVIEWPORT2);
    wr32(v + SDK_D3DVIEWPORT2, 0xA5A5A5A5u);
    wr32(v + 0x00, SDK_D3DVIEWPORT2);
    wr32(v + 0x04, 16);
    wr32(v + 0x08, 24);
    wr32(v + 0x0c, 320);
    wr32(v + 0x10, 200);
    wrf32(v + 0x24, 0.0f);
    wrf32(v + 0x28, 1.0f);
    uint32_t hr = call_method(vp, VP_SetViewport2, {v});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(rd32(v + SDK_D3DVIEWPORT2), 0xA5A5A5A5u);

    // And it round-trips through GetViewport2.
    uint32_t g = sc(0xc00);
    gm_zero(g, SDK_D3DVIEWPORT2);
    wr32(g + 0x00, SDK_D3DVIEWPORT2);
    hr = call_method(vp, 16 /* GetViewport2 */, {g});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(rd32(g + 0x04), 16);
    CHECK_EQ(rd32(g + 0x0c), 320);
    CHECK_EQ(rd32(g + 0x10), 200);
}

// GetDeviceData must accept the 16-byte DirectInput 5 record the game uses,
// and must not write past it.
static void test_device_data_stride16() {
    cpu_reset();
    memset(&g_input, 0, sizeof g_input);
    uint32_t create = tramp("DINPUT.dll", "DirectInputCreateA");
    call_shim(create, {0x400000, 0x0500, sc(0), 0});
    uint32_t di = rd32(sc(0));

    uint32_t guid = sc(0x40);
    uint8_t mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                         0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 16; ++i)
        wr8(guid + (uint32_t)i, mouse[i]);
    call_method(di, DI_CreateDevice, {guid, sc(8), 0});
    uint32_t ms = rd32(sc(8));
    CHECK(ms != 0);

    uint32_t df = sc(0x100);
    gm_zero(df, 24);
    wr32(df + 0, 24);
    wr32(df + 4, 16);
    wr32(df + 12, SDK_DIMOUSESTATE);
    call_method(ms, DID_SetDataFormat, {df});

    uint32_t prop = sc(0x300);
    gm_zero(prop, SDK_DIPROPDWORD);
    wr32(prop + 0, SDK_DIPROPDWORD);
    wr32(prop + 4, 16);
    wr32(prop + 16, 32);
    CHECK_EQ(call_method(ms, DID_SetProperty, {1, prop}), DI_OK);
    CHECK_EQ(call_method(ms, DID_Acquire, {}), DI_OK);

    g_input.mouse_dx = 5;
    g_input.mouse_buttons[0] = 0x80;

    // The game's own call shape: 10 events into a 160-byte buffer at a
    // 16-byte stride, with a canary just past it.
    uint32_t inout = sc(0x500);
    wr32(inout, 10);
    uint32_t data = sc(0x800);
    gm_zero(data, 10 * SDK_DIDEVICEOBJECTDATA);
    wr32(data + 10 * SDK_DIDEVICEOBJECTDATA, 0x5A5A5A5Au);
    uint32_t hr = call_method(ms, DID_GetDeviceData, {SDK_DIDEVICEOBJECTDATA, data, inout, 0});
    CHECK_EQ(hr, DI_OK);
    uint32_t n = rd32(inout);
    CHECK_EQ(n, 2);                                           // the x delta and the button
    CHECK_EQ(rd32(data + 0 * SDK_DIDEVICEOBJECTDATA + 0), 0); // dwOfs lX
    CHECK_EQ((int32_t)rd32(data + 0 * SDK_DIDEVICEOBJECTDATA + 4), 5);
    CHECK_EQ(rd32(data + 1 * SDK_DIDEVICEOBJECTDATA + 0), 12); // button 0
    CHECK_EQ(rd32(data + 1 * SDK_DIDEVICEOBJECTDATA + 4), 0x80);
    CHECK_EQ(rd32(data + 10 * SDK_DIDEVICEOBJECTDATA), 0x5A5A5A5Au);

    // A stride the SDK never defined is refused rather than half-honoured.
    wr32(inout, 4);
    CHECK_EQ(call_method(ms, DID_GetDeviceData, {12, data, inout, 0}), DIERR_INVALIDPARAM);
}

// GetClipStatus writes 32 bytes; the dword after must be untouched.
static void test_clipstatus_canary() {
    cpu_reset();
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    uint32_t cs = sc(0xc00);
    gm_zero(cs, SDK_D3DCLIPSTATUS);
    wr32(cs + SDK_D3DCLIPSTATUS, 0xC0FFEE00u);
    uint32_t hr = call_method(dev, DEV_GetClipStatus, {cs});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(rd32(cs + SDK_D3DCLIPSTATUS), 0xC0FFEE00u);
}

// Guest-controlled sizes that would wrap a 32-bit product are refused, and no
// draw reaches the host.
static void test_overflow_rejection() {
    cpu_reset();
    g_draws.clear();
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);

    uint32_t verts = sc(0x800);
    // 32 * 0x08000001 wraps to 32: a naive check would validate 32 bytes and
    // forward the full count.
    uint32_t hr = call_method(dev, DEV_DrawPrimitive,
                              {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, verts, 0x08000001u, 0});
    CHECK_EQ(hr, D3D_OK_); // the method succeeds; the draw is dropped
    CHECK_EQ(g_draws.size(), 0);

    // An index naming a vertex that does not exist is refused too.
    for (uint32_t v = 0; v < 4; ++v)
        gm_zero(verts + v * 32, 32);
    uint32_t idx = sc(0xa00);
    wr16(idx + 0, 0);
    wr16(idx + 2, 1);
    wr16(idx + 4, 99);
    hr = call_method(dev, DEV_DrawIndexedPrimitive,
                     {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, verts, 4, idx, 3, 0});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_draws.size(), 0);

    // A valid draw still goes through, so the guard is not simply blocking.
    wr16(idx + 4, 2);
    hr = call_method(dev, DEV_DrawIndexedPrimitive,
                     {D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, verts, 4, idx, 3, 0});
    CHECK_EQ(hr, D3D_OK_);
    CHECK_EQ(g_draws.size(), 1);

    // A surface whose dimensions would overflow the arena is refused.
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN);
    wr32(desc + DDSD_OFF_dwWidth, 0x40000000u);
    wr32(desc + DDSD_OFF_dwHeight, 0x40000000u);
    wr32(sc(4), 0xdeadbeef);
    hr = call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    CHECK(hr != DD_OK);
    CHECK_EQ(rd32(sc(4)), 0);
}

// IUnknown identity is stable, DirectDraw and Direct3D query each other, and a
// released parent stays alive while a dependent interface is held.
static void test_identity_and_parent() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));

    static const uint8_t iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, iid_unknown[i]);
    CHECK_EQ(call_method(dd, DD_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t unk1 = rd32(sc(0x60));
    CHECK(unk1 != 0);

    // Create a lower-numbered view, which used to displace the identity.
    uint8_t dd2[16] = {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11,
                       0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, dd2[i]);
    call_method(dd, DD_QueryInterface, {iid, sc(0x64)});
    uint32_t ddraw2 = rd32(sc(0x64));
    CHECK(ddraw2 != 0);

    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, iid_unknown[i]);
    CHECK_EQ(call_method(dd, DD_QueryInterface, {iid, sc(0x68)}), S_OK);
    CHECK_EQ(rd32(sc(0x68)), unk1);
    // The same object reached through another interface reports the same
    // IUnknown, which is how COM defines object identity.
    CHECK_EQ(call_method(ddraw2, DD_QueryInterface, {iid, sc(0x6c)}), S_OK);
    CHECK_EQ(rd32(sc(0x6c)), unk1);

    // DirectDraw to Direct3D, and back again.
    uint8_t d3d2[16] = {0xC1, 0x1E, 0xAE, 0x6A, 0x2A, 0x66, 0xD0, 0x11,
                        0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, d3d2[i]);
    CHECK_EQ(call_method(dd, DD_QueryInterface, {iid, sc(0x70)}), S_OK);
    uint32_t d3d = rd32(sc(0x70));
    CHECK(d3d != 0);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, dd2[i]);
    CHECK_EQ(call_method(d3d, 0 /* QueryInterface */, {iid, sc(0x74)}), S_OK);
    CHECK_EQ(rd32(sc(0x74)), ddraw2);
    // Direct3D is an interface on the DirectDraw object, so it reports the
    // same controlling IUnknown: one object, one identity, one lifetime.
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, iid_unknown[i]);
    CHECK_EQ(call_method(d3d, 0 /* QueryInterface */, {iid, sc(0x78)}), S_OK);
    CHECK_EQ(rd32(sc(0x78)), unk1);

    // Releasing every DirectDraw reference while Direct3D is still held must
    // not destroy the object underneath it.
    // Eight references stand on the object now: DirectDrawCreate, four
    // IUnknown queries, one IDirectDraw2 query, the IDirect3D2 query and the
    // reverse IDirectDraw2 query. Direct3D is a view of this same object, so
    // they all count against one number. Releasing seven leaves the one the
    // IDirect3D2 pointer holds, which is the case under test.
    CHECK(com_this(dd) != nullptr);
    call_method(dd, DD_Release, {});     // the create reference
    call_method(ddraw2, DD_Release, {}); // the IDirectDraw2 query
    call_method(dd, DD_Release, {});     // the three IUnknown queries
    call_method(dd, DD_Release, {});
    call_method(ddraw2, DD_Release, {});
    call_method(dd, DD_Release, {}); // the reverse query through D3D
    call_method(dd, DD_Release, {}); // the IUnknown query through D3D
    CHECK(com_this(d3d) != nullptr);
    CHECK(com_this(dd) != nullptr);   // held alive by the Direct3D object
    call_method(d3d, DD_Release, {}); // the IDirect3D2 query
    CHECK(com_this(d3d) == nullptr);
    CHECK(com_this(dd) == nullptr); // and now the parent goes too
}

// A duplicated sound buffer keeps the samples alive after the original goes.
static void test_dsound_duplicate_lifetime() {
    cpu_reset();
    g_plays.clear();
    uint32_t create = tramp("DSOUND.dll", "ord1");
    call_shim(create, {0, sc(0), 0});
    uint32_t ds = rd32(sc(0));

    uint32_t wfx = sc(0x100);
    wr16(wfx + 0, 1);
    wr16(wfx + 2, 1);
    wr32(wfx + 4, 22050);
    wr32(wfx + 8, 22050);
    wr16(wfx + 12, 1);
    wr16(wfx + 14, 8);
    wr16(wfx + 16, 0);

    uint32_t bd = sc(0x200);
    gm_zero(bd, SDK_DSBUFFERDESC);
    wr32(bd + 0, SDK_DSBUFFERDESC);
    wr32(bd + 4, DSBCAPS_STATIC);
    wr32(bd + 8, 256);
    wr32(bd + 16, wfx);
    CHECK_EQ(call_method(ds, DS_CreateSoundBuffer, {bd, sc(4), 0}), DS_OK);
    uint32_t orig = rd32(sc(4));
    CHECK(orig != 0);

    // Fill it, so the duplicate has something identifiable to play.
    call_method(orig, B_Lock, {0, 256, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0});
    uint32_t p1 = rd32(sc(0x300));
    for (uint32_t i = 0; i < 256; ++i)
        wr8(p1 + i, (uint8_t)(0xC0 ^ i));
    call_method(orig, B_Unlock, {p1, 256, 0, 0});

    CHECK_EQ(call_method(ds, 5 /* DuplicateSoundBuffer */, {orig, sc(8)}), DS_OK);
    uint32_t dup = rd32(sc(8));
    CHECK(dup != 0);

    HeapStats before = heap_stats();
    call_method(orig, S_Release, {});
    // Releasing the original must not hand the samples back to the heap while
    // the duplicate still points at them.
    CHECK_EQ(heap_stats().used_blocks, before.used_blocks - 1); // the view only

    g_plays.clear();
    CHECK_EQ(call_method(dup, B_Play, {0, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1);
    if (!g_plays.empty()) {
        bool ok = true;
        for (uint32_t i = 0; i < 256; ++i)
            if (g_plays[0].pcm[i] != (uint8_t)(0xC0 ^ i)) {
                ok = false;
                break;
            }
        CHECK(ok);
    }
    call_method(dup, B_Stop, {});
    call_method(dup, S_Release, {});
}

// SetSurfaceDesc validates the whole replacement span and does not free
// memory the guest owns.
static void test_setsurfacedesc_ownership() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    wr32(desc + DDSD_OFF_dwWidth, 32);
    wr32(desc + DDSD_OFF_dwHeight, 32);
    call_method(dd, DD_CreateSurface, {desc, sc(4), 0});
    uint32_t s1 = rd32(sc(4));
    CHECK(s1 != 0);

    // IDirectDrawSurface3 is where SetSurfaceDesc lives.
    uint32_t iid = sc(0x40);
    uint8_t s3[16] = {0x00, 0x4E, 0x04, 0xDA, 0xB2, 0x69, 0xD0, 0x11,
                      0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, s3[i]);
    CHECK_EQ(call_method(s1, S_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t v3 = rd32(sc(0x60));
    CHECK(v3 != 0);

    // A pitch that cannot hold a row is refused.
    uint32_t sd = sc(0x200);
    gm_zero(sd, SDK_DDSURFACEDESC);
    wr32(sd + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(sd + DDSD_OFF_dwFlags, DDSD_LPSURFACE | DDSD_PITCH);
    wr32(sd + DDSD_OFF_lpSurface, g_scratch);
    wr32(sd + DDSD_OFF_lPitch, 4); // a 32-pixel row needs 32
    CHECK_EQ(call_method(v3, 39 /* SetSurfaceDesc */, {sd, 0}), DDERR_INVALIDPARAMS);

    // A pointer near the end of the arena whose span would run off it, too.
    wr32(sd + DDSD_OFF_lPitch, 64);
    wr32(sd + DDSD_OFF_lpSurface, GUEST_SIZE - 64);
    CHECK_EQ(call_method(v3, 39, {sd, 0}), DDERR_INVALIDPARAMS);

    // A valid guest-owned buffer is accepted, and releasing the surface must
    // not free it: the heap block count stays put.
    uint32_t owned = heap_alloc(64 * 32, true, 16);
    CHECK(owned != 0);
    wr32(sd + DDSD_OFF_lpSurface, owned);
    CHECK_EQ(call_method(v3, 39, {sd, 0}), DD_OK);
    CHECK_EQ(heap_size(owned), 64u * 32u);
    call_method(v3, S_Release, {});
    call_method(s1, S_Release, {});
    // Still a live block of exactly the size we asked for: nothing freed it.
    CHECK_EQ(heap_size(owned), 64u * 32u);
    heap_free(owned);
}

// Every way the record can fail to match the recovered contract is a refused
// handle, not a silent one.
static void test_qmixer_failure() {
    cpu_reset();
    uint32_t init = tramp("QMIXER.dll", "QSWaveMixInitEx");
    uint32_t initdata = sc(0x100);
    gm_zero(initdata, 0x40);
    wr32(initdata + 0, 0x40);
    wr32(initdata + 8, 22050);
    uint32_t hmix = call_shim(init, {initdata});
    CHECK(hmix != 0);

    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    uint32_t owd = sc(0x300);

    // An all-zero record names no format at all.
    gm_zero(owd, 20);
    CHECK_EQ(call_shim(open_wave, {hmix, owd, 8}), 0u);

    // A well-formed record, used as the baseline for the negative cases.
    uint32_t wfx = sc(0x200);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + 0x00, 1);
    wr16(wfx + 0x02, 1);
    wr32(wfx + 0x04, 11025);
    wr32(wfx + 0x08, 11025);
    wr16(wfx + 0x0c, 1);
    wr16(wfx + 0x0e, 8);
    uint32_t pcm = sc(0x400);
    for (uint32_t i = 0; i < 32; ++i)
        wr8(pcm + i, (uint8_t)i);
    wr32(owd + 0x00, wfx);
    wr32(owd + 0x04, pcm);
    wr32(owd + 0x08, 32);
    uint32_t good = call_shim(open_wave, {hmix, owd, 8});
    CHECK(good != 0);

    // A compressed format is refused rather than played as if it were PCM.
    wr16(wfx + 0x00, 2); // WAVE_FORMAT_ADPCM
    CHECK_EQ(call_shim(open_wave, {hmix, owd, 8}), 0u);
    wr16(wfx + 0x00, 1);

    // A sample count that runs off the end of the arena.
    wr32(owd + 0x04, GUEST_SIZE - 16);
    wr32(owd + 0x08, 4096);
    CHECK_EQ(call_shim(open_wave, {hmix, owd, 8}), 0u);
    wr32(owd + 0x04, pcm);
    wr32(owd + 0x08, 32);

    // A format the mixer cannot play.
    wr16(wfx + 0x0e, 24);
    CHECK_EQ(call_shim(open_wave, {hmix, owd, 8}), 0u);
    wr16(wfx + 0x0e, 8);

    // The streaming form, with no callback to obtain samples from: field 1 is
    // a length rather than a pointer, so there is nothing to play and opening
    // the wave has to fail rather than hand back a handle that is silent.
    // Streaming with a real callback is covered by the QMixer streaming test.
    wr32(owd + 0x04, 0x7800); // a length, not a pointer
    wr32(owd + 0x08, 0);      // no callback
    wr32(owd + 0x0c, 0);
    CHECK_EQ(call_shim(open_wave, {hmix, owd, 0x11}), 0u);

    // And the good record still opens, so the guards are not blanket refusals.
    wr32(owd + 0x04, pcm);
    wr32(owd + 0x08, 32);
    wr32(owd + 0x0c, 0);
    CHECK(call_shim(open_wave, {hmix, owd, 8}) != 0);

    uint32_t close = tramp("QMIXER.dll", "QSWaveMixCloseSession");
    call_shim(close, {hmix});
}

// dx_reset must leave no cached guest address behind: after it, everything
// works against the fresh arena.
static void test_reset() {
    mem_init();
    dx_reset();
    g_scratch = heap_alloc(0x4000, true, 16);
    CHECK(g_scratch != 0);
    CHECK_EQ(com_live_count(), 0u);

    cpu_reset();
    g_presents.clear();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    uint32_t hr = call_shim(create, {0, sc(0), 0});
    CHECK_EQ(hr, DD_OK);
    uint32_t dd = rd32(sc(0));
    CHECK(dd != 0);
    // The vtable was rebuilt in the new arena and its slots still dispatch.
    CHECK(imports_is_trampoline(rd32(rd32(dd + COM_OFF_vtbl))));
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {640, 480, 8}), DD_OK);

    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(4), 0}), DD_OK);
    uint32_t primary = rd32(sc(4));
    CHECK(primary != 0);
    uint32_t ld = sc(0x200);
    gm_zero(ld, SDK_DDSURFACEDESC);
    wr32(ld + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    CHECK_EQ(call_method(primary, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
    CHECK(rd32(ld + DDSD_OFF_lpSurface) != 0);
    CHECK_EQ(call_method(primary, S_Unlock, {0}), DD_OK);
    CHECK_EQ(g_presents.size(), 1);
}

// A rectangle touching the bottom edge with a non-zero left edge is legal: the
// locked span is (h-1) whole rows plus the last row's width, not h whole rows.
static void test_partial_lock_bottom_edge() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN);
    wr32(desc + DDSD_OFF_dwWidth, 64);
    wr32(desc + DDSD_OFF_dwHeight, 64);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(4), 0}), DD_OK);
    uint32_t surf = rd32(sc(4));
    CHECK(surf != 0);

    // The exact case the old bound rejected: bottom-right corner, left > 0.
    uint32_t rect = sc(0x200);
    wr32(rect + 0, 32);  // left
    wr32(rect + 4, 60);  // top
    wr32(rect + 8, 64);  // right
    wr32(rect + 12, 64); // bottom
    uint32_t ld = sc(0x300);
    gm_zero(ld, SDK_DDSURFACEDESC);
    wr32(ld + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    uint32_t hr = call_method(surf, S_Lock, {rect, ld, DDLOCK_WAIT, 0});
    CHECK_EQ(hr, DD_OK);
    CHECK_EQ(rd32(ld + DDSD_OFF_dwWidth), 32u);
    CHECK_EQ(rd32(ld + DDSD_OFF_dwHeight), 4u);
    uint32_t p = rd32(ld + DDSD_OFF_lpSurface);
    CHECK(p != 0);
    // The last row of the region is writable, which is what the bound governs.
    uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
    wr8(p + 3 * pitch + 31, 0xAB);
    CHECK_EQ(rd8(p + 3 * pitch + 31), 0xAB);
    CHECK_EQ(call_method(surf, S_Unlock, {0}), DD_OK);

    // The very last pixel of the surface, as a 1x1 rectangle.
    wr32(rect + 0, 63);
    wr32(rect + 4, 63);
    wr32(rect + 8, 64);
    wr32(rect + 12, 64);
    gm_zero(ld, SDK_DDSURFACEDESC);
    wr32(ld + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    CHECK_EQ(call_method(surf, S_Lock, {rect, ld, DDLOCK_WAIT, 0}), DD_OK);
    CHECK_EQ(call_method(surf, S_Unlock, {0}), DD_OK);

    // A rectangle past the edge is still refused.
    wr32(rect + 0, 32);
    wr32(rect + 4, 60);
    wr32(rect + 8, 65);
    wr32(rect + 12, 64);
    gm_zero(ld, SDK_DDSURFACEDESC);
    wr32(ld + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    CHECK_EQ(call_method(surf, S_Lock, {rect, ld, DDLOCK_WAIT, 0}), DDERR_INVALIDRECT);
}

// Flip moves pixel ownership with the pixels, so a guest-owned buffer that has
// been flipped to the front is still not the shim's to free.
static void test_flip_ownership() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    wr32(desc + DDSD_OFF_dwBackBufferCount, 1);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(4), 0}), DD_OK);
    uint32_t primary = rd32(sc(4));
    uint32_t caps = sc(0x180);
    wr32(caps, DDSCAPS_BACKBUFFER);
    CHECK_EQ(call_method(primary, S_GetAttachedSurface, {caps, sc(8)}), DD_OK);
    uint32_t back = rd32(sc(8));
    CHECK(back != 0);

    // Point the back buffer at memory the guest owns.
    uint32_t iid = sc(0x40);
    uint8_t s3[16] = {0x00, 0x4E, 0x04, 0xDA, 0xB2, 0x69, 0xD0, 0x11,
                      0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, s3[i]);
    CHECK_EQ(call_method(back, S_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t back3 = rd32(sc(0x60));
    CHECK(back3 != 0);

    uint32_t owned = heap_alloc(704 * 480, true, 16);
    CHECK(owned != 0);
    uint32_t sd = sc(0x200);
    gm_zero(sd, SDK_DDSURFACEDESC);
    wr32(sd + DDSD_OFF_dwSize, SDK_DDSURFACEDESC);
    wr32(sd + DDSD_OFF_dwFlags, DDSD_LPSURFACE | DDSD_PITCH);
    wr32(sd + DDSD_OFF_lpSurface, owned);
    wr32(sd + DDSD_OFF_lPitch, 704);
    CHECK_EQ(call_method(back3, 39 /* SetSurfaceDesc */, {sd, 0}), DD_OK);

    // Flipping moves that buffer to the front. Releasing everything must not
    // free it: it belongs to the guest, not to the shim.
    CHECK_EQ(call_method(primary, S_Flip, {0, DDFLIP_WAIT}), DD_OK);
    call_method(back3, S_Release, {});
    call_method(back, S_Release, {});
    call_method(primary, S_Release, {});
    CHECK_EQ(heap_size(owned), 704u * 480u);
    heap_free(owned);
}

// Out-parameters that cannot be written are refused rather than written to.
static void test_out_pointer_guards() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));

    // A dword pointer with only two bytes of arena left after it.
    const uint32_t edge = GUEST_SIZE - 2;

    uint32_t entries = sc(0x400);
    gm_zero(entries, 256 * 4);
    CHECK_EQ(
        call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_INITIALIZE, entries, sc(12), 0}),
        DD_OK);
    uint32_t pal = rd32(sc(12));
    CHECK(pal != 0);
    CHECK_EQ(call_method(pal, 3 /* GetCaps */, {edge}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(pal, 3, {0}), DDERR_INVALIDPARAMS);

    CHECK_EQ(call_method(dd, 4 /* CreateClipper */, {0, sc(16), 0}), DD_OK);
    uint32_t clip = rd32(sc(16));
    CHECK(clip != 0);
    CHECK_EQ(call_method(clip, 4 /* GetHWnd */, {edge}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(clip, 6 /* IsClipListChanged */, {edge}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(clip, 6, {0}), DDERR_INVALIDPARAMS);
    // A writable pointer still works, so these are guards and not refusals.
    CHECK_EQ(call_method(clip, 6, {sc(0x20)}), DD_OK);
    CHECK_EQ(rd32(sc(0x20)), 0u);

    // A surface's DDSCAPS2 output needs all sixteen bytes, not just four.
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});
    uint32_t desc = sc(0x100);
    gm_zero(desc, SDK_DDSURFACEDESC2);
    wr32(desc + DDSD_OFF_dwSize, SDK_DDSURFACEDESC2);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN);
    wr32(desc + DDSD_OFF_dwWidth, 8);
    wr32(desc + DDSD_OFF_dwHeight, 8);
    uint32_t iid = sc(0x40);
    uint8_t dd4[16] = {0x9A, 0x50, 0x59, 0x9C, 0xBD, 0x39, 0xD1, 0x11,
                       0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5};
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, dd4[i]);
    CHECK_EQ(call_method(dd, DD_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t dd4p = rd32(sc(0x60));
    CHECK_EQ(call_method(dd4p, DD_CreateSurface, {desc, sc(4), 0}), DD_OK);
    uint32_t s4 = rd32(sc(4));
    CHECK(s4 != 0);
    CHECK_EQ(call_method(s4, 14 /* GetCaps */, {GUEST_SIZE - 8}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(s4, 14, {sc(0x30)}), DD_OK);
    CHECK_EQ(rd32(sc(0x30)), DDSCAPS_OFFSCREENPLAIN);
}

// ---------------------------------------------------------------------------
// IDirectDrawColorControl on a surface: QueryInterface reaches it, the
// defaults are the SDK's, dwSize is validated, and SetColorControls writes
// only the fields its flags name.
// ---------------------------------------------------------------------------
static void test_color_control() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    CHECK(dd != 0);
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 8});

    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_PRIMARYSURFACE);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
    uint32_t surf = rd32(sc(0x10));
    CHECK(surf != 0);

    // QueryInterface reaches it from the surface.
    static const uint8_t iid_cc[16] = {0xE0, 0x0E, 0x9F, 0x4B, 0x7E, 0x0D, 0xD0, 0x11,
                                       0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, iid_cc[i]);
    wr32(sc(0x60), 0xdeadbeef);
    CHECK_EQ(call_method(surf, S_QueryInterface, {iid, sc(0x60)}), S_OK);
    uint32_t cc = rd32(sc(0x60));
    CHECK(cc != 0 && cc != 0xdeadbeef);
    // A separate view of one object: a different vtable, the same identity.
    CHECK(rd32(cc + COM_OFF_vtbl) != rd32(surf + COM_OFF_vtbl));
    CHECK_EQ(rd32(cc + COM_OFF_obj), rd32(surf + COM_OFF_obj));

    // The defaults the SDK documents, and the structure really is 40 bytes:
    // a canary one dword past the end must survive.
    uint32_t p = sc(0x100);
    gm_zero(p, 64);
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    wr32(p + DDCOLORCONTROL_SIZE, 0xA5A5A5A5u);
    CHECK_EQ(call_method(cc, 3 /* GetColorControls */, {p}), DD_OK);
    CHECK_EQ(rd32(p + DDCC_OFF_dwFlags), DDCOLOR_ALL);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lBrightness), 750);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lContrast), 10000);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lHue), 0);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lSaturation), 10000);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lSharpness), 5);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lGamma), 1);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lColorEnable), 1);
    CHECK_EQ(rd32(p + DDCOLORCONTROL_SIZE), 0xA5A5A5A5u);

    // A wrong dwSize is refused rather than filled in.
    wr32(p + DDCC_OFF_dwSize, 52);
    CHECK_EQ(call_method(cc, 3 /* GetColorControls */, {p}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(cc, 4 /* SetColorControls */, {p}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(cc, 3 /* GetColorControls */, {0}), DDERR_INVALIDPARAMS);

    // Set only brightness: the other fields keep their values even though the
    // structure carries different numbers in them.
    gm_zero(p, 64);
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    wr32(p + DDCC_OFF_dwFlags, DDCOLOR_BRIGHTNESS);
    wr32(p + DDCC_OFF_lBrightness, (uint32_t)(int32_t)1234);
    wr32(p + DDCC_OFF_lContrast, (uint32_t)(int32_t)7777);
    CHECK_EQ(call_method(cc, 4 /* SetColorControls */, {p}), DD_OK);
    gm_zero(p, 64);
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    CHECK_EQ(call_method(cc, 3 /* GetColorControls */, {p}), DD_OK);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lBrightness), 1234);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lContrast), 10000);

    // An undefined flag is rejected and changes nothing.
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    wr32(p + DDCC_OFF_dwFlags, 0x8000u);
    wr32(p + DDCC_OFF_lBrightness, (uint32_t)(int32_t)99);
    CHECK_EQ(call_method(cc, 4 /* SetColorControls */, {p}), DDERR_INVALIDPARAMS);
    gm_zero(p, 64);
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    CHECK_EQ(call_method(cc, 3 /* GetColorControls */, {p}), DD_OK);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lBrightness), 1234);

    // Two surfaces have independent controls.
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_OFFSCREENPLAIN);
    wr32(desc + DDSD_OFF_dwWidth, 32);
    wr32(desc + DDSD_OFF_dwHeight, 32);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x14), 0}), DD_OK);
    uint32_t other = rd32(sc(0x14));
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, iid_cc[i]);
    CHECK_EQ(call_method(other, S_QueryInterface, {iid, sc(0x64)}), S_OK);
    uint32_t cc2 = rd32(sc(0x64));
    CHECK(cc2 != cc);
    gm_zero(p, 64);
    wr32(p + DDCC_OFF_dwSize, DDCOLORCONTROL_SIZE);
    CHECK_EQ(call_method(cc2, 3 /* GetColorControls */, {p}), DD_OK);
    CHECK_EQ((int32_t)rd32(p + DDCC_OFF_lBrightness), 750);
}

// ---------------------------------------------------------------------------
// A FourCC surface is refused as a pixel format, not as a surface type, and
// nothing in the advertised set contradicts that: GetFourCCCodes reports none
// and EnumTextureFormats offers none.
// ---------------------------------------------------------------------------
static uint32_t g_fourcc_formats = 0;
static uint32_t g_fourcc_texfmt_fourcc = 0;

static void test_fourcc_is_a_pixel_format_error() {
    cpu_reset();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    // 'PVRC', the format the game's PowerVR path asks for.
    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 64);
    wr32(desc + DDSD_OFF_dwHeight, 64);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, DDPF_FOURCC);
    wr32(pf + DDPF_OFF_dwFourCC, 0x43525650u); // 'PVRC'
    wr32(sc(0x10), 0xdeadbeef);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DDERR_INVALIDPIXELFORMAT);
    CHECK_EQ(rd32(sc(0x10)), 0);

    // The refusal is consistent with what the driver advertises.
    wr32(sc(0x20), 0xdeadbeef);
    CHECK_EQ(call_method(dd, DD_GetFourCCCodes, {sc(0x20), 0}), DD_OK);
    CHECK_EQ(rd32(sc(0x20)), 0u);

    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    g_fourcc_formats = 0;
    g_fourcc_texfmt_fourcc = 0;
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "EnumTextureFormatsCallback",
        [](X86 *c) {
            uint32_t d = arg(c, 0);
            uint32_t f = rd32(d + DDSD_OFF_ddpfPixelFormat + DDPF_OFF_dwFlags);
            ++g_fourcc_formats;
            if (f & DDPF_FOURCC)
                ++g_fourcc_texfmt_fourcc;
            set_eax(c, DDENUMRET_OK);
        },
        2);
    CHECK_EQ(call_method(dev, DEV_EnumTextureFormats, {cb, 0}), D3D_OK_);
    CHECK(g_fourcc_formats > 0);
    CHECK_EQ(g_fourcc_texfmt_fourcc, 0u); // not one of them was FourCC
}

// ---------------------------------------------------------------------------
// An 8-bit palettised texture: create it, attach a palette, lock and write
// indices, unlock, and take the handle. The renderer must receive the indices
// AND the palette, and must be given them again when either changes. A
// texture whose palette never arrives draws black, which is what a missing
// palette looks like on screen.
// ---------------------------------------------------------------------------
// A texture is identified to a mod by its content, not by its handle.
//
// The handle is a slot number DirectDraw hands out and reuses, so two
// different textures wear the same one over a run and the same texture wears
// several. An override keyed on it would follow the slot rather than the
// picture. So the shim hashes the pixels, the palette and the dimensions at
// every upload, and that is what a provider is asked about.
static void test_texture_content_hash() {
    cpu_reset();
    g_uploads.clear();
    g_tex_hashes.clear();
    g_tex_override.clear();
    g_tex_hook_on = true;

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 4);
    wr32(desc + DDSD_OFF_dwHeight, 4);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, DDPF_RGB | DDPF_PALETTEINDEXED8);
    wr32(pf + DDPF_OFF_dwRGBBitCount, 8);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
    uint32_t tex_surf = rd32(sc(0x10));
    CHECK(tex_surf != 0);

    uint32_t entries = sc(0x400);
    gm_zero(entries, 256 * 4);
    for (uint32_t i = 0; i < 4; ++i)
        wr32(entries + i * 4, i * 0x00404040u);
    CHECK_EQ(
        call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x18), 0}),
        DD_OK);
    CHECK_EQ(call_method(tex_surf, S_SetPalette, {rd32(sc(0x18))}), DD_OK);

    // Writes the given indices through a real Lock pointer.
    auto write_pixels = [&](int seed) {
        uint32_t ld = sc(0x300);
        gm_zero(ld, DDSD_SIZE);
        wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
        CHECK_EQ(call_method(tex_surf, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
        uint32_t bits = rd32(ld + DDSD_OFF_lpSurface);
        uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                wr8(bits + y * pitch + x, (uint8_t)((x + y + seed) % 4));
        CHECK_EQ(call_method(tex_surf, S_Unlock, {0}), DD_OK);
    };

    write_pixels(0);
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(tex_surf, S_QueryInterface, {iid, sc(0x1c)}), S_OK);
    uint32_t tex = rd32(sc(0x1c));
    CHECK(tex != 0);
    CHECK_EQ(call_method(tex, 3 /* GetHandle */, {dev, sc(0x20)}), D3D_OK_);
    CHECK(g_tex_hashes.size() >= 1);
    uint64_t first = g_tex_hashes.empty() ? 0 : g_tex_hashes.back();
    CHECK(first != 0);

    // The same content again is the same texture, whatever the handle says.
    write_pixels(0);
    CHECK(g_tex_hashes.size() >= 2);
    CHECK_EQ(g_tex_hashes.back(), first);

    // Different content is a different texture.
    write_pixels(1);
    CHECK(g_tex_hashes.size() >= 3);
    CHECK(g_tex_hashes.back() != first);
    uint64_t second = g_tex_hashes.back();

    // A provider that claims that content gets its pixels presented to the
    // host as R,G,B,A bytes, which is these masks on a little-endian target.
    g_tex_override.assign(4 * 4 * 4, 0x5a);
    g_tex_override_for = second;
    g_uploads.clear();
    write_pixels(1);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1) {
        const TextureUpload &u = g_uploads[0];
        CHECK_EQ(u.bpp, 32);
        CHECK_EQ(u.pitch, 16);
        CHECK_EQ(u.rmask, 0x000000ffu);
        CHECK_EQ(u.gmask, 0x0000ff00u);
        CHECK_EQ(u.bmask, 0x00ff0000u);
        CHECK_EQ(u.amask, 0xff000000u);
        CHECK(!u.has_palette);
        CHECK(!u.pixels.empty() && u.pixels[0] == 0x5a);
    }

    // And the content it does not claim goes through untouched.
    g_tex_override_for = 0;
    g_uploads.clear();
    write_pixels(0);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1)
        CHECK_EQ(g_uploads[0].bpp, 8);

    g_tex_hook_on = false;
    g_tex_override.clear();
}

// Every byte of every row is hashed, whatever the format.
//
// The first version read two bytes per pixel for anything above 8 bpp, so a
// 32-bpp surface had the second half of each row left out and two textures
// differing only there would have been handed the same override.
static void test_texture_hash_covers_the_whole_row() {
    cpu_reset();
    g_uploads.clear();
    g_tex_hashes.clear();
    g_tex_override.clear();
    g_tex_hook_on = true;

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 4);
    wr32(desc + DDSD_OFF_dwHeight, 4);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, DDPF_RGB);
    wr32(pf + DDPF_OFF_dwRGBBitCount, 32);
    wr32(pf + DDPF_OFF_dwRBitMask, 0x00ff0000u);
    wr32(pf + DDPF_OFF_dwGBitMask, 0x0000ff00u);
    wr32(pf + DDPF_OFF_dwBBitMask, 0x000000ffu);
    wr32(pf + DDPF_OFF_dwRGBAlphaBitMask, 0xff000000u);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
    uint32_t tex_surf = rd32(sc(0x10));
    CHECK(tex_surf != 0);

    // Writes four 32-bit pixels a row; `tail` changes only the two on the
    // right, which is the half the old hash never read.
    auto write_pixels = [&](uint32_t tail) {
        uint32_t ld = sc(0x300);
        gm_zero(ld, DDSD_SIZE);
        wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
        CHECK_EQ(call_method(tex_surf, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
        uint32_t bits = rd32(ld + DDSD_OFF_lpSurface);
        uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                wr32(bits + y * pitch + x * 4, x < 2 ? 0x11223344u : (0x55667700u | tail));
        CHECK_EQ(call_method(tex_surf, S_Unlock, {0}), DD_OK);
    };

    write_pixels(0);
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(tex_surf, S_QueryInterface, {iid, sc(0x1c)}), S_OK);
    uint32_t tex = rd32(sc(0x1c));
    CHECK(tex != 0);
    CHECK_EQ(call_method(tex, 3 /* GetHandle */, {dev, sc(0x20)}), D3D_OK_);
    CHECK(g_tex_hashes.size() >= 1);
    uint64_t first = g_tex_hashes.empty() ? 0 : g_tex_hashes.back();
    CHECK_EQ(g_uploads.empty() ? 0 : g_uploads.back().bpp, 32);

    write_pixels(0);
    CHECK(g_tex_hashes.size() >= 2);
    CHECK_EQ(g_tex_hashes.back(), first); // same bytes, same texture

    write_pixels(0xaa); // only the right-hand half moves
    CHECK(g_tex_hashes.size() >= 3);
    CHECK(g_tex_hashes.back() != first);

    g_tex_hook_on = false;
}

// A 24-bpp surface is stored four bytes to the pixel, and the hash covers all
// four.
//
// ddraw.cpp:84 gives every surface deeper than 16 bpp four bytes of storage,
// so the fourth byte of a 24-bpp pixel is real memory the guest can write.
// Hashing the nominal three would leave it out and two textures differing
// only there would be handed the same override.
static void test_texture_hash_covers_padding_bytes() {
    cpu_reset();
    g_uploads.clear();
    g_tex_hashes.clear();
    g_tex_override.clear();
    g_tex_hook_on = true;

    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 4);
    wr32(desc + DDSD_OFF_dwHeight, 4);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, DDPF_RGB);
    wr32(pf + DDPF_OFF_dwRGBBitCount, 24);
    wr32(pf + DDPF_OFF_dwRBitMask, 0x00ff0000u);
    wr32(pf + DDPF_OFF_dwGBitMask, 0x0000ff00u);
    wr32(pf + DDPF_OFF_dwBBitMask, 0x000000ffu);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
    uint32_t tex_surf = rd32(sc(0x10));
    CHECK(tex_surf != 0);

    // Every pixel keeps the same three colour bytes; only the LAST pixel's
    // fourth byte moves. Four pixels of four bytes is sixteen bytes a row, and
    // the nominal depth would have read twelve of them - so byte fifteen is
    // exactly the one a width-times-three hash never sees. Changing an earlier
    // pixel's padding byte would not have proved anything: those fall inside
    // the first twelve bytes and a wrong hash reads them anyway.
    auto write_pixels = [&](uint8_t pad) {
        uint32_t ld = sc(0x300);
        gm_zero(ld, DDSD_SIZE);
        wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
        CHECK_EQ(call_method(tex_surf, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
        uint32_t bits = rd32(ld + DDSD_OFF_lpSurface);
        uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
        CHECK(pitch >= 16); // four pixels of four bytes
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                uint32_t at = bits + y * pitch + x * 4;
                wr8(at + 0, 0x11);
                wr8(at + 1, 0x22);
                wr8(at + 2, 0x33);
                wr8(at + 3, x == 3 ? pad : 0x00);
            }
        CHECK_EQ(call_method(tex_surf, S_Unlock, {0}), DD_OK);
    };

    write_pixels(0);
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(tex_surf, S_QueryInterface, {iid, sc(0x1c)}), S_OK);
    uint32_t tex = rd32(sc(0x1c));
    CHECK(tex != 0);
    CHECK_EQ(call_method(tex, 3 /* GetHandle */, {dev, sc(0x20)}), D3D_OK_);
    CHECK(g_tex_hashes.size() >= 1);
    uint64_t first = g_tex_hashes.empty() ? 0 : g_tex_hashes.back();

    write_pixels(0);
    CHECK(g_tex_hashes.size() >= 2);
    CHECK_EQ(g_tex_hashes.back(), first);

    write_pixels(0xcc); // only the last pixel's fourth byte
    CHECK(g_tex_hashes.size() >= 3);
    CHECK(g_tex_hashes.back() != first);

    g_tex_hook_on = false;
}

static void test_palettised_texture_upload() {
    cpu_reset();
    g_uploads.clear();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});

    // An 8-bit texture in a 16-bit mode: the pixel format says palettised, so
    // the surface is 8bpp whatever the display is.
    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
    wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
    wr32(desc + DDSD_OFF_dwWidth, 4);
    wr32(desc + DDSD_OFF_dwHeight, 4);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, DDPF_RGB | DDPF_PALETTEINDEXED8);
    wr32(pf + DDPF_OFF_dwRGBBitCount, 8);
    CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
    uint32_t tex_surf = rd32(sc(0x10));
    CHECK(tex_surf != 0);

    // A palette with three recognisable colours.
    uint32_t entries = sc(0x400);
    gm_zero(entries, 256 * 4);
    wr32(entries + 0 * 4, 0x00000000u);
    wr32(entries + 1 * 4, 0x000000FFu); // PALETTEENTRY is R,G,B,flags
    wr32(entries + 2 * 4, 0x0000FF00u);
    wr32(entries + 3 * 4, 0x00FF0000u);
    CHECK_EQ(
        call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x18), 0}),
        DD_OK);
    uint32_t pal = rd32(sc(0x18));
    CHECK(pal != 0);
    CHECK_EQ(call_method(tex_surf, S_SetPalette, {pal}), DD_OK);

    // Write indices through a real Lock pointer.
    uint32_t ld = sc(0x300);
    gm_zero(ld, DDSD_SIZE);
    wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
    CHECK_EQ(call_method(tex_surf, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
    uint32_t bits = rd32(ld + DDSD_OFF_lpSurface);
    uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
    CHECK(bits != 0);
    CHECK(pitch >= 4);
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            wr8(bits + y * pitch + x, (uint8_t)((x + y) % 4));
    CHECK_EQ(call_method(tex_surf, S_Unlock, {0}), DD_OK);

    // Until a handle exists there is no texture to upload: an Unlock before
    // GetHandle must not invent one.
    CHECK_EQ(g_uploads.size(), 0u);

    // QueryInterface to IDirect3DTexture2 and take the handle. That is the
    // point the surface becomes a texture, and the upload must carry both the
    // indices and the palette.
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    CHECK_EQ(call_method(tex_surf, S_QueryInterface, {iid, sc(0x1c)}), S_OK);
    uint32_t tex = rd32(sc(0x1c));
    CHECK(tex != 0);

    g_uploads.clear();
    CHECK_EQ(call_method(tex, 3 /* GetHandle */, {dev, sc(0x20)}), D3D_OK_);
    CHECK(rd32(sc(0x20)) != 0);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1) {
        const TextureUpload &u = g_uploads[0];
        CHECK_EQ(u.bpp, 8);
        CHECK_EQ(u.width, 4);
        CHECK_EQ(u.height, 4);
        CHECK(u.has_palette);
        // The indices, exactly as written.
        CHECK((int)u.pixels.size() >= u.pitch * u.height);
        bool indices_ok = u.pixels.size() >= (size_t)u.pitch * 4;
        for (int y = 0; y < 4 && indices_ok; ++y)
            for (int x = 0; x < 4; ++x)
                if (u.pixels[(size_t)y * u.pitch + x] != (uint8_t)((x + y) % 4))
                    indices_ok = false;
        CHECK(indices_ok);
        // And the colours those indices resolve against.
        CHECK_EQ(u.palette[0] & 0xffffffu, 0x000000u);
        CHECK_EQ(u.palette[1] & 0xffffffu, 0xFF0000u);
        CHECK_EQ(u.palette[2] & 0xffffffu, 0x00FF00u);
        CHECK_EQ(u.palette[3] & 0xffffffu, 0x0000FFu);
    }

    // Editing the pixels sends them again.
    g_uploads.clear();
    CHECK_EQ(call_method(tex_surf, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
    wr8(rd32(ld + DDSD_OFF_lpSurface), 3);
    CHECK_EQ(call_method(tex_surf, S_Unlock, {0}), DD_OK);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1)
        CHECK_EQ(g_uploads[0].pixels[0], 3u);

    // Changing the palette entries sends the texture again with the new
    // colours, even though not one index moved.
    g_uploads.clear();
    gm_zero(entries, 256 * 4);
    // SetEntries reads from the start of the array; dwStartingEntry is where
    // they land, not where they come from.
    wr32(entries + 0, 0x0000FFFFu); // index 1 becomes yellow
    CHECK_EQ(call_method(pal, P_SetEntries, {0, 1, 1, entries}), DD_OK);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1) {
        CHECK(g_uploads[0].has_palette);
        CHECK_EQ(g_uploads[0].palette[1] & 0xffffffu, 0xFFFF00u);
    }

    // Attaching a different palette does too: the indices mean something new.
    gm_zero(entries, 256 * 4);
    wr32(entries + 2 * 4, 0x00FFFFFFu);
    CHECK_EQ(
        call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, sc(0x24), 0}),
        DD_OK);
    uint32_t pal2 = rd32(sc(0x24));
    g_uploads.clear();
    CHECK_EQ(call_method(tex_surf, S_SetPalette, {pal2}), DD_OK);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1)
        CHECK_EQ(g_uploads[0].palette[2] & 0xffffffu, 0xFFFFFFu);
}

// ---------------------------------------------------------------------------
// EnumTextureFormats offers exactly what a plain 1998 HAL offered, and every
// format it offers can actually be created.
// ---------------------------------------------------------------------------
static std::vector<std::array<uint32_t, 6>> g_texfmts;

static void test_texture_formats() {
    cpu_reset();
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "TexFormats",
        [](X86 *c) {
            uint32_t pf = arg(c, 0) + DDSD_OFF_ddpfPixelFormat;
            g_texfmts.push_back({rd32(pf + DDPF_OFF_dwFlags), rd32(pf + DDPF_OFF_dwRGBBitCount),
                                 rd32(pf + DDPF_OFF_dwRBitMask), rd32(pf + DDPF_OFF_dwGBitMask),
                                 rd32(pf + DDPF_OFF_dwBBitMask),
                                 rd32(pf + DDPF_OFF_dwRGBAlphaBitMask)});
            set_eax(c, DDENUMRET_OK);
        },
        2);
    g_texfmts.clear();
    CHECK_EQ(call_method(dev, DEV_EnumTextureFormats, {cb, 0}), D3D_OK_);
    CHECK_EQ(g_texfmts.size(), 6u);

    // The four formats the Wine trace of the original shows it being offered
    // and able to use, in the order it was offered them. P8 is deliberately
    // absent: the original was never offered it, though it does create one
    // palettised texture directly through CreateSurface.
    const uint32_t want[6][6] = {
        {DDPF_RGB, 16, 0x7c00, 0x03e0, 0x001f, 0},                         // B5G5R5X1
        {DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000}, // B5G5R5A1
        {DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000}, // B4G4R4A4
        {DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0},                         // B5G6R5
        {DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0},
        {DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xff0000, 0xff00, 0xff, 0xff000000},
    };
    for (size_t i = 0; i < g_texfmts.size() && i < 6; ++i) {
        for (int j = 0; j < 6; ++j)
            CHECK_EQ(g_texfmts[i][j], want[i][j]);
        CHECK((g_texfmts[i][0] & DDPF_FOURCC) == 0);
    }

    // Every advertised format can be created, so the enumeration is not a
    // promise the surface allocator breaks.
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});
    for (auto &f : g_texfmts) {
        uint32_t desc = sc(0x200);
        gm_zero(desc, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
        wr32(desc + DDSD_OFF_dwFlags, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
        wr32(desc + DDSD_OFF_ddsCaps, DDSCAPS_TEXTURE);
        wr32(desc + DDSD_OFF_dwWidth, 8);
        wr32(desc + DDSD_OFF_dwHeight, 8);
        uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
        wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
        wr32(pf + DDPF_OFF_dwFlags, f[0]);
        wr32(pf + DDPF_OFF_dwRGBBitCount, f[1]);
        wr32(pf + DDPF_OFF_dwRBitMask, f[2]);
        wr32(pf + DDPF_OFF_dwGBitMask, f[3]);
        wr32(pf + DDPF_OFF_dwBBitMask, f[4]);
        wr32(pf + DDPF_OFF_dwRGBAlphaBitMask, f[5]);
        CHECK_EQ(call_method(dd, DD_CreateSurface, {desc, sc(0x10), 0}), DD_OK);
        uint32_t surf = rd32(sc(0x10));
        CHECK(surf != 0);
        if (!surf)
            continue;

        // And the format survives the trip to the renderer. A surface created
        // as 4-4-4-4 that arrives as 5-6-5 would be drawn with the wrong
        // colours and no alpha, which is exactly what advertising a format
        // the pipeline cannot carry would look like.
        uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                            0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
        uint32_t iid = sc(0x40);
        for (int i = 0; i < 16; ++i)
            wr8(iid + (uint32_t)i, tex2[i]);
        if (call_method(surf, S_QueryInterface, {iid, sc(0x1c)}) != S_OK) {
            CHECK(false);
            continue;
        }
        uint32_t tex = rd32(sc(0x1c));
        g_uploads.clear();
        CHECK_EQ(call_method(tex, TEX_GetHandle, {dev, sc(0x20)}), D3D_OK_);
        CHECK_EQ(g_uploads.size(), 1u);
        if (g_uploads.size() != 1)
            continue;
        const TextureUpload &u = g_uploads[0];
        CHECK_EQ((uint32_t)u.bpp, f[1]);
        CHECK_EQ(u.rmask, f[2]);
        CHECK_EQ(u.gmask, f[3]);
        CHECK_EQ(u.bmask, f[4]);
        CHECK_EQ(u.amask, f[5]);
        CHECK((f[0] & DDPF_FOURCC) == 0);
    }
}

// ---------------------------------------------------------------------------
// The DirectSound data path end to end: the exact bytes the guest writes
// through a Lock pointer are the bytes the host is asked to play, with the
// buffer's own format. Covers the wrap pair, both sample widths, and that a
// duplicate shares the original's data rather than a copy of it.
// ---------------------------------------------------------------------------
static uint32_t make_dsound() {
    uint32_t create = tramp("DSOUND.dll", "ord1");
    call_shim(create, {0, sc(0), 0});
    uint32_t ds = rd32(sc(0));
    call_method(ds, DS_SetCooperativeLevel, {0x20004, 3});
    return ds;
}

static uint32_t make_buffer(uint32_t ds, uint32_t chans, uint32_t rate, uint32_t bits,
                            uint32_t bytes, uint32_t out) {
    uint32_t wfx = sc(0x100);
    uint32_t align = chans * (bits / 8);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, (uint16_t)chans);
    wr32(wfx + WFX_OFF_nSamplesPerSec, rate);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, rate * align);
    wr16(wfx + WFX_OFF_nBlockAlign, (uint16_t)align);
    wr16(wfx + WFX_OFF_wBitsPerSample, (uint16_t)bits);
    wr16(wfx + WFX_OFF_cbSize, 0);
    uint32_t bd = sc(0x200);
    gm_zero(bd, DSBUFFERDESC_SIZE);
    wr32(bd + DSBD_OFF_dwSize, DSBUFFERDESC_SIZE);
    wr32(bd + DSBD_OFF_dwFlags, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_STATIC);
    wr32(bd + DSBD_OFF_dwBufferBytes, bytes);
    wr32(bd + DSBD_OFF_lpwfxFormat, wfx);
    if (call_method(ds, DS_CreateSoundBuffer, {bd, out, 0}) != DS_OK)
        return 0;
    return rd32(out);
}

static void test_dsound_data_path() {
    cpu_reset();
    g_plays.clear();
    uint32_t ds = make_dsound();
    CHECK(ds != 0);

    // --- 16-bit stereo: a known signed waveform, byte for byte.
    const uint32_t kBytes = 512;
    uint32_t buf = make_buffer(ds, 2, 22050, 16, kBytes, sc(4));
    CHECK(buf != 0);
    CHECK_EQ(call_method(buf, B_Lock, {0, kBytes, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t p1 = rd32(sc(0x300));
    CHECK_EQ(rd32(sc(0x304)), kBytes);
    CHECK_EQ(rd32(sc(0x308)), 0u); // no wrap: the second region is empty
    CHECK_EQ(rd32(sc(0x30c)), 0u);
    std::vector<uint8_t> want(kBytes);
    for (uint32_t i = 0; i < kBytes / 2; ++i) {
        // A signed triangle, so a byte-order or sign mistake is visible.
        int16_t v = (int16_t)(((int)i % 64) * 512 - 16384);
        want[i * 2 + 0] = (uint8_t)(v & 0xff);
        want[i * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
        wr8(p1 + i * 2 + 0, want[i * 2 + 0]);
        wr8(p1 + i * 2 + 1, want[i * 2 + 1]);
    }
    CHECK_EQ(call_method(buf, B_Unlock, {p1, kBytes, 0, 0}), DS_OK);

    g_plays.clear();
    CHECK_EQ(call_method(buf, B_Play, {0, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        const PlayRecord &r = g_plays[0];
        CHECK_EQ(r.rate, 22050);
        CHECK_EQ(r.channels, 2);
        CHECK_EQ(r.bits, 16);
        CHECK_EQ(r.loop, 0);
        CHECK_EQ(r.bytes, kBytes);
        CHECK(r.pcm == want);
    }

    // --- The wrap pair. A lock that runs off the end hands back two regions
    // that together cover the request, and writing through both reaches the
    // host as one contiguous buffer.
    CHECK_EQ(
        call_method(buf, B_Lock, {kBytes - 8, 16, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
        DS_OK);
    uint32_t w1 = rd32(sc(0x300)), l1 = rd32(sc(0x304));
    uint32_t w2 = rd32(sc(0x308)), l2 = rd32(sc(0x30c));
    CHECK_EQ(l1, 8u);
    CHECK_EQ(l2, 8u);
    CHECK(w2 != 0);
    CHECK(w1 != w2);
    for (uint32_t i = 0; i < 8; ++i) {
        wr8(w1 + i, 0xA1);
        want[kBytes - 8 + i] = 0xA1;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        wr8(w2 + i, 0xB2);
        want[i] = 0xB2;
    }
    CHECK_EQ(call_method(buf, B_Unlock, {w1, l1, w2, l2}), DS_OK);
    g_plays.clear();
    CHECK_EQ(call_method(buf, B_Play, {0, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1)
        CHECK(g_plays[0].pcm == want);

    // A lock starting past the end is refused rather than wrapped.
    CHECK_EQ(call_method(buf, B_Lock, {kBytes, 4, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DSERR_INVALIDPARAM);

    // --- 8-bit mono is unsigned, centred on 0x80, and is not sign-converted
    // on the way to the host.
    uint32_t b8 = make_buffer(ds, 1, 11025, 8, 256, sc(8));
    CHECK(b8 != 0);
    CHECK_EQ(call_method(b8, B_Lock, {0, 256, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t q1 = rd32(sc(0x300));
    std::vector<uint8_t> want8(256);
    for (uint32_t i = 0; i < 256; ++i) {
        want8[i] = (uint8_t)i;
        wr8(q1 + i, (uint8_t)i);
    }
    CHECK_EQ(call_method(b8, B_Unlock, {q1, 256, 0, 0}), DS_OK);
    g_plays.clear();
    CHECK_EQ(call_method(b8, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].bits, 8);
        CHECK_EQ(g_plays[0].channels, 1);
        CHECK_EQ(g_plays[0].rate, 11025);
        CHECK_EQ(g_plays[0].loop, 1);
        CHECK(g_plays[0].pcm == want8);
    }

    // SetFrequency changes the rate the host is told, not the data.
    CHECK_EQ(call_method(b8, B_SetFrequency, {22050}), DS_OK);
    g_plays.clear();
    CHECK_EQ(call_method(b8, B_Play, {0, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].rate, 22050);
        CHECK(g_plays[0].pcm == want8);
    }

    // --- A duplicate shares the original's sample data: a write through the
    // original's Lock pointer is heard by the duplicate. DirectSound
    // duplicates the interface, not the samples.
    CHECK_EQ(call_method(ds, DS_DuplicateSoundBuffer, {buf, sc(0xc)}), DS_OK);
    uint32_t dup = rd32(sc(0xc));
    CHECK(dup != 0);
    CHECK(dup != buf);
    CHECK_EQ(call_method(buf, B_Lock, {0, 4, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t d1 = rd32(sc(0x300));
    for (uint32_t i = 0; i < 4; ++i) {
        wr8(d1 + i, 0x5C);
        want[i] = 0x5C;
    }
    CHECK_EQ(call_method(buf, B_Unlock, {d1, 4, 0, 0}), DS_OK);
    g_plays.clear();
    CHECK_EQ(call_method(dup, B_Play, {0, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK(g_plays[0].pcm == want);
        CHECK_EQ(g_plays[0].bits, 16);
        CHECK_EQ(g_plays[0].channels, 2);
    }
    // and it is a separate voice, so the two can sound at once.
    if (g_plays.size() == 1)
        CHECK(g_plays[0].channel >= 0);

    // --- GetFormat reports back exactly what the buffer was created with.
    uint32_t got = sc(0x400);
    gm_zero(got, 32);
    CHECK_EQ(call_method(buf, B_GetFormat, {got, 18, sc(0x420)}), DS_OK);
    CHECK_EQ(rd16(got + WFX_OFF_wFormatTag), WAVE_FORMAT_PCM);
    CHECK_EQ(rd16(got + WFX_OFF_nChannels), 2);
    CHECK_EQ(rd32(got + WFX_OFF_nSamplesPerSec), 22050u);
    CHECK_EQ(rd16(got + WFX_OFF_wBitsPerSample), 16);
    CHECK_EQ(rd16(got + WFX_OFF_nBlockAlign), 4);
    CHECK_EQ(rd32(got + WFX_OFF_nAvgBytesPerSec), 22050u * 4u);
}

// ---------------------------------------------------------------------------
// The video player's refill gate, driven the way the player drives it.
//
// 0057df10 returns the bytes from the game's own write offset forward to the
// play cursor, and 0057dc80 refills only when that reaches the length of the
// next chunk. So the cursor the shim reports is not a readout: it is the thing
// that decides whether the guest ever writes again. A cursor that sits a fixed
// distance ahead of the write offset - which is what "the last byte fed minus
// what is still in flight" reports, because the guest deliberately keeps about
// a lap in flight - stops the gate opening and the movie stalls on a black
// screen with no error anywhere.
//
// This runs the whole loop: poll, open the gate, lock, write, unlock, repeat,
// with a host that plays what it is given at the sample rate. It asserts the
// gate opens every time and the write offset goes right round the ring more
// than once, which is the property the stall broke.
// ---------------------------------------------------------------------------
// Plays `n` more bytes of the stream. The host's own cursor counts on from the
// offset it resumed at, at the sample rate, and never goes backwards; the
// queue drains behind it.
static void audio_play_bytes(uint64_t n) {
    g_stream_played += (uint32_t)n;
    g_queued_bytes = g_queued_bytes > n ? (uint32_t)(g_queued_bytes - n) : 0;
}

static void test_dsound_stream_pacing() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queues.clear();
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queued_accepted = 0;
    g_queue_enabled = true;
    g_test_audio_pos = 0;

    const uint32_t kRing = 32768; // the original's, at 22050/2/16
    const uint32_t kAlign = 4;
    uint32_t ds = make_dsound();
    uint32_t buf = make_buffer(ds, 2, 22050, 16, kRing, sc(4));
    CHECK(buf != 0);
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);

    // The player's own state: where it will write next, and the chunk sizes it
    // alternates between, both from the trace.
    uint32_t write = 0;
    const uint32_t chunks[2] = {3584, 3472};
    int refills = 0;
    int stalls = 0;
    uint32_t last_play = 0;
    bool converted = false;

    // Four hundred ten-millisecond ticks: forty seconds, several laps of a
    // ring that holds 0.37 of one.
    for (int tick = 0; tick < 400; ++tick) {
        uint32_t chunk = chunks[refills & 1];
        // Ten milliseconds of audio at 22050 Hz stereo 16-bit, whole frames.
        if (converted)
            audio_play_bytes(880);
        else
            g_test_audio_pos = (uint32_t)(((uint64_t)tick * 880) % kRing);

        CHECK_EQ(call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)}), DS_OK);
        uint32_t play = rd32(sc(0x530));
        CHECK(play < kRing);
        last_play = play;
        // It is the play cursor, so it is where the host actually is: where the
        // stream began plus what has been played since. Reporting anything
        // measured against how much is in flight instead gives a cursor that
        // tracks the guest's own write offset, which is the shape the video
        // player's gate cannot work with.
        if (converted) {
            uint32_t want = (uint32_t)(((uint64_t)g_stream_base + g_stream_played) % kRing);
            CHECK_EQ(play, want);
        }

        // 0057df10: the distance from the write offset forward to the cursor.
        uint32_t gap = play >= write ? play - write : play + kRing - write;
        if (gap < chunk) {
            ++stalls;
            continue;
        }

        CHECK_EQ(
            call_method(buf, B_Lock, {write, chunk, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
            DS_OK);
        uint32_t a1 = rd32(sc(0x300)), l1 = rd32(sc(0x304));
        uint32_t a2 = rd32(sc(0x308)), l2 = rd32(sc(0x30c));
        for (uint32_t i = 0; i < l1; ++i)
            wr8(a1 + i, (uint8_t)(0x40 + (refills & 0x3f)));
        for (uint32_t i = 0; i < l2; ++i)
            wr8(a2 + i, (uint8_t)(0x40 + (refills & 0x3f)));
        g_plays.clear();
        CHECK_EQ(call_method(buf, B_Unlock, {a1, l1, a2, l2}), DS_OK);
        if (!converted) {
            // The first refill converts the ring, and the conversion is not a
            // play: nothing is stopped and nothing restarts.
            CHECK_EQ(g_plays.size(), 0u);
            converted = true;
        } else {
            // and no refill after it restarts the voice.
            CHECK_EQ(g_plays.size(), 0u);
        }
        write = (write + chunk) % kRing;
        ++refills;
    }

    // The gate opened, over and over, and the write offset went right round
    // the ring more than once. A cursor pinned a fixed distance ahead of the
    // write offset gives one refill and then nothing at all.
    CHECK(refills >= 30);
    CHECK_EQ((uint32_t)(g_queues.size() > 0), 1u);
    CHECK(last_play < kRing);
    // Every byte the guest wrote reached the host, in order and none twice.
    uint64_t queued = 0;
    for (const PlayRecord &q : g_queues)
        queued += q.bytes;
    CHECK_EQ(queued, g_queued_accepted);
    CHECK(queued >= (uint64_t)kRing);

    // Running dry must not freeze the cursor, and this is the deadlock it
    // would otherwise be. The host has played everything it was given, so the
    // cursor arrives exactly at the guest's write offset and the gate wants a
    // whole chunk beyond it. On real hardware the cursor does not stop there:
    // it runs on into whatever is still in the ring, and the gate reopens. A
    // cursor that stops means the guest never writes, which means nothing is
    // ever queued, which means the cursor never moves again - and the movie
    // waits on it with nothing to show that anything is wrong.
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    uint32_t chunk = chunks[refills & 1];
    bool reopened = false;
    uint32_t first_dry = 0, last_dry = 0;
    // The gate wants forty milliseconds of audio, so this polls for up to two
    // seconds of real time. No check inside the loop: the assertion is that it
    // reopened at all, and one that reopens has nothing else to say.
    for (int i = 0; i < 400 && !reopened; ++i) {
        // The host paces a dry stream from its own clock; here the test is
        // the clock, so it advances it by a millisecond of audio a step.
        audio_play_bytes(880);
        call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)});
        uint32_t p = rd32(sc(0x530));
        if (!first_dry)
            first_dry = p ? p : 1;
        last_dry = p;
        uint32_t gap = p >= write ? p - write : p + kRing - write;
        if (gap >= chunk)
            reopened = true;
    }
    CHECK(reopened);
    CHECK(last_dry < kRing);

    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    g_queue_enabled = false;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queued_accepted = 0;
    g_queues.clear();
    g_test_audio_pos = 0;
}

// ---------------------------------------------------------------------------
// A streamed wave's buffer is the shim's, and freeing the wave has to free it.
//
// The game opens a wave per sound and frees the previous one before each new
// play - 388 of them in one scripted level, and a session is far longer than
// that. Nothing else ever frees the buffer this shim allocates for a streamed
// wave, so leaking it here leaks the guest's heap at the rate the game makes
// sounds. What that looks like from the outside is not a crash: the allocation
// eventually fails, read_wave_record refuses the wave, and the sounds stop.
// ---------------------------------------------------------------------------
static void test_qmixer_stream_buffer_lifetime() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queue_enabled = false;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 4, 2}), 0u);

    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 1);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 11025);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 11025 * 2);
    wr16(wfx + WFX_OFF_nBlockAlign, 2);
    wr16(wfx + WFX_OFF_wBitsPerSample, 16);

    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "LifetimeCallback",
        [](X86 *c) {
            uint32_t dst = arg(c, 0), bytes = arg(c, 1);
            for (uint32_t i = 0; i < bytes; ++i)
                wr8(dst + i, 0x40);
            set_eax(c, 1);
        },
        3);

    const uint32_t kChunk = 0x800;
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, kChunk);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    wr32(rec + QSOWD_OFF_pvContext, 0xF00D0000u);

    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    uint32_t free_wave = tramp("QMIXER.dll", "QSWaveMixFreeWave");
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");

    // The game's own shape: open, play, free the previous, open the next.
    uint32_t before = heap_stats().used_blocks;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < 40; ++i) {
        uint32_t h = call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED | 1});
        CHECK(h != 0);
        CHECK_EQ(call_shim(play, {hmix, 2, 0x20, h, 0, 0}), 0u);
        if (prev)
            CHECK_EQ(call_shim(free_wave, {hmix, prev}), 0u);
        prev = h;
    }
    CHECK_EQ(call_shim(free_wave, {hmix, prev}), 0u);
    uint32_t after = heap_stats().used_blocks;

    // Forty sounds, every one of them freed, and the heap is where it started.
    // Before this was fixed it grew by one block of the wave's buffer size per
    // sound and never came back.
    CHECK_EQ(after, before);

    // Closing the session releases the buffers of waves the game never freed.
    for (uint32_t i = 0; i < 5; ++i) {
        uint32_t h = call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED | 1});
        CHECK(h != 0);
    }
    CHECK(heap_stats().used_blocks > before);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixCloseSession"), {hmix}), 0u);
    CHECK_EQ(heap_stats().used_blocks, before);

    qmixer_reset();
}

// ---------------------------------------------------------------------------
// How far ahead of the sound the game's stream reader is allowed to run.
//
// This is not a buffering preference. The size the game hands OpenWaveEx is
// the size of ONE streaming buffer - 0x3c00 at 0056f90a in the play routine,
// 0x7800 when the flag at [edi+0x6a] is set - and QMixer fills that buffer and
// calls back when it needs the next one. The callback at 00576660 forwards to
// a virtual method on the game's own emitter and reports end of stream when
// that method returns 1, so every pull advances state inside the game, not
// inside the mixer.
//
// Pulling all four chunks at play time therefore ran the emitter about 1.4
// seconds of audio ahead of anything audible. Measured in the headless smoke,
// the effect on the game was large and one-directional:
//
//     chunks pulled per call    plays    position updates per play
//     4 (what this replaced)      387                         1.17
//     the original, for scale     136                        29.70
//
// The original gives a sound about thirty position updates because it is still
// playing thirty frames later. Ours got one, because the game had already been
// told the sound was over.
// ---------------------------------------------------------------------------
static uint32_t g_prefetch_calls = 0;

static void test_qmixer_stream_prefetch() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queue_enabled = true;
    g_prefetch_calls = 0;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 4, 2}), 0u);

    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 1);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 11025);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 11025 * 2);
    wr16(wfx + WFX_OFF_nBlockAlign, 2);
    wr16(wfx + WFX_OFF_wBitsPerSample, 16);

    // A source with far more to give than the buffer holds, which is the case
    // that separates "filled the buffer" from "drained the game".
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "PrefetchCallback",
        [](X86 *c) {
            uint32_t dst = arg(c, 0), bytes = arg(c, 1);
            for (uint32_t i = 0; i < bytes; ++i)
                wr8(dst + i, 0x30);
            ++g_prefetch_calls;
            set_eax(c, 1); // non-zero: there is more after this
        },
        3);

    const uint32_t kChunk = 0x800;
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, kChunk);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    wr32(rec + QSOWD_OFF_pvContext, 0xF00D0000u);

    uint32_t h =
        call_shim(tramp("QMIXER.dll", "QSWaveMixOpenWaveEx"), {hmix, rec, QSWAVEMIX_STREAMED | 1});
    CHECK(h != 0);

    // Opening a wave must not read from it. The game has not asked for the
    // sound yet, and a read here would advance its emitter before the play.
    CHECK_EQ(g_prefetch_calls, 0u);

    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixPlayEx"), {hmix, 2, 0x20, h, 0, 0}), 0u);

    // Two: one buffer playing and one waiting behind it. Not four, and not as
    // many as the source will give.
    CHECK_EQ(g_prefetch_calls, 2u);
    CHECK_EQ(g_plays.size(), 1u);

    // The pump tops the queue up as it drains, one buffer at a time rather
    // than emptying the source.
    uint32_t before = g_prefetch_calls;
    g_queued_bytes = 0;
    g_voice_remaining = 0; // the host has played what it had
    qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_prefetch_calls, before + 1);

    // And with a buffer still waiting it pulls nothing at all, so the reader
    // never gets further than one buffer ahead of the one being played.
    g_queued_bytes = kChunk;
    before = g_prefetch_calls;
    qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_prefetch_calls, before);

    // Nothing has told the game its sound is over, because it is not.
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixFreeWave"), {hmix, h}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixCloseSession"), {hmix}), 0u);
    qmixer_reset();
}

// ---------------------------------------------------------------------------
// Which host question the refill gate asks.
//
// It reads host_audio_voice_remaining_bytes, not host_audio_queued_bytes. The
// two are different on purpose. queued_bytes is the DirectSound streaming
// contract - how much of what THIS CALLER appended is still to play - and a
// play resets it, because a play starts a new sound rather than continuing
// one. QMixer does not append into a ring: it plays a sound on a named channel
// and refills behind it, so a play is exactly the moment it needs a large
// answer. Reading queued_bytes told the gate nothing was outstanding thirty
// milliseconds into a second-and-a-half sound; impl-t7 counted 635 refills in
// one run of the game that way.
//
// Folding the voice's sound into queued_bytes instead of adding a query was
// tried and rejected: it took the headless intro from no dropouts to
// thirty-one silent gaps, because a ring writer then waits out a whole
// re-issued lap before appending.
// ---------------------------------------------------------------------------
static uint32_t g_gate_calls = 0;

static void test_qmixer_refill_gate() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queues.clear();
    g_queue_enabled = true;
    g_gate_calls = 0;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 4, 2}), 0u);

    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 2);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 11025);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 11025 * 4);
    wr16(wfx + WFX_OFF_nBlockAlign, 4);
    wr16(wfx + WFX_OFF_wBitsPerSample, 16);

    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "GateCallback",
        [](X86 *c) {
            uint32_t dst = arg(c, 0), bytes = arg(c, 1);
            for (uint32_t i = 0; i < bytes; ++i)
                wr8(dst + i, 0x60);
            ++g_gate_calls;
            set_eax(c, 1); // always more to come
        },
        3);

    const uint32_t kChunk = 0x3c00; // the size the game asks for
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, kChunk);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    wr32(rec + QSOWD_OFF_pvContext, 0xF00D0000u);

    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");

    uint32_t h1 = call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED | 1});
    CHECK(h1 != 0);
    CHECK_EQ(call_shim(play, {hmix, 2, 0x20, h1, 0, 0}), 0u);

    // The play primes two buffers: one submitted, one appended behind it. So
    // the voice has both in front of it and queued_bytes counts only the
    // appended one, because a play resets it and the submitted buffer is the
    // play. That difference of exactly one buffer is the defect: the gate
    // reading queued_bytes is told half of what is really ahead of the voice,
    // and on a channel whose whole sound arrived with the play it is told
    // nothing is ahead at all.
    CHECK_EQ(host_audio_voice_remaining_bytes(g_plays.back().channel), 2 * kChunk);
    CHECK_EQ(g_queued_bytes, kChunk);

    // So the pump leaves the guest alone while the voice is full.
    uint32_t settled = g_gate_calls;
    for (int i = 0; i < 20; ++i)
        qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_gate_calls, settled);

    // Drain it below a buffer and exactly one refill follows.
    g_voice_remaining = kChunk / 2;
    qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_gate_calls, settled + 1);

    // The knob puts the old gate back, and the old gate is the defect: with
    // the voice holding two buffers and nothing appended behind it,
    // queued_bytes reads zero and the pump refills a channel that needs
    // nothing. That is the difference the contract argument rests on, so it
    // is asserted here rather than only described.
    setenv("POPM_QMIX_GATE", "queue", 1);
    qmixer_gate_reset_for_test();
    g_voice_remaining = 2 * kChunk;
    g_queued_bytes = 0;
    uint32_t old_gate = g_gate_calls;
    qmixer_frame_pump(&g_cpu);
    CHECK(g_gate_calls > old_gate);
    unsetenv("POPM_QMIX_GATE");
    qmixer_gate_reset_for_test();

    // A play that replaces a sound still playing reports the NEW sound's
    // length, not zero and not what was left of the old one.
    uint32_t h2 = call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED | 1});
    CHECK(h2 != 0);
    CHECK_EQ(call_shim(play, {hmix, 2, 0x20, h2, 0, 0}), 0u);
    // Its own two buffers, and nothing of what it replaced.
    CHECK_EQ(host_audio_voice_remaining_bytes(g_plays.back().channel),
             g_plays.back().bytes + kChunk);
    CHECK_EQ(g_plays.back().bytes, kChunk);
    settled = g_gate_calls;
    for (int i = 0; i < 20; ++i)
        qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_gate_calls, settled); // and is not refilled at once

    // Played through frame by frame, the voice never reaches zero: every
    // refill lands while a buffer is still in front of it. A dropout here is
    // a silent gap in the game.
    uint32_t dropouts = 0;
    const uint32_t heard = 11025 * 4 / 60 / 4 * 4; // a frame, whole samples
    for (int frame = 0; frame < 300; ++frame) {
        g_voice_remaining = g_voice_remaining > heard ? g_voice_remaining - heard : 0;
        qmixer_frame_pump(&g_cpu);
        if (g_voice_remaining == 0)
            ++dropouts;
    }
    CHECK_EQ(dropouts, 0u);

    g_queue_enabled = false;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queues.clear();
    qmixer_reset();
}

// ---------------------------------------------------------------------------
// QMixer's volume scale, which is the library's and not a guess.
//
// QMixer.dll's parameter handler for the volume tag is three instructions:
//
//     18006d0b  fild  dword ptr [edi + 4]        ; the value the caller passed
//     18006d0e  fmul  dword ptr [0x180244d4]     ; 3.051851e-05, which is 1/32767
//     ...       fstp  dword ptr [esi + 0x50]     ; the channel's gain
//
// So it is a linear amplitude on 0 to 32767. The game passes 14190, which is a
// gain of 0.433 and about -7.3 dB; read as hundredths of a decibel, as it was,
// every positive number clamped to unity and every sound played at full
// volume with no mix at all.
// ---------------------------------------------------------------------------
static void test_qmixer_volume_scale() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queue_enabled = false;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);

    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 1);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 22050);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 22050);
    wr16(wfx + WFX_OFF_nBlockAlign, 1);
    wr16(wfx + WFX_OFF_wBitsPerSample, 8);
    uint32_t data = sc(0x1000);
    for (uint32_t i = 0; i < 64; ++i)
        wr8(data + i, (uint8_t)i);
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, data);
    wr32(rec + QSOWD_OFF_dwDataSize, 64);
    uint32_t hwave = call_shim(tramp("QMIXER.dll", "QSWaveMixOpenWaveEx"), {hmix, rec, 8});
    CHECK(hwave != 0);
    uint32_t set_vol = tramp("QMIXER.dll", "QSWaveMixSetVolume");
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");

    struct Case {
        uint32_t qmix;
        int32_t millibels;
    };
    const Case cases[] = {
        {32767, 0},    // full scale
        {40000, 0},    // above it, still full scale
        {16384, -602}, // half amplitude, about -6 dB
        {14190, -727}, // what the game passes
        {3277, -2000}, // a tenth
        {1, -9031},    // the quietest the scale can express, short of zero
        {0, -10000},   // silence, which zero means on a linear scale
    };
    uint32_t ch = 0;
    for (const Case &t : cases) {
        CHECK_EQ(call_shim(set_vol, {hmix, ch, 0, t.qmix}), 0u);
        g_plays.clear();
        CHECK_EQ(call_shim(play, {hmix, ch, 0x20, hwave, 0, 0}), 0u);
        CHECK_EQ(g_plays.size(), 1u);
        if (g_plays.size() == 1)
            CHECK_EQ(g_plays[0].volume, t.millibels);
        ++ch;
    }

    // A channel nobody has set a volume on is at full scale, not silent: zero
    // is silence on this scale, so the default cannot be zero.
    g_plays.clear();
    CHECK_EQ(call_shim(play, {hmix, 20, 0x20, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1)
        CHECK_EQ(g_plays[0].volume, 0);

    qmixer_reset();
}

// ---------------------------------------------------------------------------
// A streamed wave is refilled without the game ever calling QSWaveMixPump.
//
// This is the shape of the bug that made the menu music play its opening
// second and stop. Real QMixer runs its own mixing thread, so QSWaveMixPump is
// for an application that wants to drive the mixer by hand and this game does
// not: zero calls in a whole run. Everything that refilled a streamed wave
// hung off that call, so nothing ever refilled one.
//
// The tick comes from qmixer_frame_pump now, which dx_register_shims hands to
// host_set_frame_pump and the guest's own message loop runs between frames.
// What is asserted here is the property that was missing: with QSWaveMixPump
// never called, the stream is still fed, and it is fed by appending rather
// than by starting the sound again.
// ---------------------------------------------------------------------------
static void test_qmixer_frame_pump() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queues.clear();
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queued_accepted = 0;
    g_queue_enabled = true;
    g_ch_streaming = false;
    g_test_audio_pos = 0;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 15, 2}), 0u);

    // 11025 Hz stereo 16-bit, which is what the menu music actually is.
    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 2);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 11025);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 11025 * 4);
    wr16(wfx + WFX_OFF_nBlockAlign, 4);
    wr16(wfx + WFX_OFF_wBitsPerSample, 16);

    static uint32_t g_fp_calls = 0;
    static uint8_t g_fp_fill = 0x10;
    g_fp_calls = 0;
    g_fp_fill = 0x10;
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "FramePumpCallback",
        [](X86 *c) {
            uint32_t dst = arg(c, 0), bytes = arg(c, 1);
            for (uint32_t i = 0; i < bytes; ++i)
                wr8(dst + i, g_fp_fill);
            ++g_fp_calls;
            set_eax(c, 1); // non-zero: more will follow
        },
        3);

    const uint32_t kChunk = 0x400;
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, kChunk);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    wr32(rec + QSOWD_OFF_pvContext, 0xBEEF0000u);
    uint32_t hwave =
        call_shim(tramp("QMIXER.dll", "QSWaveMixOpenWaveEx"), {hmix, rec, QSWAVEMIX_STREAMED | 1});
    CHECK(hwave != 0);

    // Play. One host play to start the sound, and the channel becomes a stream
    // on the same call: until it is one the host counts the buffer that
    // started it as the whole sound and retires the voice when it runs out.
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixPlayEx"), {hmix, 0, 0x20, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].rate, 11025);
        CHECK_EQ(g_plays[0].channels, 2);
        CHECK_EQ(g_plays[0].bits, 16);
        CHECK_EQ(g_plays[0].loop, 0);
    }
    CHECK(g_ch_streaming);

    // Now play it, without ever calling QSWaveMixPump. The frame pump is what
    // the message loop runs, so this is that loop.
    g_plays.clear();
    g_queues.clear();
    uint32_t before_calls = g_fp_calls;
    g_fp_fill = 0x20;
    for (int frame = 0; frame < 40; ++frame) {
        // A frame's worth of audio has been heard since the last one. Both
        // counters fall: the voice has that much less of its sound left, and
        // the queue behind it that much less appended. The refill gate reads
        // the voice, so a test that drained only the queue would have the
        // gate looking at a number nothing ever moved.
        uint32_t heard = 11025 * 4 / 60;
        heard -= heard % 4;
        g_queued_bytes = g_queued_bytes > heard ? g_queued_bytes - heard : 0;
        g_voice_remaining = g_voice_remaining > heard ? g_voice_remaining - heard : 0;
        qmixer_frame_pump(&g_cpu);
    }

    CHECK(g_fp_calls > before_calls); // the guest was asked for more
    CHECK(!g_queues.empty());         // and it reached the host
    CHECK_EQ(g_plays.size(), 0u);     // without restarting the sound
    bool right = !g_queues.empty();
    for (const PlayRecord &q : g_queues) {
        if (q.bytes % kChunk != 0)
            right = false;
        for (uint8_t b : q.pcm)
            if (b != 0x20)
                right = false;
    }
    CHECK(right); // the callback's bytes, unaltered

    // And it does not pull every frame: with the queue still deep enough the
    // pump leaves the guest alone.
    g_queued_bytes = 8 * kChunk;
    g_voice_remaining = 8 * kChunk;
    uint32_t settled = g_fp_calls;
    for (int frame = 0; frame < 10; ++frame)
        qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_fp_calls, settled);

    // The frame pump is re-entrant-safe: the callback it ends in is guest code
    // and guest code reaches the message loop.
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixStopChannel"), {hmix, 0, 0}), 0u);
    g_plays.clear();
    g_queues.clear();
    for (int frame = 0; frame < 5; ++frame)
        qmixer_frame_pump(&g_cpu);
    CHECK_EQ(g_queues.size(), 0u); // a stopped channel is left alone
    CHECK_EQ(g_plays.size(), 0u);

    g_queue_enabled = false;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queued_accepted = 0;
    g_queues.clear();
    g_ch_streaming = false;
    qmixer_reset();
}

// ---------------------------------------------------------------------------
// QMixer channel management, against what the game actually calls.
//
// Two things here were wrong in a way no test would have caught, because both
// were guesses about an SDK whose header this project does not have, and the
// evidence for both is the game's own calls: QSWaveMixOpenChannel(hMix, 15, 2)
// followed by plays on channel 0, and QSWaveMixEnableChannel(hMix, 0, 8, 0)
// followed immediately by a play on channel 0 that has to be heard.
//
// QMixer.dll settles the first: its OpenChannel switches on the third argument
// with `cmp eax,3 / ja` and a four-entry jump table, so 2 is one of four modes
// and, given the game then uses channels 0 upwards, it is a count.
// ---------------------------------------------------------------------------
static void test_qmixer_channels() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queue_enabled = false;

    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);

    // A static wave, the kind a sound effect is.
    uint32_t wfx = sc(0xc00);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
    wr16(wfx + WFX_OFF_nChannels, 1);
    wr32(wfx + WFX_OFF_nSamplesPerSec, 22050);
    wr32(wfx + WFX_OFF_nAvgBytesPerSec, 22050);
    wr16(wfx + WFX_OFF_nBlockAlign, 1);
    wr16(wfx + WFX_OFF_wBitsPerSample, 8);
    uint32_t data = sc(0x1000);
    for (uint32_t i = 0; i < 256; ++i)
        wr8(data + i, (uint8_t)(i ^ 0x5a));
    uint32_t rec = sc(0x1800);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, data);
    wr32(rec + QSOWD_OFF_dwDataSize, 256);
    uint32_t hwave = call_shim(tramp("QMIXER.dll", "QSWaveMixOpenWaveEx"), {hmix, rec, 8});
    CHECK(hwave != 0);

    // The game's own call: fifteen channels, not channel fifteen.
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 15, 2}), 0u);

    // EnableChannel with a zero fourth argument, then a play on the same
    // channel. The play has to be heard: the game does exactly this and every
    // sound on the channel was being dropped.
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixEnableChannel"), {hmix, 0, 8, 0}), 0u);
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");
    g_plays.clear();
    CHECK_EQ(call_shim(play, {hmix, 0, 0x20, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].bits, 8);
        CHECK_EQ(g_plays[0].channels, 1);
        CHECK_EQ(g_plays[0].bytes, 256u);
    }

    // Every one of the fifteen sounds, not just the one that was opened by
    // index. Each gets its own host voice so they can sound at once.
    g_plays.clear();
    for (uint32_t ch = 1; ch < 15; ++ch)
        CHECK_EQ(call_shim(play, {hmix, ch, 0x20, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 14u);
    bool distinct = true;
    for (size_t i = 1; i < g_plays.size(); ++i)
        if (g_plays[i].channel == g_plays[i - 1].channel)
            distinct = false;
    CHECK(distinct);

    // Opening all of them is the other documented mode.
    qmixer_reset();
    hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 0, 1}), 0u);

    // A channel index past the end of the table is still refused rather than
    // growing it without bound.
    hwave = call_shim(tramp("QMIXER.dll", "QSWaveMixOpenWaveEx"), {hmix, rec, 8});
    CHECK(hwave != 0);
    g_plays.clear();
    CHECK(call_shim(play, {hmix, 4096, 0x20, hwave, 0, 0}) != 0u);
    CHECK_EQ(g_plays.size(), 0u);

    qmixer_reset();
}

// ---------------------------------------------------------------------------
// Formats, end to end. Whatever the guest declared in its WAVEFORMATEX is what
// the host is told, and the bytes are handed over untouched - 8-bit PCM is
// unsigned and centred on 0x80, 16-bit is signed little-endian, and a mono
// buffer stays mono rather than being widened here. All four combinations are
// checked because a shim that gets the sample width right and the channel
// count wrong plays at half speed, and one that gets the sign wrong plays a
// buzz, and both arrive as "garbled".
//
// GetCaps is checked alongside them because the game reads its own buffer size
// back from it rather than remembering it: 0057e1e0 creates the streaming
// buffer, calls GetCaps, stores dwBufferBytes in its own record, and falls
// back to 0x8000 only when there is no device at all.
// ---------------------------------------------------------------------------
static void test_dsound_formats() {
    cpu_reset();
    g_plays.clear();
    g_queue_enabled = false;
    g_test_audio_pos = 0;
    uint32_t ds = make_dsound();

    struct Case {
        uint32_t chans, rate, bits, bytes, align;
    };
    const Case cases[] = {
        {1, 11025, 8, 256, 1},   // 8-bit mono
        {2, 22050, 8, 512, 2},   // 8-bit stereo
        {1, 22050, 16, 512, 2},  // 16-bit mono
        {2, 22050, 16, 1024, 4}, // 16-bit stereo, the game's own format
    };
    uint32_t slot = 0x600;
    for (const Case &t : cases) {
        uint32_t buf = make_buffer(ds, t.chans, t.rate, t.bits, t.bytes, sc(slot));
        slot += 4;
        CHECK(buf != 0);

        // A pattern that is wrong in a visibly different way for every
        // mistake: the two channels differ and consecutive frames differ.
        std::vector<uint8_t> want(t.bytes);
        CHECK_EQ(
            call_method(buf, B_Lock, {0, t.bytes, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
            DS_OK);
        uint32_t p = rd32(sc(0x300));
        CHECK_EQ(rd32(sc(0x304)), t.bytes);
        uint32_t frames = t.bytes / t.align;
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t ch = 0; ch < t.chans; ++ch) {
                if (t.bits == 8) {
                    // Unsigned, silence at 0x80, a different level per side.
                    want[f * t.align + ch] = (uint8_t)(0x80 + (int)(f % 100) - 50 + (int)ch * 3);
                } else {
                    int16_t v = (int16_t)((int)(f % 200) * 150 - 15000 + (int)ch * 7);
                    want[f * t.align + ch * 2 + 0] = (uint8_t)(v & 0xff);
                    want[f * t.align + ch * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
                }
            }
        }
        for (uint32_t i = 0; i < t.bytes; ++i)
            wr8(p + i, want[i]);
        CHECK_EQ(call_method(buf, B_Unlock, {p, t.bytes, 0, 0}), DS_OK);

        g_plays.clear();
        CHECK_EQ(call_method(buf, B_Play, {0, 0, 0}), DS_OK);
        CHECK_EQ(g_plays.size(), 1u);
        if (g_plays.size() == 1) {
            const PlayRecord &r = g_plays[0];
            CHECK_EQ((uint32_t)r.rate, t.rate);
            CHECK_EQ((uint32_t)r.channels, t.chans);
            CHECK_EQ((uint32_t)r.bits, t.bits);
            CHECK_EQ(r.bytes, t.bytes);
            CHECK(r.pcm == want); // byte for byte, no conversion here
        }

        // GetFormat reports what it was created with, block align included.
        uint32_t got = sc(0x400);
        gm_zero(got, 32);
        CHECK_EQ(call_method(buf, B_GetFormat, {got, 18, sc(0x420)}), DS_OK);
        CHECK_EQ(rd16(got + WFX_OFF_wFormatTag), WAVE_FORMAT_PCM);
        CHECK_EQ((uint32_t)rd16(got + WFX_OFF_nChannels), t.chans);
        CHECK_EQ(rd32(got + WFX_OFF_nSamplesPerSec), t.rate);
        CHECK_EQ((uint32_t)rd16(got + WFX_OFF_wBitsPerSample), t.bits);
        CHECK_EQ((uint32_t)rd16(got + WFX_OFF_nBlockAlign), t.align);
        CHECK_EQ(rd32(got + WFX_OFF_nAvgBytesPerSec), t.rate * t.align);

        // GetCaps reports the size the game reads its own buffer length from.
        uint32_t caps = sc(0x440);
        gm_zero(caps, 24);
        wr32(caps, 20);
        CHECK_EQ(call_method(buf, B_GetCaps, {caps}), DS_OK);
        CHECK_EQ(rd32(caps + 8), t.bytes);
        CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    }

    // A QMixer wave carries its own WAVEFORMATEX and is handed over the same
    // way, so the same matrix goes through that path too.
    qmixer_reset();
    uint32_t hmix = call_shim(tramp("QMIXER.dll", "QSWaveMixInitEx"), {0});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");
    uint32_t ch = 0;
    for (const Case &t : cases) {
        uint32_t wfx = sc(0xc00);
        gm_zero(wfx, SDK_WAVEFORMATEX);
        wr16(wfx + WFX_OFF_wFormatTag, WAVE_FORMAT_PCM);
        wr16(wfx + WFX_OFF_nChannels, (uint16_t)t.chans);
        wr32(wfx + WFX_OFF_nSamplesPerSec, t.rate);
        wr32(wfx + WFX_OFF_nAvgBytesPerSec, t.rate * t.align);
        wr16(wfx + WFX_OFF_nBlockAlign, (uint16_t)t.align);
        wr16(wfx + WFX_OFF_wBitsPerSample, (uint16_t)t.bits);
        // Well clear of everything else in the scratch block: the largest
        // case here is a kilobyte of samples.
        uint32_t data = sc(0x1000);
        std::vector<uint8_t> want(t.bytes);
        for (uint32_t i = 0; i < t.bytes; ++i) {
            want[i] = (uint8_t)(i * 13 + t.bits);
            wr8(data + i, want[i]);
        }
        uint32_t rec = sc(0x1800);
        gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
        wr32(rec + QSOWD_OFF_lpFormat, wfx);
        wr32(rec + QSOWD_OFF_lpData, data);
        wr32(rec + QSOWD_OFF_dwDataSize, t.bytes);
        uint32_t hwave = call_shim(open_wave, {hmix, rec, 8});
        CHECK(hwave != 0);
        CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, ch, 0}), 0u);
        g_plays.clear();
        CHECK_EQ(call_shim(play, {hmix, ch, 0, hwave, 0, 0}), 0u);
        CHECK_EQ(g_plays.size(), 1u);
        if (g_plays.size() == 1) {
            CHECK_EQ((uint32_t)g_plays[0].rate, t.rate);
            CHECK_EQ((uint32_t)g_plays[0].channels, t.chans);
            CHECK_EQ((uint32_t)g_plays[0].bits, t.bits);
            CHECK(g_plays[0].pcm == want);
        }
        ++ch;
    }
    qmixer_reset();
}

// ---------------------------------------------------------------------------
// Streaming into a looping DirectSound buffer, which is how the game plays its
// music and speech. The numbers are the original's, from the Wine trace of
// buffer 0120FDF8: 32768 bytes, 22050 Hz stereo 16-bit, Play(0, 0,
// DSBPLAY_LOOPING) while the ring is still empty, then a thread polling
// GetCurrentPosition every ten milliseconds and locking three or four
// kilobytes at a running offset - 0, 3584, 7056, 10640 - about a lap ahead of
// the play cursor. No notification positions anywhere on that path.
//
// What is asserted is that the ring stops being a loop the moment the guest
// writes into it: the samples are appended, the voice is not restarted, and
// the play cursor the guest polls follows what the host has actually heard.
// ---------------------------------------------------------------------------
static void test_dsound_stream() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_queues.clear();
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queue_enabled = true;
    g_test_audio_pos = 0;

    uint32_t ds = make_dsound();
    const uint32_t kRing = 32768;
    uint32_t buf = make_buffer(ds, 2, 22050, 16, kRing, sc(4));
    CHECK(buf != 0);
    CHECK_EQ(call_method(buf, B_SetFrequency, {22050}), DS_OK);
    CHECK_EQ(call_method(buf, B_SetVolume, {0}), DS_OK);

    // Play finds the ring empty, exactly as the original does, and the host is
    // handed a loop because nothing has written into it yet.
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].loop, 1);
        CHECK_EQ(g_plays[0].bytes, kRing);
        CHECK_EQ(g_plays[0].rate, 22050);
        CHECK_EQ(g_plays[0].channels, 2);
        CHECK_EQ(g_plays[0].bits, 16);
        CHECK_EQ(g_plays[0].start_offset, 0u);
    }

    // The original's first poll before its first lock: playpos 3760, and a
    // write cursor ten milliseconds ahead of it.
    g_test_audio_pos = 3760;
    CHECK_EQ(call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)}), DS_OK);
    CHECK_EQ(rd32(sc(0x530)), 3760u);
    CHECK_EQ(rd32(sc(0x534)), 3760u + 880u);

    // The first refill. The ring becomes a stream: the rest of the lap is
    // re-issued as a one-shot from the live cursor and the guest's bytes are
    // appended behind it.
    g_plays.clear();
    g_queues.clear();
    CHECK_EQ(call_method(buf, B_Lock, {0, 3584, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t p1 = rd32(sc(0x300));
    CHECK_EQ(rd32(sc(0x304)), 3584u);
    std::vector<uint8_t> chunk(3584);
    for (uint32_t i = 0; i < 3584; ++i) {
        chunk[i] = (uint8_t)(i * 7 + 1);
        wr8(p1 + i, chunk[i]);
    }
    CHECK_EQ(call_method(buf, B_Unlock, {p1, 3584, 0, 0}), DS_OK);

    // The conversion is not a play: host_audio_stream turns the loop into a
    // stream at the cursor, so nothing is stopped and nothing restarts.
    CHECK_EQ(g_plays.size(), 0u);
    CHECK_EQ(g_stream_base, 3760u);
    CHECK_EQ(g_queues.size(), 1u);
    if (g_queues.size() == 1) {
        CHECK_EQ(g_queues[0].bytes, 3584u);
        CHECK(g_queues[0].pcm == chunk); // the guest's bytes, unaltered
    }

    // The cursor the guest polls is the host's own count of what it has
    // played, and it has not moved: nothing has been heard since the last look.
    CHECK_EQ(call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)}), DS_OK);
    CHECK_EQ(rd32(sc(0x530)), 3760u);

    // Every refill after that is an append and nothing else. The offsets are
    // the original's.
    const uint32_t offs[] = {3584, 7056, 10640};
    const uint32_t lens[] = {3472, 3584, 3472};
    const uint32_t poll[] = {8464, 12228, 15052};
    for (int i = 0; i < 3; ++i) {
        g_plays.clear();
        g_queues.clear();
        g_stream_played = poll[i] - 3760;
        CHECK_EQ(call_method(buf, B_Lock,
                             {offs[i], lens[i], sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
                 DS_OK);
        uint32_t q = rd32(sc(0x300));
        for (uint32_t k = 0; k < lens[i]; ++k)
            wr8(q + k, (uint8_t)(0x20 + i));
        CHECK_EQ(call_method(buf, B_Unlock, {q, lens[i], 0, 0}), DS_OK);
        CHECK_EQ(g_plays.size(), 0u); // the voice is never restarted
        CHECK_EQ(g_queues.size(), 1u);
        if (g_queues.size() == 1)
            CHECK_EQ(g_queues[0].bytes, lens[i]);
        // and the cursor keeps tracking what the host has consumed.
        CHECK_EQ(call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)}), DS_OK);
        CHECK_EQ(rd32(sc(0x530)), poll[i]);
    }

    // A refill that runs off the end of the ring is two appends, because a run
    // that crosses the end of a ring is two runs in play order. This one also
    // jumps rather than continuing where the last left off, which is the
    // resync path: the shim says so once and follows the guest.
    g_plays.clear();
    g_queues.clear();
    CHECK_EQ(call_method(buf, B_Lock,
                         {kRing - 1024, 2048, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t w1 = rd32(sc(0x300)), w2 = rd32(sc(0x308));
    CHECK_EQ(rd32(sc(0x304)), 1024u);
    CHECK_EQ(rd32(sc(0x30c)), 1024u);
    for (uint32_t k = 0; k < 1024; ++k) {
        wr8(w1 + k, 0xE1);
        wr8(w2 + k, 0xE2);
    }
    CHECK_EQ(call_method(buf, B_Unlock, {w1, 1024, w2, 1024}), DS_OK);
    CHECK_EQ(g_plays.size(), 0u);
    CHECK_EQ(g_queues.size(), 2u);
    if (g_queues.size() == 2) {
        CHECK_EQ(g_queues[0].bytes, 1024u);
        CHECK_EQ(g_queues[1].bytes, 1024u);
        CHECK_EQ(g_queues[0].pcm[0], 0xE1);
        CHECK_EQ(g_queues[1].pcm[0], 0xE2);
    }

    // A host that refuses an append outright, with no stream to convert to
    // either, is the only case that goes back to re-submitting. The contract
    // says a stream that has momentarily run dry still accepts, so a refusal
    // here means the voice is gone rather than behind.
    // Stop ends the stream, and the Play after it is a loop again until the
    // guest writes: a new sound is not a continuation of the last one.
    g_plays.clear();
    g_queues.clear();
    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1)
        CHECK_EQ(g_plays[0].loop, 1);
    CHECK_EQ(g_queues.size(), 0u);

    // On a host that cannot continue a sound the ring stays a loop and the
    // refill is a re-submission: audible, but it plays.
    g_queue_enabled = false;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_plays.clear();
    g_queues.clear();
    g_test_audio_pos = 2048;
    CHECK_EQ(call_method(buf, B_Lock, {0, 1024, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t z = rd32(sc(0x300));
    for (uint32_t k = 0; k < 1024; ++k)
        wr8(z + k, 0x77);
    CHECK_EQ(call_method(buf, B_Unlock, {z, 1024, 0, 0}), DS_OK);
    CHECK_EQ(g_queues.size(), 0u);
    // One play: host_audio_stream says no without changing anything, so the
    // ring is never converted and the refill is a re-submission from the live
    // cursor. The buffer is marked, so it is asked once rather than at every
    // refill.
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].loop, 1);             // still a loop
        CHECK_EQ(g_plays[0].start_offset, 2048u); // from the live cursor
        CHECK_EQ(g_plays[0].bytes, kRing);
    }

    // Asked once: the next refill re-submits the loop and nothing else.
    g_plays.clear();
    g_test_audio_pos = 4096;
    CHECK_EQ(call_method(buf, B_Lock, {1024, 1024, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t z2 = rd32(sc(0x300));
    for (uint32_t k = 0; k < 1024; ++k)
        wr8(z2 + k, 0x78);
    CHECK_EQ(call_method(buf, B_Unlock, {z2, 1024, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].loop, 1);
        CHECK_EQ(g_plays[0].start_offset, 4096u);
    }

    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    g_test_audio_pos = 0;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queues.clear();
}

// ---------------------------------------------------------------------------
// DirectSound notification positions. The game streams its music by parking a
// worker thread on two events registered with IDirectSoundNotify and refilling
// whichever half of a looping buffer the play cursor has just left, so a shim
// that never signals them never gets a second half of anything. This drives
// the play cursor by hand and asserts the events fire exactly when the cursor
// reaches their offsets, including across the loop point.
// ---------------------------------------------------------------------------
static void test_dsound_notify() {
    cpu_reset();
    qmixer_reset();
    g_plays.clear();
    g_test_audio_pos = 0;

    uint32_t ds = make_dsound();
    CHECK(ds != 0);
    const uint32_t kBytes = 1024, kHalf = 512;
    uint32_t buf = make_buffer(ds, 2, 22050, 16, kBytes, sc(4));
    CHECK(buf != 0);

    // IID_IDirectSoundNotify, which the game asks the buffer for.
    static const uint8_t IID_NOTIFY[16] = {0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11,
                                           0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16};
    uint32_t iid = sc(0x500);
    for (uint32_t i = 0; i < 16; ++i)
        wr8(iid + i, IID_NOTIFY[i]);
    CHECK_EQ(call_method(buf, B_QueryInterface, {iid, sc(0x510)}), DS_OK);
    uint32_t notify = rd32(sc(0x510));
    CHECK(notify != 0);
    CHECK(notify != buf); // a separate view on the same buffer

    // Two manual-reset events, as the game creates them.
    uint32_t createev = tramp("KERNEL32.dll", "CreateEventA");
    uint32_t reset = tramp("KERNEL32.dll", "ResetEvent");
    uint32_t wait = tramp("KERNEL32.dll", "WaitForSingleObject");
    uint32_t ev0 = call_shim(createev, {0, 1, 0, 0});
    uint32_t ev1 = call_shim(createev, {0, 1, 0, 0});
    CHECK(ev0 != 0);
    CHECK(ev1 != 0);
    CHECK(ev0 != ev1);

    // DSBPOSITIONNOTIFY is {dwOffset, hEventNotify}: the start of the buffer
    // and its half-way point, which is the pair the game registers.
    uint32_t list = sc(0x520);
    wr32(list + 0, 0);
    wr32(list + 4, ev0);
    wr32(list + 8, kHalf);
    wr32(list + 12, ev1);
    CHECK_EQ(call_method(notify, N_SetNotificationPositions, {2, list}), DS_OK);

    uint32_t pump = tramp("QMIXER.dll", "QSWaveMixPump");
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);

    // Nothing has moved yet, so nothing has been reached.
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev0, 0}), 0x102u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0x102u);

    // Part way into the first half: still nothing.
    g_test_audio_pos = kHalf - 4;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev0, 0}), 0x102u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0x102u);

    // Reaching the half-way point signals that position and only that one.
    g_test_audio_pos = kHalf;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0u);
    CHECK_EQ(call_shim(wait, {ev0, 0}), 0x102u);
    call_shim(reset, {ev1});

    // The cursor wraps. Offset 0 is reached only by wrapping, and that is
    // exactly when the game refills the second half.
    g_test_audio_pos = 8;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev0, 0}), 0u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0x102u);
    call_shim(reset, {ev0});

    // A second lap signals the half-way point again rather than firing once
    // for the life of the buffer.
    g_test_audio_pos = kHalf + 16;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0u);
    CHECK_EQ(call_shim(wait, {ev0, 0}), 0x102u);
    call_shim(reset, {ev1});

    // GetCurrentPosition reports the same cursor the notifications are
    // measured against.
    CHECK_EQ(call_method(buf, B_GetCurrentPosition, {sc(0x530), sc(0x534)}), DS_OK);
    CHECK_EQ(rd32(sc(0x530)), kHalf + 16u);

    // A refill through Lock/Unlock resubmits from where the cursor actually
    // is, not from the top of the buffer: a stream fed from offset 0 on every
    // refill is the sound of this going wrong. This test host cannot continue
    // a sound, so host_audio_stream refuses without changing anything and the
    // ring stays a loop.
    g_plays.clear();
    CHECK_EQ(call_method(buf, B_Lock, {0, kHalf, sc(0x300), sc(0x304), sc(0x308), sc(0x30c), 0}),
             DS_OK);
    uint32_t p1 = rd32(sc(0x300));
    for (uint32_t i = 0; i < kHalf; ++i)
        wr8(p1 + i, (uint8_t)(i & 0xff));
    CHECK_EQ(call_method(buf, B_Unlock, {p1, kHalf, 0, 0}), DS_OK);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        CHECK_EQ(g_plays[0].start_offset, kHalf + 16u);
        CHECK_EQ(g_plays[0].loop, 1);
    }

    // DSBPN_OFFSETSTOP is reported when the buffer stops.
    uint32_t evstop = call_shim(createev, {0, 1, 0, 0});
    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    wr32(list + 0, 0xffffffffu);
    wr32(list + 4, evstop);
    wr32(list + 8, kHalf);
    wr32(list + 12, ev1);
    CHECK_EQ(call_method(notify, N_SetNotificationPositions, {2, list}), DS_OK);
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    CHECK_EQ(call_shim(wait, {evstop, 0}), 0x102u);
    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    CHECK_EQ(call_shim(wait, {evstop, 0}), 0u);

    // Positions cannot be changed under a playing buffer, and one past the end
    // of the buffer is refused rather than kept where it could never fire.
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    CHECK_EQ(call_method(notify, N_SetNotificationPositions, {2, list}), DSERR_INVALIDCALL);
    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    wr32(list + 0, kBytes);
    CHECK_EQ(call_method(notify, N_SetNotificationPositions, {2, list}), DS_OK);
    g_test_audio_pos = 0;

    // Clearing the list stops everything firing.
    CHECK_EQ(call_method(notify, N_SetNotificationPositions, {0, 0}), DS_OK);
    CHECK_EQ(call_method(buf, B_Play, {0, 0, DSBPLAY_LOOPING}), DS_OK);
    g_test_audio_pos = kHalf;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(call_shim(wait, {ev1, 0}), 0x102u);
    CHECK_EQ(call_method(buf, B_Stop, {}), DS_OK);
    g_test_audio_pos = 0;
}

// ---------------------------------------------------------------------------
// QMixer streamed waves. The samples arrive through a guest callback that the
// pump calls, so the test supplies a real guest callback and asserts that the
// bytes it writes are the bytes the host is asked to play, that the pump pulls
// again when the previous buffer has been heard, and that a callback which
// reports it is finished ends the sound instead of repeating it.
// ---------------------------------------------------------------------------
static uint32_t g_stream_calls = 0;
static uint32_t g_stream_limit = 0; // calls before the source runs dry
static uint8_t g_stream_fill = 0;   // the byte the callback writes
static uint32_t g_stream_ctx_seen = 0;
static uint32_t g_stream_size_seen = 0;

static void test_qmixer_streaming() {
    cpu_reset();
    g_plays.clear();
    g_stops.clear();
    qmixer_reset();

    uint32_t init = tramp("QMIXER.dll", "QSWaveMixInitEx");
    uint32_t initdata = sc(0x100);
    gm_zero(initdata, 0x40);
    wr32(initdata + 0, 0x40);
    wr32(initdata + 8, 0x5622);
    uint32_t hmix = call_shim(init, {initdata});
    CHECK(hmix != 0);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixActivate"), {hmix, 1}), 0u);
    CHECK_EQ(call_shim(tramp("QMIXER.dll", "QSWaveMixOpenChannel"), {hmix, 1, 2}), 0u);

    // 22050 Hz, stereo, 16-bit: the format the game's music path uses.
    uint32_t wfx = sc(0x200);
    gm_zero(wfx, SDK_WAVEFORMATEX);
    wr16(wfx + 0x00, 1); // WAVE_FORMAT_PCM
    wr16(wfx + 0x02, 2); // channels
    wr32(wfx + 0x04, 22050);
    wr32(wfx + 0x08, 22050 * 4);
    wr16(wfx + 0x0c, 4);
    wr16(wfx + 0x0e, 16);
    wr16(wfx + 0x10, 0);

    // The guest callback, with the game's own convention: it fills the buffer
    // it is given and returns non-zero while more will follow, and zero on the
    // chunk that is the last one. The game's wrapper at 0x576660 is stdcall
    // with (buffer, bytes, context), and its caller at 0056f090 treats a zero
    // result as end-of-stream while still playing the chunk it just got.
    static uint32_t cb = imports_alloc_trampoline(
        "TEST", "StreamCallback",
        [](X86 *c) {
            uint32_t dst = arg(c, 0), bytes = arg(c, 1), ctx = arg(c, 2);
            g_stream_ctx_seen = ctx;
            g_stream_size_seen = bytes;
            for (uint32_t i = 0; i < bytes; ++i)
                wr8(dst + i, g_stream_fill);
            ++g_stream_calls;
            set_eax(c, g_stream_calls >= g_stream_limit ? 0u : 1u);
        },
        3);

    // The record the game builds: format, chunk size, callback, context.
    const uint32_t kChunk = 0x400;
    uint32_t rec = sc(0x300);
    gm_zero(rec, QSWAVEMIXOPENWAVEDATA_SIZE);
    wr32(rec + QSOWD_OFF_lpFormat, wfx);
    wr32(rec + QSOWD_OFF_lpData, kChunk);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    wr32(rec + QSOWD_OFF_pvContext, 0xC0FFEE00u);

    uint32_t open_wave = tramp("QMIXER.dll", "QSWaveMixOpenWaveEx");
    g_stream_calls = 0;
    g_stream_limit = 1000;
    g_stream_fill = 0x11;
    uint32_t hwave = call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED | 1});
    CHECK(hwave != 0);
    // Opening pulls nothing: the samples are wanted when the sound plays.
    CHECK_EQ(g_stream_calls, 0u);

    // Playing pulls the first buffer and hands it over immediately.
    uint32_t play = tramp("QMIXER.dll", "QSWaveMixPlayEx");
    g_plays.clear();
    CHECK_EQ(call_shim(play, {hmix, 1, 0, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    CHECK(g_stream_calls > 0);
    CHECK_EQ(g_stream_ctx_seen, 0xC0FFEE00u);
    CHECK_EQ(g_stream_size_seen, kChunk);
    uint32_t first_bytes = 0;
    if (g_plays.size() == 1) {
        const PlayRecord &r = g_plays[0];
        CHECK_EQ(r.rate, 22050);
        CHECK_EQ(r.channels, 2);
        CHECK_EQ(r.bits, 16);
        CHECK_EQ(r.loop, 0); // a stream is refilled, never host-looped
        CHECK(r.bytes >= kChunk);
        CHECK_EQ(r.bytes % kChunk, 0u);
        first_bytes = r.bytes;
        bool all = !r.pcm.empty();
        for (uint8_t b : r.pcm)
            if (b != 0x11)
                all = false;
        CHECK(all); // the callback's bytes, unaltered
    }

    // The pump does not refill while the buffer it handed over is still being
    // heard: the host copied those samples, so overwriting them early would
    // cut the sound short.
    uint32_t pump = tramp("QMIXER.dll", "QSWaveMixPump");
    g_test_audio_pos = 0;
    g_plays.clear();
    uint32_t before = g_stream_calls;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(g_plays.size(), 0u);
    CHECK_EQ(g_stream_calls, before);

    // Once it has been heard, the pump pulls again and hands over the next
    // buffer, with the new samples.
    g_test_audio_pos = first_bytes;
    g_stream_fill = 0x22;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        bool all = !g_plays[0].pcm.empty();
        for (uint8_t b : g_plays[0].pcm)
            if (b != 0x22)
                all = false;
        CHECK(all);
    }

    // The final chunk is played, not thrown away. The callback reports the end
    // by returning zero on the chunk it has just filled, so that chunk still
    // has to reach the host; only the pump after it has been heard stops the
    // channel.
    g_stream_limit = g_stream_calls + 1; // the very next call is the last one
    g_stream_fill = 0x33;
    g_plays.clear();
    g_test_audio_pos = first_bytes;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(g_plays.size(), 1u);
    if (g_plays.size() == 1) {
        // Exactly one chunk: the pull stopped at the callback that said so.
        CHECK_EQ(g_plays[0].bytes, kChunk);
        bool all = !g_plays[0].pcm.empty();
        for (uint8_t b : g_plays[0].pcm)
            if (b != 0x33)
                all = false;
        CHECK(all);
    }
    g_plays.clear();
    g_test_audio_pos = kChunk;         // the last chunk has been heard
    CHECK_EQ(call_shim(pump, {}), 0u); // so the channel stops
    CHECK_EQ(g_plays.size(), 0u);
    g_plays.clear();
    CHECK_EQ(call_shim(pump, {}), 0u); // and stays stopped
    CHECK_EQ(g_plays.size(), 0u);

    // --- On a host that can continue a sound, a refill is queued behind what
    // is still playing instead of replacing it. That is the difference between
    // a stream and a seam at every chunk boundary, so it is asserted rather
    // than assumed: no new host_audio_play, and the queued bytes are the
    // callback's own.
    g_queue_enabled = true;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queues.clear();
    g_plays.clear();
    g_stream_limit = 1000;
    g_stream_fill = 0x44;
    g_test_audio_pos = 0;
    CHECK_EQ(call_shim(play, {hmix, 1, 0, hwave, 0, 0}), 0u);
    CHECK_EQ(g_plays.size(), 1u); // the sound still starts with a play
    // and the queue behind it is filled on the same call. Waiting for a pump
    // would leave the first hand-over as the only one with nothing in front of
    // it, which is the one place a stream is late by construction.
    CHECK_EQ(g_queues.size(), 1u);
    if (g_queues.size() == 1) {
        bool all = !g_queues[0].pcm.empty();
        for (uint8_t b : g_queues[0].pcm)
            if (b != 0x44)
                all = false;
        CHECK(all);
    }

    // The pump continues it rather than restarting it. Once what was primed
    // has been heard, the next look tops it up again.
    g_plays.clear();
    g_queues.clear();
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_stream_fill = 0x55;
    g_test_audio_pos = first_bytes;
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(g_plays.size(), 0u); // continued, not restarted
    CHECK_EQ(g_queues.size(), 1u);
    if (g_queues.size() == 1) {
        CHECK(g_queues[0].bytes > 0);
        bool all = true;
        for (uint8_t b : g_queues[0].pcm)
            if (b != 0x55)
                all = false;
        CHECK(all);
    }

    // And while a whole buffer is still waiting, the pump leaves it alone
    // rather than pulling from the guest every frame. The queue has to be
    // told it is full for this: the refill above put one chunk in, and the
    // threshold is one chunk, so the pump is entitled to top it up until the
    // host reports that much waiting.
    g_queued_bytes = g_queues.empty() ? 0 : g_queues[0].bytes;
    uint32_t calls_before = g_stream_calls;
    g_queues.clear();
    CHECK_EQ(call_shim(pump, {}), 0u);
    CHECK_EQ(g_queues.size(), 0u);
    CHECK_EQ(g_stream_calls, calls_before);
    CHECK_EQ(g_plays.size(), 0u);

    g_queue_enabled = false;
    g_queued_bytes = 0;
    g_voice_remaining = 0;
    g_queues.clear();
    qmixer_reset();
    g_plays.clear();

    // A record with no callback is refused rather than opened silent.
    wr32(rec + QSOWD_OFF_pfnCallback, 0);
    CHECK_EQ(call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED}), 0u);
    wr32(rec + QSOWD_OFF_pfnCallback, cb);
    // So is an impossible chunk size.
    wr32(rec + QSOWD_OFF_lpData, 0);
    CHECK_EQ(call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED}), 0u);
    wr32(rec + QSOWD_OFF_lpData, 0x40000000u);
    CHECK_EQ(call_shim(open_wave, {hmix, rec, QSWAVEMIX_STREAMED}), 0u);

    qmixer_reset();
    g_test_audio_pos = 0;
}

// ---------------------------------------------------------------------------
// The original fills its textures by blitting into them, not by Load: the
// Wine trace has 2303 Blt and 3108 BltFast against zero
// IDirect3DTexture2::Load. A blit into a texture surface must therefore reach
// the renderer exactly as an Unlock does. A texture that is only ever written
// by blit and never re-uploaded draws as untextured geometry.
//
// The descriptors here are the ones the trace actually shows, including the
// complex single-level mipmap form that 1072 of the original's 1455
// CreateSurface calls use.
// ---------------------------------------------------------------------------
static uint32_t make_texture(uint32_t dd, uint32_t caps, uint32_t w, uint32_t h, uint32_t pf_flags,
                             uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a,
                             bool mipmapcount, uint32_t out) {
    uint32_t desc = sc(0x200);
    gm_zero(desc, DDSD_SIZE);
    wr32(desc + DDSD_OFF_dwSize, DDSD_SIZE);
    uint32_t flags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    if (mipmapcount)
        flags |= DDSD_MIPMAPCOUNT;
    wr32(desc + DDSD_OFF_dwFlags, flags);
    wr32(desc + DDSD_OFF_ddsCaps, caps);
    wr32(desc + DDSD_OFF_dwWidth, w);
    wr32(desc + DDSD_OFF_dwHeight, h);
    if (mipmapcount)
        wr32(desc + DDSD_OFF_dwMipMapCount, 1);
    uint32_t pf = desc + DDSD_OFF_ddpfPixelFormat;
    wr32(pf + DDPF_OFF_dwSize, DDPF_SIZE);
    wr32(pf + DDPF_OFF_dwFlags, pf_flags);
    wr32(pf + DDPF_OFF_dwRGBBitCount, bits);
    wr32(pf + DDPF_OFF_dwRBitMask, r);
    wr32(pf + DDPF_OFF_dwGBitMask, g);
    wr32(pf + DDPF_OFF_dwBBitMask, b);
    wr32(pf + DDPF_OFF_dwRGBAlphaBitMask, a);
    if (call_method(dd, DD_CreateSurface, {desc, out, 0}) != DD_OK)
        return 0;
    return rd32(out);
}

static uint32_t texture_handle_of(uint32_t dev, uint32_t surf, uint32_t out) {
    uint8_t tex2[16] = {0x02, 0x15, 0x28, 0x93, 0xF8, 0x8C, 0xD0, 0x11,
                        0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29};
    uint32_t iid = sc(0x40);
    for (int i = 0; i < 16; ++i)
        wr8(iid + (uint32_t)i, tex2[i]);
    if (call_method(surf, S_QueryInterface, {iid, out}) != S_OK)
        return 0;
    uint32_t tex = rd32(out);
    if (!tex)
        return 0;
    call_method(tex, TEX_GetHandle, {dev, sc(0x2c)});
    return tex;
}

static void test_blit_into_texture_uploads() {
    cpu_reset();
    g_uploads.clear();
    uint32_t create = tramp("DDRAW.dll", "DirectDrawCreate");
    call_shim(create, {0, sc(0), 0});
    uint32_t dd = rd32(sc(0));
    call_method(dd, DD_SetCooperativeLevel, {0x1234, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN});
    call_method(dd, DD_SetDisplayMode, {640, 480, 16});
    uint32_t dev = make_d3d_device();
    CHECK(dev != 0);

    // Every texture descriptor shape the trace shows, all 16-bit forms.
    struct Shape {
        const char *what;
        uint32_t caps;
        uint32_t w, h;
        uint32_t pff, bits, r, g, b, a;
        bool mip;
    };
    const Shape shapes[] = {
        {"complex mipmap 565 16x16",
         DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_MIPMAP, 16, 16, DDPF_RGB,
         16, 0xf800, 0x07e0, 0x001f, 0, true},
        {"complex mipmap 4444 32x32",
         DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_MIPMAP, 32, 32,
         DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000, true},
        {"sysmem 4444 32x32", DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, 32, 32,
         DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000, false},
        {"vidmem 565 128x128", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 128, 128, DDPF_RGB, 16,
         0xf800, 0x07e0, 0x001f, 0, false},
        {"sysmem 1555 64x64", DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, 64, 64,
         DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000, false},
    };
    for (const Shape &sh : shapes) {
        uint32_t t = make_texture(dd, sh.caps, sh.w, sh.h, sh.pff, sh.bits, sh.r, sh.g, sh.b, sh.a,
                                  sh.mip, sc(0x10));
        CHECK(t != 0);
        if (!t)
            continue;
        CHECK(texture_handle_of(dev, t, sc(0x1c)) != 0);

        // A source of the same shape, filled with a recognisable value.
        uint32_t src = make_texture(dd, DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, sh.w, sh.h, sh.pff,
                                    sh.bits, sh.r, sh.g, sh.b, sh.a, false, sc(0x14));
        CHECK(src != 0);
        if (!src)
            continue;
        uint32_t ld = sc(0x300);
        gm_zero(ld, DDSD_SIZE);
        wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
        CHECK_EQ(call_method(src, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
        uint32_t bits = rd32(ld + DDSD_OFF_lpSurface);
        uint32_t pitch = rd32(ld + DDSD_OFF_lPitch);
        for (uint32_t y = 0; y < sh.h; ++y)
            for (uint32_t x = 0; x < sh.w; ++x)
                wr16(bits + y * pitch + x * 2, (uint16_t)(0x1234 + x + y));
        CHECK_EQ(call_method(src, S_Unlock, {0}), DD_OK);

        // Blt into the texture: the renderer must be given the new pixels.
        g_uploads.clear();
        CHECK_EQ(call_method(t, S_Blt, {0, src, 0, DDBLT_WAIT, 0}), DD_OK);
        CHECK_EQ(g_uploads.size(), 1u);
        if (g_uploads.size() == 1) {
            const TextureUpload &u = g_uploads[0];
            CHECK_EQ((uint32_t)u.width, sh.w);
            CHECK_EQ((uint32_t)u.height, sh.h);
            CHECK_EQ((uint32_t)u.bpp, sh.bits);
            CHECK_EQ(u.rmask, sh.r);
            CHECK_EQ(u.amask, sh.a);
            bool ok = u.pixels.size() >= (size_t)u.pitch * sh.h;
            if (ok)
                for (uint32_t y = 0; y < sh.h && ok; ++y)
                    for (uint32_t x = 0; x < sh.w; ++x) {
                        const uint8_t *q = u.pixels.data() + (size_t)y * u.pitch + x * 2;
                        if ((uint16_t)(q[0] | (q[1] << 8)) != (uint16_t)(0x1234 + x + y)) {
                            ok = false;
                            break;
                        }
                    }
            CHECK(ok);
        }

        // BltFast too: the original uses it more than Blt.
        g_uploads.clear();
        CHECK_EQ(call_method(t, S_BltFast, {0, 0, src, 0, DDBLTFAST_WAIT}), DD_OK);
        CHECK_EQ(g_uploads.size(), 1u);
    }

    // A colour-keyed blit into a texture uploads too: the keyed pixels are the
    // ones left alone, and the rest still have to reach the renderer.
    uint32_t dst = make_texture(dd, DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 4, 4, DDPF_RGB, 16,
                                0xf800, 0x07e0, 0x001f, 0, false, sc(0x10));
    uint32_t src = make_texture(dd, DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, 4, 4, DDPF_RGB, 16,
                                0xf800, 0x07e0, 0x001f, 0, false, sc(0x14));
    CHECK(dst != 0 && src != 0);
    CHECK(texture_handle_of(dev, dst, sc(0x1c)) != 0);
    uint32_t ck = sc(0x500);
    wr32(ck + DDCK_OFF_lo, 0x0000);
    wr32(ck + DDCK_OFF_hi, 0x0000);
    CHECK_EQ(call_method(src, S_SetColorKey, {DDCKEY_SRCBLT, ck}), DD_OK);
    uint32_t ld = sc(0x300);
    gm_zero(ld, DDSD_SIZE);
    wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
    CHECK_EQ(call_method(src, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
    uint32_t sb = rd32(ld + DDSD_OFF_lpSurface), sp = rd32(ld + DDSD_OFF_lPitch);
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            wr16(sb + y * sp + x * 2, (uint16_t)((x == 0) ? 0x0000 : 0xBEEF));
    CHECK_EQ(call_method(src, S_Unlock, {0}), DD_OK);
    g_uploads.clear();
    CHECK_EQ(call_method(dst, S_BltFast, {0, 0, src, 0, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY}),
             DD_OK);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1) {
        const TextureUpload &u = g_uploads[0];
        bool ok = u.pixels.size() >= (size_t)u.pitch * 4;
        // Column 0 was keyed out and kept whatever was there; the rest copied.
        for (uint32_t y = 0; y < 4 && ok; ++y)
            for (uint32_t x = 1; x < 4; ++x) {
                const uint8_t *q = u.pixels.data() + (size_t)y * u.pitch + x * 2;
                if ((uint16_t)(q[0] | (q[1] << 8)) != 0xBEEF) {
                    ok = false;
                    break;
                }
            }
        CHECK(ok);
    }

    // An 8-bit texture written by blit resolves through the palette attached
    // to it, not just when it is written by Unlock. The original creates one
    // palettised texture and fills it the same way it fills the others.
    uint32_t p8 = make_texture(dd, DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, 4, 4,
                               DDPF_PALETTEINDEXED8 | DDPF_RGB, 8, 0, 0, 0, 0, false, sc(0x10));
    uint32_t p8src = make_texture(dd, DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE, 4, 4,
                                  DDPF_PALETTEINDEXED8 | DDPF_RGB, 8, 0, 0, 0, 0, false, sc(0x14));
    CHECK(p8 != 0 && p8src != 0);
    uint32_t ents = sc(0x800);
    gm_zero(ents, 256 * 4);
    wr32(ents + 5 * 4, 0x00FF8040u); // PALETTEENTRY is R,G,B,flags
    CHECK_EQ(
        call_method(dd, DD_CreatePalette, {DDPCAPS_8BIT | DDPCAPS_ALLOW256, ents, sc(0x18), 0}),
        DD_OK);
    uint32_t p8pal = rd32(sc(0x18));
    CHECK(p8pal != 0);
    CHECK_EQ(call_method(p8, S_SetPalette, {p8pal}), DD_OK);
    CHECK(texture_handle_of(dev, p8, sc(0x1c)) != 0);
    gm_zero(ld, DDSD_SIZE);
    wr32(ld + DDSD_OFF_dwSize, DDSD_SIZE);
    CHECK_EQ(call_method(p8src, S_Lock, {0, ld, DDLOCK_WAIT, 0}), DD_OK);
    uint32_t pb = rd32(ld + DDSD_OFF_lpSurface), pp = rd32(ld + DDSD_OFF_lPitch);
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            wr8(pb + y * pp + x, 5);
    CHECK_EQ(call_method(p8src, S_Unlock, {0}), DD_OK);
    g_uploads.clear();
    CHECK_EQ(call_method(p8, S_Blt, {0, p8src, 0, DDBLT_WAIT, 0}), DD_OK);
    CHECK_EQ(g_uploads.size(), 1u);
    if (g_uploads.size() == 1) {
        const TextureUpload &u = g_uploads[0];
        CHECK_EQ(u.bpp, 8);
        CHECK(u.has_palette);
        CHECK_EQ(u.pixels[0], 5u);                     // the index blitted in
        CHECK_EQ(u.palette[5] & 0xffffffu, 0x4080FFu); // and the colour it means
    }

    // A texture filled before anything asks for its handle is still uploaded
    // with those pixels rather than empty: the write marks it, GetHandle sends
    // it. The original calls GetHandle 2242 times for far fewer surfaces.
    uint32_t late = make_texture(dd, DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 4, 4, DDPF_RGB, 16,
                                 0xf800, 0x07e0, 0x001f, 0, false, sc(0x10));
    CHECK(late != 0);
    g_uploads.clear();
    CHECK_EQ(call_method(late, S_BltFast, {0, 0, src, 0, DDBLTFAST_WAIT}), DD_OK);
    CHECK_EQ(g_uploads.size(), 0u); // no handle yet, nothing to upload to
    CHECK(texture_handle_of(dev, late, sc(0x1c)) != 0);
    CHECK_EQ(g_uploads.size(), 1u); // and the handle brings the pixels
    if (g_uploads.size() == 1) {
        const uint8_t *q = g_uploads[0].pixels.data() + 2;
        CHECK_EQ((uint32_t)(uint16_t)(q[0] | (q[1] << 8)), 0xBEEFu);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    mem_init();
    imports_init();
    dx_register_shims();

    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch) {
        fprintf(stderr, "cannot allocate scratch\n");
        return 1;
    }

    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"vtable integrity", test_vtable_integrity},
        {"resolution depth lifetime", test_resolution_depth_lifetime},
        {"gradient, flip, present", test_gradient_flip},
        {"blt and colour key", test_blt_and_colorkey},
        {"display ABI", test_display_abi},
        {"record and coverage", test_record_basic_and_coverage},
        {"keyed blit coverage", test_keyed_blit_coverage_and_key_values},
        {"fill, upload, flags", test_fill_upload_and_flags},
        {"revisions and leases", test_revision_bumps_and_retained_lease},
        {"frame sealing", test_seal_events},
        {"unchanged texture uploads", test_unchanged_texture_uploads},
        {"frame command pool", test_frame_command_pool},
        {"release frees leases", test_frame_release_frees_leases},
        {"presenter seal and retire", test_presenter_seal_hook_and_retirement_queue},
        {"screen class", test_screen_class},
        {"access counts", test_access_counts_by_reason},
        {"cursor learned", test_cursor_surface_learned},
        {"palette-only frames", test_palette_only_frames_seal},
        {"offscreen flip", test_offscreen_flip_does_not_seal},
        {"v4 unlock rect", test_v4_unlock_takes_a_rect},
        {"lock write records", test_lock_write_records},
        {"lock clusters", test_lock_diff_partial_records_and_payload},
        {"DC write diff", test_getdc_releasedc},
        {"palette versions", test_palette_versions},
        {"storage generations", test_storage_generations},
        {"draw snapshot is deep", test_draw_snapshot_is_deep},
        {"no reader/no readback", test_no_reader_no_readback},
        {"first HUD/overlay", test_first_hud_boundary_and_overlay_pass},
        {"overlay mapping", test_overlay_mapping_rule},
        {"interleave fallback", test_inexpressible_interleave_triggers_legacy_replay},
        {"scene/HUD intersection", test_later_scene_overlay_intersects_hud},
        {"HUD exclusions/groups", test_hud_rule_exclusions_and_grouping},
        {"projected UI containment", test_transformed_overlay_containment},
        {"dirty readers", test_each_reader_reads_back_only_dirty},
        {"contained dirty draws", test_contained_dirty_draws},
        {"flip dirty transfer", test_flip_transfers_dirty_region},
        {"draw and blit order", test_draw_and_blit_share_one_order},
        {"clear recorded", test_clear_is_recorded_with_its_rects},
        {"draw screen bounds", test_draw_screen_bounds},
        {"draw leases texture", test_draw_leases_its_texture_revision},
        {"texture handle lifetime", test_texture_handle_lifetime},
        {"texture load counted", test_texture_load_is_counted},
        {"upload paths bump", test_every_upload_path_bumps_the_revision},
        {"draw uploads a rev", test_draw_uploads_a_revision_the_renderer_lacks},
        {"palette bumps a tex", test_palette_write_bumps_a_texture_revision},
        {"revisions are unique", test_revisions_are_unique_across_surfaces},
        {"dinput notification", test_dinput_event_notification},
        {"mouse motion survives", test_mouse_motion_survives_keyboard_poll},
        {"unchanged state buffered", test_unchanged_state_produces_no_buffered_event},
        {"re-attach a palette", test_setpalette_self},
        {"colour key at 16 bpp", test_colorkey_16bpp},
        {"QueryInterface", test_query_interface},
        {"display modes", test_enum_display_modes},
        {"configurable modes", test_configurable_display_modes},
        {"Classic probe surfaces", test_classic_probe_surface_creation},
        {"Direct3D pipeline", test_d3d_pipeline},
        {"DirectSound", test_dsound},
        {"DirectInput", test_dinput},
        {"QMixer", test_qmixer},
        {"weanetr", test_weanetr},
        {"reference counts", test_refcounts},
        {"SDK record sizes", test_sdk_abi},
        {"SetRenderTarget self", test_setrendertarget_same_surface},
        {"FindDevice, literal", test_find_device_literal},
        {"viewport record", test_viewport_record},
        {"GetDeviceData stride", test_device_data_stride16},
        {"GetClipStatus canary", test_clipstatus_canary},
        {"overflow rejection", test_overflow_rejection},
        {"identity and parent", test_identity_and_parent},
        {"duplicate PCM lifetime", test_dsound_duplicate_lifetime},
        {"SetSurfaceDesc owner", test_setsurfacedesc_ownership},
        {"QMixer wave failure", test_qmixer_failure},
        {"partial lock, edges", test_partial_lock_bottom_edge},
        {"flip ownership", test_flip_ownership},
        {"out-pointer guards", test_out_pointer_guards},
        {"DirectSound data path", test_dsound_data_path},
        {"audio formats", test_dsound_formats},
        {"DirectSound notify", test_dsound_notify},
        {"DirectSound streaming", test_dsound_stream},
        {"FMV refill gate", test_dsound_stream_pacing},
        {"QMixer streaming", test_qmixer_streaming},
        {"QMixer channels", test_qmixer_channels},
        {"QMixer frame pump", test_qmixer_frame_pump},
        {"QMixer volume scale", test_qmixer_volume_scale},
        {"QMixer stream lifetime", test_qmixer_stream_buffer_lifetime},
        {"QMixer stream prefetch", test_qmixer_stream_prefetch},
        {"QMixer refill gate", test_qmixer_refill_gate},
        {"blit into a texture", test_blit_into_texture_uploads},
        {"palettised texture", test_palettised_texture_upload},
        {"texture content hash", test_texture_content_hash},
        {"texture hash row span", test_texture_hash_covers_the_whole_row},
        {"texture hash padding", test_texture_hash_covers_padding_bytes},
        {"texture formats", test_texture_formats},
        {"colour control", test_color_control},
        {"FourCC is a format error", test_fourcc_is_a_pixel_format_error},
        {"dx_reset", test_reset},
    };
    for (auto &t : tests) {
        int before = g_failures;
        t.fn();
        printf("%-26s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        printf("all dx tests passed\n");
    return g_failures ? 1 : 0;
}
