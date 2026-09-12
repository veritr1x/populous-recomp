#include "compositor.h"
#ifdef POPM_COMPOSITOR_TEST_UI_DOUBLE
#include "tests/compositor_ui_double.h"
#elif __has_include("ui_layer.h")
#include "ui_layer.h"
#else
#include "ui_frame_contract.h"
#endif

#include <simd/simd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace {
using Registry = std::map<uint64_t, Anchor>;
std::mutex registry_mutex;
Registry anchors;
std::mutex pointer_mutex;
bool pointer_valid = false;
int32_t pointer_x = 0, pointer_y = 0;
struct CompositeRect {
    double x, y, w, h;
};

Registry registry_snapshot() {
    std::lock_guard lock(registry_mutex);
    return anchors;
}
bool valid(const CompositorInput *in) {
    return in && in->guest_w > 0 && in->guest_h > 0 && in->drawable_w > 0 && in->drawable_h > 0;
}
int8_t default_axis(int p, int extent, int domain) {
    const double centre = double(p) + double(extent) / 2;
    if (std::abs(centre - double(domain) / 2) <= 8)
        return 0;
    return centre <= double(domain) / 2 ? -1 : 1;
}
double anchored_axis(int p, int domain, int drawable, int scale, int8_t a) {
    if (a < 0)
        return double(p) * scale;
    if (a > 0)
        return drawable - (double(domain) - p) * scale;
    return (drawable - double(domain) * scale) / 2 + double(p) * scale;
}
CompositeRect whole_rect(const CompositorInput &in, bool integer) {
    const double scale =
        integer ? compositor_ui_scale(in.drawable_h, in.guest_h, in.scale_override)
                : std::min(double(in.drawable_w) / in.guest_w, double(in.drawable_h) / in.guest_h);
    const double w = in.guest_w * scale, h = in.guest_h * scale;
    return {(in.drawable_w - w) / 2, (in.drawable_h - h) / 2, w, h};
}
CompositeRect pixel_rect(CompositeRect r) {
    const double x = std::round(r.x), y = std::round(r.y);
    return {x, y, std::round(r.x + r.w) - x, std::round(r.y + r.h) - y};
}
CompositeRect mapped_element_rect(const CompositorInput &in, const UiElement &e,
                                  const Registry &registry) {
    if (e.is_cursor) {
        std::lock_guard lock(pointer_mutex);
        if (pointer_valid) {
            double sx, sy;
            if (!in.legacy && !in.classic && in.cls == HOST_SCREEN_GAMEPLAY)
                sx = sy = compositor_ui_scale(in.drawable_h, in.guest_h, in.scale_override);
            else {
                const auto whole =
                    whole_rect(in, !in.legacy && !in.classic && in.cls == HOST_SCREEN_MENU);
                sx = whole.w / in.guest_w;
                sy = whole.h / in.guest_h;
            }
            return {double(pointer_x), double(pointer_y), e.w * sx, e.h * sy};
        }
    }
    if (in.legacy || in.classic || in.cls != HOST_SCREEN_GAMEPLAY) {
        const auto whole = whole_rect(in, !in.legacy && !in.classic && in.cls == HOST_SCREEN_MENU);
        const double sx = whole.w / in.guest_w, sy = whole.h / in.guest_h;
        return {whole.x + e.x * sx, whole.y + e.y * sy, e.w * sx, e.h * sy};
    }
    const int scale = compositor_ui_scale(in.drawable_h, in.guest_h, in.scale_override);
    if (e.is_cursor) {
        if (std::isfinite(in.scene.scale_x) && in.scene.scale_x > 0 &&
            std::isfinite(in.scene.scale_y) && in.scene.scale_y > 0 &&
            std::isfinite(in.scene.offset_x) && std::isfinite(in.scene.offset_y)) {
            return {e.x * double(in.scene.scale_x) + in.scene.offset_x,
                    e.y * double(in.scene.scale_y) + in.scene.offset_y, double(e.w) * scale,
                    double(e.h) * scale};
        }
        const auto whole = whole_rect(in, true);
        return {whole.x + double(e.x) * scale, whole.y + double(e.y) * scale, double(e.w) * scale,
                double(e.h) * scale};
    }
    const auto found = registry.find(e.id);
    const Anchor a = found == registry.end() ? compositor_default_anchor(&e, in.guest_w, in.guest_h)
                                             : found->second;
    return {anchored_axis(e.x, in.guest_w, in.drawable_w, scale, a.h),
            anchored_axis(e.y, in.guest_h, in.drawable_h, scale, a.v), double(e.w) * scale,
            double(e.h) * scale};
}
CompositeRect element_rect(const CompositorInput &in, const UiElement &e,
                           const Registry &registry) {
    // Use the same integral edges for rasterization and the published layout,
    // including odd drawable dimensions and fractional scene-mapped cursors.
    return pixel_rect(mapped_element_rect(in, e, registry));
}
std::vector<const UiElement *> ordered(const UiFrame *ui) {
    std::vector<const UiElement *> result;
    if (ui)
        for (const auto &e : ui->elements)
            result.push_back(&e);
    std::stable_sort(result.begin(), result.end(),
                     [](const auto *a, const auto *b) { return a->last_seq < b->last_seq; });
    return result;
}

NSString *const shader = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 rect; float4 uv; float2 drawable; uint opaque; uint pad; };
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying compositor_vertex(uint id [[vertex_id]], constant Quad& q [[buffer(0)]]) {
    const float2 corners[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 p=q.rect.xy+corners[id]*q.rect.zw;
    Varying v;
    v.position=float4(p.x/q.drawable.x*2-1,1-p.y/q.drawable.y*2,0,1);
    v.uv=q.uv.xy+corners[id]*q.uv.zw;
    return v;
}
fragment float4 compositor_fragment(Varying v [[stage_in]],
                                    constant Quad& q [[buffer(0)]],
                                    texture2d<float> tex [[texture(0)]]) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    float4 c=tex.sample(nearest,v.uv);
    if(q.opaque) c.a=1;
    return c;
}
)metal";

enum Blend { Opaque, Straight, Premultiplied };
struct Pipelines {
    id<MTLDevice> device;
    MTLPixelFormat format;
    id<MTLRenderPipelineState> state[3];
};
std::mutex pipeline_mutex;
std::vector<Pipelines> pipeline_cache;
Pipelines pipelines(id<MTLDevice> device, MTLPixelFormat format) {
    std::lock_guard lock(pipeline_mutex);
    for (const auto &p : pipeline_cache)
        if (p.device == device && p.format == format)
            return p;
    Pipelines p{};
    p.device = device;
    p.format = format;
    NSError *error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:shader options:nil error:&error];
    if (!lib) {
        fprintf(stderr, "compositor: shader compile failed: %s\n", error.description.UTF8String);
        return p;
    }
    auto desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [lib newFunctionWithName:@"compositor_vertex"];
    desc.fragmentFunction = [lib newFunctionWithName:@"compositor_fragment"];
    auto color = desc.colorAttachments[0];
    color.pixelFormat = format;
    for (int blend = Opaque; blend <= Premultiplied; ++blend) {
        color.blendingEnabled = blend != Opaque;
        color.sourceRGBBlendFactor =
            blend == Straight ? MTLBlendFactorSourceAlpha : MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        p.state[blend] = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!p.state[blend]) {
            fprintf(stderr, "compositor: pipeline failed: %s\n", error.description.UTF8String);
            return p;
        }
    }
    pipeline_cache.push_back(p);
    return p;
}
struct Quad {
    simd_float4 rect, uv;
    simd_float2 drawable;
    uint32_t opaque, pad;
};
// Encode a textured compositor quad with explicit destination, UVs and alpha convention.
// Reject empty or wholly offscreen rectangles before issuing Metal draw commands.
void draw(id<MTLRenderCommandEncoder> encoder, const Pipelines &p, const CompositorInput &in,
          id<MTLTexture> tex, CompositeRect dst, CompositeRect uv, Blend blend) {
    if (!tex || !p.state[blend] || dst.w <= 0 || dst.h <= 0 || dst.x >= in.drawable_w ||
        dst.y >= in.drawable_h || dst.x + dst.w <= 0 || dst.y + dst.h <= 0)
        return;
    Quad q{{float(dst.x), float(dst.y), float(dst.w), float(dst.h)},
           {float(uv.x), float(uv.y), float(uv.w), float(uv.h)},
           {float(in.drawable_w), float(in.drawable_h)},
           uint32_t(blend == Opaque),
           0};
    [encoder setRenderPipelineState:p.state[blend]];
    [encoder setVertexBytes:&q length:sizeof q atIndex:0];
    [encoder setFragmentBytes:&q length:sizeof q atIndex:0];
    [encoder setFragmentTexture:tex atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
id<MTLTexture> upload(id<MTLDevice> device, const UiElement &e) {
    if (e.w <= 0 || e.h <= 0)
        return nil;
    const size_t n = size_t(e.w) * size_t(e.h);
    if (n > std::numeric_limits<size_t>::max() / 4 || e.rgba.size() < n * 4 || e.mask.size() < n)
        return nil;
    std::vector<uint8_t> pixels(e.rgba.begin(), e.rgba.begin() + n * 4);
    for (size_t i = 0; i < n; ++i)
        if (!e.mask[i])
            pixels[4 * i + 3] = 0;
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:e.w
                                                                  height:e.h
                                                               mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    if (!tex) {
        fprintf(stderr, "compositor: UI texture allocation failed (%dx%d)\n", e.w, e.h);
        return nil;
    }
    [tex replaceRegion:MTLRegionMake2D(0, 0, e.w, e.h)
           mipmapLevel:0
             withBytes:pixels.data()
           bytesPerRow:size_t(e.w) * 4];
    return tex;
}

// An overlay was encoded in guest UI space. Crop it to its HUD owner and move
// that crop with the owner's anchor. Subtract newer overlapping source rects
// so one overlay pixel cannot be duplicated onto two independently moved HUDs.
void subtract(std::vector<CompositeRect> &pieces, CompositeRect b) {
    std::vector<CompositeRect> next;
    for (auto a : pieces) {
        const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
        const double r = std::min(a.x + a.w, b.x + b.w), bottom = std::min(a.y + a.h, b.y + b.h);
        if (x >= r || y >= bottom) {
            next.push_back(a);
            continue;
        }
        if (y > a.y)
            next.push_back({a.x, a.y, a.w, y - a.y});
        if (bottom < a.y + a.h)
            next.push_back({a.x, bottom, a.w, a.y + a.h - bottom});
        if (x > a.x)
            next.push_back({a.x, y, x - a.x, bottom - y});
        if (r < a.x + a.w)
            next.push_back({r, y, a.x + a.w - r, bottom - y});
    }
    pieces.swap(next);
}
} // namespace

int compositor_ui_scale(int drawable_h, int guest_h, int override_) {
    return std::clamp(override_ ? override_ : drawable_h / std::max(1, guest_h), 1, 4);
}
Anchor compositor_default_anchor(const UiElement *e, int guest_w, int guest_h) {
    if (!e)
        return {-1, -1};
    return {default_axis(e->x, e->w, guest_w), default_axis(e->y, e->h, guest_h)};
}
void compositor_set_anchor(uint64_t id, Anchor a) {
    a.h = std::clamp<int>(a.h, -1, 1);
    a.v = std::clamp<int>(a.v, -1, 1);
    std::lock_guard lock(registry_mutex);
    anchors[id] = a;
}
void compositor_clear_anchor(uint64_t id) {
    std::lock_guard lock(registry_mutex);
    anchors.erase(id);
}
bool compositor_element_rect_on_drawable(const CompositorInput *in, uint64_t id, int *x, int *y,
                                         int *w, int *h) {
    if (!valid(in) || !in->ui)
        return false;
    const auto registry = registry_snapshot();
    for (const auto &e : in->ui->elements)
        if (e.id == id) {
            if (e.w <= 0 || e.h <= 0)
                return false;
            const auto r = element_rect(*in, e, registry);
            const double values[] = {r.x, r.y, r.w, r.h};
            for (double value : values)
                if (!std::isfinite(value) || value < std::numeric_limits<int>::min() ||
                    value > std::numeric_limits<int>::max())
                    return false;
            if (x)
                *x = int(std::lround(r.x));
            if (y)
                *y = int(std::lround(r.y));
            if (w)
                *w = int(std::lround(r.w));
            if (h)
                *h = int(std::lround(r.h));
            return true;
        }
    return false;
}
uint32_t compositor_element_ids(const UiFrame *ui, uint64_t *out, uint32_t max) {
    const auto elements = ordered(ui);
    const size_t count = std::min(elements.size(), size_t(UINT32_MAX));
    if (out)
        for (size_t i = 0; i < std::min(count, size_t(max)); ++i)
            out[i] = elements[i]->id;
    return uint32_t(count);
}
bool compositor_resolve_scene(CompositorSceneHistory *history, CompositorInput *in,
                              bool had_draws) {
    if (!history || !in)
        return false;
    const int domain = in->scene.domain_w > 0 ? in->scene.domain_w : in->guest_w;
    const bool changed = history->guest_w != in->guest_w || history->guest_h != in->guest_h ||
                         history->domain_w != domain || history->classic != in->classic;
    if (in->cls != HOST_SCREEN_GAMEPLAY || in->legacy || changed) {
        history->world = nil;
        history->overlay = nil;
        if (in->cls != HOST_SCREEN_GAMEPLAY || in->legacy)
            return false;
    }
    if (had_draws) {
        history->world = in->world;
        history->overlay = in->overlay;
        history->guest_w = in->guest_w;
        history->guest_h = in->guest_h;
        history->domain_w = domain;
        history->classic = in->classic;
        return false;
    }
    in->world = history->world;
    in->overlay = history->overlay;
    if (!history->world)
        return false;
    ++history->scene_reused;
    return true;
}

void compositor_compose(const CompositorInput *in, id<MTLTexture> out, id<MTLCommandBuffer> cb) {
    if (!valid(in) || !out || !cb || out.width != NSUInteger(in->drawable_w) ||
        out.height != NSUInteger(in->drawable_h) || out.sampleCount != 1)
        return;
    const auto p = pipelines(out.device, out.pixelFormat);
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = out;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
        return;
    encoder.label = @"Populous compositor";
    const CompositeRect full_uv{0, 0, 1, 1};
    if (in->legacy) {
        draw(encoder, p, *in, in->legacy_frame, pixel_rect(whole_rect(*in, false)), full_uv,
             Opaque);
        if (in->settings_page) {
            auto page = *in;
            page.guest_w = 640;
            page.guest_h = 480;
            draw(encoder, p, page, in->settings_page, pixel_rect(whole_rect(page, true)), full_uv,
                 Straight);
        }
        [encoder endEncoding];
        return;
    }
    const auto elements = ordered(in->ui);
    const auto registry = registry_snapshot();
    if (in->cls == HOST_SCREEN_GAMEPLAY)
        draw(encoder, p, *in, in->world,
             in->classic ? pixel_rect(whole_rect(*in, false))
                         : CompositeRect{0, 0, double(in->drawable_w), double(in->drawable_h)},
             full_uv, Opaque);
    for (const auto *e : elements) {
        const auto dst = element_rect(*in, *e, registry);
        if (dst.x >= in->drawable_w || dst.y >= in->drawable_h || dst.x + dst.w <= 0 ||
            dst.y + dst.h <= 0)
            continue;
        draw(encoder, p, *in, upload(out.device, *e), dst, full_uv, Straight);
    }
    if (in->cls == HOST_SCREEN_GAMEPLAY && in->overlay) {
        for (size_t i = 0; i < elements.size(); ++i) {
            const auto &e = *elements[i];
            if (!e.is_hud || e.is_cursor || e.w <= 0 || e.h <= 0)
                continue;
            const double x = std::max(0, e.x), y = std::max(0, e.y);
            const double r = std::min(double(in->guest_w), double(e.x) + e.w);
            const double b = std::min(double(in->guest_h), double(e.y) + e.h);
            if (r <= x || b <= y)
                continue;
            std::vector<CompositeRect> pieces{{x, y, r - x, b - y}};
            for (size_t j = i + 1; j < elements.size(); ++j) {
                const auto &later = *elements[j];
                if (later.is_hud && !later.is_cursor && later.w > 0 && later.h > 0)
                    subtract(pieces,
                             {double(later.x), double(later.y), double(later.w), double(later.h)});
            }
            const auto dst = element_rect(*in, e, registry);
            const double sx = dst.w / e.w, sy = dst.h / e.h;
            for (auto part : pieces) {
                draw(encoder, p, *in, in->overlay,
                     {dst.x + (part.x - e.x) * sx, dst.y + (part.y - e.y) * sy, part.w * sx,
                      part.h * sy},
                     {part.x / in->guest_w, part.y / in->guest_h, part.w / in->guest_w,
                      part.h / in->guest_h},
                     Premultiplied);
            }
        }
    }
    if (in->settings_page) {
        auto page = *in;
        page.guest_w = 640;
        page.guest_h = 480;
        draw(encoder, p, page, in->settings_page, pixel_rect(whole_rect(page, true)), full_uv,
             Straight);
    }
    [encoder endEncoding];
}

void compositor_set_pointer_position(bool valid, int32_t x, int32_t y) {
    std::lock_guard lock(pointer_mutex);
    pointer_valid = valid;
    pointer_x = x;
    pointer_y = y;
}

LayoutSnapshot compositor_layout_snapshot(const CompositorInput *in) {
    LayoutSnapshot result;
    if (!valid(in))
        return result;
    result.drawable_w = in->drawable_w;
    result.drawable_h = in->drawable_h;
    result.guest_w = in->guest_w;
    result.guest_h = in->guest_h;
    result.ui_scale = compositor_ui_scale(in->drawable_h, in->guest_h, in->scale_override);
    result.scene = in->scene;
    result.cls = in->cls;
    result.scale_override = in->scale_override;
    result.legacy = in->legacy;
    result.classic = in->classic;
    if (in->legacy || in->classic || in->cls != HOST_SCREEN_GAMEPLAY ||
        !std::isfinite(result.scene.scale_x) || result.scene.scale_x <= 0 ||
        !std::isfinite(result.scene.scale_y) || result.scene.scale_y <= 0 ||
        !std::isfinite(result.scene.offset_x) || !std::isfinite(result.scene.offset_y) ||
        result.scene.domain_w <= 0) {
        const auto r = whole_rect(*in, !in->legacy && !in->classic && in->cls == HOST_SCREEN_MENU);
        result.scene = {float(r.w / in->guest_w), float(r.h / in->guest_h), float(r.x), float(r.y),
                        in->guest_w};
    }
    const auto registry = registry_snapshot();
    if (in->ui)
        for (const auto &e : in->ui->elements) {
            if (e.w <= 0 || e.h <= 0)
                continue;
            const auto r = element_rect(*in, e, registry);
            const double values[] = {r.x, r.y, r.w, r.h};
            bool ok = r.w > 0 && r.h > 0;
            for (double value : values)
                ok &= std::isfinite(value) && value >= INT32_MIN && value <= INT32_MAX;
            if (ok)
                result.elements.push_back({e.id,
                                           e.last_seq,
                                           {e.x, e.y, e.w, e.h},
                                           {int(r.x), int(r.y), int(r.w), int(r.h)},
                                           e.is_cursor});
        }
    std::stable_sort(result.elements.begin(), result.elements.end(),
                     [](const auto &a, const auto &b) { return a.last_seq < b.last_seq; });
    return result;
}

LayoutSnapshot compositor_resize_layout(const LayoutSnapshot &layout, int w, int h) {
    if (w <= 0 || h <= 0 || layout.drawable_w <= 0 || layout.drawable_h <= 0)
        return layout;
    UiFrame ui{};
    for (const auto &e : layout.elements) {
        UiElement item{};
        item.id = e.id;
        item.last_seq = e.last_seq;
        item.x = e.guest.x;
        item.y = e.guest.y;
        item.w = e.guest.w;
        item.h = e.guest.h;
        item.is_cursor = e.is_cursor;
        ui.elements.push_back(std::move(item));
    }
    CompositorInput in{};
    in.ui = &ui;
    in.cls = layout.cls;
    in.guest_w = layout.guest_w;
    in.guest_h = layout.guest_h;
    in.drawable_w = w;
    in.drawable_h = h;
    in.scale_override = layout.scale_override;
    in.legacy = layout.legacy;
    in.classic = layout.classic;
    in.scene = layout.scene;
    in.scene.scale_x *= double(w) / layout.drawable_w;
    in.scene.offset_x *= double(w) / layout.drawable_w;
    in.scene.scale_y *= double(h) / layout.drawable_h;
    in.scene.offset_y *= double(h) / layout.drawable_h;
    auto result = compositor_layout_snapshot(&in);
    result.frame_id = layout.frame_id;
    return result;
}
