// present.h - what main.mm and the tests need from present.mm.
//
// The pixel maths is here as plain functions with no Metal in them, because
// that is the part a headless test can check: a palette expansion either
// produces the right bytes or it does not, and no GPU is needed to find out.
#pragma once
#include <stdint.h>
#include <stddef.h> // size_t, for the stats line writers below

#ifdef __cplusplus
extern "C" {
#endif

// Expand one presented surface to 8-bit RGBA, the format the presenter
// uploads. `pitch` is the distance in bytes between rows of `src`; `out` is
// tightly packed w*h*4 bytes.
//
// The palette entries are the shim's 0x00RRGGBB, so the alpha byte in them is
// not alpha and is replaced with 255: a presented frame is opaque.
void host_present_expand_indexed(const uint8_t *src, int w, int h, int pitch,
                                 const uint32_t *palette, uint8_t *out);
// 5-6-5, the depth the front end switches to. Each channel is scaled to the
// full 0..255 range rather than shifted, so white stays white.
void host_present_expand_rgb565(const uint8_t *src, int w, int h, int pitch, uint8_t *out);

// Where a guest frame lands inside a drawable, aspect preserved. Integer
// scaling when a whole multiple fits, which keeps 320x200-era art free of
// resampling seams; the largest fractional scale otherwise, because a window
// smaller than the guest mode still has to show all of it.
struct HostFit {
    double x, y, w, h; // the drawable rectangle the frame occupies
    double scale;      // drawable pixels per guest pixel
};
struct HostFit host_present_fit(double drawable_w, double drawable_h, int guest_w, int guest_h);

// The same mapping run backwards: a point in the drawable, in drawable pixels
// with the origin at the top left, becomes a pixel in the guest's frame. A
// point in the letterbox border clamps to the edge rather than being dropped,
// which is what a full-screen game's own cursor clamp does anyway.
void host_present_point_to_guest(double drawable_w, double drawable_h, int guest_w, int guest_h,
                                 double px, double py, int32_t *out_x, int32_t *out_y);

// ---------------------------------------------------------------------------
// Frame dumps, for looking at actual pixels instead of describing them.
//
// POP_HOST_DUMP_EVERY=N writes every Nth presented frame - what the window
// shows, after everything has been composited into the surface - and the
// Direct3D render target as it stood at EndScene, to
// build/recomp/live/frames/. POP_HOST_DUMP_DIR moves that directory.
// ---------------------------------------------------------------------------
// 0 when dumping is off.
uint32_t host_dump_every(void);
const char *host_dump_dir(void);
// Binary PPM, which needs no library to write and none to read. `rgb` is
// tightly packed w*h*3.
int host_write_ppm(const char *path, const uint8_t *rgb, int w, int h);

// ---------------------------------------------------------------------------
// The two script verbs whose decision is arithmetic, split out so a test can
// make it without a game, a window or a frame that came from one.
// ---------------------------------------------------------------------------

// What a `probe` decided. The frame is 24-bit RGB, w*h, top row first.
typedef enum HostProbeVerdict {
    HOST_PROBE_MATCH = 0,    // within tolerance on every channel
    HOST_PROBE_MISMATCH = 1, // a channel was further out than the tolerance
    HOST_PROBE_OUTSIDE = 2,  // the coordinate is not in the frame
    HOST_PROBE_NO_FRAME = 3, // nothing has been presented yet
} HostProbeVerdict;

// `found` and `worst` are filled in for MATCH and MISMATCH and left alone
// otherwise. Any of the out pointers may be null.
int host_probe_pixel(const uint8_t *rgb, int w, int h, int x, int y, int r, int g, int b, int tol,
                     int *found_r, int *found_g, int *found_b, int *worst);

// Whether an armed `dumpat` fires on this present. It fires only on a gameplay
// frame: a reference frame is a picture of the simulation, and a menu or a
// movie frame that happened to satisfy the claim is a picture of something
// else. `at_least` selects >= over >.
int host_dumpat_should_fire(int armed, int is_gameplay, double now, double threshold, int at_least);

// The display mode the guest most recently selected, 0 before it selects one.
void host_present_mode(int *w, int *h, int *bpp);
// Enhanced layered gameplay obtains world/UI pixels from sealed frame leases.
// Other classes and Classic still require the complete CPU compatibility image.
int host_present_needs_legacy_pixels(void);
// Called on the guest's thread when the mode changes, so the host can resize
// its window. It is not safe to touch AppKit from it directly; main.mm records
// the request and acts on it from its own event pump.
void host_present_on_mode_change(void (*fn)(int w, int h, int bpp));

// How many frames have been presented.
uint32_t host_present_count(void);

// Presentation counted per display mode, because a frame rate measured over a
// whole run is a frame rate for the menu and the loading screen as much as for
// gameplay, and those are not the same number.
//
// `presents` is how many times the guest presented at this mode. `drawn`
// counts only frames that got a drawable and had a present enqueued on it: a
// present that arrives while the main thread is elsewhere is staged, a second
// one before the main thread comes back replaces it rather than queueing, and
// a turn where the drawable was not available draws nothing at all.
//
// Two durations, because neither one alone is the answer:
//
//   * `seconds` is the wall clock spent at this mode, loading screens and
//     pauses included. It is what the run took, not what it ran at.
//   * `active_seconds` sums only the gaps between consecutive drawn frames
//     that are short enough to be part of a running loop, so a mode that was
//     left sitting on a loading screen does not have that time counted
//     against its rate.
//   * `best_window_fps` is the highest rate sustained over any five-second
//     stretch. That is the figure the acceptance run wants: "30 fps while
//     playing" is a claim about a stretch of play, not about an average that
//     a menu can drag down or a burst can flatter.
// The frame-rate arithmetic, kept apart from the presenter so it can be driven
// with made-up timestamps in a test. A frame is folded in when it has actually
// reached the screen.
#define HOST_RATE_WINDOW_FRAMES 1024
struct HostRateMeter {
    uint32_t drawn;
    double active_seconds;  // gaps short enough to be a running loop
    double best_window_fps; // the best five seconds of it
    double last;            // when the previous frame was drawn
    double window[HOST_RATE_WINDOW_FRAMES];
    int head, count;
};
void host_rate_meter_reset(struct HostRateMeter *meter);
void host_rate_meter_frame(struct HostRateMeter *meter, double now);
// The longest gap between two frames that still counts as a running loop, and
// the width of the sustained-rate window, both in seconds.
double host_rate_active_gap(void);
double host_rate_window(void);

struct HostPresentRate {
    int w, h, bpp;
    uint32_t presents;
    double seconds;
    // Every frame drawn at this mode.
    struct HostRateMeter frames;
    // Only the frames the Direct3D device had drawn into, which is this host's
    // signal for "the guest is in a level": the front end renders in software
    // into a DirectDraw surface and submits no device draws at all. The
    // acceptance run reads this one, so a front end that runs at 200 fps
    // cannot stand in for gameplay at the same resolution.
    struct HostRateMeter gameplay;
};
int host_present_rate_count(void);
void host_present_rate(int index, struct HostPresentRate *out);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// The stats line, shared by every host.
//
// One formatter, here, because two hosts printing "the same" line in two files
// drift: the windowed host and the smoke were already a per-mode gameplay line
// apart, and a baseline compares the text. host_stats_gameplay_line() writes
// the line both print, and tools/recomp/display_compare.py parses exactly it.
//
// The phases are wall time, in milliseconds, accumulated over the run:
//
//   guest      translated guest code, which is also handed to the sampling
//              profiler through profile_note_guest_ms so one measurement
//              serves both
//   shim       host code answering the guest's DirectX and Win32 calls
//   composite  the presenter's own work
//   wait       retained instrumentation field; target exhaustion now drops
//              the frame and never waits on presenter completion
//
// Uploads and readbacks are counts, and readbacks carry the reason, because
// "one readback" and "one readback because a Blt read the surface" are
// different facts and only the second says what to fix.
// ---------------------------------------------------------------------------
typedef enum HostPhase {
    HOST_PHASE_GUEST = 0,
    HOST_PHASE_SHIM,
    HOST_PHASE_COMPOSITE,
    HOST_PHASE_WAIT,
    HOST_PHASE_COUNT
} HostPhase;

// Why a frame's pixels had to come back from the GPU. One per reader in the
// coherence contract, so a breach names the path that caused it.
typedef enum HostReadbackReason {
    HOST_READBACK_LOCK = 0,
    HOST_READBACK_GETDC,
    HOST_READBACK_BLT_SOURCE,
    HOST_READBACK_DSTKEY,
    HOST_READBACK_DUPLICATE,
    HOST_READBACK_TEXTURE_LOAD,
    HOST_READBACK_FLIP,
    HOST_READBACK_COUNT
} HostReadbackReason;

#ifdef __cplusplus
extern "C" {
#endif
void host_stats_note_phase(HostPhase phase, double ms);
void host_stats_note_upload(uint32_t bytes);
void host_stats_note_readback(HostReadbackReason reason);
// Writes the line both hosts print. Returns the length written, or 0 if the
// buffer is too small - and writes nothing rather than half a line, because a
// truncated stats line parses as different numbers rather than as an error.
size_t host_stats_gameplay_line(char *out, size_t cap);
// The access line, from host_access_counts(). Same contract.
size_t host_stats_access_line(char *out, size_t cap);
// For tests: forget everything counted so far.
void host_stats_reset(void);
#ifdef __cplusplus
}
#endif

// Presentation counters are independent of the old active-gap rate meter.
#ifdef __cplusplus
extern "C" {
#endif
void host_present_stop(void); // only after the guest scheduler has stopped
void host_frame_seal(void);
void host_present_first_write(void);
void host_present_stage_rgba(const uint8_t *rgba, int w, int h);
// Includes bounded command-completion recovery; faults reports that degraded
// path. Only drawable acknowledgements enter the windowed continuous metric.
uint64_t host_present_unique_completed(void);
uint64_t host_present_repeats(void);
uint64_t host_present_drops(void);
uint64_t host_present_waits(void);
// Distinct fault reasons since service start, including completion fallback.
uint64_t host_present_faults(void);
uint64_t host_present_scene_reused(void);
uint64_t host_present_transition_epoch(void);
int host_metric_continuous(double *min_bucket, double *elapsed_s);
int host_metric_throughput(double *min_bucket, double *elapsed_s);
void host_present_tick_for_test(double ts);
#ifdef __cplusplus
}
#endif

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CGDirectDisplay.h>
#include "compositor.h"
#include <vector>

void host_present_start(CAMetalLayer *layer, CGDirectDisplayID display);
void host_present_start_offscreen(id<MTLCommandQueue> queue, int w, int h);
// Main-thread messages. No GPU work or wait for the worker here.
void host_present_resize(int drawable_w, int drawable_h, CGDirectDisplayID display);
void host_present_install_layer(CAMetalLayer *layer, int w, int h, CGDirectDisplayID display);
void host_present_set_shared_queue(id<MTLCommandQueue> queue);
id<MTLCommandQueue> host_present_shared_queue(void);

struct HostSceneTarget {
    id<MTLTexture> world = nil, overlay = nil;
    int w = 0, h = 0;
};
// Guest thread, first write only. Four slots; exhaustion drops the frame without waiting.
bool host_present_running();
void host_present_drop_current();
#include <memory>
// A dirty surface pins its pool storage until coherence or transfer to a new writer.
std::shared_ptr<void> host_present_target_lease(id<MTLTexture> world);
HostSceneTarget host_present_acquire_target(int guest_w, int guest_h, int scene_w, int scene_h);
// Copy the UI by value, never a pointer into a sealed frame. The world/overlay
// must belong to the acquired target. Task 4/6 supplies this before seal.
void host_present_set_input(const CompositorInput *input);
// Register BEFORE committing each prefix buffer on the renderer's queue. Its
// completion fences all earlier prefixes; dropped frames retire behind it.
void host_present_track_command(id<MTLCommandBuffer> command);

// Triple-buffered publication. T9 consumes a VALUE under the guest baton.
bool host_present_copy_layout(LayoutSnapshot *out);
// Read-only headless evidence. Only a GPU-completed composition is exposed;
// copying it never waits for a pending frame while the guest owns the baton.
struct HostCompletedComposite {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, guest_w = 0, guest_h = 0;
    HostScreenClass cls = HOST_SCREEN_MENU;
    uint64_t frame_id = 0;
    LayoutSnapshot layout;
};
bool host_present_copy_composite(HostCompletedComposite *out);
// Headless capture only: factory runs under the baton at seal; its returned
// closure owns values and runs after that frame's GPU completion. No guest wait.
#include <functional>
using HostFrameCapture = std::function<void(const HostCompletedComposite &)>;
using HostCaptureFactory = HostFrameCapture (*)(HostScreenClass);
void host_present_set_capture_factory(HostCaptureFactory factory);
#endif
