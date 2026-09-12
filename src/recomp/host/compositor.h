#pragma once

#include "../dx/host_api.h"
#include <vector>
#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

struct UiElement;
struct UiFrame;

enum CompositorMode { COMP_ENHANCED, COMP_CLASSIC };
struct Anchor {
    int8_t h, v;
}; // -1 left/top, 0 centre, 1 right/bottom
struct SceneMapping {
    float scale_x, scale_y, offset_x, offset_y;
    int domain_w;
};
struct LayoutRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};
struct LayoutElement {
    uint64_t id = 0;
    uint32_t last_seq = 0;
    LayoutRect guest, drawable;
    bool is_cursor = false;
};
// No frame, UiFrame, texture or pixel pointers cross the presenter/baton seam.
struct LayoutSnapshot {
    uint64_t frame_id = 0;
    std::vector<LayoutElement> elements;
    SceneMapping scene{};
    int drawable_w = 0, drawable_h = 0, guest_w = 0, guest_h = 0, ui_scale = 1;
    HostScreenClass cls = HOST_SCREEN_MENU;
    int scale_override = 0;
    bool legacy = false, classic = false;
};
struct CompositorInput;
LayoutSnapshot compositor_layout_snapshot(const CompositorInput *in);
// Rebuild geometry only, without frame storage, GPU work or presenter waits.
LayoutSnapshot compositor_resize_layout(const LayoutSnapshot &layout, int w, int h);
// Host pointer in drawable pixels. Cursors keep UI-scaled size and no anchor.
void compositor_set_pointer_position(bool valid, int32_t x, int32_t y);

#ifdef __OBJC__
struct CompositorInput {
    HostScreenClass cls;
    const UiFrame *ui;
    id<MTLTexture> world;
    // Amendment 5: guest UI-space texture, possibly already at UI scale,
    // with premultiplied alpha (rendered by blending onto transparent black).
    // Scene-mapped overlays have already been rendered into world.
    id<MTLTexture> overlay;
    int guest_w, guest_h;
    int drawable_w, drawable_h;
    int scale_override;
    SceneMapping scene;
    bool legacy;
    id<MTLTexture> legacy_frame;
    id<MTLTexture> settings_page = nil; // immutable host-rendered 640x480 page, alpha outside panel
    bool classic = false; // guest-resolution layered scene, aspect-preserving whole-frame mapping
};

// Encode only: never commits, waits, presents, or touches AppKit. The caller
// supplies a retaining command buffer, a shader-readable input and a distinct
// single-sample render target of drawable_w x drawable_h. World is the already
// mapped scene target; it is scaled to the drawable without a second mapping.
void compositor_compose(const CompositorInput *in, id<MTLTexture> out, id<MTLCommandBuffer> cb);
int compositor_ui_scale(int drawable_h, int guest_h, int override_);
Anchor compositor_default_anchor(const UiElement *e, int guest_w, int guest_h);
// Overrides preserve the guest margin to the chosen edge (or centre offset).
void compositor_set_anchor(uint64_t element_id, Anchor a);
void compositor_clear_anchor(uint64_t id);
// Returns the unclipped rectangle, also used for input mapping. Menu/FMV and
// legacy ignore anchors. Cursors use scene-mapped position and UI-scaled size.
bool compositor_element_rect_on_drawable(const CompositorInput *in, uint64_t id, int *x, int *y,
                                         int *w, int *h);
// Returns total count; writes at most max ids in ascending last_seq order.
uint32_t compositor_element_ids(const UiFrame *ui, uint64_t *out, uint32_t max);

// One instance per presenter. Amendment 16 needs had_draws, deliberately kept
// outside the binding CompositorInput interface. ARC holds the texture objects;
// the presenter must also pin their frame/target lease against pool reuse, and
// retire that lease after outstanding GPU work completes (T8).
struct CompositorSceneHistory {
    id<MTLTexture> world = nil;
    id<MTLTexture> overlay = nil;
    uint64_t scene_reused = 0;
    int guest_w = 0, guest_h = 0, domain_w = 0;
    bool classic = false;
};
// Called once per sealed frame, not once per display-link repeat. Returns true
// only when old textures were substituted. A class/legacy transition forgets
// the old scene, as does a guest resolution/domain change; the lifetime counter
// remains available to the presenter.
bool compositor_resolve_scene(CompositorSceneHistory *history, CompositorInput *in, bool had_draws);

#endif // __OBJC__
