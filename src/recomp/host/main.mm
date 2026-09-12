#include "../mods/display_settings.h"

static void post_drawable_size();
// main.mm - the windowed macOS host for the recompiled game.
//
// The guest owns the main thread. That is not a shortcut: the game never
// blocks in GetMessage, it drains its queue with PeekMessageA and spins on
// GetTickCount, so there is no point at which an event loop could take over.
// Instead the host rides on the guest's own clock reads, exactly as the
// headless host does - src/recomp/host/boot.cpp calls back about once a
// millisecond - and that callback is where AppKit gets its turn:
//
//     boot_run() -> run_entry() -> ... guest frame loop ...
//                       -> GetTickCount -> boot tick -> pump() -> NSApp events
//
// pump() drains the NSEvent queue, translates each event into both of the
// input paths the game reads (the DirectInput device state in input.mm and the
// Win32 message queue). Sealed frames are presented by a dedicated worker.
//
// Nothing here draws the game itself. present_thread.mm owns the drawable and
// d3d_render.mm owns the Direct3D scene; this file owns the window, the
// events and the lifetime.
#include "boot.h"
#include "present.h"
#import <QuartzCore/CATransaction.h>
#include "audio.h"
#include "audio_capture.h"
#include "input.h"
#include "input_gate.h"
#include "window_presentation.h"
#include "midi.h"
#include "d3d_render.h"
#include "gpu/gpu_factory.h"
#include "gpu/metal/metal_bridge.h"
#include "../runtime/loader.h"
#include "../runtime/win32.h"
#include "../runtime/mods_seam.h"
#include "../dx/dx.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <mach-o/dyld.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// AppKit has supported this since 10.13.2, but omits it from public headers.
// SDL's Cocoa backend uses the same window-local confinement facility. Check
// availability before using it; no cursor decoupling or repeated warps.
@interface NSWindow (PopPointerConfinement)
@property(nonatomic) NSRect mouseConfinementRect;
@end

#include <string>
#include <atomic>

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
// ---------------------------------------------------------------------------

@interface PopWindowDelegate : NSObject <NSWindowDelegate>
@end

// Command-Q sends terminate:, which ends the process where it stands. That is
// the wrong ending for this host: the guest has its own shutdown, and the mod
// runtime has exits, reclamation and an unload that all happen after the guest
// stops. So termination is routed through the SAME close request the window's
// close button uses. Cancel AppKit's termination so sendEvent returns to the
// guest-owned event pump; main returns once the guest and host teardown finish.
// NSTerminateLater would enter a nested modal loop, preventing that shutdown.
@interface PopAppDelegate : NSObject <NSApplicationDelegate>
- (void)openSettings:(id)sender;
@end

// A plain NSView refuses first responder by default, which leaves the window with
// nothing to send a key event to. The pump takes key events off the queue
// itself so it would still see them, but a view that cannot become first
// responder also cannot be the one AppKit reports as focused, and anything
// that asks would be told the wrong thing.
@interface PopMetalView : NSView
@end
@implementation PopMetalView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)canBecomeKeyView {
    return YES;
}
// A click in the window activates the app and reaches the game in the same
// event, rather than being swallowed as the activating click.
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}
@end

namespace {

NSWindow *g_window = nil;
PopMetalView *g_view = nil;
id<MTLDevice> g_device = nil;
PopWindowDelegate *g_window_delegate = nil;
// Held here because NSApp.delegate is a WEAK property: assigning a freshly
// allocated object to it and keeping no other reference deallocates it at
// once, and the termination hook would never be called.
PopAppDelegate *g_app_delegate = nil;
bool g_close_requested = false; // the user asked to close
bool g_guest_activated = false; // WM_ACTIVATEAPP(1) has been delivered
bool g_focused = false;
int g_mode_w = 640, g_mode_h = 480;
bool g_mode_dirty = false;
int g_window_mode = 0, g_wanted_window_mode = 0;
bool g_fullscreen_transition = false;
NSRect g_pointer_confinement = NSZeroRect; // view coordinates; main thread only
NSRect g_pointer_window_confinement = NSZeroRect;
bool g_pointer_sample_valid = false;
int32_t g_pointer_sample_x = 0, g_pointer_sample_y = 0;
int g_pointer_sample_w = 0, g_pointer_sample_h = 0;
int g_fullscreen_presentation_state = -1;
NSApplicationPresentationOptions g_fullscreen_presentation =
    NSApplicationPresentationFullScreen | NSApplicationPresentationAutoHideDock |
    NSApplicationPresentationAutoHideMenuBar;
bool g_borderless_frame_dirty = true;
NSRect g_windowed_frame{};
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

// The largest whole scale at which the guest's frame still fits comfortably on
// the screen this window is opening on, so a 640x480 mode is not a postage
// stamp on a 5K display and a 1024x768 mode still fits on a laptop.
int window_scale_for(int gw, int gh) {
    NSScreen *screen = [NSScreen mainScreen];
    if (!screen || gw <= 0 || gh <= 0)
        return 1;
    NSRect visible = screen.visibleFrame;
    int scale = 1;
    while (scale < 4 && (scale + 1) * gw <= visible.size.width * 0.95 &&
           (scale + 1) * gh <= visible.size.height * 0.95)
        ++scale;
    return scale;
}

// Decode AppKit points into drawable pixels only. Layout selection, capture,
// drag ownership and guest motion are applied later under the guest baton.
void view_point_to_drawable(NSPoint window_point, int32_t *out_x, int32_t *out_y, int *width,
                            int *height) {
    *out_x = 0;
    *out_y = 0;
    *width = 0;
    *height = 0;
    if (!g_view)
        return;
    NSPoint p = [g_view convertPoint:window_point fromView:nil];
    CGSize drawable = [g_view convertRectToBacking:g_view.bounds].size;
    NSSize bounds = g_view.bounds.size;
    if (bounds.width <= 0 || bounds.height <= 0)
        return;
    *width = (int)drawable.width;
    *height = (int)drawable.height;
    if (!NSIsEmptyRect(g_pointer_confinement)) {
        *out_x = host_confined_pointer_pixel(p.x, g_pointer_confinement.origin.x,
                                             g_pointer_confinement.size.width, *width);
        *out_y = host_confined_pointer_pixel(p.y, g_pointer_confinement.origin.y,
                                             g_pointer_confinement.size.height, *height, true);
        return;
    }
    *out_x = (int32_t)floor(p.x / bounds.width * drawable.width);
    *out_y = (int32_t)floor((bounds.height - p.y) / bounds.height * drawable.height);
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
//
// Decoded, not stored: an NSEvent cannot be replayed later, and its
// locationInWindow and deltas are only meaningful now.
// ---------------------------------------------------------------------------
struct PendingInput {
    enum Kind { MOTION, BUTTON, WHEEL, KEY, MODIFIERS, FOCUS, RELEASE_CAPTURE } kind;
    int32_t x = 0, y = 0, dz = 0;
    double dx = 0, dy = 0; // Drawable deltas preserve subpixel motion in the queue.
    int drawable_w = 0, drawable_h = 0;
    int button = 0;
    bool down = false;
    bool inside = false, edge = false;
    uint16_t mac = 0;
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
        //
        // Without this a guest thread with nothing runnable parks on the
        // scheduler's condition for up to a second, having drained the queue
        // on its way in, and input arriving a moment later sits in hand while
        // it sleeps out the slice.
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

// What the scheduler asks and calls. `drain` runs from a point where nothing
// else is in guest code, so it is as safe as the tick.
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
    // belongs to the AppKit thread, which observes this at the next pump.
    g_platform_capture_requested = want;
    if ([NSThread isMainThread])
        update_platform_pointer_capture();
}

// Capture is for a window that is key, showing, and not displaying the mod
// settings page - the page is navigated with a real cursor. It is not tied to
// what the guest is doing: an FMV reads the mouse exactly as gameplay does.
bool pointer_capture_wanted() {
    if (!g_focused || !g_window || !g_window.isKeyWindow || g_escape_held)
        return false;
    if (g_window.isMiniaturized || !g_window.isVisible)
        return false;
    if (mods_page_visible())
        return false;
    return true;
}

// Fullscreen edge scrolling belongs to the game. Auto-hide still reveals the
// Dock/menu bar when the associated OS pointer reaches an edge; hiding the
// cursor alone does not prevent that. Keep absolute accelerated pointer input
// and use AppKit's fullscreen presentation policy to suppress those reveals.
void update_fullscreen_presentation() {
    if (!g_window || g_fullscreen_transition ||
        !(g_window.styleMask & NSWindowStyleMaskFullScreen)) {
        g_fullscreen_presentation_state = -1;
        return;
    }
    const bool playing = g_platform_capture_requested && pointer_capture_wanted() &&
                         NSApp.isActive && !g_close_requested;
    // Fullscreen's effective options need not equal the application's getter.
    // Apply on state changes, not on every event-pump pass (or recursively
    // while AppKit processes the change).
    if (g_fullscreen_presentation_state == int(playing))
        return;
    g_fullscreen_presentation_state = int(playing);
    const auto options = host_fullscreen_presentation(g_fullscreen_presentation, playing);
    NSApp.presentationOptions = options;
    fprintf(stderr,
            "[host] fullscreen edge scrolling %s (requested 0x%lx, app 0x%lx, system 0x%lx)\n",
            playing ? "owns screen edges" : "released to macOS", (unsigned long)options,
            (unsigned long)NSApp.presentationOptions,
            (unsigned long)NSApp.currentSystemPresentationOptions);
}

void update_platform_pointer_capture() {
    const bool want = g_platform_capture_requested && pointer_capture_wanted() && NSApp.isActive &&
                      !g_close_requested && !g_fullscreen_transition;
    if (want && !g_pointer_hidden) {
        [NSCursor hide];
        g_pointer_hidden = true;
    }
    if (!want && g_pointer_hidden) {
        [NSCursor unhide];
        g_pointer_hidden = false;
    }

    // A regular window retains its resize/desktop escape behavior. Borderless
    // and fullscreen need real confinement; hiding alone leaves OS hot edges
    // reachable. The view's safe bounds keep the notch/layout fix intact.
    NSRect rect = want && g_window_mode != 0 && g_view
                      ? host_pointer_confinement_rect(g_view.bounds)
                      : NSZeroRect;
    if (g_window && [g_window respondsToSelector:@selector(setMouseConfinementRect:)]) {
        const NSRect windowRect =
            NSIsEmptyRect(rect) ? NSZeroRect : [g_view convertRect:rect toView:nil];
        if (!NSEqualRects(g_pointer_window_confinement, windowRect)) {
            g_pointer_window_confinement = windowRect;
            g_window.mouseConfinementRect = windowRect;
            const NSRect actual = g_window.mouseConfinementRect;
            fprintf(stderr, "[host] native confinement readback: %.1f,%.1f %.1fx%.1f points\n",
                    actual.origin.x, actual.origin.y, actual.size.width, actual.size.height);
        }
        if (!NSEqualRects(g_pointer_confinement, rect)) {
            fprintf(stderr,
                    "[host] pointer confinement: %.1f,%.1f %.1fx%.1f points (window mode %d)\n",
                    windowRect.origin.x, windowRect.origin.y, windowRect.size.width,
                    windowRect.size.height, g_window_mode);
            g_pointer_confinement = rect;
            g_pointer_sample_valid = false;
        }
    }
    update_fullscreen_presentation();
}

bool window_has_resize_edges() {
    return g_window_mode == 0 && !g_fullscreen_transition &&
           !(g_window.styleMask & NSWindowStyleMaskFullScreen);
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
    if (trace && CACurrentMediaTime() - last_trace >= 0.1) {
        last_trace = CACurrentMediaTime();
        const auto guest = host_guest_pointer_resolve(g_mem, GUEST_SIZE, 0x00d0595c);
        fprintf(stderr,
                "[pointer-game] drawable %d,%d hit %d at %d,%d delivered %d guest %d,%d bounds "
                "%d,%d,%d,%d\n",
                x, y, int(hit.kind), hit.gx, hit.gy, delivered, guest.x, guest.y, guest.left,
                guest.top, guest.right, guest.bottom);
    }
    if (host_pointer_captured() && g_buttons && g_view && window_has_resize_edges()) {
        double px, py;
        host_pointer_drawable_position(&px, &py);
        CGSize size = [g_view convertRectToBacking:g_view.bounds].size;
        const double margin = 8 * g_window.backingScaleFactor;
        if (host_pointer_at_resize_edge(px, py, size.width, size.height, margin))
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

void sample_captured_pointer() {
    if (NSIsEmptyRect(g_pointer_confinement))
        return;
    const NSPoint point = [g_window convertPointFromScreen:NSEvent.mouseLocation];
    PendingInput e;
    e.kind = PendingInput::MOTION;
    view_point_to_drawable(point, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
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

void handle_mouse_move(NSEvent *event) {
    PendingInput e;
    e.kind = PendingInput::MOTION;
    NSPoint point = event.locationInWindow;
    if (@available(macOS 26.0, *)) {
        // Tahoe can replay stale event positions at window edges after a
        // Space/focus change. The global cursor query remains correct (also
        // used by SDL's Cocoa mouseMoved workaround). Convert through the
        // actual NSWindow so secondary displays and safe-area offsets work.
        point = [g_window convertPointFromScreen:NSEvent.mouseLocation];
    }
    view_point_to_drawable(point, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    static const bool trace = getenv("POPM_TRACE_POINTER") != nullptr;
    static double last_trace = 0;
    if (trace && event.timestamp - last_trace >= 0.1) {
        last_trace = event.timestamp;
        const NSPoint global = NSEvent.mouseLocation;
        const NSPoint view = [g_view convertPoint:point fromView:nil];
        fprintf(stderr,
                "[pointer] event %.1f,%.1f global %.1f,%.1f window %.1f,%.1f view %.1f,%.1f "
                "drawable %d,%d/%d,%d clip %d delta %.1f,%.1f\n",
                event.locationInWindow.x, event.locationInWindow.y, global.x, global.y, point.x,
                point.y, view.x, view.y, e.x, e.y, e.drawable_w, e.drawable_h,
                !NSIsEmptyRect(g_pointer_confinement), event.deltaX, event.deltaY);
    }
    if (!NSIsEmptyRect(g_pointer_confinement))
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

void handle_button(NSEvent *event, int button, bool down) {
    PendingInput e;
    e.kind = PendingInput::BUTTON;
    e.button = button;
    e.down = down;
    NSPoint point = [g_view convertPoint:event.locationInWindow fromView:nil];
    e.inside = NSPointInRect(point, g_view.bounds);
    e.edge = window_has_resize_edges() &&
             host_pointer_at_resize_edge(point.x, point.y, g_view.bounds.size.width,
                                         g_view.bounds.size.height, 8);
    view_point_to_drawable(event.locationInWindow, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    queue_or_apply(e);
}

// The key paths live in input_gate.cpp, which owns the decision, the delivery
// and the two kinds of key state they need. What is left here is the NSEvent
// decoding, which is the only part that needs AppKit.
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

void apply_key(uint16_t mac, bool down, uint32_t character, uint32_t flags) {
    // Every key event carries the modifier flags, and this is the only place
    // a modifier held across a focus change can be noticed: no flagsChanged
    // arrives for one that never changed. A diff, so it emits nothing when
    // the state already agrees.
    host_gate_sync_modifiers(flags);
    host_key_event(mac, down, character);
}

void handle_key(NSEvent *event, bool down) {
    if (event.keyCode == 53) {
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
    NSString *characters = event.charactersIgnoringModifiers;
    PendingInput e;
    e.kind = PendingInput::KEY;
    e.mac = event.keyCode;
    e.down = down;
    e.character = characters.length == 1 ? (uint32_t)[characters characterAtIndex:0] : 0u;
    e.flags = (uint32_t)event.modifierFlags;
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
    e.flags = focused ? (uint32_t)[NSEvent modifierFlags] : 0u;
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
        apply_key(e.mac, e.down, e.character, e.flags);
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
// and button events around it - which is the point. Applying a focus loss the
// moment it is noticed, while presses decoded before it are still queued,
// would replay those presses after the release and leave them stuck down.
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
        // effects - re-associating the cursor and showing it again - are keyed
        // off that flag, so doing it the other way round would leave the real
        // cursor hidden and decoupled with nothing recorded as holding it.
        apply_pointer_capture(false);
        // Nothing that was down can be seen coming up while another
        // application has the focus, so it all comes up now - including the
        // host's own copy of the button and key state.
        host_gate_release_all();
        g_buttons = 0;
    }
}

// One NSEvent, translated into both of the input paths the game reads: the
// DirectInput device state, and the Win32 message queue.
void handle_event(NSEvent *event) {
    bool ours = event.window == g_window;
    switch (event.type) {
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        if (ours || !NSIsEmptyRect(g_pointer_confinement))
            handle_mouse_move(event);
        break;
    case NSEventTypeLeftMouseDown:
        if (ours)
            handle_button(event, 0, true);
        break;
    case NSEventTypeLeftMouseUp:
        if (ours)
            handle_button(event, 0, false);
        break;
    case NSEventTypeRightMouseDown:
        if (ours)
            handle_button(event, 1, true);
        break;
    case NSEventTypeRightMouseUp:
        if (ours)
            handle_button(event, 1, false);
        break;
    case NSEventTypeOtherMouseDown:
        if (ours)
            handle_button(event, 2, true);
        break;
    case NSEventTypeOtherMouseUp:
        if (ours)
            handle_button(event, 2, false);
        break;
    case NSEventTypeScrollWheel:
        if (ours) {
            PendingInput e;
            e.kind = PendingInput::WHEEL;
            e.dz = (int32_t)lround(event.scrollingDeltaY * 120.0);
            // WM_MOUSEWHEEL carries the position of the wheel event itself. A
            // cached position would be wherever the pointer last moved to,
            // which is not where the wheel was turned if it was turned without
            // moving first - so it is read here and carried with the event.
            view_point_to_drawable(event.locationInWindow, &e.x, &e.y, &e.drawable_w,
                                   &e.drawable_h);
            queue_or_apply(e);
        }
        break;
    case NSEventTypeKeyDown:
        // Command combinations belong to the application, not the game:
        // Command-Q has to quit and Command-W has to close.
        if (event.modifierFlags & NSEventModifierFlagCommand) {
            [NSApp sendEvent:event];
            return;
        }
        handle_key(event, true);
        return; // the game consumed it
    case NSEventTypeKeyUp:
        if (event.modifierFlags & NSEventModifierFlagCommand) {
            [NSApp sendEvent:event];
            return;
        }
        handle_key(event, false);
        return;
    case NSEventTypeFlagsChanged: {
        PendingInput e;
        e.kind = PendingInput::MODIFIERS;
        e.flags = (uint32_t)event.modifierFlags;
        queue_or_apply(e);
    } break;
    default:
        break;
    }
    // Everything the game consumed still has to reach AppKit for the window to
    // behave: the key events above are the ones the game owns outright, and
    // they return rather than falling through to here.
    [NSApp sendEvent:event];
}

// Drains the event queue, waiting until `until` for the first one. A nil date
// means take what is already there and return; a real date is a genuine sleep
// inside AppKit, which is what stops the host spinning while the guest waits.
// Returns true when something changed the input state.
int service(NSDate *until) {
    uint32_t before = host_input_notify_count();
    size_t queued_before = 0;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        queued_before = g_pending_input.size();
    }
    NSDate *deadline = until;
    for (;;) {
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:deadline
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (!event)
            break;
        handle_event(event);
        // Whatever else is already queued is taken without waiting again: the
        // wait was for the first event, not for each of them.
        deadline = nil;
    }
    // Input that was only QUEUED still counts as input having arrived: the
    // guest should re-check its condition and come back through the tick,
    // which is where the queue is applied. Reporting nothing here would make
    // the guest sit out the rest of a slice for input already in hand.
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (g_pending_input.size() != queued_before)
            return 1;
    }
    return host_idle_wait_result(before, host_input_notify_count());
}

// The housekeeping every turn does once the events are in.
void after_events() {
    [NSApp updateWindows];
    // This host pumps events itself rather than entering NSApplication.run.
    // Commit layer installation/resize even during a busy guest event loop.
    [CATransaction flush];

    // A shell-launched process does not always come forward on its own, and a
    // window that never became key receives no key events at all. Ask again,
    // briefly, rather than once at startup and never afterwards.
    if (!NSApp.isActive && boot_elapsed() < 3.0) {
        [NSApp activateIgnoringOtherApps:YES];
        [g_window makeKeyAndOrderFront:nil];
    }
    note_focus(g_window.isKeyWindow && NSApp.isActive);

    // Capture follows the window and the page. Released whenever the window
    // is not key, is minimised or is showing the settings page; taken back by
    // a click, not automatically, so a user who alt-tabbed away does not have
    // the pointer seized the moment the window comes forward.
    if (host_pointer_captured() && !pointer_capture_wanted())
        apply_pointer_capture(false);
    update_platform_pointer_capture();
    // Fullscreen chrome can reroute or omit NSWindow motion events at an
    // edge. Captured input owns the pointer: sample its current position even
    // without an event for our window, and deliver only changes. The input
    // queue still serializes this with the guest and focus/button events.
    sample_captured_pointer();

    if (g_close_requested && !boot_close_requested())
        boot_request_close("the window was closed");
}

// Borderless is a desktop window: keep the entire drawable below the menu bar
// and camera housing, and outside the Dock. visibleFrame accounts for all of
// these, including on displays with a notch. Re-read it after screen changes.
void fit_borderless_window() {
    NSScreen *screen = g_window.screen ?: NSScreen.mainScreen;
    if (!screen)
        return;
    g_borderless_frame_dirty = false;
    const NSRect frame = screen.visibleFrame;
    if (NSIsEmptyRect(frame) || NSEqualRects(g_window.frame, frame))
        return;
    [g_window setFrame:frame display:YES];
    post_drawable_size();
    fprintf(stderr,
            "[host] borderless usable frame: %.0f,%.0f %.0fx%.0f points "
            "(screen %.0fx%.0f, scale %.1f)\n",
            frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
            screen.frame.size.width, screen.frame.size.height, g_window.backingScaleFactor);
}

// Consume a posted setting on the AppKit thread. Native fullscreen completes
// through delegate notifications; another setting can supersede it meanwhile.
void apply_window_mode() {
    int request = host_display_take_window();
    if (request >= 0)
        g_wanted_window_mode = request;
    if (!g_window || g_fullscreen_transition)
        return;
    const bool fullscreen = (g_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fullscreen) {
        if (g_wanted_window_mode != 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            [g_window toggleFullScreen:nil];
        }
        return;
    }
    if (g_window_mode == g_wanted_window_mode) {
        if (g_window_mode == 1 && g_borderless_frame_dirty)
            fit_borderless_window();
        return;
    }
    if (g_window_mode == 0)
        g_windowed_frame = g_window.frame;
    if (g_wanted_window_mode == 1) {
        g_window.styleMask = NSWindowStyleMaskBorderless;
        g_window_mode = 1;
        fit_borderless_window();
    } else {
        g_window.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        if (g_windowed_frame.size.width > 0)
            [g_window setFrame:g_windowed_frame display:YES];
        g_window_mode = 0;
        if (g_wanted_window_mode == 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            [g_window toggleFullScreen:nil];
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
        [g_window setContentSize:NSMakeSize(g_mode_w * scale, g_mode_h * scale)];
        [g_window center];
        post_drawable_size();
    }
    host_set_client_size(host_main_window(), g_mode_w, g_mode_h);
    // The window may never be smaller than the guest's frame, or the integer
    // scaling has no whole multiple to take.
    g_window.contentMinSize = NSMakeSize(g_mode_w, g_mode_h);
}

// True when this thread may touch AppKit at all.
bool on_host_thread() {
    if (boot_on_run_thread() && [NSThread isMainThread])
        return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        fprintf(stderr, "[host] a guest worker thread reached the host; AppKit is "
                        "serviced only on the main thread\n");
    }
    return false;
}

// One turn of the host's event loop, called from inside guest code.
//
// The tick can arrive on any guest thread: the runtime's cooperative scheduler
// hands the baton to real pthreads and whichever one holds it reads the clock.
// AppKit and Metal belong to the thread that called boot_run, so a tick on any
// other thread does nothing at all and hands control straight back - the
// scheduler will pass the baton on, and the main thread will pump when its
// turn comes.
void pump() {
    if (!on_host_thread())
        return;
    @autoreleasepool {
        // First: anything an idle slice decoded but could not apply. This
        // thread reached here through the guest's own clock read, so it holds
        // the baton and a mod callback dispatched from here is serialised
        // against every other guest thread, which is the whole point.
        deliver_pending_input();
        apply_window_mode();
        apply_mode_change();
        service(nil);
        after_events();
        host_gate_pointer_tick();
    }
}

// The runtime's idle wait, called on the run thread when the guest is about to
// block: a Sleep, or a wait on an object that nothing has signalled yet.
//
// Without it the host is never serviced while the guest waits, and since the
// guest spends most of a video frame waiting, the window stops answering and
// the pointer becomes a spinner. With it the wait happens inside AppKit, which
// is a real sleep rather than a spin, and every event that arrives during it is
// delivered immediately instead of at the end of the slice.
//
// Returns 1 when input arrived, so the caller can re-check its own condition
// at once: the game's DirectInput threads are woken by that input, and making
// them wait out the rest of the slice would be latency with no cause a person
// could see.
int idle_wait(double seconds) {
    if (!on_host_thread())
        return 0;
    if (seconds < 0.0)
        seconds = 0.0;
    int changed = 0;
    @autoreleasepool {
        apply_window_mode();
        apply_mode_change();
        changed = service(seconds > 0.0 ? [NSDate dateWithTimeIntervalSinceNow:seconds] : nil);
        after_events();
    }
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
        // Completed/presented frames and the continuous acceptance metric
        // are reported below. The old active-gap meter is not that metric.
    }
    // The two lines the display baseline is made of, in the same words the
    // smoke host uses, because display_compare.py parses the text and a
    // baseline is only comparable against a run that speaks it. Without these
    // the comparator could read a smoke log and not an app log, which is the
    // wrong way round for a tool whose acceptance figure is the live run.
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
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return "";
    std::string dir(path);
    for (int depth = 0; depth < 12; ++depth) {
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos)
            break;
        dir = dir.substr(0, slash);
        std::string candidate = dir + "/original/gog/D3DPopTB.exe";
        if (access(candidate.c_str(), R_OK) == 0) {
            // The runtime resolves the guest's relative paths against the
            // process's working directory, so the checkout root becomes it.
            if (chdir(dir.c_str()) != 0)
                fprintf(stderr, "[host] could not enter %s\n", dir.c_str());
            return candidate;
        }
    }
    return "";
}

} // namespace

@implementation PopAppDelegate
- (void)openSettings:(id)sender {
    (void)sender;
    mods_options_request();
}
- (void)applicationDidChangeScreenParameters:(NSNotification *)note {
    (void)note;
    g_borderless_frame_dirty = true;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    // Exactly what closing the window does. The guest runs its own shutdown,
    // the host unwinds it if it will not, and the mod teardown follows. Repeated
    // requests are harmless: after_events posts the close only once.
    g_close_requested = true;
    // Keep control on the guest's stack so after_events and boot_run can finish.
    // AppKit's deferred termination runs a modal loop on this same thread.
    return NSTerminateCancel;
}
@end

static CGDirectDisplayID current_display() {
    NSNumber *number = g_window.screen.deviceDescription[@"NSScreenNumber"];
    return number ? number.unsignedIntValue : CGMainDisplayID();
}
static void post_drawable_size() {
    CGSize size = [g_view convertRectToBacking:g_view.bounds].size;
    // A layer-hosting view leaves its layer geometry to the host.
    g_view.layer.frame = g_view.bounds;
    CAMetalLayer *layer = (CAMetalLayer *)g_view.layer;
    // AppKit owns layer geometry. Apply it immediately, including during live
    // resize; the presenter only receives the coalesced latest size.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contentsScale = g_window.backingScaleFactor;
    if (!CGSizeEqualToSize(layer.drawableSize, size))
        layer.drawableSize = size;
    [CATransaction commit];
    static int last_w = 0, last_h = 0;
    static CGDirectDisplayID last_display = 0;
    const auto display = current_display();
    if (last_w == int(size.width) && last_h == int(size.height) && last_display == display)
        return;
    last_w = int(size.width);
    last_h = int(size.height);
    last_display = display;
    host_present_resize(last_w, last_h);
}
static std::unique_ptr<gpu::Device> g_gpu;
static CAMetalLayer *install_metal_layer() {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = g_device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The worker copies a cached composition into a drawable for repeats.
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = 3;
    layer.allowsNextDrawableTimeout = YES;
    layer.contentsScale = g_window.backingScaleFactor;
    layer.drawableSize = [g_view convertRectToBacking:g_view.bounds].size;
    // Assign first to make this a layer-hosting view, owned by our worker.
    g_view.layer = layer;
    g_view.wantsLayer = YES;
    layer.frame = g_view.bounds;
    layer.presentsWithTransaction = NO;
    return layer;
}

@implementation PopWindowDelegate
- (void)windowDidResignKey:(NSNotification *)note {
    (void)note;
    g_platform_capture_requested = false;
    update_platform_pointer_capture();
    g_escape_held = false;
    note_focus(false);
    update_fullscreen_presentation();
}
- (void)windowWillStartLiveResize:(NSNotification *)note {
    (void)note;
    g_platform_capture_requested = false;
    update_platform_pointer_capture();
    PendingInput release;
    release.kind = PendingInput::RELEASE_CAPTURE;
    queue_or_apply(release);
}
- (void)windowDidEnterFullScreen:(NSNotification *)note {
    (void)note;
    g_fullscreen_transition = false;
    g_window_mode = 2;
    post_drawable_size();
    update_platform_pointer_capture();
}
- (NSApplicationPresentationOptions)window:(NSWindow *)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)options {
    (void)window;
    g_fullscreen_presentation = options;
    return host_fullscreen_presentation(options, !g_escape_held && !mods_page_visible());
}
- (void)windowWillExitFullScreen:(NSNotification *)note {
    (void)note;
    // Let AppKit restore its regular presentation while it animates out.
    g_fullscreen_transition = true;
    update_platform_pointer_capture();
    NSApp.presentationOptions = g_fullscreen_presentation;
}
- (void)windowDidExitFullScreen:(NSNotification *)note {
    (void)note;
    g_fullscreen_transition = false;
    g_window_mode = 0;
    post_drawable_size();
    update_platform_pointer_capture();
}
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window {
    (void)window;
    g_fullscreen_transition = false;
    g_wanted_window_mode = g_window_mode = 0;
}
- (void)windowDidFailToExitFullScreen:(NSWindow *)window {
    (void)window;
    g_fullscreen_transition = false;
    g_wanted_window_mode = g_window_mode = 2;
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    // The guest closes itself: WM_CLOSE runs its own shutdown path, and the
    // window stays up until it is done so the last frame does not vanish
    // mid-teardown.
    g_close_requested = true;
    return NO;
}
- (void)windowDidResize:(NSNotification *)note {
    (void)note;
    if (g_view)
        post_drawable_size();
}
- (void)windowDidChangeBackingProperties:(NSNotification *)note {
    (void)note;
    if (g_view)
        post_drawable_size();
}
- (void)windowDidChangeScreen:(NSNotification *)note {
    (void)note;
    g_borderless_frame_dirty = true;
    if (!g_view || !g_device)
        return;
    CAMetalLayer *layer = install_metal_layer();
    CGSize size = [g_view convertRectToBacking:g_view.bounds].size;
    host_present_install_surface((__bridge void *)layer, int(size.width), int(size.height));
}
@end

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        std::string exe = find_exe_relative_to_bundle();
        if (exe.empty()) {
            fprintf(stderr, "PopRecomp: original/gog/D3DPopTB.exe was not found above this "
                            "executable.\nRun the binary from the checkout, or set "
                            "POP_RECOMP_EXE to the image.\n");
            return 2;
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        g_gpu = gpu::create_default_device();
        id<MTLDevice> device = g_gpu ? gpu::metal::device(g_gpu.get()) : nil;
        if (!device) {
            fprintf(stderr, "PopRecomp: no Metal device is available\n");
            return 3;
        }

        int scale = window_scale_for(g_mode_w, g_mode_h);
        NSRect frame = NSMakeRect(0, 0, g_mode_w * scale, g_mode_h * scale);
        g_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        g_window.title = @"Populous: The Beginning";
        g_window.acceptsMouseMovedEvents = YES;
        g_window.contentMinSize = NSMakeSize(g_mode_w, g_mode_h);
        g_window_delegate = [[PopWindowDelegate alloc] init];
        g_window.delegate = g_window_delegate;

        g_device = device;
        g_view = [[PopMetalView alloc] initWithFrame:frame];
        g_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        g_window.contentView = g_view;

        // The renderer first: the presenter takes its command queue so a
        // present cannot run ahead of the scene it is showing.
        D3DRenderer *renderer = new D3DRenderer(g_gpu.get());
        if (!renderer->ok())
            return 3;
        D3DRenderer::setShared(renderer);
        host_present_set_device(g_gpu.get());

        CAMetalLayer *layer = install_metal_layer();
        fprintf(stderr, "Mouse capture: click inside to capture; hold Escape to release; drag to "
                        "the window edge to resize.\n");
        NSString *modesPath = [[NSBundle mainBundle] pathForResource:@"classic-modes"
                                                              ofType:@"json"];
        mods_display_live_defaults();
        mods_display_load_modes(modesPath ? modesPath.fileSystemRepresentation
                                          : "tools/recomp/baseline/classic-modes.json");
        host_present_on_mode_change(on_mode_change);
        // Every change to the input state wakes the guest's DirectInput
        // threads, which wait on an event rather than polling. Installed here
        // rather than referenced from input.mm, which links none of the shims.
        host_input_set_notify(dinput_host_input_changed);

        // Without a menu there is no Command-Q, and an application that can
        // only be quit by closing its window is a trap when the guest wedges.
        NSMenu *menubar = [[NSMenu alloc] init];
        NSMenuItem *app_item = [[NSMenuItem alloc] init];
        [menubar addItem:app_item];
        NSMenu *app_menu = [[NSMenu alloc] init];
        g_app_delegate = [[PopAppDelegate alloc] init];
        NSMenuItem *settings_item = [app_menu addItemWithTitle:@"Settings…"
                                                        action:@selector(openSettings:)
                                                 keyEquivalent:@","];
        settings_item.target = g_app_delegate;
        [app_menu addItem:[NSMenuItem separatorItem]];
        [app_menu addItemWithTitle:@"Quit Populous"
                            action:@selector(terminate:)
                     keyEquivalent:@"q"];
        app_item.submenu = app_menu;
        NSApp.mainMenu = menubar;

        [g_window center];
        [g_window makeKeyAndOrderFront:nil];
        [g_window makeFirstResponder:g_view];
        NSApp.delegate = g_app_delegate;
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp updateWindows];
        [g_window displayIfNeeded];
        [CATransaction flush];
        // Start against the visible window's display and attached layer.
        layer = (CAMetalLayer *)g_view.layer;
        NSCAssert(g_window.isVisible && g_window.contentView == g_view &&
                      g_view.window == g_window && g_view.wantsLayer &&
                      [layer isKindOfClass:[CAMetalLayer class]] && layer.drawableSize.width > 0 &&
                      layer.drawableSize.height > 0,
                  @"presenter must start on the visible window's attached, nonzero Metal layer");
        host_present_start((__bridge void *)layer, int(layer.drawableSize.width),
                           int(layer.drawableSize.height));
        // Said out loud, because "the keyboard does nothing" and "the window
        // never became key" look identical from the outside.
        printf("PopRecomp: window key %s, app active %s, first responder %s\n",
               g_window.isKeyWindow ? "yes" : "no", NSApp.isActive ? "yes" : "no",
               g_window.firstResponder == g_view ? "the Metal view" : "something else");
        fflush(stdout);

        BootOptions options;
        options.name = "windowed";
        options.exe = exe.c_str();
        // This host has a real window with real focus, so activation is
        // delivered from the NSEvents that carry it rather than synthesised.
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
        // there would freeze the game at the moment the music starts. Here
        // there is no guest thread yet, and the executable is loaded, which is
        // what the bank's path is resolved against.
        host_midi_startup(win32_midi_soundfont_path().c_str());

        // POP_HOST_AUDIO_CAPTURE=<path.wav> writes the mixer's own output for
        // the whole run. The shim can report every sound delivered and the run
        // can still sound wrong, because everything after the guest hands over
        // its PCM is the host's; this is the only way to look at that.
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
        [g_window orderOut:nil];
        // Cmd-Q follows this same ordinary return after guest, mod and presenter
        // teardown. There is no outstanding AppKit termination reply.
    }
    return 0;
}
