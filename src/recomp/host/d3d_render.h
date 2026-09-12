// d3d_render.h - what main.mm, present.mm and the tests need from the Metal
// renderer behind host_d3d_draw.
#pragma once
#include <stdint.h>
#include "../dx/host_api.h"
#include "../dx/coherence.h"

// One vertex after the three D3DVT_ layouts have been decoded into the single
// shape the shader takes. Position is object space for D3DVT_VERTEX and
// D3DVT_LVERTEX, and already-clip-space for D3DVT_TLVERTEX, which the guest
// transformed itself.
struct HostD3DVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;     // diffuse, 0..1
    float sr, sg, sb, sa; // specular; its alpha is the vertex fog factor
};

// What a command's primitive becomes once expanded.
enum HostD3DPrimitive {
    HOST_D3D_POINTS = 0,
    HOST_D3D_LINES = 1,
    HOST_D3D_TRIANGLES = 2,
    HOST_D3D_UNSUPPORTED = -1,
};
enum HostD3DPrimitive host_d3d_primitive_kind(const struct HostD3DDraw *cmd);

// Decodes a command's vertices and expands its primitive into an independent
// list - triangles in threes, lines in pairs, points singly. Metal has no
// triangle fan, and expanding here is also what makes flat shading a matter of
// copying the first vertex's colour rather than a pipeline mode.
//
// `viewport` and the vertex type are used to turn D3DVT_TLVERTEX's screen
// coordinates back into clip space.
//
// Returns the number of vertices written, or -1 when the command names a
// primitive or a vertex type this renderer does not draw. Writes nothing past
// `max_out`; pass a null `out` to ask for the count.
int host_d3d_expand(const struct HostD3DDraw *cmd, struct HostD3DVertex *out, int max_out);

// Draws submitted since the last present. Non-zero means the frame about to be
// presented had the Direct3D device draw into it, which is this host's signal
// that the guest is in a level rather than in the front end: the menu renders
// in software into a DirectDraw surface and creates no device draws at all.
// The frame-rate acceptance uses it so a fast menu cannot stand in for
// gameplay.
uint32_t host_d3d_draws_since_present(void);
void host_d3d_note_presented(void);

// Guest thread seal boundary: closes/commits any open prefix without readback.
// EndScene is not a seal, so a pump can encounter an open batch.
void host_d3d_seal_commands(void);

// Cumulative storage allocations and bounded submissions, for regressions.
struct HostCommandStorageStats {
    uint64_t cpu_growths, argument_buffers, scene_textures;
    uint64_t early_submissions, parallel_readbacks;
};
struct HostHDTextureStats {
    uint64_t draws, loads, hits, refused, resident_bytes, budget_bytes;
    uint64_t detail_draws;
};
// Totals, for the run report.
uint32_t host_d3d_total_draws(void);
uint32_t host_d3d_total_textures(void);
// Whether the renderer still holds that revision of that texture. The property
// a frame's lease exists to give, and invisible from outside without this.
// C linkage, like the rest of the host API, because the shim calls it too.
extern "C" int host_render_texture_revision_alive_for_test(uint32_t handle, uint32_t revision);
// Drops every texture and revision, for a test that wants to count from zero.
extern "C" void host_render_reset_for_test(void);
// Frames the renderer put back into its render target's own memory. A gameplay
// run with a Direct3D device and none of these has a broken flush path.
uint32_t host_d3d_total_flushes(void);
// The largest observed share of the native render target above black, sampled
// from existing coherence, legacy and explicit scene readbacks. Partial reads
// contribute only their lit pixels divided by the whole target area (a lower
// bound); full reads give the exact ratio. No additional GPU readback is made.
// Alpha is excluded; host_d3d_reset_coherence starts a new observation interval.
double host_d3d_peak_nonblack(void);

#ifdef __OBJC__
#import <Metal/Metal.h>

@interface PopD3DRenderer : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device;
// The host's queue, so renderer and presenter share one: commands on one
// queue run in submission order and nothing orders two queues against each other.
- (instancetype)initWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue;
- (HostHDTextureStats)hdTextureStats;
// The renderer the host_d3d_* callbacks use. main.mm sets it once the Metal
// device exists; the tests set their own.
+ (PopD3DRenderer *)shared;
+ (void)setShared:(PopD3DRenderer *)renderer;

// The colour buffer the scene is drawn into. It mirrors the render target's
// pixels; the surface's own memory is the copy that counts.
@property(nonatomic, readonly) id<MTLTexture> colorTarget;
// Sealed-frame consumers use the frame's texture and completion, not the
// mutable current target. The frame lease must cover GPU completion/present.
- (id<MTLTexture>)colorTargetForFrame:(uint64_t)frame;
- (id<MTLCommandBuffer>)completionForFrame:(uint64_t)frame;
// The queue the scene is submitted on. present.mm draws the drawable on this
// same queue, because commands on one queue run in submission order and
// nothing orders two queues against each other.
@property(nonatomic, readonly) id<MTLCommandQueue> commandQueue;

// The device's render target, or null when the device goes away.
- (void)setRenderTarget:(const struct HostD3DSurface *)target;
// Put anything drawn since the last flush into the surface's own pixels.
- (void)flushSurface:(const struct HostD3DSurface *)surface why:(const char *)why;
// The guest arena is gone. Drop everything pending without writing it: every
// pixel pointer the renderer holds names memory that is not there any more.
- (void)discard;

// Task 7 supplies drawable resolution here; zero restores guest dimensions.
- (void)setSceneWidth:(int)width height:(int)height;
- (void)beginScene;
- (void)endScene;
- (void)sealCommands;
- (void)clearFlags:(uint32_t)flags
             rects:(const int32_t *)rects
             count:(uint32_t)count
             color:(uint32_t)color
             depth:(float)depth;
- (void)draw:(const struct HostD3DDraw *)cmd;
// The same draw, sampling a NAMED texture revision rather than whichever one
// the guest has uploaded most recently. A frame is composited after the guest
// has moved on, so "the current texture" is the wrong answer by then.
- (void)draw:(const struct HostD3DDraw *)cmd revision:(uint32_t)revision;
- (void)uploadTexture:(const struct HostD3DTexture *)tex;
- (void)destroyTexture:(uint32_t)handle;
// A revision is kept while any frame holds it, and dropped when it is neither
// the current one nor held.
// NO when the renderer never received that revision, so the caller can upload
// it and ask again.
- (BOOL)retainTexture:(uint32_t)handle revision:(uint32_t)revision;
- (void)releaseTexture:(uint32_t)handle revision:(uint32_t)revision;
- (BOOL)hasTexture:(uint32_t)handle revision:(uint32_t)revision;
- (HostCommandStorageStats)commandStorageStats;

// Waits for the scene to finish and copies the colour target out as tightly
// packed BGRA8. For the offscreen tests; nothing on the display path uses it.
- (BOOL)readPixels:(void *)out width:(int *)width height:(int *)height;
@end
#endif

void host_d3d_collect_present_targets();
