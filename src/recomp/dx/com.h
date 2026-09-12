// com.h - COM object and vtable emulation in guest memory.
//
// A guest COM interface pointer is the address of a 16-byte header allocated
// from the runtime heap:
//
//     +0   uint32  lpVtbl     guest address of the interface's vtable
//     +4   uint32  magic      COM_MAGIC, so a stray pointer is caught
//     +8   uint32  obj        index of the host-side ComObj
//     +12  uint32  iface      which ComIface this view is
//
// The vtable is a second guest allocation of one dword per slot, each holding
// an import trampoline address obtained from imports_alloc_trampoline(). A
// guest `call [vtbl + 4*n]` therefore lands in imports_dispatch, which runs
// the slot's shim and then pops the return address plus 4*argc bytes exactly
// as the real `ret n` would. Vtables are built once per interface and shared
// by every object of that interface, which is what the real DLLs do and what
// Task 4's parity comparison expects (it excludes COM vtable pointers).
//
// One host ComObj can be reachable through several interface views: an
// IDirectDraw2 and the IDirectDraw4 the guest gets from QueryInterface are
// two guest pointers with two vtables and one refcount, as COM requires.
// com_view() allocates a view lazily and caches it, so QueryInterface for the
// same interface twice returns the same pointer.
#pragma once
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "dxtypes.h"

#include <map>
#include <string>
#include <vector>

static const uint32_t COM_MAGIC = 0x504f504du; // 'POPM'

enum {
    COM_OFF_vtbl = 0,
    COM_OFF_magic = 4,
    COM_OFF_obj = 8,
    COM_OFF_iface = 12,
    COM_VIEW_SIZE = 16,
};

// Every interface the shims expose. The order is not significant except that
// IF_NONE is 0 and IF_COUNT bounds the per-object view table.
enum ComIface : uint16_t {
    IF_NONE = 0,
    IF_DIRECTDRAW,
    IF_DIRECTDRAW2,
    IF_DIRECTDRAW4,
    IF_DDSURFACE,
    IF_DDSURFACE2,
    IF_DDSURFACE3,
    IF_DDSURFACE4,
    IF_DDPALETTE,
    IF_DDCLIPPER,
    IF_DDCOLORCONTROL,
    IF_D3D,
    IF_D3D2,
    IF_D3DDEVICE2,
    IF_D3DVIEWPORT2,
    IF_D3DMATERIAL2,
    IF_D3DLIGHT,
    IF_D3DTEXTURE2,
    IF_DSOUND,
    IF_DSBUFFER,
    IF_DS3DBUFFER,
    IF_DS3DLISTENER,
    IF_DSNOTIFY,
    IF_DINPUT,
    IF_DINPUTDEVICE,
    IF_COUNT
};

// What a host object actually is. Several interfaces map to one kind.
// Direct3D has no kind of its own: IDirect3D and IDirect3D2 are interfaces on
// the DirectDraw object, exactly as in DirectX, so they share its refcount and
// its controlling IUnknown.
enum ComKind : uint16_t {
    K_NONE = 0,
    K_DDRAW,
    K_SURFACE,
    K_PALETTE,
    K_CLIPPER,
    K_D3DDEVICE,
    K_VIEWPORT,
    K_MATERIAL,
    K_LIGHT,
    K_DSOUND,
    K_DSBUFFER,
    K_DINPUT,
    K_DIDEVICE,
};

// ---------------------------------------------------------------------------
// The host-side object. One fat struct rather than a class hierarchy: these
// are shims, the field set is small and fixed, and a flat record keeps every
// method one field access away from what it needs.
// ---------------------------------------------------------------------------
struct ComObj {
    uint32_t id = 0;
    ComKind kind = K_NONE;
    int32_t refs = 0;
    bool alive = false;
    uint32_t views[IF_COUNT] = {0}; // guest address of each interface view
    // The first view ever handed out. COM requires QueryInterface(IID_IUnknown)
    // to return the same pointer for the lifetime of the object, so this is
    // fixed on first use and never revised when a lower-numbered view appears.
    uint32_t identity = 0;

    // --- K_DDRAW
    uint32_t hwnd = 0;
    uint32_t coop_level = 0;
    uint32_t mode_w = 0, mode_h = 0, mode_bpp = 0;
    bool mode_set = false;
    std::vector<uint32_t> surfaces; // ids, for RestoreAllSurfaces

    // --- K_SURFACE
    uint32_t caps = 0;
    uint32_t width = 0, height = 0, bpp = 0, pitch = 0;
    uint32_t pixels = 0; // guest address of the pixel memory
    uint32_t pixels_bytes = 0;
    uint32_t rmask = 0, gmask = 0, bmask = 0, amask = 0;
    uint32_t palette_obj = 0;
    uint32_t clipper_obj = 0;
    uint32_t back_obj = 0;            // next surface in the flip chain
    uint32_t front_obj = 0;           // the primary this back buffer belongs to
    bool implicit_backbuffer = false; // CreateSurface child: lifetime belongs to the flip chain
    uint32_t zbuffer_obj = 0;         // the Z buffer attached to this surface
    // Pixels changed while this surface had no texture handle. GetHandle
    // sends them when the handle is made, so a texture that was filled before
    // anything asked for its handle is not uploaded empty.
    bool tex_dirty = false;
    uint32_t owner_dd = 0;
    uint32_t ckey_src_lo = 0, ckey_src_hi = 0;
    uint32_t ckey_dst_lo = 0, ckey_dst_hi = 0;
    bool has_ckey_src = false, has_ckey_dst = false;
    int32_t lock_count = 0;
    bool is_primary = false;
    // True when the shim allocated `pixels` and must free them. False after
    // SetSurfaceDesc points the surface at a buffer the guest owns.
    bool owns_pixels = false;
    uint32_t texture_handle = 0; // non-zero once GetHandle was called
    uint32_t dc_handle = 0;      // pseudo HDC handed out by GetDC

    // --- K_PALETTE
    uint32_t pal_flags = 0;
    uint32_t pal[256] = {0}; // 0x00RRGGBB

    // --- K_CLIPPER
    uint32_t clip_hwnd = 0;
    std::vector<int32_t> clip_rects; // 4 LONGs each

    // --- K_D3DDEVICE
    uint32_t dev_d3d = 0;
    uint32_t render_target = 0; // surface id
    uint32_t current_viewport = 0;
    bool in_scene = false;
    std::vector<uint32_t> viewports;
    uint32_t render_state[D3D_RENDERSTATE_MAX] = {0};
    uint32_t light_state[D3D_LIGHTSTATE_MAX] = {0};
    float transform[D3DTRANSFORMSTATE_MAX][16] = {{0}};
    bool transform_set[D3DTRANSFORMSTATE_MAX] = {false};

    // --- K_VIEWPORT
    uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    float vp_minz = 0.0f, vp_maxz = 1.0f;
    uint32_t vp_background = 0; // material handle
    uint32_t vp_device = 0;
    std::vector<uint32_t> lights;

    // --- K_MATERIAL / K_LIGHT: the guest's own record, kept verbatim.
    std::vector<uint8_t> blob;
    uint32_t handle = 0; // material handle the guest sees

    // --- K_DSOUND
    uint32_t ds_coop = 0;
    uint32_t ds_hwnd = 0;

    // --- K_DSBUFFER
    int32_t channel = -1;
    uint32_t buf_bytes = 0;
    uint32_t buf_pixels = 0; // guest address of the PCM data
    uint32_t buf_flags = 0;
    uint32_t rate = 22050, nchannels = 1, bits = 16, block_align = 2;
    int32_t volume = 0, pan = 0;
    uint32_t frequency = 0; // 0 = the format's own rate
    bool playing = false, looping = false, is_primary_buffer = false;
    uint32_t play_cursor = 0, write_cursor = 0;
    uint32_t lock_off = 0, lock_len = 0;
    float pos3d[3] = {0, 0, 0}, vel3d[3] = {0, 0, 0};
    uint32_t notify_count = 0;

    // --- K_DINPUT / K_DIDEVICE
    uint32_t di_version = 0;
    uint32_t dev_type = 0; // DIDEVTYPE_MOUSE / _KEYBOARD
    bool acquired = false;
    uint32_t di_coop = 0;
    uint32_t data_format_size = 0;
    uint32_t buffer_size = 0;  // DIPROP_BUFFERSIZE
    uint32_t notify_event = 0; // SetEventNotification, 0 when none
    uint32_t axis_mode_absolute = 0;
    uint8_t last_keys[256] = {0};
    uint8_t last_buttons[8] = {0};
    int32_t last_x = 0, last_y = 0, last_z = 0;
    uint32_t sequence = 0;
    std::vector<uint32_t> events; // packed dwOfs/dwData pairs

    // IDirectDrawColorControl, per surface. The defaults are the DX6 SDK's.
    uint32_t cc_flags = 0x7f; // every field is valid
    int32_t cc_brightness = 750;
    int32_t cc_contrast = 10000;
    int32_t cc_hue = 0;
    int32_t cc_saturation = 10000;
    int32_t cc_sharpness = 5;
    int32_t cc_gamma = 1;
    int32_t cc_colorenable = 1;
};

// One vtable slot.
struct ComMethod {
    const char *name;
    uint8_t argc; // dwords popped, including `this`
    void (*fn)(X86 *);
};

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
// Drops every object and vtable. Call after mem_init(), which invalidates the
// guest heap the objects live in.
void com_reset();

ComObj *com_new(ComKind kind);
ComObj *com_get(uint32_t id);
// The object a guest interface pointer refers to, or null when `addr` is not
// one of ours. `want` may be IF_NONE to accept any interface.
ComObj *com_this(uint32_t addr, ComIface want = IF_NONE);
// Same, reading `this` from the shim's first stack argument.
ComObj *com_this_arg(X86 *c, ComIface want = IF_NONE);
ComIface com_iface_of(uint32_t addr);

// Guest address of `o` seen through `iface`, allocating the view on first use.
uint32_t com_view(ComObj *o, ComIface iface);
void com_addref(ComObj *o);
// Drops one reference and destroys at zero. Returns the new count.
int32_t com_release(ComObj *o);
// Final teardown regardless of references, for an implicitly owned object
// whose parent's destruction ends its lifetime (DirectDraw flip-chain children).
void com_destroy(ComObj *o);
// A module may hook its resource teardown here.
void com_set_destructor(ComKind kind, void (*fn)(ComObj *));

// ---------------------------------------------------------------------------
// Vtables
// ---------------------------------------------------------------------------
// Builds (once) the guest vtable for `iface` and records it so com_view can
// use it. `dll` and `iface_name` name the trampolines, e.g. "DDRAW.dll" and
// "IDirectDraw2", giving "DDRAW.dll!IDirectDraw2::Blt" in the import report.
uint32_t com_define(ComIface iface, const char *dll, const char *iface_name,
                    const ComMethod *methods, size_t count);
uint32_t com_vtable_of(ComIface iface);
// Which kinds an interface may be a view of. Registering this is what makes
// QueryInterface refuse an interface the object does not implement.
void com_bind(ComIface iface, ComKind kind);
const char *com_iface_name(ComIface iface);

// ---------------------------------------------------------------------------
// Shared slot implementations. Every interface's vtable uses these for slots
// 0, 1 and 2.
// ---------------------------------------------------------------------------
void com_QueryInterface(X86 *c); // argc 3
void com_AddRef(X86 *c);         // argc 1
void com_Release(X86 *c);        // argc 1

// Registers `iid` (16 bytes) as naming `iface`. QueryInterface matches on it.
void com_register_iid(ComIface iface, const uint8_t iid[16]);
// The interface a guest GUID names, or IF_NONE.
ComIface com_iface_for_iid(uint32_t guest_guid_addr);

// A kind may redirect QueryInterface to a different object: an IDirectDraw
// asked for IID_IDirect3D2 returns the Direct3D object it owns, not itself.
// Returning null means "this object", and returning null with `handled` set
// false is the normal case.
typedef ComObj *(*ComQiHook)(ComObj *self, ComIface want);
void com_set_qi_hook(ComKind kind, ComQiHook hook);

// ---------------------------------------------------------------------------
// Small helpers every module needs.
// ---------------------------------------------------------------------------
// Sets EAX and logs the call at level 2.
static inline void com_ret(X86 *c, uint32_t hr) {
    set_eax(c, hr);
}

// Writes an interface pointer through a guest LPVOID*, tolerating a null
// pointer (which is a caller bug, so it reports DDERR_INVALIDPARAMS).
bool com_out_ptr(uint32_t out_addr, uint32_t value);

// ---------------------------------------------------------------------------
// Checked guest spans. Every size a shim derives from guest-supplied values
// goes through these: a 32-bit product like `stride * count` can wrap and pass
// a naive bounds check while the caller then walks the unwrapped length, so
// the multiplication is done in 64 bits and the result is range-checked before
// anything is read or written.
// ---------------------------------------------------------------------------
// True when [addr, addr + bytes) lies wholly inside the guest arena.
static inline bool gm_fits(uint32_t addr, uint64_t bytes) {
    return (uint64_t)addr + bytes <= (uint64_t)GUEST_SIZE;
}
// True when [addr, addr + count*elem) lies wholly inside the guest arena.
// count and elem are each at most 2^32-1, so the product cannot overflow 64
// bits and no separate overflow test is needed.
static inline bool gm_fits_n(uint32_t addr, uint64_t count, uint64_t elem) {
    return gm_fits(addr, count * elem);
}
// The byte size of a w*h*bytes_per_pixel image, or 0 when it would exceed the
// arena. Callers treat 0 as "refuse this surface".
static inline uint64_t gm_image_bytes(uint64_t pitch, uint64_t height) {
    uint64_t n = pitch * height;
    return n <= (uint64_t)GUEST_SIZE ? n : 0;
}

// Zeroes `n` bytes of guest memory.
void gm_zero(uint32_t addr, uint32_t n);
// Copies within guest memory.
void gm_copy(uint32_t dst, uint32_t src, uint32_t n);

// A slot that is part of the interface but does nothing here. It logs once
// under its trampoline name and returns `hr`. Used through the COM_STUB macro
// so every unimplemented slot still has the right argc and a name in the
// import coverage report.
void com_stub_ok(X86 *c);
void com_stub_notimpl(X86 *c);

// Declares a named slot body that logs once and returns `hr`. Preferred over
// com_stub_ok/com_stub_notimpl wherever knowing which method the guest called
// would help, because a shim cannot discover its own trampoline name.
#define DX_STUB(name, hr)                                                                          \
    static void name(X86 *c) {                                                                     \
        log_once("dx." #name, "dx: " #name " is not implemented; returning " #hr);                 \
        set_eax(c, (uint32_t)(hr));                                                                \
    }

// Total live objects, for the tests and for leak reporting.
uint32_t com_live_count();
// Highest object id ever handed out. Ids run 1..com_object_count(); com_get
// returns null for the ones already destroyed, so this bounds a scan over
// every object that still exists.
uint32_t com_object_count();
void com_dump(FILE *out);
