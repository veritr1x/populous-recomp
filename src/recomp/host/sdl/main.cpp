// main.cpp - the windowed host for the recompiled game, on SDL3.
//
// The guest owns the main thread. That is not a shortcut: the game never
// blocks in GetMessage, it drains its queue with PeekMessageA and spins on
// GetTickCount, so there is no point at which an event loop could take over.
// Instead the host rides on the guest's own clock reads, exactly as the
// headless host does - src/recomp/host/boot.cpp calls back about once a
// millisecond - and that callback is where SDL gets its turn:
//
//     boot_run() -> run_entry() -> ... guest frame loop ...
//                       -> GetTickCount -> boot tick -> pump() -> SDL events
//
// pump() drains the SDL event queue, translates each event into both of the
// input paths the game reads (the DirectInput device state in input.cpp and
// the Win32 message queue). Sealed frames are presented by a dedicated worker.
//
// Nothing here draws the game itself. present_thread.cpp owns the swapchain
// and d3d_render.cpp owns the Direct3D scene; this file owns the window, the
// events and the lifetime.
#include "../../mods/display_settings.h"
#include "../../platform/os.h"
#include "../audio.h"
#include "../audio_capture.h"
#include "../boot.h"
#include "../d3d_render.h"
#include "../gpu/gpu_factory.h"
#include "../input.h"
#include "../input_gate.h"
#include "../midi.h"
#include "../present.h"
#include "../window_presentation.h"
#include "keymap.h"
#include "../../dx/dx.h"
#include "../../runtime/loader.h"
#include "../../runtime/mods_seam.h"
#include "../../runtime/win32.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Win32 messages this host delivers.
enum {
    WM_PAINT_ = 0x000f,
    WM_CLOSE_ = 0x0010,
    WM_ACTIVATE_ = 0x0006,
    WM_SETFOCUS_ = 0x0007,
    WM_KILLFOCUS_ = 0x0008,
    WM_ACTIVATEAPP_ = 0x001c,
    WM_KEYDOWN_ = 0x0100,
    WM_KEYUP_ = 0x0101,
    WM_CHAR_ = 0x0102,
    WM_SYSKEYDOWN_ = 0x0104,
    WM_SYSKEYUP_ = 0x0105,
    WM_MOUSEMOVE_ = 0x0200,
    WM_LBUTTONDOWN_ = 0x0201,
    WM_LBUTTONUP_ = 0x0202,
    WM_RBUTTONDOWN_ = 0x0204,
    WM_RBUTTONUP_ = 0x0205,
    WM_MBUTTONDOWN_ = 0x0207,
    WM_MBUTTONUP_ = 0x0208,
    WM_MOUSEWHEEL_ = 0x020a,
};

namespace {

SDL_Window *g_window = nullptr;
SDL_MetalView g_metal_view = nullptr;
void *g_surface = nullptr; // the CAMetalLayer the presenter draws into
std::unique_ptr<gpu::Device> g_gpu;
bool g_close_requested = false; // the user asked to close
bool g_guest_activated = false; // WM_ACTIVATEAPP(1) has been delivered
bool g_focused = false;
int g_mode_w = 640, g_mode_h = 480;
bool g_mode_dirty = false;
int g_window_mode = 0, g_wanted_window_mode = 0;
bool g_fullscreen_transition = false;
HostRect g_pointer_confinement; // window points; main thread only
bool g_pointer_sample_valid = false;
int32_t g_pointer_sample_x = 0, g_pointer_sample_y = 0;
int g_pointer_sample_w = 0, g_pointer_sample_h = 0;
bool g_borderless_frame_dirty = true;
int g_windowed_x = 0, g_windowed_y = 0, g_windowed_w = 0, g_windowed_h = 0;
uint32_t g_pending_mode_w = 0, g_pending_mode_h = 0;
uint8_t g_buttons = 0; // for the MK_ bits in a mouse message

uint32_t make_lparam(int32_t x, int32_t y) {
    return ((uint32_t)(y & 0xffff) << 16) | (uint32_t)(x & 0xffff);
}
// The wParam every mouse message carries: which buttons are down and whether
// Shift or Control is held. A game that reads it and finds Shift never set
// cannot tell a shift-click from a click.
int32_t g_cursor_x = 0, g_cursor_y = 0;

// The modifiers AS THE GUEST KNOWS THEM: a consumed Shift is not in here,
// so it cannot appear in a mouse message the guest does see.
uint32_t mouse_wparam() {
    return host_mouse_wparam(g_buttons, host_guest_modifiers());
}
void post(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    uint32_t hwnd = host_main_window();
    if (hwnd)
        host_post_message(hwnd, msg, wparam, lparam);
}

uint32_t current_modifier_flags() {
    return host_modifier_flags_from_sdl(SDL_GetModState());
}

// The largest whole scale at which the guest's frame still fits comfortably on
// the screen this window is opening on, so a 640x480 mode is not a postage
// stamp on a 5K display and a 1024x768 mode still fits on a laptop.
int window_scale_for(int gw, int gh) {
    SDL_Rect visible;
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (gw <= 0 || gh <= 0 || !SDL_GetDisplayUsableBounds(display, &visible))
        return 1;
    int scale = 1;
    while (scale < 4 && (scale + 1) * gw <= visible.w * 0.95 &&
           (scale + 1) * gh <= visible.h * 0.95)
        ++scale;
    return scale;
}

// The window's size in points and in drawable pixels.
void window_sizes(int *bw, int *bh, int *dw, int *dh) {
    *bw = *bh = *dw = *dh = 0;
    if (!g_window)
        return;
    SDL_GetWindowSize(g_window, bw, bh);
    SDL_GetWindowSizeInPixels(g_window, dw, dh);
}

// Decode window points into drawable pixels only. Layout selection, capture,
// drag ownership and guest motion are applied later under the guest baton.
void view_point_to_drawable(double px, double py, int32_t *out_x, int32_t *out_y, int *width,
                            int *height) {
    *out_x = 0;
    *out_y = 0;
    *width = 0;
    *height = 0;
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (bw <= 0 || bh <= 0)
        return;
    *width = dw;
    *height = dh;
    if (!g_pointer_confinement.empty()) {
        *out_x = host_confined_pointer_pixel(px, g_pointer_confinement.x, g_pointer_confinement.w,
                                             *width);
        *out_y = host_confined_pointer_pixel(py, g_pointer_confinement.y, g_pointer_confinement.h,
                                             *height);
        return;
    }
    *out_x = (int32_t)floor(px / bw * dw);
    *out_y = (int32_t)floor(py / bh * dh);
}

// ---------------------------------------------------------------------------
// Input is DECODED when the event arrives and APPLIED when the baton is ours.
//
// The runtime services this host from inside a scheduler idle slice, with its
// lock down and another guest thread free to hold the baton and run guest code
// for the whole slice. Applying input there would deliver to the guest, and
// dispatch mod callbacks, concurrently with that thread's hooks - callbacks
// that capture a view of guest memory and touch the settings, heap and event
// registries. So an event that arrives while parked is decoded into the queue
// below and applied from pump(), which runs on a thread that holds the baton.
// ---------------------------------------------------------------------------
struct PendingInput {
    enum Kind { MOTION, BUTTON, WHEEL, KEY, MODIFIERS, FOCUS, RELEASE_CAPTURE } kind;
    int32_t x = 0, y = 0, dz = 0;
    double dx = 0, dy = 0; // Drawable deltas preserve subpixel motion in the queue.
    int drawable_w = 0, drawable_h = 0;
    int button = 0;
    bool down = false;
    bool inside = false, edge = false;
    uint16_t key = 0;
    uint32_t character = 0;
    uint32_t flags = 0;
};
std::vector<PendingInput> g_pending_input;
// The queue is FILLED on the host thread inside an idle slice, with the
// scheduler's lock down, and DRAINED by whichever guest thread holds the baton
// - the tick, or the scheduler itself when nothing else can run. Those are two
// different threads running at the same time, so the queue needs its own lock.
// It is held only across the push and the swap, never across an apply: a mod
// callback must not run with a host lock held.
std::mutex g_pending_input_m;

void apply_input(const PendingInput &e);
void apply_focus(bool focused, uint32_t modifier_flags);

// True when the event was held back rather than applied.
bool queue_or_apply(const PendingInput &e) {
    if (sched_in_idle_slice()) {
        {
            std::lock_guard<std::mutex> held(g_pending_input_m);
            g_pending_input.push_back(e);
        }
        // Announced AFTER the input is in the queue, because a thread woken by
        // this goes straight to the emptiness check - and OUTSIDE the queue
        // lock, because the scheduler asks input_pending() while holding its
        // own mutex. Taking the two in the other order here would be a
        // lock-ordering inversion between them.
        sched_input_arrived();
        return true;
    }
    apply_input(e);
    return false;
}

// Called from pump(), which the guest reaches through its own clock read, so
// the calling thread holds the baton.
void deliver_pending_input() {
    std::vector<PendingInput> batch;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (g_pending_input.empty())
            return;
        batch.swap(g_pending_input);
    }
    // Applied outside the lock, in arrival order. Order is the whole point: a
    // focus loss sits in this queue among the presses it must follow, so
    // replaying a press after it cannot leave a key stuck down.
    for (const PendingInput &e : batch)
        apply_input(e);
}

// ---------------------------------------------------------------------------
// The platform half of pointer capture. The policy and the arithmetic are in
// input_gate.cpp, where they can be tested without a window; what is left here
// is what only a window can do: hide and confine the associated system cursor.
// Window positions remain the motion source in both capture states.
// ---------------------------------------------------------------------------
bool g_pointer_hidden = false;
std::atomic<bool> g_escape_held{false};
std::atomic<bool> g_platform_capture_requested{false};
void update_platform_pointer_capture();

void apply_pointer_capture(bool want) {
    host_pointer_capture(want);
    // Queue draining can run on a guest worker. All native cursor/window work
    // belongs to the main thread, which observes this at the next pump.
    g_platform_capture_requested = want;
    if (SDL_IsMainThread())
        update_platform_pointer_capture();
}

bool window_minimized_or_hidden() {
    const SDL_WindowFlags flags = g_window ? SDL_GetWindowFlags(g_window) : 0;
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
}
bool window_focused() {
    return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}
bool window_fullscreen() {
    return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

// Capture is for a window that is focused, showing, and not displaying the mod
// settings page - the page is navigated with a real cursor. It is not tied to
// what the guest is doing: an FMV reads the mouse exactly as gameplay does.
bool pointer_capture_wanted() {
    if (!g_focused || !g_window || !window_focused() || g_escape_held)
        return false;
    if (window_minimized_or_hidden())
        return false;
    if (mods_page_visible())
        return false;
    return true;
}

void update_platform_pointer_capture() {
    const bool want = g_platform_capture_requested && pointer_capture_wanted() &&
                      !g_close_requested && !g_fullscreen_transition;
    if (want && !g_pointer_hidden) {
        SDL_HideCursor();
        g_pointer_hidden = true;
    }
    if (!want && g_pointer_hidden) {
        SDL_ShowCursor();
        g_pointer_hidden = false;
    }

    // A regular window retains its resize/desktop escape behavior. Borderless
    // and fullscreen need real confinement; hiding alone leaves OS hot edges
    // reachable.
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    HostRect rect = want && g_window_mode != 0 && g_window
                        ? host_pointer_confinement_rect({0, 0, double(bw), double(bh)})
                        : HostRect{};
    if (g_window && (rect.x != g_pointer_confinement.x || rect.y != g_pointer_confinement.y ||
                     rect.w != g_pointer_confinement.w || rect.h != g_pointer_confinement.h)) {
        if (rect.empty())
            SDL_SetWindowMouseRect(g_window, nullptr);
        else {
            SDL_Rect clip{int(rect.x), int(rect.y), int(rect.w), int(rect.h)};
            SDL_SetWindowMouseRect(g_window, &clip);
        }
        fprintf(stderr, "[host] pointer confinement: %.1f,%.1f %.1fx%.1f points (window mode %d)\n",
                rect.x, rect.y, rect.w, rect.h, g_window_mode);
        g_pointer_confinement = rect;
        g_pointer_sample_valid = false;
    }
}

bool window_has_resize_edges() {
    return g_window_mode == 0 && !g_fullscreen_transition && !window_fullscreen();
}

bool input_pending() {
    std::lock_guard<std::mutex> held(g_pending_input_m);
    return !g_pending_input.empty();
}
void drain_input() {
    deliver_pending_input();
}

void apply_motion(int32_t x, int32_t y, double drawable_dx, double drawable_dy) {
    HitResult hit;
    const bool delivered = host_gate_window_motion(x, y, drawable_dx, drawable_dy, &hit);
    static const bool trace = getenv("POPM_TRACE_POINTER") != nullptr;
    static double last_trace = 0;
    const double now = (double(os_monotonic_ns()) / 1e9);
    if (trace && now - last_trace >= 0.1) {
        last_trace = now;
        const auto guest = host_guest_pointer_resolve(g_mem, GUEST_SIZE, 0x00d0595c);
        fprintf(stderr,
                "[pointer-game] drawable %d,%d hit %d at %d,%d delivered %d guest %d,%d bounds "
                "%d,%d,%d,%d\n",
                x, y, int(hit.kind), hit.gx, hit.gy, delivered, guest.x, guest.y, guest.left,
                guest.top, guest.right, guest.bottom);
    }
    if (host_pointer_captured() && g_buttons && g_window && window_has_resize_edges()) {
        double px, py;
        host_pointer_drawable_position(&px, &py);
        int bw, bh, dw, dh;
        window_sizes(&bw, &bh, &dw, &dh);
        const double margin = 8 * (bw > 0 ? double(dw) / bw : 1.0);
        if (host_pointer_at_resize_edge(px, py, dw, dh, margin))
            apply_pointer_capture(false);
    }
    if (!delivered)
        return;
    x = hit.gx;
    y = hit.gy;
    g_cursor_x = x;
    g_cursor_y = y;
    post(WM_MOUSEMOVE_, mouse_wparam(), make_lparam(x, y));
}

// Where the pointer is right now, in window points.
bool pointer_in_window_points(double *px, double *py) {
    float gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    int wx, wy;
    if (!g_window || !SDL_GetWindowPosition(g_window, &wx, &wy))
        return false;
    *px = gx - wx;
    *py = gy - wy;
    return true;
}

void sample_captured_pointer() {
    if (g_pointer_confinement.empty())
        return;
    double px, py;
    if (!pointer_in_window_points(&px, &py))
        return;
    PendingInput e;
    e.kind = PendingInput::MOTION;
    view_point_to_drawable(px, py, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    if (g_pointer_sample_valid && e.x == g_pointer_sample_x && e.y == g_pointer_sample_y &&
        e.drawable_w == g_pointer_sample_w && e.drawable_h == g_pointer_sample_h)
        return;
    g_pointer_sample_valid = true;
    g_pointer_sample_x = e.x;
    g_pointer_sample_y = e.y;
    g_pointer_sample_w = e.drawable_w;
    g_pointer_sample_h = e.drawable_h;
    queue_or_apply(e);
}

void handle_mouse_move(const SDL_MouseMotionEvent &motion) {
    PendingInput e;
    e.kind = PendingInput::MOTION;
    view_point_to_drawable(motion.x, motion.y, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    static const bool trace = getenv("POPM_TRACE_POINTER") != nullptr;
    static double last_trace = 0;
    const double now = (double(os_monotonic_ns()) / 1e9);
    if (trace && now - last_trace >= 0.1) {
        last_trace = now;
        fprintf(stderr, "[pointer] event %.1f,%.1f drawable %d,%d/%d,%d clip %d delta %.1f,%.1f\n",
                motion.x, motion.y, e.x, e.y, e.drawable_w, e.drawable_h,
                !g_pointer_confinement.empty(), motion.xrel, motion.yrel);
    }
    if (!g_pointer_confinement.empty())
        sample_captured_pointer();
    else
        queue_or_apply(e);
}

void apply_button(int button, bool down, int32_t x, int32_t y, bool inside, bool edge) {
    if (down && (!inside || edge)) {
        apply_pointer_capture(false);
        return;
    }
    // A click inside the window is how the pointer is taken back after it was
    // released - by focus loss, or by the settings page closing.
    int32_t dx, dy;
    auto hit = host_gate_window_pointer(x, y, &dx, &dy);
    if (hit.kind == HitResult::HIT_NONE) {
        if (down)
            return;
        // An outside release must still release a guest button held during
        // an edge drag, even though the outside position has no hit owner.
        hit.gx = g_cursor_x;
        hit.gy = g_cursor_y;
    }
    if (!host_pointer_captured() && host_pointer_can_capture(pointer_capture_wanted(), inside, down,
                                                             g_escape_held, mods_page_visible()))
        apply_pointer_capture(true);
    x = hit.gx;
    y = hit.gy;
    if (!down && !(g_buttons & ~(1u << button)))
        host_gate_end_drag();
    if (host_gate_button(button, down, x, y))
        return;
    if (down)
        host_gate_begin_drag(&hit);
    g_cursor_x = x;
    g_cursor_y = y;
    // Absolute placement alone does not move Populous's integrated cursor.
    host_input_motion(x, y, dx, dy);
    post(WM_MOUSEMOVE_, mouse_wparam(), make_lparam(x, y));
    if (down)
        g_buttons |= (uint8_t)(1u << button);
    else
        g_buttons &= (uint8_t)~(1u << button);
    host_input_button(button, down);
    static const uint32_t msgs[3][2] = {
        {WM_LBUTTONUP_, WM_LBUTTONDOWN_},
        {WM_RBUTTONUP_, WM_RBUTTONDOWN_},
        {WM_MBUTTONUP_, WM_MBUTTONDOWN_},
    };
    if (button < 3)
        post(msgs[button][down ? 1 : 0], mouse_wparam(), make_lparam(x, y));
}

void handle_button(const SDL_MouseButtonEvent &event, int button, bool down) {
    PendingInput e;
    e.kind = PendingInput::BUTTON;
    e.button = button;
    e.down = down;
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    e.inside = event.x >= 0 && event.y >= 0 && event.x < bw && event.y < bh;
    e.edge = window_has_resize_edges() && host_pointer_at_resize_edge(event.x, event.y, bw, bh, 8);
    view_point_to_drawable(event.x, event.y, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    queue_or_apply(e);
}

// The key paths live in input_gate.cpp, which owns the decision, the delivery
// and the two kinds of key state they need. What is left here is the SDL
// event decoding.
void apply_wheel(int32_t dz, int32_t x, int32_t y) {
    if (host_gate_wheel(dz))
        return;
    int32_t dx, dy;
    auto hit = host_gate_window_pointer(x, y, &dx, &dy);
    if (hit.kind == HitResult::HIT_NONE)
        return;
    x = hit.gx;
    y = hit.gy;
    host_input_motion(x, y, dx, dy);
    host_input_wheel(dz);
    post(WM_MOUSEWHEEL_, ((uint32_t)dz << 16) | mouse_wparam(), make_lparam(x, y));
}

void apply_key(uint16_t key, bool down, uint32_t character, uint32_t flags) {
    // Every key event carries the modifier flags, and this is the only place
    // a modifier held across a focus change can be noticed. A diff, so it
    // emits nothing when the state already agrees.
    host_gate_sync_modifiers(flags);
    host_key_event(key, down, character);
}

// A modifier key is an ordinary key event to SDL; the host diffs modifier
// state as a whole, so it arrives as the flags after the change.
bool is_modifier_scancode(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LCTRL ||
           sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT ||
           sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI || sc == SDL_SCANCODE_CAPSLOCK;
}

void handle_key(const SDL_KeyboardEvent &event, bool down) {
    if (is_modifier_scancode(event.scancode)) {
        PendingInput e;
        e.kind = PendingInput::MODIFIERS;
        e.flags = host_modifier_flags_from_sdl(event.mod);
        queue_or_apply(e);
        return;
    }
    if (event.scancode == SDL_SCANCODE_ESCAPE) {
        g_escape_held = down;
        if (down) {
            // Release before queuing guest input: it must work even while the
            // scheduler cannot grant the guest baton.
            g_platform_capture_requested = false;
            update_platform_pointer_capture();
            PendingInput release;
            release.kind = PendingInput::RELEASE_CAPTURE;
            queue_or_apply(release);
        }
    }
    const uint16_t key = host_keycode_from_scancode(event.scancode);
    if (key == 0xffff)
        return;
    PendingInput e;
    e.kind = PendingInput::KEY;
    e.key = key;
    e.down = down;
    // The character the key produces with no modifier held, which is what a
    // WM_CHAR for it carries; keycodes above the Unicode range are not characters.
    const SDL_Keycode plain = SDL_GetKeyFromScancode(event.scancode, SDL_KMOD_NONE, false);
    e.character = plain < 0x40000000 && plain >= 0x20 ? (uint32_t)plain : 0u;
    e.flags = host_modifier_flags_from_sdl(event.mod);
    queue_or_apply(e);
}

// Called from the event pump, including inside an idle slice. It decides that
// the focus changed and records WHEN, relative to the input around it; nothing
// guest-visible happens here.
void note_focus(bool focused) {
    if (focused == g_focused && g_guest_activated)
        return;
    g_focused = focused;
    g_guest_activated = true;
    PendingInput e;
    e.kind = PendingInput::FOCUS;
    e.down = focused;
    // Read now, not at apply time: what is held at the moment focus returns is
    // what the guest missed, and by the time the queue drains it may differ.
    e.flags = focused ? current_modifier_flags() : 0u;
    queue_or_apply(e);
}

void apply_input(const PendingInput &e) {
    if (e.kind == PendingInput::MOTION || e.kind == PendingInput::BUTTON ||
        e.kind == PendingInput::WHEEL)
        host_gate_fallback_layout(e.drawable_w, e.drawable_h);
    switch (e.kind) {
    case PendingInput::MOTION:
        apply_motion(e.x, e.y, e.dx, e.dy);
        break;
    case PendingInput::BUTTON:
        apply_button(e.button, e.down, e.x, e.y, e.inside, e.edge);
        break;
    case PendingInput::WHEEL:
        apply_wheel(e.dz, e.x, e.y);
        break;
    case PendingInput::KEY:
        apply_key(e.key, e.down, e.character, e.flags);
        break;
    case PendingInput::MODIFIERS:
        host_modifier_event(e.flags);
        break;
    case PendingInput::FOCUS:
        apply_focus(e.down, e.flags);
        break;
    case PendingInput::RELEASE_CAPTURE:
        apply_pointer_capture(false);
        break;
    }
}

// The whole of what a focus change does to the guest, including the two paths
// that reach a mod callback: the modifier resync on gain and the release on
// loss. Runs under the baton, from the queue, in arrival order with the key
// and button events around it - which is the point.
void apply_focus(bool focused, uint32_t modifier_flags) {
    if (!host_main_window())
        return; // the guest has no window yet
    post(WM_ACTIVATEAPP_, focused ? 1 : 0, 0);
    post(WM_ACTIVATE_, focused ? 1 : 0, 0);
    post(focused ? WM_SETFOCUS_ : WM_KILLFOCUS_, 0, 0);
    if (focused)
        post(WM_PAINT_, 0, 0);
    // Whatever is held right now became held while another application had
    // the focus, so no event announced it and the gate's state says it is up.
    if (focused)
        host_gate_sync_modifiers(modifier_flags);
    if (!focused) {
        // The pointer goes back to the user BEFORE the state is cleared:
        // host_gate_release_all drops the capture flag, and the platform
        // effects are keyed off that flag.
        apply_pointer_capture(false);
        // Nothing that was down can be seen coming up while another
        // application has the focus, so it all comes up now - including the
        // host's own copy of the button and key state.
        host_gate_release_all();
        g_buttons = 0;
    }
}

void post_drawable_size();

// One SDL event, translated into both of the input paths the game reads: the
// DirectInput device state, and the Win32 message queue.
void handle_event(const SDL_Event &event) {
    const SDL_WindowID ours = g_window ? SDL_GetWindowID(g_window) : 0;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.windowID == ours || !g_pointer_confinement.empty())
            handle_mouse_move(event.motion);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.windowID == ours) {
            const int button = event.button.button == SDL_BUTTON_LEFT     ? 0
                               : event.button.button == SDL_BUTTON_RIGHT  ? 1
                               : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                          : -1;
            if (button >= 0)
                handle_button(event.button, button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.windowID == ours) {
            PendingInput e;
            e.kind = PendingInput::WHEEL;
            const double steps =
                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            e.dz = (int32_t)lround(steps * 120.0);
            // WM_MOUSEWHEEL carries the position of the wheel event itself.
            view_point_to_drawable(event.wheel.mouse_x, event.wheel.mouse_y, &e.x, &e.y,
                                   &e.drawable_w, &e.drawable_h);
            queue_or_apply(e);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Command combinations belong to the application, not the game:
        // Command-Q quits through SDL_EVENT_QUIT and Command-H hides.
        if (event.key.mod & SDL_KMOD_GUI)
            break;
        handle_key(event.key, event.type == SDL_EVENT_KEY_DOWN);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_platform_capture_requested = false;
        update_platform_pointer_capture();
        g_escape_held = false;
        note_focus(false);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        note_focus(true);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        post_drawable_size();
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        g_borderless_frame_dirty = true;
        post_drawable_size();
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        g_fullscreen_transition = false;
        g_window_mode = 2;
        post_drawable_size();
        update_platform_pointer_capture();
        break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        g_fullscreen_transition = false;
        g_window_mode = 0;
        post_drawable_size();
        update_platform_pointer_capture();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        // The guest closes itself: WM_CLOSE runs its own shutdown path, and
        // the window stays up until it is done so the last frame does not
        // vanish mid-teardown.
        g_close_requested = true;
        break;
    default:
        break;
    }
}

// Drains the event queue, waiting up to `seconds` for the first one. Zero
// means take what is already there and return; a real wait is a genuine sleep
// inside SDL, which is what stops the host spinning while the guest waits.
// Returns true when something changed the input state.
int service(double seconds) {
    uint32_t before = host_input_notify_count();
    size_t queued_before = 0;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        queued_before = g_pending_input.size();
    }
    SDL_Event event;
    bool first = true;
    for (;;) {
        bool got;
        if (first && seconds > 0.0)
            got = SDL_WaitEventTimeout(&event, int(seconds * 1000.0));
        else
            got = SDL_PollEvent(&event);
        first = false;
        if (!got)
            break;
        handle_event(event);
    }
    // Input that was only QUEUED still counts as input having arrived: the
    // guest should re-check its condition and come back through the tick,
    // which is where the queue is applied.
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (g_pending_input.size() != queued_before)
            return 1;
    }
    return host_idle_wait_result(before, host_input_notify_count());
}

// The housekeeping every turn does once the events are in.
void after_events() {
    // A shell-launched process does not always come forward on its own, and a
    // window that never gained focus receives no key events at all. Ask again,
    // briefly, rather than once at startup and never afterwards.
    if (g_window && !window_focused() && boot_elapsed() < 3.0)
        SDL_RaiseWindow(g_window);
    note_focus(window_focused());

    // Capture follows the window and the page. Released whenever the window
    // is not focused, is minimised or is showing the settings page; taken back
    // by a click, not automatically.
    if (host_pointer_captured() && !pointer_capture_wanted())
        apply_pointer_capture(false);
    update_platform_pointer_capture();
    // Fullscreen chrome can reroute or omit motion events at an edge.
    // Captured input owns the pointer: sample its current position even
    // without an event for our window, and deliver only changes.
    sample_captured_pointer();

    if (g_close_requested && !boot_close_requested())
        boot_request_close("the window was closed");
}

// Borderless is a desktop window: keep the entire drawable inside the usable
// area of its display, below the menu bar and outside the Dock.
void fit_borderless_window() {
    SDL_Rect usable;
    if (!g_window || !SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &usable))
        return;
    g_borderless_frame_dirty = false;
    int x, y, w, h;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    if (x == usable.x && y == usable.y && w == usable.w && h == usable.h)
        return;
    SDL_SetWindowPosition(g_window, usable.x, usable.y);
    SDL_SetWindowSize(g_window, usable.w, usable.h);
    post_drawable_size();
    fprintf(stderr, "[host] borderless usable frame: %d,%d %dx%d points\n", usable.x, usable.y,
            usable.w, usable.h);
}

// Consume a posted setting on the main thread. Fullscreen completes through
// window events; another setting can supersede it meanwhile.
void apply_window_mode() {
    int request = host_display_take_window();
    if (request >= 0)
        g_wanted_window_mode = request;
    if (!g_window || g_fullscreen_transition)
        return;
    if (window_fullscreen()) {
        if (g_wanted_window_mode != 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            SDL_SetWindowFullscreen(g_window, false);
        }
        return;
    }
    if (g_window_mode == g_wanted_window_mode) {
        if (g_window_mode == 1 && g_borderless_frame_dirty)
            fit_borderless_window();
        return;
    }
    if (g_window_mode == 0) {
        SDL_GetWindowPosition(g_window, &g_windowed_x, &g_windowed_y);
        SDL_GetWindowSize(g_window, &g_windowed_w, &g_windowed_h);
    }
    if (g_wanted_window_mode == 1) {
        SDL_SetWindowBordered(g_window, false);
        g_window_mode = 1;
        fit_borderless_window();
    } else {
        SDL_SetWindowBordered(g_window, true);
        if (g_windowed_w > 0) {
            SDL_SetWindowSize(g_window, g_windowed_w, g_windowed_h);
            SDL_SetWindowPosition(g_window, g_windowed_x, g_windowed_y);
        }
        g_window_mode = 0;
        if (g_wanted_window_mode == 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            // Borderless desktop fullscreen: no display mode change.
            SDL_SetWindowFullscreenMode(g_window, nullptr);
            SDL_SetWindowFullscreen(g_window, true);
        }
    }
    post_drawable_size();
}

// The display mode changed under us, which resizes the window.
void apply_mode_change() {
    if (!g_mode_dirty)
        return;
    g_mode_dirty = false;
    host_pointer_set_mode((int)g_pending_mode_w, (int)g_pending_mode_h);
    g_mode_w = (int)g_pending_mode_w;
    g_mode_h = (int)g_pending_mode_h;
    int scale = window_scale_for(g_mode_w, g_mode_h);
    if (g_window_mode == 0 && !g_fullscreen_transition) {
        SDL_SetWindowSize(g_window, g_mode_w * scale, g_mode_h * scale);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        post_drawable_size();
    }
    host_set_client_size(host_main_window(), g_mode_w, g_mode_h);
    // The window may never be smaller than the guest's frame, or the integer
    // scaling has no whole multiple to take.
    SDL_SetWindowMinimumSize(g_window, g_mode_w, g_mode_h);
}

// True when this thread may touch the window at all.
bool on_host_thread() {
    if (boot_on_run_thread() && SDL_IsMainThread())
        return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        fprintf(stderr, "[host] a guest worker thread reached the host; the window is "
                        "serviced only on the main thread\n");
    }
    return false;
}

// One turn of the host's event loop, called from inside guest code.
//
// The tick can arrive on any guest thread: the runtime's cooperative scheduler
// hands the baton to real pthreads and whichever one holds it reads the clock.
// The window belongs to the thread that called boot_run, so a tick on any
// other thread does nothing at all and hands control straight back.
void pump() {
    if (!on_host_thread())
        return;
    // First: anything an idle slice decoded but could not apply. This thread
    // reached here through the guest's own clock read, so it holds the baton
    // and a mod callback dispatched from here is serialised against every
    // other guest thread, which is the whole point.
    deliver_pending_input();
    apply_window_mode();
    apply_mode_change();
    service(0.0);
    after_events();
    host_gate_pointer_tick();
}

// The runtime's idle wait, called on the run thread when the guest is about to
// block. Returns 1 when input arrived, so the caller can re-check its own
// condition at once.
int idle_wait(double seconds) {
    if (!on_host_thread())
        return 0;
    if (seconds < 0.0)
        seconds = 0.0;
    apply_window_mode();
    apply_mode_change();
    int changed = service(seconds);
    after_events();
    return changed;
}

void on_mode_change(int w, int h, int bpp) {
    (void)bpp;
    if (w <= 0 || h <= 0)
        return;
    g_pending_mode_w = (uint32_t)w;
    g_pending_mode_h = (uint32_t)h;
    g_mode_dirty = true;
}

// The watchdog takes the report lock before calling this and main takes it
// around its own call, so everything below reads a consistent snapshot.
void report(FILE *out, bool abnormal) {
    fprintf(out, "\n== windowed run ==\n");
    fprintf(out, "stopped:            %s\n", boot_stop_reason());
    fprintf(out, "elapsed:            %.1fs\n", boot_elapsed());
    fprintf(out, "presented frames:   %u\n", host_present_count());
    // Per display mode, because a rate averaged over a whole run mixes the
    // menu and the loading screen in with gameplay.
    for (int i = 0; i < host_present_rate_count(); ++i) {
        HostPresentRate r;
        memset(&r, 0, sizeof r);
        host_present_rate(i, &r);
        fprintf(out, "  %4dx%-4d %2dbpp:  %u guest presents; %.1fs at this mode\n", r.w, r.h, r.bpp,
                r.presents, r.seconds);
    }
    // The two lines the display baseline is made of, in the same words the
    // smoke host uses, because display_compare.py parses the text.
    {
        char line[768];
        if (host_stats_gameplay_line(line, sizeof line))
            fprintf(out, "gameplay: %s\n", line);
        if (host_stats_access_line(line, sizeof line))
            fprintf(out, "%s\n", line);
    }
    fprintf(out, "input changes:      %u announced to the DirectInput shim\n",
            host_input_notify_count());
    fprintf(out,
            "Direct3D:           %u draws, %u textures, %u write-backs into the "
            "render target\n",
            host_d3d_total_draws(), host_d3d_total_textures(), host_d3d_total_flushes());
    // What the mixer actually produced, when the run was asked to capture it.
    host_capture_print(out);
    boot_print_dx_objects(out);
    boot_print_exit_code(out);
    boot_print_undeliverable(out);
    fflush(out);
    boot_print_import_stats(out, abnormal);
}

// The .app can be started from anywhere, and the guest's file system is rooted
// at the directory holding the EXE. Walking up from the executable finds the
// checkout whichever way the app was launched.
std::string find_exe_relative_to_bundle() {
    if (const char *env = getenv("POP_RECOMP_EXE"))
        return env;
    char path[4096];
    if (os_exe_path(path, sizeof path) != 0)
        return "";
    std::string dir(path);
    for (int depth = 0; depth < 12; ++depth) {
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos)
            break;
        dir = dir.substr(0, slash);
        std::string candidate = dir + "/original/gog/D3DPopTB.exe";
        FILE *f = fopen(candidate.c_str(), "rb");
        if (f) {
            fclose(f);
            // The runtime resolves the guest's relative paths against the
            // process's working directory, so the checkout root becomes it.
            if (os_chdir(dir.c_str()) != 0)
                fprintf(stderr, "[host] could not enter %s\n", dir.c_str());
            return candidate;
        }
    }
    return "";
}

// The bundled classic-modes table when this is an app bundle, else the checkout's.
std::string classic_modes_path() {
    char path[4096];
    if (os_exe_path(path, sizeof path) == 0) {
        std::string exe(path);
        const size_t macos = exe.rfind("/Contents/MacOS/");
        if (macos != std::string::npos) {
            std::string candidate = exe.substr(0, macos) + "/Contents/Resources/classic-modes.json";
            if (FILE *f = fopen(candidate.c_str(), "rb")) {
                fclose(f);
                return candidate;
            }
        }
    }
    return "tools/recomp/baseline/classic-modes.json";
}

void post_drawable_size() {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    static int last_w = 0, last_h = 0;
    if (dw <= 0 || dh <= 0 || (last_w == dw && last_h == dh))
        return;
    last_w = dw;
    last_h = dh;
    host_present_resize(dw, dh);
}

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    std::string exe = find_exe_relative_to_bundle();
    if (exe.empty()) {
        fprintf(stderr, "PopRecomp: original/gog/D3DPopTB.exe was not found above this "
                        "executable.\nRun the binary from the checkout, or set "
                        "POP_RECOMP_EXE to the image.\n");
        return 2;
    }

    // A click that brings the window forward reaches the game in the same
    // event, rather than being swallowed as the activating click.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "PopRecomp: SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }

    g_gpu = gpu::create_default_device();
    if (!g_gpu) {
        fprintf(stderr, "PopRecomp: no GPU device is available (%s)\n",
                gpu::default_backend_name());
        return 3;
    }

    int scale = window_scale_for(g_mode_w, g_mode_h);
    g_window = SDL_CreateWindow("Populous: The Beginning", g_mode_w * scale, g_mode_h * scale,
                                SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        fprintf(stderr, "PopRecomp: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 3;
    }
    SDL_SetWindowMinimumSize(g_window, g_mode_w, g_mode_h);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    g_metal_view = SDL_Metal_CreateView(g_window);
    g_surface = g_metal_view ? SDL_Metal_GetLayer(g_metal_view) : nullptr;
    if (!g_surface) {
        fprintf(stderr, "PopRecomp: no Metal layer for the window: %s\n", SDL_GetError());
        return 3;
    }

    // The renderer first: the presenter shares its device, so a present
    // cannot run ahead of the scene it is showing.
    D3DRenderer *renderer = new D3DRenderer(g_gpu.get());
    if (!renderer->ok())
        return 3;
    D3DRenderer::setShared(renderer);
    host_present_set_device(g_gpu.get());

    fprintf(stderr, "Mouse capture: click inside to capture; hold Escape to release; drag to "
                    "the window edge to resize.\n");
    mods_display_live_defaults();
    mods_display_load_modes(classic_modes_path().c_str());
    host_present_on_mode_change(on_mode_change);
    // Every change to the input state wakes the guest's DirectInput threads,
    // which wait on an event rather than polling. Installed here rather than
    // referenced from input.cpp, which links none of the shims.
    host_input_set_notify(dinput_host_input_changed);

    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    SDL_PumpEvents();
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (dw <= 0 || dh <= 0) {
        fprintf(stderr, "PopRecomp: the window has no drawable size\n");
        return 3;
    }
    host_present_start(g_surface, dw, dh);
    // Said out loud, because "the keyboard does nothing" and "the window
    // never gained focus" look identical from the outside.
    printf("PopRecomp: window %dx%d points, %dx%d pixels, focus %s\n", bw, bh, dw, dh,
           window_focused() ? "yes" : "no");
    fflush(stdout);

    BootOptions options;
    options.name = "windowed";
    options.exe = exe.c_str();
    // This host has a real window with real focus, so activation is
    // delivered from the events that carry it rather than synthesised.
    options.activate = false;
    options.tick = pump;
    // The scheduler drains the queue itself when nothing is runnable, so
    // a guest blocked on an event an input message signals is not waiting
    // for a pump that only its own clock read would trigger.
    sched_set_input_queue(input_pending, drain_input);
    options.idle_wait = idle_wait;
    options.report = report;
    // No run deadline: the run ends when the user closes the window. The
    // watchdog still backs the close, so a guest that ignores WM_CLOSE
    // cannot leave a window on screen with nothing behind it.
    options.deadline_seconds = 0.0;
    options.close_unwind_grace = 15.0;
    options.close_watchdog_grace = 30.0;

    if (!boot_load(options)) {
        host_present_stop();
        fprintf(stderr, "PopRecomp: %s\n", loader_error());
        return 2;
    }
    printf("PopRecomp: %s, entry %08x\n", loader_exe_path().c_str(), loader_entry_point());
    fflush(stdout);

    // The music's synth, built here and not later. midiOutOpen arrives on a
    // guest thread holding the cooperative scheduler baton, which stops
    // every other guest thread until it returns, so a SoundFont parsed
    // there would freeze the game at the moment the music starts.
    host_midi_startup(win32_midi_soundfont_path().c_str());

    // POP_HOST_AUDIO_CAPTURE=<path.wav> writes the mixer's own output for
    // the whole run.
    const char *capture = getenv("POP_HOST_AUDIO_CAPTURE");
    if (capture && *capture)
        host_audio_capture_begin(capture);

    boot_run();
    if (!sched_guest_threads_stopped()) {
        fprintf(stderr,
                "presenter: guest workers outlived shutdown; refusing unsafe presenter join\n");
        fflush(nullptr);
        _Exit(4);
    }
    host_present_stop(); // scheduler has stopped; join outside the guest baton
    host_audio_capture_end();
    boot_report_lock();
    report(stdout, false);
    boot_report_unlock();
    // Whatever else happens, the user gets their cursor back.
    apply_pointer_capture(false);
    SDL_HideWindow(g_window);
    SDL_Metal_DestroyView(g_metal_view);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
