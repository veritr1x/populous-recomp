#include "../dx/passes.h"
// d3d_render.mm - the Metal renderer behind host_d3d_draw.
//
// src/recomp/dx/d3d.cpp records; this rasterizes. Every DrawPrimitive and
// DrawIndexedPrimitive arrives as one HostD3DDraw carrying the vertices, a
// snapshot of all 256 render states, the three transforms, the viewport and
// the texture handle.
//
// The guest encodes into a frame-owned scene target. Only a guest-visible
// reader submits and waits for a prefix and reads its dirty rectangles back.
// Prefix continuation loads both colour and depth. CPU writes upload exact
// covered pixels; the legacy whole-surface mirror is opt-in for T5 fallback.
//
// WHAT THE SHIM PROMISED, THIS KEEPS
//
// d3d.cpp advertises the full documented blend, compare, shade, texture-blend
// and raster cap sets because init_d3d rejects a device that does not, and the
// README says so in as many words: "Advertising a superset is a promise Task
// 7's renderer has to keep." So all thirteen blend factors, all eight compare
// functions, flat and Gouraud shading, the legacy texture-blend modes, both
// fog paths, the six texture filters with real mipmaps, and point, line and
// triangle primitives are implemented rather than approximated.
//
// One thing is recorded and not applied, logged once rather than dropped
// silently: vertex lighting. D3DVT_VERTEX carries a normal and needs the
// material and the light set to become a colour, and the command list carries
// neither. Such a vertex draws white. The game's own renderer transforms and
// lights before it draws, so its vertices arrive as D3DVT_TLVERTEX with the
// colour already in them.
//
// Nothing here opens a window.
#include "d3d_render.h"
#include "gpu/metal/metal_bridge.h"
#include "texture_pixels.h"
#include "texture_pack.h"
#include <mach-o/dyld.h>
#include "present.h"
#include "../runtime/display_seam.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <dispatch/dispatch.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>

// ---------------------------------------------------------------------------
// Render state numbering, from src/recomp/dx/dxtypes.h. Repeated here as the
// values the renderer reads rather than included, so this file states its own
// contract with the command list.
// ---------------------------------------------------------------------------
enum {
    RS_TEXTUREHANDLE = 1,
    RS_WRAPU = 5,
    RS_WRAPV = 6,
    RS_ZENABLE = 7,
    RS_FILLMODE = 8,
    RS_SHADEMODE = 9,
    RS_ZWRITEENABLE = 14,
    RS_ALPHATESTENABLE = 15,
    RS_TEXTUREMAG = 17,
    RS_TEXTUREMIN = 18,
    RS_SRCBLEND = 19,
    RS_DESTBLEND = 20,
    RS_TEXTUREMAPBLEND = 21,
    RS_CULLMODE = 22,
    RS_ZFUNC = 23,
    RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25,
    RS_ALPHABLENDENABLE = 27,
    RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29,
    RS_FOGCOLOR = 34,
    RS_FOGTABLEMODE = 35,
    RS_FOGSTART = 36,
    RS_FOGEND = 37,
    RS_FOGDENSITY = 38,
    RS_TEXTUREADDRESS = 41,
    RS_TEXTUREADDRESSU = 44,
    RS_TEXTUREADDRESSV = 45,
};
enum {
    PT_POINTLIST = 1,
    PT_LINELIST = 2,
    PT_LINESTRIP = 3,
    PT_TRIANGLELIST = 4,
    PT_TRIANGLESTRIP = 5,
    PT_TRIANGLEFAN = 6
};
enum { VT_VERTEX = 1, VT_LVERTEX = 2, VT_TLVERTEX = 3 };

namespace {

uint32_t g_total_draws = 0;
uint32_t g_draws_since_present = 0;
uint32_t g_total_textures = 0;
uint32_t g_total_flushes = 0;
double g_peak_nonblack = 0.0;

size_t count_nonblack(const uint8_t *bgra, size_t pixels) {
    size_t lit = 0;
    for (size_t i = 0; i < pixels; ++i)
        if (bgra[i * 4] > 8 || bgra[i * 4 + 1] > 8 || bgra[i * 4 + 2] > 8)
            ++lit;
    return lit;
}

void note_scene_nonblack(size_t lit, size_t target_pixels) {
    double ratio = target_pixels ? (double)lit / (double)target_pixels : 0.0;
    if (ratio > g_peak_nonblack)
        g_peak_nonblack = ratio;
}

void log_once(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_once(const char *key, const char *fmt, ...) {
    static std::map<std::string, bool> seen;
    if (seen[key])
        return;
    seen[key] = true;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[host] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

uint32_t rs_raw(const HostD3DDraw *c, uint32_t i) {
    if (!c->render_state || i >= c->render_state_count)
        return 0;
    return c->render_state[i];
}
// Several render states use 0 for "never set", and the documented default is
// what a device that was never told would do.
uint32_t rs(const HostD3DDraw *c, uint32_t i, uint32_t d) {
    uint32_t v = rs_raw(c, i);
    return v ? v : d;
}
// D3DVALUE render states are floats in a DWORD, not small integers.
float rs_value(const HostD3DDraw *c, uint32_t i, float d) {
    uint32_t v = rs_raw(c, i);
    if (!v)
        return d;
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// A D3DCOLOR is 0xAARRGGBB.
void unpack(uint32_t argb, float *r, float *g, float *b, float *a) {
    *a = ((argb >> 24) & 0xff) / 255.0f;
    *r = ((argb >> 16) & 0xff) / 255.0f;
    *g = ((argb >> 8) & 0xff) / 255.0f;
    *b = (argb & 0xff) / 255.0f;
}

float f32(const void *p, size_t off) {
    float v;
    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}
uint32_t u32(const void *p, size_t off) {
    uint32_t v;
    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}

// Decode one vertex of the given type. Screen-space types are turned into clip
// space here, because that is where the viewport is known.
HostD3DVertex decode(const HostD3DDraw *c, uint32_t i) {
    const uint8_t *p = (const uint8_t *)c->vertices + (size_t)i * c->vertex_stride;
    HostD3DVertex v;
    memset(&v, 0, sizeof v);
    v.a = 1.0f;
    switch (c->vertex_type) {
    case VT_VERTEX:
        // x y z, normal, tu tv. No colour: see the note at the top of the file.
        v.x = f32(p, 0);
        v.y = f32(p, 4);
        v.z = f32(p, 8);
        v.w = 1.0f;
        v.u = f32(p, 24);
        v.v = f32(p, 28);
        v.r = v.g = v.b = 1.0f;
        v.sa = 1.0f;
        break;
    case VT_LVERTEX:
        // x y z, reserved, colour, specular, tu tv.
        v.x = f32(p, 0);
        v.y = f32(p, 4);
        v.z = f32(p, 8);
        v.w = 1.0f;
        unpack(u32(p, 16), &v.r, &v.g, &v.b, &v.a);
        unpack(u32(p, 20), &v.sr, &v.sg, &v.sb, &v.sa);
        v.u = f32(p, 24);
        v.v = f32(p, 28);
        break;
    case VT_TLVERTEX:
    default: {
        // sx sy sz rhw, colour, specular, tu tv. Already transformed, already
        // divided: sx and sy are pixels inside the viewport and rhw is 1/w.
        float sx = f32(p, 0), sy = f32(p, 4), sz = f32(p, 8), rhw = f32(p, 12);
        unpack(u32(p, 16), &v.r, &v.g, &v.b, &v.a);
        unpack(u32(p, 20), &v.sr, &v.sg, &v.sb, &v.sa);
        v.u = f32(p, 24);
        v.v = f32(p, 28);

        float vx = (float)c->viewport[0], vy = (float)c->viewport[1];
        float vw = (float)c->viewport[2], vh = (float)c->viewport[3];
        if (vw <= 0.0f)
            vw = 1.0f;
        if (vh <= 0.0f)
            vh = 1.0f;
        float ndc_x = (sx - vx) / vw * 2.0f - 1.0f;
        float ndc_y = 1.0f - (sy - vy) / vh * 2.0f;
        // Restoring w from rhw is what makes the texture coordinates
        // perspective-correct again: the rasterizer interpolates u/w and 1/w,
        // which is only the same as interpolating u linearly when w is 1.
        float w = (rhw > 0.0f) ? 1.0f / rhw : 1.0f;
        // sz is already the depth value the device should write. The viewport
        // depth range is left at 0..1 for this path so it is not applied a
        // second time: the guest applied it when it produced sz.
        v.x = ndc_x * w;
        v.y = ndc_y * w;
        v.z = sz * w;
        v.w = w;
        break;
    }
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------

HostD3DPrimitive host_d3d_primitive_kind(const HostD3DDraw *c) {
    if (!c)
        return HOST_D3D_UNSUPPORTED;
    switch (c->primitive_type) {
    case PT_POINTLIST:
        return HOST_D3D_POINTS;
    case PT_LINELIST:
    case PT_LINESTRIP:
        return HOST_D3D_LINES;
    case PT_TRIANGLELIST:
    case PT_TRIANGLESTRIP:
    case PT_TRIANGLEFAN:
        return HOST_D3D_TRIANGLES;
    default:
        return HOST_D3D_UNSUPPORTED;
    }
}

// Expand supported guest primitive topologies into the host vertex list. Preserve strip
// winding and the provoking vertex used for flat color; return -1 for unsupported input.
int host_d3d_expand(const HostD3DDraw *c, HostD3DVertex *out, int max_out) {
    if (!c || !c->vertices || !c->vertex_count)
        return 0;
    if (c->vertex_type != VT_VERTEX && c->vertex_type != VT_LVERTEX &&
        c->vertex_type != VT_TLVERTEX)
        return -1;
    HostD3DPrimitive kind = host_d3d_primitive_kind(c);
    if (kind == HOST_D3D_UNSUPPORTED)
        return -1;

    uint32_t n = c->indices ? c->index_count : c->vertex_count;
    auto at = [&](uint32_t k) -> uint32_t { return c->indices ? c->indices[k] : k; };

    // Flat shading takes the whole primitive's colour from its first vertex,
    // which is a copy once the primitive is a list and not a mode at all.
    bool flat = rs(c, RS_SHADEMODE, 2) == 1;

    int written = 0;
    auto push = [&](const HostD3DVertex &v) {
        if (out && written < max_out)
            out[written] = v;
        ++written;
    };
    auto emit = [&](const uint32_t *index, int count) {
        HostD3DVertex t[3];
        for (int k = 0; k < count; ++k)
            t[k] = decode(c, index[k]);
        if (flat) {
            for (int k = 1; k < count; ++k) {
                t[k].r = t[0].r;
                t[k].g = t[0].g;
                t[k].b = t[0].b;
                t[k].a = t[0].a;
                t[k].sr = t[0].sr;
                t[k].sg = t[0].sg;
                t[k].sb = t[0].sb;
                t[k].sa = t[0].sa;
            }
        }
        for (int k = 0; k < count; ++k)
            push(t[k]);
    };

    switch (c->primitive_type) {
    case PT_POINTLIST:
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t x[1] = {at(i)};
            emit(x, 1);
        }
        break;
    case PT_LINELIST:
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            uint32_t x[2] = {at(i), at(i + 1)};
            emit(x, 2);
        }
        break;
    case PT_LINESTRIP:
        for (uint32_t i = 0; i + 1 < n; ++i) {
            uint32_t x[2] = {at(i), at(i + 1)};
            emit(x, 2);
        }
        break;
    case PT_TRIANGLELIST:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            uint32_t x[3] = {at(i), at(i + 1), at(i + 2)};
            emit(x, 3);
        }
        break;
    case PT_TRIANGLESTRIP:
        // Every other triangle has its first two vertices swapped, which is
        // what keeps a strip's winding consistent.
        for (uint32_t i = 0; i + 2 < n; ++i) {
            uint32_t even[3] = {at(i), at(i + 1), at(i + 2)};
            uint32_t odd[3] = {at(i + 1), at(i), at(i + 2)};
            emit((i & 1) ? odd : even, 3);
        }
        break;
    case PT_TRIANGLEFAN:
        for (uint32_t i = 1; i + 1 < n; ++i) {
            uint32_t x[3] = {at(0), at(i), at(i + 1)};
            emit(x, 3);
        }
        break;
    default:
        return -1;
    }
    return written;
}

uint32_t host_d3d_draws_since_present(void) {
    return g_draws_since_present;
}

// A texture's identity in the renderer is the pair, not the handle: the guest
// reuses handles and re-uploads over them, and a frame that has not been
// composited yet is still holding the revision it was drawn against.
static inline uint64_t tex_key(uint32_t handle, uint32_t revision) {
    return ((uint64_t)handle << 32) | revision;
}
void host_d3d_note_presented(void) {
    g_draws_since_present = 0;
}
uint32_t host_d3d_total_draws(void) {
    return g_total_draws;
}
uint32_t host_d3d_total_textures(void) {
    return g_total_textures;
}
uint32_t host_d3d_total_flushes(void) {
    return g_total_flushes;
}
double host_d3d_peak_nonblack(void) {
    return g_peak_nonblack;
}
extern "C" void host_d3d_reset_readback_metrics(void) {
    g_peak_nonblack = 0.0;
}

// ---------------------------------------------------------------------------
// The shader. One pipeline shape for every draw: what changes between draws is
// the uniform block, the blend factors and the depth state.
// ---------------------------------------------------------------------------
static NSString *const kShader = @R"metal(
#include <metal_stdlib>
using namespace metal;

struct HVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
    float sr, sg, sb, sa;
};
struct Uniforms {
    float4x4 mvp;
    uint  pretransformed;
    uint  textured;
    uint  texblend;
    uint  alphatest;
    uint  alphafunc;
    float alpharef;
    uint  specular;
    uint  texture_has_alpha;
    uint  fogmode;        // 0 off, 1 vertex, 2 exp, 3 exp2, 4 linear
    float fogstart, fogend, fogdensity;
    float fogr, fogg, fogb;
    float pointsize;
    uint terrain_detail;
};
struct VOut {
    float4 position [[position]];
    float  size [[point_size]];
    float2 uv;
    float4 color;
    float4 spec;
    float  fogdist;
};
// Colour, and the coverage mask that says this pixel was rasterized. A
// discarded fragment writes neither.
struct FOut {
    float4 color    [[color(0)]];
    float  coverage [[color(1)]];
};

vertex VOut d3d_vertex(uint vid [[vertex_id]],
                       const device HVertex* v [[buffer(0)]],
                       constant Uniforms& u [[buffer(1)]]) {
    HVertex s = v[vid];
    VOut o;
    o.position = u.pretransformed ? float4(s.x, s.y, s.z, s.w)
                                  : u.mvp * float4(s.x, s.y, s.z, 1.0);
    o.size = u.pointsize;
    o.uv = float2(s.u, s.v);
    o.color = float4(s.r, s.g, s.b, s.a);
    o.spec = float4(s.sr, s.sg, s.sb, s.sa);
    // Table fog is a function of eye-space distance, which after a standard
    // projection is the clip w.
    o.fogdist = abs(o.position.w);
    return o;
}

// Shade a world fragment using the guest material and optional terrain detail.
// Apply the legacy texture/alpha rules before writing color and coverage attachments.
fragment FOut d3d_fragment(VOut in [[stage_in]],
                           constant Uniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]],
                           texture2d<float> detail [[texture(1)]],
                           sampler detail_samp [[sampler(1)]]) {
    float4 c = in.color;
    if (u.textured != 0) {
        float4 t = tex.sample(samp, in.uv);
        if (u.terrain_detail != 0) {
            // An independent material sample supplies real fine structures;
            // enlarging the guest's 16/32 texels cannot create those. The
            // source remains the color/lighting/coastline mask. Blue water
            // fades this land-only layer out, including mixed shore texels.
            float3 chroma = t.rgb / max(max(t.r, t.g), max(t.b, 0.01f));
            float land = 1.0f - smoothstep(0.02f, 0.22f, chroma.b - chroma.r);
            float grass = smoothstep(-0.03f, 0.16f, chroma.g - chroma.r);
            float stone = 1.0f - smoothstep(0.06f, 0.28f, max(chroma.r, chroma.g) - min(chroma.r, min(chroma.g, chroma.b)));
            float sand = smoothstep(0.28f, 0.62f, max(t.r, t.g));
            float4 structure = (detail.sample(detail_samp, in.uv) * 255.0f - 128.0f) / 128.0f;
            float material = mix(structure.a, structure.g, sand);
            material = mix(material, structure.b, stone);
            material = mix(material, structure.r, grass);
            t.rgb = saturate(t.rgb * (1.0f + material * 0.85f * land));
        }
        switch (u.texblend) {
            case 1: case 7:                                   // DECAL, COPY
                c = t; break;
            case 2:                                           // MODULATE
                // The legacy rule: the texture's alpha when it has one,
                // otherwise the diffuse's. Taking the vertex alpha from a
                // vertex that never set it - and plenty of D3DTLVERTEX
                // colours leave it at zero - makes every blended pixel
                // invisible and the whole scene the colour of the clear.
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
            case 3:                                           // DECALALPHA
                c = float4(mix(in.color.rgb, t.rgb, t.a), in.color.a); break;
            case 5:                                           // DECALMASK
                c = float4(t.rgb, in.color.a); break;
            case 8:                                           // ADD
                c = float4(saturate(t.rgb + in.color.rgb), in.color.a); break;
            case 4:                                           // MODULATEALPHA
                c = float4(t.rgb * in.color.rgb, t.a * in.color.a); break;
            default:
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
        }
    }
    if (u.specular != 0) c = float4(saturate(c.rgb + in.spec.rgb), c.a);
    if (u.alphatest != 0) {
        bool pass;
        switch (u.alphafunc) {
            case 1: pass = false;                break;   // NEVER
            case 2: pass = c.a <  u.alpharef;    break;   // LESS
            case 3: pass = c.a == u.alpharef;    break;   // EQUAL
            case 4: pass = c.a <= u.alpharef;    break;   // LESSEQUAL
            case 5: pass = c.a >  u.alpharef;    break;   // GREATER
            case 6: pass = c.a != u.alpharef;    break;   // NOTEQUAL
            case 7: pass = c.a >= u.alpharef;    break;   // GREATEREQUAL
            default: pass = true;                break;   // ALWAYS
        }
        if (!pass) discard_fragment();
    }
    if (u.fogmode != 0) {
        // The fog factor is how much of the fragment survives: 1 is clear.
        float f = 1.0;
        float d = in.fogdist;
        switch (u.fogmode) {
            case 1: f = in.spec.a; break;                          // vertex fog
            case 2: f = exp(-u.fogdensity * d); break;             // EXP
            case 3: f = exp(-(u.fogdensity * d) * (u.fogdensity * d)); break;
            default:                                               // LINEAR
                f = (u.fogend - d) / max(u.fogend - u.fogstart, 1e-6);
                break;
        }
        f = saturate(f);
        c = float4(mix(float3(u.fogr, u.fogg, u.fogb), c.rgb, f), c.a);
    }
    FOut o;
    o.color = c;
    o.coverage = 1.0;
    return o;
}
// Reduce guest-visible reads on the GPU; never decompress a 4K texture on
// the CPU merely to select 640x480 pixel centres. Each lane also counts its
// disjoint native cell so existing full-resolution diagnostics stay exact.
kernel void guest_readback_fused(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    uint2 lo=(guest*native_size+guest_size-1)/guest_size;
    uint2 hi=((guest+1)*native_size+guest_size-1)/guest_size;
    uint lit=0;
    for(uint y=lo.y;y<hi.y;++y) for(uint x=lo.x;x<hi.x;++x)
        lit+=any(round(color.read(uint2(x,y)).rgb*255.0f)>8.0f) ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),lit);
}
// Guest sampling and native brightness reduction have different grids.
// Keeping the sampling lanes short avoids a variable native-cell loop in
// every guest lane. Packing and coverage rules match the fused reference.
kernel void guest_readback(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),0);
}
// A 16x16 threadgroup covers a 32x32 native tile. Each lane reads four
// adjacent pixels and each SIMD group writes one sum. Padded lanes contribute
// zero but participate in the reduction; no contended global atomic is used.
kernel void native_brightness(texture2d<float, access::read> color [[texture(0)]],
                              device uint* output [[buffer(0)]],
                              constant uint4* p [[buffer(1)]],
                              uint2 tid [[thread_position_in_grid]],
                              uint2 group [[threadgroup_position_in_grid]],
                              uint lane [[thread_index_in_simdgroup]],
                              uint simd_group [[simdgroup_index_in_threadgroup]]) {
    uint2 pos=p[0].xy+tid*2;
    uint lit=0;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        uint2 q=pos+uint2(x,y);
        if(all(q<p[0].zw)) lit+=any(round(color.read(q).rgb*255.0f)>8.0f) ? 1:0;
    }
    uint sum=simd_sum(lit);
    if(lane==0) output[p[1].x+(group.y*p[1].y+group.x)*p[1].z+simd_group]=sum;
}
// Seed a native target directly from an immutable guest-sized buffer. Integer
// channel expansion and floor(x * guest / native) preserve the CPU upload's
// exact pixels without expanding the whole 4K image on the guest thread.
vertex float4 surface_upload_vertex(uint id [[vertex_id]]) {
    float2 corner=float2((id<<1)&2,id&2);
    return float4(corner*2.0f-1.0f,0,1);
}
fragment float4 surface_upload_fragment(float4 position [[position]],
    device const uchar* pixels [[buffer(0)]],
    constant uint4* p [[buffer(1)]], constant uint* palette [[buffer(2)]]) {
    uint2 xy=uint2(position.xy)*p[0].xy/p[0].zw;
    uint offset=xy.y*p[1].x+xy.x*(p[1].y==8 ? 1:(p[1].y<=16 ? 2:4));
    uint3 rgb;
    if(p[1].y==8) {
        uint index=pixels[offset];
        uint c=p[1].z ? palette[index] : index*0x010101u;
        rgb=uint3((c>>16)&255,(c>>8)&255,c&255);
    } else {
        uint value=uint(pixels[offset])|(uint(pixels[offset+1])<<8);
        if(p[1].y>16) value|=(uint(pixels[offset+2])<<16)|(uint(pixels[offset+3])<<24);
        rgb=((uint3(value)&p[2].xyz)>>p[3].xyz)*255/p[4].xyz;
    }
    return float4(float3(rgb)/255.0f,1);
}
)metal";

namespace {

struct alignas(16) Uniforms {
    float mvp[16];
    uint32_t pretransformed;
    uint32_t textured;
    uint32_t texblend;
    uint32_t alphatest;
    uint32_t alphafunc;
    float alpharef;
    uint32_t specular;
    uint32_t texture_has_alpha;
    uint32_t fogmode;
    float fogstart, fogend, fogdensity;
    float fogr, fogg, fogb;
    float pointsize;
    uint32_t terrain_detail;
};

MTLBlendFactor blend_factor(uint32_t d3d, bool for_dest, bool *both) {
    *both = false;
    switch (d3d) {
    case 1:
        return MTLBlendFactorZero;
    case 2:
        return MTLBlendFactorOne;
    case 3:
        return MTLBlendFactorSourceColor;
    case 4:
        return MTLBlendFactorOneMinusSourceColor;
    case 5:
        return MTLBlendFactorSourceAlpha;
    case 6:
        return MTLBlendFactorOneMinusSourceAlpha;
    case 7:
        return MTLBlendFactorDestinationAlpha;
    case 8:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:
        return MTLBlendFactorDestinationColor;
    case 10:
        return MTLBlendFactorOneMinusDestinationColor;
    case 11:
        return MTLBlendFactorSourceAlphaSaturated;
    // The two BOTH factors set the source and the destination together: the
    // caller only names one of them and the other follows, which is why the
    // renderer has to be told this was one of them.
    case 12:
        *both = true;
        return for_dest ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorSourceAlpha;
    case 13:
        *both = true;
        return for_dest ? MTLBlendFactorSourceAlpha : MTLBlendFactorOneMinusSourceAlpha;
    default:
        return for_dest ? MTLBlendFactorZero : MTLBlendFactorOne;
    }
}

MTLCompareFunction compare_function(uint32_t d3d) {
    switch (d3d) {
    case 1:
        return MTLCompareFunctionNever;
    case 2:
        return MTLCompareFunctionLess;
    case 3:
        return MTLCompareFunctionEqual;
    case 4:
        return MTLCompareFunctionLessEqual;
    case 5:
        return MTLCompareFunctionGreater;
    case 6:
        return MTLCompareFunctionNotEqual;
    case 7:
        return MTLCompareFunctionGreaterEqual;
    default:
        return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode address_mode(uint32_t d3d) {
    switch (d3d) {
    case 2:
        return MTLSamplerAddressModeMirrorRepeat;
    case 3:
        return MTLSamplerAddressModeClampToEdge;
    case 4:
        return MTLSamplerAddressModeClampToBorderColor;
    default:
        return MTLSamplerAddressModeRepeat;
    }
}

// The six D3DFILTER_ values are a minification filter and a mip filter in one
// number, and Metal wants them apart.
void min_filter(uint32_t d3d, MTLSamplerMinMagFilter *minf, MTLSamplerMipFilter *mip) {
    switch (d3d) {
    case 2:
        *minf = MTLSamplerMinMagFilterLinear;
        *mip = MTLSamplerMipFilterNotMipmapped;
        break;
    case 3:
        *minf = MTLSamplerMinMagFilterNearest;
        *mip = MTLSamplerMipFilterNearest;
        break;
    case 4:
        *minf = MTLSamplerMinMagFilterNearest;
        *mip = MTLSamplerMipFilterLinear;
        break;
    case 5:
        *minf = MTLSamplerMinMagFilterLinear;
        *mip = MTLSamplerMipFilterNearest;
        break;
    case 6:
        *minf = MTLSamplerMinMagFilterLinear;
        *mip = MTLSamplerMipFilterLinear;
        break;
    default:
        *minf = MTLSamplerMinMagFilterNearest;
        *mip = MTLSamplerMipFilterNotMipmapped;
        break;
    }
}

// D3DMATRIX holds M with row-vector maths: the guest computes v' = v * M. A
// Metal float4x4 multiplies a column vector, so the matrix it needs is the
// transpose, and v * W * V * P becomes Pt * Vt * Wt * v.
//
// The transpose costs nothing. A D3DMATRIX in memory is m[row*4 + col]; a
// Metal float4x4's storage is columns[j][i] = A_ij, and A_ij = M_ji means
// memory[j*4 + i] = m[j*4 + i]. Column j is D3D row j and the bytes go
// straight across. The loop is written out rather than memcpy'd so the
// indexing above can be read off it.
void d3d_matrix_to_metal(const float *d3d, float *metal_column_major) {
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            metal_column_major[j * 4 + i] = d3d[j * 4 + i];
}

void matrix_multiply(const float *a, const float *b, float *out) {
    // Column-major, Metal convention: out = a * b.
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + i] * b[j * 4 + k];
            out[j * 4 + i] = s;
        }
}

void identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Where a mask's bits start and how many values it holds, for packing a
// channel back into a 16-bit surface pixel.
void mask_shape(uint32_t mask, int *shift, uint32_t *max) {
    *shift = 0;
    *max = 0;
    if (!mask)
        return;
    while (!((mask >> *shift) & 1))
        ++*shift;
    *max = mask >> *shift;
}

PopD3DRenderer *g_shared = nil;

} // namespace

// ---------------------------------------------------------------------------

// Four slots: writing, two mailbox entries, presenting (plan amendment 1).
// Colour, depth and raster coverage always travel together across prefixes.
// A journal starts at target acquisition. Earlier CPU-only writes are already
// in the seed. Draws refer to the frame arena; resolved CPU records are copied
// immediately, before their temporary/guest pointers can change. This also
// preserves stretched, self and keyed blits exactly as the shim executed them.
struct ReplayStep {
    uint32_t seq = 0;
    bool draw = false, barrier = false;
    HostDrawMapping mapping = HOST_MAPPING_SCENE;
    HostD3DDrawSnapshot snapshot{};
    HostBlitRecord record{};
    uint32_t palette[256]{};
    bool hasPalette = false;
    std::vector<uint8_t> pixels, coverage;
};
struct ReplayJournal {
    std::vector<uint8_t> seed;
    uint32_t palette[256]{};
    bool hasPalette = false, done = false, pendingCheckpoint = false;
    std::vector<ReplayStep> steps;
    // GPU-only copies until fallback actually needs them. They are buffers,
    // not additional scene targets, and never cause a normal-frame readback.
    id<MTLBuffer> checkpoint[3] = {nil, nil, nil};
    NSUInteger pitch[3] = {0, 0, 0};
    int width = 0, height = 0;
    void reset() {
        seed.clear();
        steps.clear();
        hasPalette = done = pendingCheckpoint = false;
        for (auto &buffer : checkpoint)
            buffer = nil;
        width = height = 0;
    }
};
// All inputs are frozen after the GPU completion wait. Each shard owns
// disjoint destination rows; dispatch_apply_f joins them before guest code
// can see the surface again. No renderer, mod or guest callback runs here.
struct ReadbackCopy {
    const HostDirtyRect *rects;
    const size_t *offsets;
    const uint32_t *sampled;
    uint8_t *dst;
    const uint8_t *index_cache;
    uint32_t count, workers, rm, gm, bm;
    int pitch, bpp, rs, gs, bs;
    struct alignas(64) Result {
        size_t lit = 0;
        bool failed = false;
    } result[4];
};
void copy_readback_rows(void *context, size_t shard) {
    auto &c = *(ReadbackCopy *)context;
    size_t lit = 0;
    bool failed = false;
    for (uint32_t i = 0; i < c.count; ++i) {
        auto r = c.rects[i];
        const uint32_t *sampled = c.sampled + c.offsets[i] * 2;
        int first = r.y0 + (r.y1 - r.y0) * shard / c.workers;
        int last = r.y0 + (r.y1 - r.y0) * (shard + 1) / c.workers;
        for (int y = first; y < last; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                size_t at = size_t(y - r.y0) * (r.x1 - r.x0) + (x - r.x0);
                uint32_t pixel = sampled[at * 2];
                lit += sampled[at * 2 + 1];
                if (!(pixel >> 24))
                    continue;
                uint32_t b = pixel & 255, g = (pixel >> 8) & 255, red = (pixel >> 16) & 255;
                uint8_t *out = c.dst + (size_t)y * c.pitch;
                if (c.bpp == 8) {
                    if (!c.index_cache) {
                        failed = true;
                        continue;
                    }
                    out[x] = c.index_cache[((red >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
                } else {
                    uint32_t p =
                        c.rm ? ((red * c.rm / 255) << c.rs) | ((g * c.gm / 255) << c.gs) |
                                   ((b * c.bm / 255) << c.bs)
                             : ((red * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255);
                    if (!c.rm && c.bpp > 16)
                        p = (red << 16) | (g << 8) | b;
                    const int bytes = texture_storage_bytes(c.bpp);
                    for (int k = 0; k < bytes; ++k)
                        out[x * bytes + k] = uint8_t(p >> (8 * k));
                }
            }
    }
    c.result[shard].lit = lit;
    c.result[shard].failed = failed;
}

struct HDTexture {
    id<MTLTexture> texture = nil;
    id<MTLCommandBuffer> lastUse = nil;
    uint64_t bytes = 0, stamp = 0;
    bool alpha = false;
};
struct HDCache {
    pop_hd::Pack pack;
    std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<HDTexture>> entries;
    uint64_t budget = 512ull * 1024 * 1024, used = 0, clock = 0, hits = 0, loads = 0, refused = 0,
             draws = 0;
    uint64_t detail_draws = 0;
    bool reserve(uint64_t bytes) {
        if (bytes > budget) {
            ++refused;
            return false;
        }
        while (used + bytes > budget) {
            auto victim = entries.end();
            for (auto i = entries.begin(); i != entries.end(); ++i) {
                auto &t = i->second;
                if (t->lastUse && t->lastUse.status >= MTLCommandBufferStatusCompleted)
                    t->lastUse = nil;
                if (t.use_count() != 1 || t->lastUse)
                    continue;
                if (victim == entries.end() || t->stamp < victim->second->stamp)
                    victim = i;
            }
            if (victim == entries.end()) {
                ++refused;
                return false;
            }
            used -= victim->second->bytes;
            entries.erase(victim);
        }
        return true;
    }
};

struct SceneSlot {
    id<MTLTexture> color = nil, depth = nil, coverage = nil, staging = nil, coverageStaging = nil;
    id<MTLCommandBuffer> last = nil;
    HostD3DSurface surface{};
    uint32_t palette[256]{};
    uint32_t generation = 0;
    uint64_t frame = 0;
    bool unclaimed = false, initialized = false;
    ReplayJournal replay;
    bool separateLayers = false, materializedLayers = false;
    int scene_domain_w = 0;
    HostSceneTarget presentTarget;
    gpu::Texture legacyImport;
    std::shared_ptr<void> coherenceLease;
    id<MTLTexture> overlayDepth = nil, overlayCoverage = nil;
};

// Until the renderer itself runs over gpu.h (plan Task 5), the presenter's
// texture handles become Metal objects here and the renderer's Metal command
// buffers report their completion through the presenter's prefix fence.
static id<MTLTexture> present_texture(gpu::Texture t) {
    return t ? gpu::metal::export_texture(host_present_device(), t) : nil;
}
static void track_native_command(id<MTLCommandBuffer> cb) {
    void *handle = host_present_prefix_begin();
    if (!handle)
        return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
      host_present_prefix_done(handle, c.status == MTLCommandBufferStatusCompleted);
    }];
}
// A compose with renderer-owned textures. Importing an object the presenter
// already owns returns the presenter's handle, so imports are never destroyed
// here: the few long-lived slot textures simply stay registered.
static void compose_native(const CompositorInput &in, id<MTLTexture> world, id<MTLTexture> out,
                           id<MTLCommandBuffer> cb) {
    gpu::Device *dev = host_present_device();
    if (!dev)
        return;
    CompositorInput copy = in;
    copy.world = gpu::metal::import_texture(dev, world);
    gpu::CommandBuffer command = gpu::metal::import_command(dev, cb);
    compositor_compose(dev, &copy, gpu::metal::import_texture(dev, out), command);
    gpu::metal::forget_command(dev, command);
}

@implementation PopD3DRenderer {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> library_;
    id<MTLTexture> color_;
    id<MTLTexture> depth_;
    // 1 where the device rasterized since the last write-back, 0 elsewhere.
    id<MTLTexture> coverage_;
    id<MTLTexture> staging_; // shared storage, for readback
    id<MTLTexture> coverage_staging_;
    id<MTLTexture> white_;
    id<MTLComputePipelineState> readback_pipeline_, readback_fused_pipeline_, brightness_pipeline_;
    id<MTLRenderPipelineState> surface_upload_pipeline_;
    bool cpu_surface_upload_;
    id<MTLBuffer> readback_buffer_, brightness_buffer_;
    bool tiled_readback_;
    id<MTLCommandBuffer> command_;
    id<MTLRenderCommandEncoder> encoder_;
    std::map<uint64_t, id<MTLRenderPipelineState>> *pipelines_;
    std::map<uint64_t, id<MTLDepthStencilState>> *depths_;
    std::map<uint64_t, id<MTLSamplerState>> *samplers_;
    // Textures by (handle, revision), because a frame is composited after the
    // guest has moved on: a draw submitted against revision 1 must still find
    // revision 1 when the compositor gets to it, however many uploads the
    // guest has made since. `alpha` is per revision for the same reason - a
    // re-upload can change whether the texture carries alpha at all.
    //
    // A revision is dropped when it is neither the current one nor leased by
    // a frame, which is what keeps this from growing by one texture a frame.
    struct TexEntry {
        id<MTLTexture> texture = nil;
        std::shared_ptr<HDTexture> enhanced;
        bool alpha = false;
        bool smallOpaqueTile = false;
        uint32_t leases = 0;
    };
    std::map<uint64_t, TexEntry> *textures_;
    HDCache hd_;
    std::shared_ptr<HDTexture> terrain_detail_;
    // handle -> the revision most recently uploaded, which is what a draw that
    // names no revision gets.
    std::map<uint32_t, uint32_t> *texture_current_;
    int width_, height_;
    int guest_width_, guest_height_, scene_width_, scene_height_;
    bool incremental_;
    bool replaying_;
    SceneSlot slots_[4];
    HostSceneTarget acquiringTarget_;
    bool frame_dropped_;
    bool overlay_rendering_;
    int active_slot_;
    uint32_t target_generation_;
    id<MTLCommandBuffer> last_command_;

    // The render target: which surface, in what format. `pixels` is never
    // cached across a call the shim could have changed it in - Flip moves it -
    // so the shim re-states the target whenever it moves and every flush
    // carries the pointer afresh.
    uint32_t target_id_;
    // Which surface the mirror's contents belong to. It is the render target
    // in the ordinary case, and it is what a flush matches on: the target can
    // change while the mirror still holds a scene, and that scene belongs to
    // the surface it was drawn for, not to whichever surface is current now.
    uint32_t pending_id_;
    void *target_pixels_;
    int target_pitch_, target_bpp_;
    uint32_t target_rmask_, target_gmask_, target_bmask_;
    uint32_t target_palette_[256];
    bool target_has_palette_;
    // The 5-5-5 nearest-index cache for writing back into an 8-bit target,
    // rebuilt when the palette changes.
    std::vector<uint8_t> *index_cache_;
    bool index_cache_valid_;

    bool gpu_dirty_;    // the mirror holds something the surface does not
    bool needs_upload_; // the surface holds something the mirror does not
    bool in_scene_;
    uint32_t submit_draws_, encoded_draws_, early_submissions_, readback_workers_;
    FILE *readback_timings_;
    id<MTLCounterSampleBuffer> sample_counter_, brightness_counter_;

    bool pending_clear_color_, pending_clear_depth_;
    // The coverage mask has to go back to zero after every write-back, and a
    // whole-target colour clear puts it to one: the clear touched every pixel.
    bool coverage_reset_pending_;
    MTLClearColor pending_color_;
    float pending_depth_;
    bool cull_enabled_;
    HostCommandStorageStats storage_stats_;
    std::vector<HostD3DVertex> vertex_scratch_;
    std::vector<uint8_t> upload_scratch_, mask_scratch_;
    struct ArgumentBuffer {
        id<MTLBuffer> buffer = nil;
        id<MTLCommandBuffer> owner = nil;
        uint64_t frame = 0;
    };
    std::vector<ArgumentBuffer> argument_pool_;
    size_t argument_cursor_;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device {
    return [self initWithDevice:device queue:[device newCommandQueue]];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue {
    self = [super init];
    if (!self)
        return nil;
    device_ = device;
    queue_ = queue;
    NSError *error = nil;
    library_ = [device newLibraryWithSource:kShader options:nil error:&error];
    if (!library_) {
        fprintf(stderr, "[host] the Direct3D renderer's shaders did not compile: %s\n",
                error ? error.localizedDescription.UTF8String : "unknown error");
        return nil;
    }
    auto upload = [MTLRenderPipelineDescriptor new];
    upload.vertexFunction = [library_ newFunctionWithName:@"surface_upload_vertex"];
    upload.fragmentFunction = [library_ newFunctionWithName:@"surface_upload_fragment"];
    upload.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    surface_upload_pipeline_ = [device newRenderPipelineStateWithDescriptor:upload error:&error];
    if (!surface_upload_pipeline_) {
        fprintf(stderr, "[host] surface upload pipeline failed: %s\n",
                error.localizedDescription.UTF8String);
        return nil;
    }
    const char *upload_mode = getenv("POP_HOST_SURFACE_UPLOAD");
    cpu_surface_upload_ = upload_mode && !strcmp(upload_mode, "cpu");
    readback_pipeline_ =
        [device newComputePipelineStateWithFunction:[library_ newFunctionWithName:@"guest_readback"]
                                              error:&error];
    readback_fused_pipeline_ = [device
        newComputePipelineStateWithFunction:[library_ newFunctionWithName:@"guest_readback_fused"]
                                      error:&error];
    brightness_pipeline_ = [device
        newComputePipelineStateWithFunction:[library_ newFunctionWithName:@"native_brightness"]
                                      error:&error];
    const char *kernel = getenv("POP_HOST_READBACK_KERNEL");
    // Keep the established path unless explicitly testing the candidate:
    // repeated gameplay runs have not shown a throughput benefit yet.
    tiled_readback_ = kernel && strcmp(kernel, "tiled") == 0;
    if (!readback_pipeline_ || !readback_fused_pipeline_ || !brightness_pipeline_) {
        fprintf(stderr, "[host] readback pipeline failed: %s\n",
                error.localizedDescription.UTF8String);
        return nil;
    }
    pipelines_ = new std::map<uint64_t, id<MTLRenderPipelineState>>();
    depths_ = new std::map<uint64_t, id<MTLDepthStencilState>>();
    samplers_ = new std::map<uint64_t, id<MTLSamplerState>>();
    textures_ = new std::map<uint64_t, TexEntry>();
    texture_current_ = new std::map<uint32_t, uint32_t>();
    index_cache_ = new std::vector<uint8_t>();
    width_ = height_ = 0;
    active_slot_ = -1;
    target_id_ = 0;
    pending_id_ = 0;
    target_pixels_ = nullptr;
    target_pitch_ = target_bpp_ = 0;
    target_rmask_ = target_gmask_ = target_bmask_ = 0;
    target_has_palette_ = false;
    index_cache_valid_ = false;
    memset(target_palette_, 0, sizeof target_palette_);
    gpu_dirty_ = false;
    needs_upload_ = false;
    in_scene_ = false;
    submit_draws_ = 256;
    const char *submit = getenv("POP_HOST_D3D_SUBMIT_DRAWS");
    if (submit && *submit) {
        char *end = nullptr;
        unsigned long value = strtoul(submit, &end, 10);
        if (end && !*end && value <= 65536)
            submit_draws_ = (uint32_t)value;
    }
    readback_workers_ = 4;
    if (const char *workers = getenv("POP_HOST_READBACK_WORKERS")) {
        char *end = nullptr;
        unsigned long value = strtoul(workers, &end, 10);
        if (end && !*end && value >= 1 && value <= 4)
            readback_workers_ = (uint32_t)value;
    }
    if (const char *path = getenv("POP_HOST_READBACK_TIMINGS")) {
        readback_timings_ = fopen(path, "w");
        if (readback_timings_)
            fprintf(readback_timings_,
                    "native_w,native_h,submit_draws,early_submissions,draws_since_present,copy_"
                    "workers,kernel,last_render_gpu_ms,readback_gpu_ms,wait_ms,copy_ms,sample_"
                    "stage_ms,brightness_stage_ms\n");
    }
    // Optional diagnostic pass boundaries. Production keeps one encoder;
    // the counter path separates the kernels so their cost can be attributed.
    // Resolved Apple GPU timestamps are nanoseconds in the CPU clock domain.
    if (readback_timings_ && getenv("POP_HOST_READBACK_COUNTERS") &&
        [device supportsFamily:MTLGPUFamilyApple1] &&
        [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        for (id<MTLCounterSet> set in device.counterSets) {
            if (![set.name isEqualToString:MTLCommonCounterSetTimestamp])
                continue;
            MTLCounterSampleBufferDescriptor *desc = [MTLCounterSampleBufferDescriptor new];
            desc.counterSet = set;
            desc.storageMode = MTLStorageModeShared;
            desc.sampleCount = 2;
            sample_counter_ = [device newCounterSampleBufferWithDescriptor:desc error:&error];
            brightness_counter_ = [device newCounterSampleBufferWithDescriptor:desc error:&error];
            if (!sample_counter_ || !brightness_counter_) {
                sample_counter_ = nil;
                brightness_counter_ = nil;
            }
            break;
        }
    }
    pending_clear_color_ = pending_clear_depth_ = false;
    coverage_reset_pending_ = true;
    pending_color_ = MTLClearColorMake(0, 0, 0, 1);
    pending_depth_ = 1.0f;
    cull_enabled_ = getenv("POP_HOST_D3D_NOCULL") == nullptr;

    // Bound wherever a draw is untextured, because the fragment function's
    // texture argument has to be something.
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    white_ = [device newTextureWithDescriptor:d];
    uint32_t opaque_white = 0xffffffffu;
    [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
              mipmapLevel:0
                withBytes:&opaque_white
              bytesPerRow:4];
    std::string packPath = "build/texture-pack";
    const char *packEnv = getenv("POPM_TEXTURE_PACK_DIR");
    if (packEnv)
        packPath = packEnv;
    else {
        uint32_t n = 0;
        _NSGetExecutablePath(nullptr, &n);
        std::vector<char> path(n);
        if (n && !_NSGetExecutablePath(path.data(), &n)) {
            std::filesystem::path exe(path.data());
            if (exe.parent_path().filename() == "MacOS" &&
                exe.parent_path().parent_path().filename() == "Contents")
                packPath =
                    (exe.parent_path().parent_path() / "Resources" / "texture-pack").string();
        }
    }
    if (const char *mb = getenv("POPM_TEXTURE_BUDGET_MB")) {
        char *end = nullptr;
        auto value = strtoul(mb, &end, 10);
        if (end != mb && !*end && value >= 32 && value <= 1024)
            hd_.budget = uint64_t(value) * 1024 * 1024;
    }
    hd_.pack.open(packPath);
    terrain_detail_ = [self packTexture:0 width:0 height:0];
    // Preload the pack's priority list before the first game frame. Stop at
    // 75% of the budget, leaving headroom for textures encountered later.
    std::ifstream preload(std::filesystem::path(packPath) / "preload.txt");
    std::string hash;
    while (preload >> hash) {
        char *end = nullptr;
        uint64_t key = strtoull(hash.c_str(), &end, 16);
        auto file = hd_.pack.files.find(key);
        if (!end || *end || file == hd_.pack.files.end())
            continue;
        if (hd_.used + file->second.bytes > hd_.budget * 3 / 4)
            continue;
        (void)[self packTexture:key width:0 height:0];
    }
    return self;
}

- (void)dealloc {
    if (!hd_.pack.files.empty())
        fprintf(stderr, "[hd] loads=%llu hits=%llu refused=%llu resident=%llu budget=%llu bytes\n",
                (unsigned long long)hd_.loads, (unsigned long long)hd_.hits,
                (unsigned long long)hd_.refused, (unsigned long long)hd_.used,
                (unsigned long long)hd_.budget);
    if (readback_timings_)
        fclose(readback_timings_);
    delete pipelines_;
    delete depths_;
    delete samplers_;
    delete textures_;
    delete texture_current_;
    delete index_cache_;
}

+ (PopD3DRenderer *)shared {
    return g_shared;
}
+ (void)setShared:(PopD3DRenderer *)renderer {
    g_shared = renderer;
}

- (id<MTLTexture>)colorTarget {
    return color_;
}
- (id<MTLCommandQueue>)commandQueue {
    return queue_;
}

- (HostHDTextureStats)hdTextureStats {
    return {hd_.draws, hd_.loads, hd_.hits, hd_.refused, hd_.used, hd_.budget, hd_.detail_draws};
}

- (HostCommandStorageStats)commandStorageStats {
    return storage_stats_;
}

// The CPU scratch is reused immediately after encoding. GPU-consumed bytes
// remain immutable until their exact command buffer finishes, including
// prefixes committed before seal and work belonging to dropped frames.
- (id<MTLBuffer>)argumentBytes:(const void *)bytes length:(NSUInteger)length {
    assert(command_);
    ArgumentBuffer *available = nullptr;
    for (size_t n = 0; n < argument_pool_.size(); ++n) {
        size_t index = (argument_cursor_ + n) % argument_pool_.size();
        auto &entry = argument_pool_[index];
        bool held = false;
        for (const auto &slot : slots_)
            if (entry.frame && slot.frame == entry.frame)
                held = true;
        if (held)
            continue;
        auto status = entry.owner.status;
        if (entry.owner && status != MTLCommandBufferStatusCompleted &&
            status != MTLCommandBufferStatusError)
            continue;
        if (entry.buffer.length >= length) {
            available = &entry;
            argument_cursor_ = index + 1;
            break;
        }
    }
    if (!available) {
        id<MTLBuffer> buffer = [device_ newBufferWithLength:(length + 4095) & ~(NSUInteger)4095
                                                    options:MTLResourceStorageModeShared];
        if (!buffer) {
            fprintf(stderr, "[host] command argument allocation failed\n");
            abort();
        }
        if (argument_pool_.size() == argument_pool_.capacity())
            ++storage_stats_.cpu_growths;
        argument_pool_.push_back({buffer, nil});
        ++storage_stats_.argument_buffers;
        available = &argument_pool_.back();
        argument_cursor_ = argument_pool_.size();
    }
    available->owner = command_;
    available->frame = incremental_ && active_slot_ >= 0 ? slots_[active_slot_].frame : 0;
    memcpy(available->buffer.contents, bytes, length);
    return available->buffer;
}

- (void)bindVertices:(const std::vector<HostD3DVertex> &)vertices
             encoder:(id<MTLRenderCommandEncoder>)enc {
    NSUInteger bytes = vertices.size() * sizeof(HostD3DVertex);
    if (bytes <= 4096)
        [enc setVertexBytes:vertices.data() length:bytes atIndex:0];
    else
        [enc setVertexBuffer:[self argumentBytes:vertices.data() length:bytes] offset:0 atIndex:0];
}

// --- the mirror -------------------------------------------------------------

- (void)allocateMirror:(int)width height:(int)height {
    if (width <= 0 || height <= 0)
        return;
    if (color_ && width == width_ && height == height_) {
        if (acquiringTarget_.world)
            color_ = present_texture(acquiringTarget_.world);
        return;
    }
    [self endEncoding];
    width_ = width;
    height_ = height;

    MTLTextureDescriptor *c =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    c.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    c.storageMode = MTLStorageModePrivate;
    color_ = acquiringTarget_.world ? present_texture(acquiringTarget_.world)
                                    : [device_ newTextureWithDescriptor:c];
    if (color_ && !acquiringTarget_.world)
        ++storage_stats_.scene_textures;

    MTLTextureDescriptor *z =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    z.usage = MTLTextureUsageRenderTarget;
    z.storageMode = MTLStorageModePrivate;
    depth_ = [device_ newTextureWithDescriptor:z];
    if (depth_)
        ++storage_stats_.scene_textures;

    MTLTextureDescriptor *m =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    m.usage = MTLTextureUsageRenderTarget;
    m.storageMode = MTLStorageModePrivate;
    coverage_ = [device_ newTextureWithDescriptor:m];
    if (coverage_)
        ++storage_stats_.scene_textures;

    // A private render target cannot be read by the CPU, so a shared texture
    // is kept alongside each one to blit into and out of.
    MTLTextureDescriptor *s =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    s.usage = MTLTextureUsageShaderRead;
    s.storageMode = MTLStorageModeShared;
    staging_ = [device_ newTextureWithDescriptor:s];
    if (staging_)
        ++storage_stats_.scene_textures;

    MTLTextureDescriptor *cs =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    cs.usage = MTLTextureUsageShaderRead;
    cs.storageMode = MTLStorageModeShared;
    coverage_staging_ = [device_ newTextureWithDescriptor:cs];
    if (coverage_staging_)
        ++storage_stats_.scene_textures;

    pending_clear_color_ = pending_clear_depth_ = true;
    coverage_reset_pending_ = true;
    gpu_dirty_ = false;
    needs_upload_ = true;
}

// mem_init() has unmapped the arena and dx_reset() is dropping the objects that
// pointed into it, so target_pixels_ names memory that is not there. This is
// the one path that must NOT write back: a flush here would put a scene through
// a dangling guest address.
- (void)discard {
    if (hd_.draws)
        fprintf(stderr,
                "[hd] world_draws=%llu loads=%llu hits=%llu refused=%llu resident=%llu budget=%llu "
                "bytes\n",
                (unsigned long long)hd_.draws, (unsigned long long)hd_.loads,
                (unsigned long long)hd_.hits, (unsigned long long)hd_.refused,
                (unsigned long long)hd_.used, (unsigned long long)hd_.budget);
    [self endEncoding];
    if (command_) {
        last_command_ = command_;
        track_native_command(command_);
        [command_ commit];
        command_ = nil;
    }
    for (auto &slot : slots_)
        slot = SceneSlot{};
    active_slot_ = -1;
    incremental_ = false;
    target_generation_ = 0;
    acquiringTarget_ = {};
    frame_dropped_ = false;
    guest_width_ = guest_height_ = 0;
    gpu_dirty_ = false;
    pending_id_ = 0;
    target_id_ = 0;
    target_pixels_ = nullptr;
    needs_upload_ = true;
    coverage_reset_pending_ = true;
    pending_clear_color_ = pending_clear_depth_ = false;
}

- (void)setRenderTarget:(const HostD3DSurface *)target {
    // Anything the device drew into the target it is leaving belongs in that
    // surface before it stops being the target: the mirror is about to be
    // reused, and in the worst case reallocated at another size. The shim
    // flushes before it switches; this is the same guarantee made where the
    // mirror actually lives, so no path through the shims can lose a scene.
    if (!incremental_ && gpu_dirty_ && pending_id_ && (!target || target->id != pending_id_))
        [self legacyWriteBackPending];

    if (!target) {
        target_id_ = 0;
        target_pixels_ = nullptr;
        return;
    }
    bool format_changed = target->width != guest_width_ || target->height != guest_height_ ||
                          target->bpp != target_bpp_;
    bool changed = target->id != target_id_ || format_changed;
    target_id_ = target->id;
    guest_width_ = target->width;
    guest_height_ = target->height;
    target_pixels_ = target->pixels;
    target_pitch_ = target->pitch;
    target_bpp_ = target->bpp;
    target_rmask_ = target->rmask;
    target_gmask_ = target->gmask;
    target_bmask_ = target->bmask;
    if (target->palette) {
        if (memcmp(target_palette_, target->palette, sizeof target_palette_) != 0) {
            memcpy(target_palette_, target->palette, sizeof target_palette_);
            index_cache_valid_ = false;
        }
        target_has_palette_ = true;
    } else {
        target_has_palette_ = false;
    }
    if (changed) {
        [self allocateMirror:scene_width_ ? scene_width_ : target->width
                      height:scene_height_ ? scene_height_ : target->height];
        // Alternating front/back surfaces is normal frame traffic. Report
        // configuration changes without synchronous console I/O every frame.
        if (format_changed) {
            printf("[host] Direct3D renders into surface %u, %dx%d %dbpp\n", target->id,
                   target->width, target->height, target->bpp);
            fflush(stdout);
        }
    }
    // The guest owns those pixels between our visits, so the next draw reads
    // them in before it adds to them.
    if (!incremental_ || changed)
        needs_upload_ = true;
}

// The surface's pixels as BGRA8, which is the mirror's format.
- (void)uploadSurface {
    if (!color_ || !staging_ || !target_pixels_ || !width_ || !height_)
        return;
    if (!cpu_surface_upload_) {
        [self endEncoding];
        if (!command_)
            command_ = [queue_ commandBuffer];
        // argumentBytes copies now and retains each buffer until its exact
        // command finishes. Guest writes and palette changes after this call
        // cannot alter an upload already queued on the GPU.
        id<MTLBuffer> pixels = [self argumentBytes:target_pixels_
                                            length:(NSUInteger)target_pitch_ * guest_height_];
        int rshift = 0, gshift = 0, bshift = 0;
        uint32_t rmax = 0, gmax = 0, bmax = 0;
        uint32_t rm = target_rmask_, gm = target_gmask_, bm = target_bmask_;
        if (!rm) {
            if (target_bpp_ > 16) {
                rm = 0xff0000;
                gm = 0xff00;
                bm = 0xff;
            } else {
                rm = 0xf800;
                gm = 0x07e0;
                bm = 0x001f;
            }
        }
        mask_shape(rm, &rshift, &rmax);
        mask_shape(gm, &gshift, &gmax);
        mask_shape(bm, &bshift, &bmax);
        uint32_t p[20] = {(uint32_t)guest_width_,
                          (uint32_t)guest_height_,
                          (uint32_t)width_,
                          (uint32_t)height_,
                          (uint32_t)target_pitch_,
                          (uint32_t)target_bpp_,
                          (uint32_t)target_has_palette_,
                          0,
                          rm,
                          gm,
                          bm,
                          0,
                          (uint32_t)rshift,
                          (uint32_t)gshift,
                          (uint32_t)bshift,
                          0,
                          rmax,
                          gmax,
                          bmax,
                          0};
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color_;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        auto encoder = [command_ renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:surface_upload_pipeline_];
        [encoder setFragmentBuffer:pixels offset:0 atIndex:0];
        [encoder setFragmentBytes:p length:sizeof p atIndex:1];
        [encoder setFragmentBytes:target_palette_ length:sizeof target_palette_ atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        needs_upload_ = pending_clear_color_ = false;
        return;
    }
    needs_upload_ = false;
    std::vector<uint8_t> bgra((size_t)width_ * (size_t)height_ * 4);
    const uint8_t *src = (const uint8_t *)target_pixels_;
    int rs_shift = 0, gs_shift = 0, bs_shift = 0;
    uint32_t rmax = 0, gmax = 0, bmax = 0;
    mask_shape(target_rmask_, &rs_shift, &rmax);
    mask_shape(target_gmask_, &gs_shift, &gmax);
    mask_shape(target_bmask_, &bs_shift, &bmax);
    for (int y = 0; y < height_; ++y) {
        const uint8_t *row = src + (size_t)(y * guest_height_ / height_) * (size_t)target_pitch_;
        uint8_t *out = bgra.data() + (size_t)y * (size_t)width_ * 4;
        for (int x = 0; x < width_; ++x) {
            int gx = x * guest_width_ / width_;
            uint32_t r = 0, g = 0, b = 0;
            if (target_bpp_ == 8) {
                uint32_t c = target_has_palette_ ? target_palette_[row[gx]]
                                                 : (uint32_t)(row[gx] * 0x010101u);
                r = (c >> 16) & 0xff;
                g = (c >> 8) & 0xff;
                b = c & 0xff;
            } else {
                uint32_t p = texture_raw(row, gx, target_bpp_);
                if (rmax) {
                    r = ((p & target_rmask_) >> rs_shift) * 255 / rmax;
                    g = ((p & target_gmask_) >> gs_shift) * 255 / gmax;
                    b = ((p & target_bmask_) >> bs_shift) * 255 / bmax;
                } else if (target_bpp_ > 16) {
                    r = (p >> 16) & 255;
                    g = (p >> 8) & 255;
                    b = p & 255;
                } else {
                    r = ((p >> 11) & 0x1f) * 255 / 31;
                    g = ((p >> 5) & 0x3f) * 255 / 63;
                    b = (p & 0x1f) * 255 / 31;
                }
            }
            out[4 * x + 0] = (uint8_t)b;
            out[4 * x + 1] = (uint8_t)g;
            out[4 * x + 2] = (uint8_t)r;
            out[4 * x + 3] = 255;
        }
    }
    [staging_ replaceRegion:MTLRegionMake2D(0, 0, width_, height_)
                mipmapLevel:0
                  withBytes:bgra.data()
                bytesPerRow:(NSUInteger)width_ * 4];
    [self endEncoding];
    if (!command_)
        command_ = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
    [blit copyFromTexture:staging_
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width_, height_, 1)
                toTexture:color_
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    // The mirror now matches the surface, so a clear that was still pending
    // would wipe what was just read in.
    pending_clear_color_ = false;
}

- (void)rebuildIndexCache {
    // Nearest palette entry for every 5-5-5 colour. 32768 entries, rebuilt
    // only when the palette changes, which is what makes writing back into an
    // 8-bit render target affordable at all.
    index_cache_->assign(32768, 0);
    for (uint32_t key = 0; key < 32768; ++key) {
        int r = (int)(((key >> 10) & 31) * 255 / 31);
        int g = (int)(((key >> 5) & 31) * 255 / 31);
        int b = (int)((key & 31) * 255 / 31);
        int best = 0;
        long best_d = 1 << 30;
        for (int i = 0; i < 256; ++i) {
            uint32_t c = target_palette_[i];
            long dr = r - (long)((c >> 16) & 0xff);
            long dg = g - (long)((c >> 8) & 0xff);
            long db = b - (long)(c & 0xff);
            long d = dr * dr + dg * dg + db * db;
            if (d < best_d) {
                best_d = d;
                best = i;
                if (!d)
                    break;
            }
        }
        (*index_cache_)[key] = (uint8_t)best;
    }
    index_cache_valid_ = true;
}

- (void)flushSurface:(const HostD3DSurface *)surface why:(const char *)why {
    // POP_HOST_TRACE_FLUSH=1 prints one line per flush that actually wrote
    // something, naming what asked for it. Read against the guest's own blit
    // sequence for a frame, that says whether a software blit landed before or
    // after the write-back that could overwrite it - which is the difference
    // between a panel that is drawn and a panel that is black.
    static int trace = -1;
    if (trace < 0)
        trace = getenv("POP_HOST_TRACE_FLUSH") ? 1 : 0;
    // Any surface may be asked about. The one the mirror's contents belong to
    // is the one that gets them, whether or not it is still the render target.
    if (!surface || !pending_id_ || surface->id != pending_id_)
        return;
    // Always take the pointer the shim just handed over: Flip moves it.
    target_pixels_ = surface->pixels;
    target_pitch_ = surface->pitch;
    if (trace && gpu_dirty_)
        fprintf(stderr, "[host] flush surface %u for %s\n", surface->id, why ? why : "?");
    [self legacyWriteBackPending];
}

// Writes the mirror into the surface the mirror belongs to, using the pointer
// and pitch most recently supplied for it.
- (void)legacyWriteBackPending {
    if (!gpu_dirty_ || !color_ || !staging_ || !target_pixels_)
        return;

    // A clear that is still only a pending load action has not happened yet.
    // Flushing without opening the pass would copy out the pixels the clear
    // was about to replace, which is what a Clear followed by a Lock before
    // any EndScene would otherwise read.
    [self flush];

    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:color_
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width_, height_, 1)
                toTexture:staging_
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit copyFromTexture:coverage_
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width_, height_, 1)
                toTexture:coverage_staging_
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    track_native_command(cb);
    [cb commit];
    assert(!host_d3d_appkit_pending());
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) {
        fprintf(stderr, "[host] legacy write-back GPU failure\n");
        abort();
    }

    std::vector<uint8_t> bgra((size_t)width_ * (size_t)height_ * 4);
    [staging_ getBytes:bgra.data()
           bytesPerRow:(NSUInteger)width_ * 4
            fromRegion:MTLRegionMake2D(0, 0, width_, height_)
           mipmapLevel:0];
    std::vector<uint8_t> covered((size_t)width_ * (size_t)height_);
    [coverage_staging_ getBytes:covered.data()
                    bytesPerRow:(NSUInteger)width_
                     fromRegion:MTLRegionMake2D(0, 0, width_, height_)
                    mipmapLevel:0];

    // How much of what the device produced is above black, measured from the
    // pixels this write-back already has in hand. Sampling at a dump instead
    // catches whichever frame the dump landed on, which for a target that is
    // uploaded, drawn into and read back every frame is not the same question.
    size_t n = (size_t)width_ * (size_t)height_;
    note_scene_nonblack(count_nonblack(bgra.data(), n), n);

    int w = width_, h = height_;
    uint8_t *dst = (uint8_t *)target_pixels_;
    int rs_shift = 0, gs_shift = 0, bs_shift = 0;
    uint32_t rmax = 0, gmax = 0, bmax = 0;
    mask_shape(target_rmask_, &rs_shift, &rmax);
    mask_shape(target_gmask_, &gs_shift, &gmax);
    mask_shape(target_bmask_, &bs_shift, &bmax);
    if (target_bpp_ == 8 && target_has_palette_ && !index_cache_valid_)
        [self rebuildIndexCache];

    for (int y = 0; y < h; ++y) {
        const uint8_t *row = bgra.data() + (size_t)y * (size_t)width_ * 4;
        const uint8_t *mask = covered.data() + (size_t)y * (size_t)width_;
        uint8_t *out = dst + (size_t)y * (size_t)target_pitch_;
        for (int x = 0; x < w; ++x) {
            // A pixel the device never drew keeps the exact bytes the guest
            // wrote. Converting it to RGB and back would not return the same
            // value, and the guest's own drawing is not the renderer's to
            // change.
            if (!mask[x])
                continue;
            uint32_t b = row[4 * x + 0], g = row[4 * x + 1], r = row[4 * x + 2];
            if (target_bpp_ == 8) {
                if (index_cache_valid_) {
                    uint32_t key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
                    out[x] = (*index_cache_)[key];
                } else {
                    log_once("d3d.rt8",
                             "d3d: the render target is 8-bit with no palette; "
                             "the write-back has no index to choose and leaves it alone");
                }
            } else {
                uint32_t p;
                if (rmax) {
                    p = ((r * rmax / 255) << rs_shift) | ((g * gmax / 255) << gs_shift) |
                        ((b * bmax / 255) << bs_shift);
                } else {
                    p = ((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255);
                }
                if (!rmax && target_bpp_ > 16)
                    p = (r << 16) | (g << 8) | b;
                const int bytes = texture_storage_bytes(target_bpp_);
                for (int k = 0; k < bytes; ++k)
                    out[x * bytes + k] = uint8_t(p >> (8 * k));
            }
        }
    }
    gpu_dirty_ = false;
    pending_id_ = 0;
    // The guest owns the surface again until the next draw, and the mask goes
    // back to zero: what was written back has been accounted for.
    needs_upload_ = true;
    coverage_reset_pending_ = true;
    ++g_total_flushes;
}

// --- the encoder ------------------------------------------------------------

- (id<MTLRenderCommandEncoder>)encoder {
    if (encoder_)
        return encoder_;
    if (!color_)
        return nil;
    if (needs_upload_)
        [self uploadSurface];
    if (!command_)
        command_ = [queue_ commandBuffer];

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color_;
    pass.colorAttachments[0].loadAction =
        pending_clear_color_ ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = pending_color_;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // A whole-target colour clear covered every pixel, so the mask starts at
    // one; otherwise it starts at zero after a write-back and is carried
    // forward inside a batch.
    pass.colorAttachments[1].texture = coverage_;
    pass.colorAttachments[1].loadAction =
        pending_clear_color_ ? MTLLoadActionClear
                             : (coverage_reset_pending_ ? MTLLoadActionClear : MTLLoadActionLoad);
    pass.colorAttachments[1].clearColor =
        MTLClearColorMake(pending_clear_color_ ? 1.0 : 0.0, 0, 0, 0);
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    coverage_reset_pending_ = false;
    pass.depthAttachment.texture = depth_;
    pass.depthAttachment.loadAction = pending_clear_depth_ ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.clearDepth = pending_depth_;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pending_clear_color_ = pending_clear_depth_ = false;

    encoder_ = [command_ renderCommandEncoderWithDescriptor:pass];
    [encoder_ setFrontFacingWinding:MTLWindingClockwise];
    return encoder_;
}

- (void)endEncoding {
    if (encoder_) {
        [encoder_ endEncoding];
        encoder_ = nil;
    }
}

- (void)flush {
    // A batch that only cleared has nothing encoded yet, and a clear that
    // never reaches the GPU is not a clear. Opening the pass is what applies
    // the load actions the pending clear turned into.
    if (!encoder_ && color_ && (pending_clear_color_ || pending_clear_depth_))
        (void)[self encoder];
    [self endEncoding];
    if (command_) {
        last_command_ = command_;
        track_native_command(command_);
        [command_ commit];
        command_ = nil;
    }
}

- (BOOL)acceptsDraw {
    return !frame_dropped_;
}
- (void)collectCleanTargets {
    for (auto &slot : slots_)
        if (!host_d3d_dirty_rect(slot.surface.id, slot.generation, nullptr))
            slot.coherenceLease.reset();
}

- (void)setSceneWidth:(int)w height:(int)h {
    assert(w >= 0 && h >= 0 && ((w == 0) == (h == 0)));
    scene_width_ = w;
    scene_height_ = h;
}
- (void)saveSlot {
    if (active_slot_ < 0)
        return;
    SceneSlot &slot = slots_[active_slot_];
    slot.color = color_;
    slot.depth = depth_;
    slot.coverage = coverage_;
    slot.staging = staging_;
    slot.coverageStaging = coverage_staging_;
    slot.last = last_command_;
    slot.initialized = !needs_upload_;
}
- (void)loadSlot:(int)i {
    active_slot_ = i;
    SceneSlot &slot = slots_[i];
    color_ = slot.color;
    depth_ = slot.depth;
    coverage_ = slot.coverage;
    staging_ = slot.staging;
    coverage_staging_ = slot.coverageStaging;
    width_ = (int)color_.width;
    height_ = (int)color_.height;
    target_id_ = pending_id_ = slot.surface.id;
    target_generation_ = slot.generation;
    target_pixels_ = slot.surface.pixels;
    target_pitch_ = slot.surface.pitch;
    target_bpp_ = slot.surface.bpp;
    guest_width_ = slot.surface.width;
    guest_height_ = slot.surface.height;
    target_rmask_ = slot.surface.rmask;
    target_gmask_ = slot.surface.gmask;
    target_bmask_ = slot.surface.bmask;
    target_has_palette_ = slot.surface.palette != nullptr;
    if (memcmp(target_palette_, slot.palette, sizeof target_palette_)) {
        memcpy(target_palette_, slot.palette, sizeof target_palette_);
        index_cache_valid_ = false;
    }
    last_command_ = slot.last;
    needs_upload_ = !slot.initialized;
    pending_clear_color_ = false;
    pending_clear_depth_ = !slot.initialized;
    coverage_reset_pending_ = !slot.initialized;
    gpu_dirty_ = true;
}
- (int)slotFor:(uint32_t)s generation:(uint32_t)g {
    for (int i = 0; i < 4; ++i)
        if (slots_[i].surface.id == s && slots_[i].generation == g)
            return i;
    return -1;
}
- (void)refreshSurface:(const HostD3DSurface *)s slot:(int)i {
    SceneSlot &slot = slots_[i];
    slot.surface = *s;
    if (s->palette) {
        memcpy(slot.palette, s->palette, sizeof slot.palette);
        slot.surface.palette = slot.palette;
    }
    target_pixels_ = s->pixels;
    target_pitch_ = s->pitch;
    target_rmask_ = s->rmask;
    target_gmask_ = s->gmask;
    target_bmask_ = s->bmask;
    target_has_palette_ = s->palette != nullptr;
    if (s->palette && memcmp(target_palette_, s->palette, sizeof target_palette_)) {
        memcpy(target_palette_, s->palette, sizeof target_palette_);
        index_cache_valid_ = false;
    }
}
- (id<MTLTexture>)colorTargetForFrame:(uint64_t)frame {
    if (active_slot_ >= 0 && slots_[active_slot_].frame == frame) {
        [self flush];
        [self saveSlot];
    }
    for (auto &slot : slots_)
        if (slot.frame == frame)
            return slot.color;
    return nil;
}
- (id<MTLCommandBuffer>)completionForFrame:(uint64_t)frame {
    if (active_slot_ >= 0 && slots_[active_slot_].frame == frame) {
        [self flush];
        [self saveSlot];
    }
    for (auto &slot : slots_)
        if (slot.frame == frame)
            return slot.last;
    return nil;
}
- (void)bindSurface:(const HostD3DSurface *)s generation:(uint32_t)g frame:(uint64_t)f {
    if (!s)
        return;
    incremental_ = true;
    if (active_slot_ >= 0 && slots_[active_slot_].frame == f && target_id_ == s->id &&
        target_generation_ == g) {
        [self refreshSurface:s slot:active_slot_];
        return;
    }
    [self flush];
    [self saveSlot];
    bool managed = host_present_running();
    HostSceneTarget target{};
    if (managed) {
        target = host_present_acquire_target(s->width, s->height, scene_width_, scene_height_);
        frame_dropped_ = !target.world;
        if (frame_dropped_)
            return; // no writes into another frame's leased target
    } else
        frame_dropped_ = false;
    // Standalone renderer tests retain the pre-service unclaimed-frame path.
    for (auto &slot : slots_)
        if (slot.unclaimed && slot.frame != f &&
            (slot.last.status == MTLCommandBufferStatusCompleted ||
             slot.last.status == MTLCommandBufferStatusError)) {
            uint64_t old = slot.frame;
            [self retireFrame:old];
            host_d3d_release_unclaimed_frame(old);
        }
    int source = [self slotFor:s->id generation:g];
    if (!managed && source >= 0 && (!slots_[source].frame || slots_[source].frame == f)) {
        slots_[source].frame = f;
        [self loadSlot:source];
        [self refreshSurface:s slot:source];
        [self captureReplaySeed];
        return;
    }
    int dest = -1;
    for (int i = 0; i < 4; ++i)
        if (!slots_[i].frame && i != source &&
            !host_d3d_dirty_rect(slots_[i].surface.id, slots_[i].generation, nullptr)) {
            dest = i;
            break;
        }
    if (dest < 0) {
        // The only backpressure point: first write after seal, with no free
        // target. Unclaimed frames can be retired here after their GPU work.
        for (int i = 0; i < 4; ++i)
            if (i != source && slots_[i].unclaimed &&
                !host_d3d_dirty_rect(slots_[i].surface.id, slots_[i].generation, nullptr)) {
                uint64_t old = slots_[i].frame;
                [self retireFrame:old];
                host_d3d_release_unclaimed_frame(old);
                dest = i;
                break;
            }
    }
    if (dest < 0) {
        if (managed) {
            host_present_drop_current();
            frame_dropped_ = true;
            return;
        }
        fprintf(stderr, "[host] no free scene target; all four targets are held\n");
        abort();
    }
    // Only metadata is new. The free slot's same-sized attachments survive
    // retirement, which already proved their last GPU users have finished.
    active_slot_ = dest;
    SceneSlot &reuse = slots_[dest];
    reuse.surface = {};
    reuse.generation = 0;
    reuse.frame = 0;
    reuse.unclaimed = reuse.initialized = false;
    reuse.separateLayers = reuse.materializedLayers = false;
    reuse.scene_domain_w = 0;
    reuse.presentTarget = {};
    reuse.coherenceLease.reset();
    color_ = reuse.color;
    depth_ = reuse.depth;
    coverage_ = reuse.coverage;
    staging_ = reuse.staging;
    coverage_staging_ = reuse.coverageStaging;
    width_ = (int)color_.width;
    height_ = (int)color_.height;
    target_id_ = 0;
    guest_width_ = guest_height_ = 0;
    pending_clear_color_ = pending_clear_depth_ = coverage_reset_pending_ = true;
    needs_upload_ = true;
    gpu_dirty_ = false;
    int requestedWidth = scene_width_, requestedHeight = scene_height_;
    if (host_frame_legacy({f}))
        scene_width_ = scene_height_ = 0;
    acquiringTarget_ = target;
    if (managed) {
        scene_width_ = target.w;
        scene_height_ = target.h;
    }
    [self setRenderTarget:s];
    target_generation_ = g;
    acquiringTarget_ = {};
    scene_width_ = requestedWidth;
    scene_height_ = requestedHeight;
    if (!color_ || !depth_ || !coverage_) {
        fprintf(stderr, "[host] scene target allocation failed\n");
        abort();
    }
    SceneSlot &slot = slots_[dest];
    slot.frame = f;
    slot.generation = g;
    slot.surface = *s;
    slot.presentTarget = target;
    slot.coherenceLease = host_present_target_lease(target.world);
    if (target.overlay) {
        auto desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:s->width
                                                              height:s->height
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModePrivate;
        if (!slot.overlayDepth || slot.overlayDepth.width != s->width ||
            slot.overlayDepth.height != s->height) {
            slot.overlayDepth = [device_ newTextureWithDescriptor:desc];
            if (slot.overlayDepth)
                ++storage_stats_.scene_textures;
            desc.pixelFormat = MTLPixelFormatR8Unorm;
            slot.overlayCoverage = [device_ newTextureWithDescriptor:desc];
            if (slot.overlayCoverage)
                ++storage_stats_.scene_textures;
        }
        if (!command_)
            command_ = [queue_ commandBuffer];
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = present_texture(target.overlay);
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[1].texture = slot.overlayCoverage;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = slot.overlayDepth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1;
        [[command_ renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    if (s->palette) {
        memcpy(slot.palette, s->palette, sizeof slot.palette);
        slot.surface.palette = slot.palette;
    }
    if (source >= 0) {
        SceneSlot &old = slots_[source];
        bool sameSize = old.color.width == color_.width && old.color.height == color_.height;
        if (!command_)
            command_ = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
        id<MTLTexture> from[] = {old.color, old.depth, old.coverage};
        id<MTLTexture> to[] = {color_, depth_, coverage_};
        for (int k = 0; sameSize && k < 3; ++k)
            [blit copyFromTexture:from[k]
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width_, height_, 1)
                        toTexture:to[k]
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        if (!sameSize) {
            CompositorInput in{};
            in.cls = HOST_SCREEN_GAMEPLAY;
            in.guest_w = s->width;
            in.guest_h = s->height;
            in.drawable_w = width_;
            in.drawable_h = height_;
            compose_native(in, old.color, color_, command_);
        }
        needs_upload_ = false;
        pending_clear_color_ = false;
        pending_clear_depth_ = coverage_reset_pending_ = !sameSize;
        old.coherenceLease.reset();
        // The old frame keeps its textures, but this storage's future reads
        // belong to the newer target after the first write.
        old.surface.id = 0;
    }
    [self saveSlot];
    [self captureReplaySeed];
}
- (void)sealFrame:(uint64_t)f {
    bool ownsTarget = false;
    for (auto &slot : slots_)
        if (slot.frame == f)
            ownsTarget = true;
    if (ownsTarget &&
        (host_frame_legacy({f}) ||
         (active_slot_ >= 0 && slots_[active_slot_].materializedLayers)) &&
        !host_d3d_legacy_writeback() && ![self replayLegacyFrame:f]) {
        fprintf(stderr, "[host] legacy replay failed for frame %llu\n", (unsigned long long)f);
        abort();
    }
    [self flush];
    [self saveSlot];
    if (host_present_running() && !frame_dropped_) {
        for (auto &slot : slots_)
            if (slot.frame == f && slot.presentTarget.world) {
                CompositorInput in{};
                in.cls = host_frame_class({f});
                in.guest_w = slot.surface.width;
                in.guest_h = slot.surface.height;
                in.world = slot.presentTarget.world;
                in.overlay = slot.presentTarget.overlay;
                const int domain = slot.scene_domain_w ? slot.scene_domain_w : in.guest_w;
                in.scene = {float(slot.presentTarget.w) / domain,
                            float(slot.presentTarget.h) / in.guest_h, 0, 0, domain};
                in.legacy =
                    mods_display_classic() || host_frame_legacy({f}) || slot.materializedLayers;
                if (in.legacy) {
                    in.world = {};
                    in.overlay = {};
                    // The renderer's own colour texture. Importing the same
                    // object again returns the same handle, so the presenter's
                    // cached input stays valid across frames.
                    slot.legacyImport =
                        gpu::metal::import_texture(host_present_device(), slot.color);
                    in.legacy_frame = slot.legacyImport;
                }
                host_present_set_input(&in);
            }
    }
    frame_dropped_ = false;
    bool unclaimed = !(host_present_running() || host_d3d_claim_sealed_frame(f));
    for (auto &slot : slots_)
        if (slot.frame == f)
            slot.unclaimed = unclaimed;
}
- (void)retireFrame:(uint64_t)f {
    if (active_slot_ >= 0 && slots_[active_slot_].frame == f) {
        [self flush];
        [self saveSlot];
    }
    for (auto &slot : slots_)
        if (slot.frame == f) {
            assert(!host_d3d_appkit_pending());
            if (!slot.presentTarget.world)
                [slot.last waitUntilCompleted];
            slot.frame = 0;
            slot.unclaimed = false;
            slot.replay.reset();
            if (!host_d3d_dirty_rect(slot.surface.id, slot.generation, nullptr))
                slot.coherenceLease.reset();
        }
}
- (void)swapSurface:(uint32_t)a generation:(uint32_t)ag other:(uint32_t)b generation:(uint32_t)bg {
    [self flush];
    [self saveSlot];
    for (auto &slot : slots_) {
        if (slot.surface.id == a && slot.generation == ag) {
            slot.surface.id = b;
            slot.generation = bg + 1;
        } else if (slot.surface.id == b && slot.generation == bg) {
            slot.surface.id = a;
            slot.generation = ag + 1;
        }
    }
    if (active_slot_ >= 0)
        [self loadSlot:active_slot_];
}

// One command-buffer wait for all dirty islands touched by this reader. No
// main-queue dispatch, drawable acquisition, or AppKit call is on this path.
- (BOOL)coherentSurface:(const HostD3DSurface *)surface
             generation:(uint32_t)generation
                  rects:(const HostDirtyRect *)rects
                  count:(uint32_t)count {
    int slot = [self slotFor:surface->id generation:generation];
    if (slot < 0)
        return NO;
    // A reader needs the guest's original ordering, including CPU HUD and UI
    // overlays. Reuse T5's retained journal to materialize that picture; the
    // frame then presents this full image instead of mixing it with layers.
    if (slots_[slot].separateLayers)
        slots_[slot].materializedLayers = true;
    if (host_frame_legacy({slots_[slot].frame}) || slots_[slot].materializedLayers)
        return [self replayLegacyFrame:slots_[slot].frame];
    [self flush];
    [self saveSlot];
    int previous = active_slot_;
    [self loadSlot:slot];
    [self refreshSurface:surface slot:slot];
    [self flush];
    id<MTLCommandBuffer> render = readback_timings_ ? last_command_ : nil;
    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    size_t pixels = 0;
    std::vector<size_t> offsets;
    for (uint32_t i = 0; i < count; ++i) {
        offsets.push_back(pixels);
        pixels += size_t(rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
    }
    if (!pixels) {
        [self saveSlot];
        if (previous >= 0)
            [self loadSlot:previous];
        return YES;
    }
    if (readback_buffer_.length < pixels * 8)
        readback_buffer_ = [device_ newBufferWithLength:pixels * 8
                                                options:MTLResourceStorageModeShared];
    if (!readback_buffer_)
        return NO;
    auto make_compute = [&](id<MTLCounterSampleBuffer> counter) {
        // Dispatches read immutable textures and write disjoint output ranges.
        // No dispatch consumes another dispatch's output. Tracked resources
        // order the preceding render pass; the CPU waits for the whole buffer.
        if (!counter)
            return [cb computeCommandEncoderWithDispatchType:tiled_readback_
                                                                 ? MTLDispatchTypeConcurrent
                                                                 : MTLDispatchTypeSerial];
        MTLComputePassDescriptor *desc = [MTLComputePassDescriptor new];
        desc.sampleBufferAttachments[0].sampleBuffer = counter;
        desc.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
        desc.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
        return [cb computeCommandEncoderWithDescriptor:desc];
    };
    id<MTLComputeCommandEncoder> compute = make_compute(sample_counter_);
    [compute
        setComputePipelineState:tiled_readback_ ? readback_pipeline_ : readback_fused_pipeline_];
    [compute setTexture:color_ atIndex:0];
    [compute setTexture:coverage_ atIndex:1];
    [compute setBuffer:readback_buffer_ offset:0 atIndex:0];
    auto edge = [](int x, int n, int g) { return (int)(((int64_t)x * n + g - 1) / g); };
    for (uint32_t i = 0; i < count; ++i) {
        auto r = rects[i];
        uint32_t params[] = {uint32_t(r.x0),
                             uint32_t(r.y0),
                             uint32_t(r.x1),
                             uint32_t(r.y1),
                             uint32_t(width_),
                             uint32_t(height_),
                             uint32_t(guest_width_),
                             uint32_t(guest_height_),
                             uint32_t(offsets[i]),
                             uint32_t(edge(r.x1, width_, guest_width_)),
                             uint32_t(edge(r.y1, height_, guest_height_)),
                             0};
        [compute setBytes:params length:sizeof params atIndex:1];
        [compute dispatchThreads:MTLSizeMake(r.x1 - r.x0, r.y1 - r.y0, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    }
    size_t brightness_count = 0;
    if (tiled_readback_) {
        // Query the pipeline width instead of assuming a particular GPU's
        // SIMD width. Every dispatch uses complete 16x16 threadgroups.
        const NSUInteger simds = (256 + brightness_pipeline_.threadExecutionWidth - 1) /
                                 brightness_pipeline_.threadExecutionWidth;
        for (uint32_t i = 0; i < count; ++i) {
            auto r = rects[i];
            size_t w = edge(r.x1, width_, guest_width_) - edge(r.x0, width_, guest_width_);
            size_t h = edge(r.y1, height_, guest_height_) - edge(r.y0, height_, guest_height_);
            brightness_count += ((w + 31) / 32) * ((h + 31) / 32) * simds;
        }
        if (brightness_buffer_.length < brightness_count * 4)
            brightness_buffer_ = [device_ newBufferWithLength:brightness_count * 4
                                                      options:MTLResourceStorageModeShared];
        if (brightness_count && !brightness_buffer_) {
            [compute endEncoding];
            return NO;
        }
        if (sample_counter_) {
            [compute endEncoding];
            compute = make_compute(brightness_counter_);
            [compute setTexture:color_ atIndex:0];
        }
        [compute setComputePipelineState:brightness_pipeline_];
        if (brightness_count)
            [compute setBuffer:brightness_buffer_ offset:0 atIndex:0];
        size_t offset = 0;
        for (uint32_t i = 0; i < count; ++i) {
            auto r = rects[i];
            uint32_t x0 = edge(r.x0, width_, guest_width_), y0 = edge(r.y0, height_, guest_height_);
            uint32_t x1 = edge(r.x1, width_, guest_width_), y1 = edge(r.y1, height_, guest_height_);
            uint32_t gx = (x1 - x0 + 31) / 32, gy = (y1 - y0 + 31) / 32;
            if (!gx || !gy)
                continue;
            uint32_t params[] = {x0, y0, x1, y1, uint32_t(offset), gx, uint32_t(simds), 0};
            [compute setBytes:params length:sizeof params atIndex:1];
            [compute dispatchThreadgroups:MTLSizeMake(gx, gy, 1)
                    threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            offset += size_t(gx) * gy * simds;
        }
    }
    [compute endEncoding];
    track_native_command(cb);
    double wait_begin = readback_timings_ ? CACurrentMediaTime() : 0;
    [cb commit];
    assert(!host_d3d_appkit_pending());
    [cb waitUntilCompleted];
    last_command_ = cb;
    double copy_begin = readback_timings_ ? CACurrentMediaTime() : 0;
    if (cb.status == MTLCommandBufferStatusError)
        return NO;
    int rs = 0, gs = 0, bs = 0;
    uint32_t rm = 0, gm = 0, bm = 0;
    mask_shape(target_rmask_, &rs, &rm);
    mask_shape(target_gmask_, &gs, &gm);
    mask_shape(target_bmask_, &bs, &bm);
    if (target_bpp_ == 8 && target_has_palette_ && !index_cache_valid_)
        [self rebuildIndexCache];
    // Small reads stay on the calling thread; large reads use at most four
    // jobs on the system pool. The guest remains stopped until all rows join.
    uint32_t workers = pixels >= 128 * 1024 ? readback_workers_ : 1;
    ReadbackCopy copy{rects,
                      offsets.data(),
                      (const uint32_t *)readback_buffer_.contents,
                      (uint8_t *)target_pixels_,
                      target_has_palette_ && index_cache_valid_ ? index_cache_->data() : nullptr,
                      count,
                      workers,
                      rm,
                      gm,
                      bm,
                      target_pitch_,
                      target_bpp_,
                      rs,
                      gs,
                      bs};
    if (workers > 1) {
        dispatch_apply_f(workers, DISPATCH_APPLY_AUTO, &copy, copy_readback_rows);
        ++storage_stats_.parallel_readbacks;
    } else
        copy_readback_rows(&copy, 0);
    size_t lit = 0;
    for (uint32_t i = 0; i < workers; ++i) {
        if (copy.result[i].failed)
            return NO;
        lit += copy.result[i].lit;
    }
    if (tiled_readback_) {
        const uint32_t *counts = (const uint32_t *)brightness_buffer_.contents;
        for (size_t i = 0; i < brightness_count; ++i)
            lit += counts[i];
    }
    // The tracker supplies disjoint islands. Count only pixels read in this
    // prefix, against the entire native target: a tiny bright Lock must not
    // report a fully lit scene. This is a lower bound for partial reads and
    // the exact scene ratio for full reads, without any extra GPU readback.
    note_scene_nonblack(lit, (size_t)width_ * height_);
    const double copy_end = readback_timings_ ? CACurrentMediaTime() : 0;
    auto stage_ms = [&](id<MTLCounterSampleBuffer> counter) -> double {
        if (!counter)
            return -1;
        NSData *data = [counter resolveCounterRange:NSMakeRange(0, 2)];
        if (data.length != 2 * sizeof(MTLCounterResultTimestamp))
            return -1;
        const auto *times = (const MTLCounterResultTimestamp *)data.bytes;
        if (times[0].timestamp == MTLCounterErrorValue ||
            times[1].timestamp == MTLCounterErrorValue || times[1].timestamp < times[0].timestamp)
            return -1;
        return double(times[1].timestamp - times[0].timestamp) / 1e6;
    };
    if (readback_timings_)
        fprintf(readback_timings_, "%d,%d,%u,%u,%u,%u,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", width_,
                height_, submit_draws_, early_submissions_, g_draws_since_present, workers,
                tiled_readback_ ? "tiled" : "fused",
                render ? (render.GPUEndTime - render.GPUStartTime) * 1000 : 0,
                (cb.GPUEndTime - cb.GPUStartTime) * 1000, (copy_begin - wait_begin) * 1000,
                (copy_end - copy_begin) * 1000, stage_ms(sample_counter_),
                tiled_readback_ ? stage_ms(brightness_counter_) : -1);
    ++g_total_flushes;
    // Do not schedule a whole-surface upload or clear depth. The next encoder
    // loads all three attachments from the just-completed prefix.
    [self saveSlot];
    if (previous >= 0)
        [self loadSlot:previous];
    return YES;
}

- (void)applyCPU:(const HostD3DSurface *)surface record:(const HostBlitRecord *)r {
    if (frame_dropped_)
        return;
    if (!r || r->src.surface != HOST_SRC_CPU || !r->cpu_pixels || r->w <= 0 || r->h <= 0)
        return;
    int slot = [self slotFor:r->dst generation:r->dst_generation];
    if (slot < 0)
        return; // no GPU version: first draw uploads guest storage
    int previous = active_slot_;
    if (slot != previous) {
        [self flush];
        [self saveSlot];
        [self loadSlot:slot];
    }
    if (!replaying_) {
        [self captureReplayCheckpoint:YES];
        ReplayStep step;
        step.seq = r->seq;
        step.record = *r;
        step.hasPalette = surface->palette != nullptr;
        if (surface->palette)
            memcpy(step.palette, surface->palette, sizeof step.palette);
        size_t row = (size_t)r->w * texture_storage_bytes(r->cpu_bpp);
        step.pixels.resize(row * r->h);
        step.coverage.resize((size_t)r->w * r->h, 1);
        for (int y = 0; y < r->h; ++y)
            memcpy(step.pixels.data() + y * row, r->cpu_pixels + y * r->cpu_pitch, row);
        if (r->coverage)
            memcpy(step.coverage.data(), r->coverage, step.coverage.size());
        step.record.cpu_pitch = (int32_t)row;
        slots_[slot].replay.done = false;
        slots_[slot].replay.steps.push_back(std::move(step));
    }
    // Classic encodes HUD writes into the same guest-resolution colour
    // surface as the world. Selecting it introduces no replay or readback.
    if (!mods_display_classic() && !replaying_ && slots_[slot].presentTarget.world &&
        r->after_first_draw && !host_frame_legacy({slots_[slot].frame}) &&
        !slots_[slot].materializedLayers) {
        // The extractor owns HUD pixels. Baking them into world would leave
        // a duplicate at the old guest position after the compositor anchors UI.
        slots_[slot].separateLayers = true;
        if (slot != previous) {
            [self saveSlot];
            if (previous >= 0)
                [self loadSlot:previous];
        }
        return;
    }
    // Materialize a pending Clear before the upload, then continue in the
    // same command buffer. CPU writes do not themselves require a submission.
    if (!encoder_ && (pending_clear_color_ || pending_clear_depth_))
        (void)[self encoder];
    if (needs_upload_)
        [self uploadSurface];
    [self endEncoding];
    if (!command_)
        command_ = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
    int rs = 0, gs = 0, bs = 0;
    uint32_t rm = 0, gm = 0, bm = 0;
    mask_shape(surface->rmask, &rs, &rm);
    mask_shape(surface->gmask, &gs, &gm);
    mask_shape(surface->bmask, &bs, &bm);
    auto edge = [](int x, int n, int g) { return (int)(((int64_t)x * n + g - 1) / g); };
    for (int y = 0; y < r->h; ++y)
        for (int x = 0; x < r->w;) {
            if (r->coverage && !r->coverage[y * r->w + x]) {
                ++x;
                continue;
            }
            int start = x;
            while (x < r->w && (!r->coverage || r->coverage[y * r->w + x]))
                ++x;
            int x0 = edge(r->dst_x + start, width_, guest_width_),
                x1 = edge(r->dst_x + x, width_, guest_width_);
            int y0 = edge(r->dst_y + y, height_, guest_height_),
                y1 = edge(r->dst_y + y + 1, height_, guest_height_);
            if (x1 <= x0 || y1 <= y0)
                continue;
            size_t pitch = ((size_t)(x1 - x0) * 4 + 255) & ~255u;
            auto &data = upload_scratch_;
            auto &zero = mask_scratch_;
            size_t bytes = pitch * (y1 - y0);
            for (auto *scratch : {&data, &zero}) {
                if (scratch->capacity() < bytes) {
                    scratch->reserve(bytes);
                    ++storage_stats_.cpu_growths;
                }
                scratch->assign(bytes, 0);
            }
            for (int ny = y0; ny < y1; ++ny)
                for (int nx = x0; nx < x1; ++nx) {
                    int gx = (int)((int64_t)nx * guest_width_ / width_) - r->dst_x;
                    const uint8_t *pixel = r->cpu_pixels + (size_t)y * r->cpu_pitch +
                                           gx * texture_storage_bytes(r->cpu_bpp);
                    uint32_t red = 0, g = 0, b = 0;
                    if (r->cpu_bpp == 8) {
                        uint32_t c =
                            surface->palette ? surface->palette[*pixel] : *pixel * 0x010101u;
                        red = (c >> 16) & 255;
                        g = (c >> 8) & 255;
                        b = c & 255;
                    } else {
                        uint32_t p = texture_raw(pixel, 0, r->cpu_bpp);
                        if (rm) {
                            red = ((p & surface->rmask) >> rs) * 255 / rm;
                            g = ((p & surface->gmask) >> gs) * 255 / gm;
                            b = ((p & surface->bmask) >> bs) * 255 / bm;
                        } else if (r->cpu_bpp > 16) {
                            red = (p >> 16) & 255;
                            g = (p >> 8) & 255;
                            b = p & 255;
                        } else {
                            red = ((p >> 11) & 31) * 255 / 31;
                            g = ((p >> 5) & 63) * 255 / 63;
                            b = (p & 31) * 255 / 31;
                        }
                    }
                    uint8_t *dst = data.data() + (ny - y0) * pitch + (nx - x0) * 4;
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = red;
                    dst[3] = 255;
                }
            // Each buffer is immutable and retained by the command buffer. Reusing
            // a shared staging texture here races an earlier in-flight upload.
            id<MTLBuffer> pixels = [self argumentBytes:data.data() length:data.size()];
            id<MTLBuffer> mask = [self argumentBytes:zero.data() length:zero.size()];
            for (int k = 0; k < 2; ++k)
                [blit copyFromBuffer:k ? mask : pixels
                           sourceOffset:0
                      sourceBytesPerRow:pitch
                    sourceBytesPerImage:pitch * (y1 - y0)
                             sourceSize:MTLSizeMake(x1 - x0, y1 - y0, 1)
                              toTexture:k ? coverage_ : color_
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(x0, y0, 0)];
        }
    [blit endEncoding];
    if (slot != previous) {
        [self flush];
        [self saveSlot];
        if (previous >= 0)
            [self loadSlot:previous];
    } else
        [self saveSlot];
}

// --- state objects ----------------------------------------------------------

- (id<MTLRenderPipelineState>)pipelineForBlend:(BOOL)enabled
                                           src:(MTLBlendFactor)src
                                           dst:(MTLBlendFactor)dst
                                    writeColor:(BOOL)writeColor {
    uint64_t key = ((uint64_t)overlay_rendering_ << 41) | ((uint64_t)enabled << 40) |
                   ((uint64_t)writeColor << 32) | ((uint64_t)src << 16) | (uint64_t)dst;
    auto it = pipelines_->find(key);
    if (it != pipelines_->end())
        return it->second;

    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [library_ newFunctionWithName:@"d3d_vertex"];
    d.fragmentFunction = [library_ newFunctionWithName:@"d3d_fragment"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The coverage mask never blends: a fragment either landed here or it did
    // not. It is masked off exactly when the colour is, because a draw that
    // writes no colour has not touched the surface either.
    d.colorAttachments[1].pixelFormat = MTLPixelFormatR8Unorm;
    d.colorAttachments[1].blendingEnabled = NO;
    d.colorAttachments[1].writeMask = writeColor ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.colorAttachments[0].writeMask = writeColor ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    d.colorAttachments[0].blendingEnabled = enabled;
    d.colorAttachments[0].sourceRGBBlendFactor = src;
    d.colorAttachments[0].destinationRGBBlendFactor = dst;
    d.colorAttachments[0].sourceAlphaBlendFactor = overlay_rendering_ ? MTLBlendFactorOne : src;
    d.colorAttachments[0].destinationAlphaBlendFactor =
        overlay_rendering_ ? MTLBlendFactorOneMinusSourceAlpha : dst;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d
                                                                               error:&error];
    if (!state) {
        fprintf(stderr, "[host] Direct3D pipeline state failed: %s\n",
                error ? error.localizedDescription.UTF8String : "unknown error");
        return nil;
    }
    (*pipelines_)[key] = state;
    return state;
}

- (id<MTLDepthStencilState>)depthStateWithCompare:(MTLCompareFunction)fn write:(BOOL)write {
    uint64_t key = ((uint64_t)fn << 1) | (write ? 1u : 0u);
    auto it = depths_->find(key);
    if (it != depths_->end())
        return it->second;
    MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = fn;
    d.depthWriteEnabled = write;
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:d];
    (*depths_)[key] = state;
    return state;
}

- (id<MTLSamplerState>)samplerU:(MTLSamplerAddressMode)u
                              v:(MTLSamplerAddressMode)v
                            mag:(MTLSamplerMinMagFilter)mag
                            min:(MTLSamplerMinMagFilter)minf
                            mip:(MTLSamplerMipFilter)mip
                     anisotropy:(NSUInteger)anisotropy {
    uint64_t key = ((uint64_t)u << 16) | ((uint64_t)v << 12) | ((uint64_t)mag << 8) |
                   ((uint64_t)minf << 4) | (uint64_t)mip | (uint64_t(anisotropy) << 24);
    auto it = samplers_->find(key);
    if (it != samplers_->end())
        return it->second;
    MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
    d.sAddressMode = u;
    d.tAddressMode = v;
    d.magFilter = mag;
    d.minFilter = minf;
    d.mipFilter = mip;
    d.maxAnisotropy = anisotropy;
    id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:d];
    (*samplers_)[key] = state;
    return state;
}

// --- the callbacks ----------------------------------------------------------

- (void)beginScene {
    in_scene_ = true;
    encoded_draws_ = early_submissions_ = 0;
}
- (void)sealCommands {
    [self flush];
}

- (void)endScene {
    in_scene_ = false;
    // Not a flush to the surface: EndScene does not present, and the shim
    // calls flushSurface before anything actually looks at the pixels. This
    // only stops work sitting in an open command buffer.
    [self flush];
    [self dumpScene];
}

// The render target as the device left it, before anything the guest blits
// over it. Separate from the presented frame on purpose: seeing both says
// whether a dark scene was rendered dark or darkened afterwards.
- (void)dumpScene {
    uint32_t every = host_dump_every();
    if (!every || !color_ || !width_ || !height_)
        return;
    static uint32_t scenes = 0;
    if ((scenes++ % every) != 0)
        return;
    std::vector<uint8_t> bgra((size_t)width_ * (size_t)height_ * 4);
    int w = 0, h = 0;
    if (![self readPixels:bgra.data() width:&w height:&h])
        return;
    std::vector<uint8_t> rgb((size_t)w * (size_t)h * 3);
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/scene_%05u.ppm", host_dump_dir(), scenes - 1);
    host_write_ppm(path, rgb.data(), w, h);
}

- (void)clearFlags:(uint32_t)flags
             rects:(const int32_t *)rects
             count:(uint32_t)count
             color:(uint32_t)color
             depth:(float)depth {
    if (!color_)
        return;
    float r, g, b, a;
    unpack(color, &r, &g, &b, &a);

    bool clear_color = (flags & 1) != 0; // D3DCLEAR_TARGET
    bool clear_depth = (flags & 2) != 0; // D3DCLEAR_ZBUFFER
    if (!clear_color && !clear_depth)
        return;
    if (clear_color) {
        gpu_dirty_ = true;
        pending_id_ = target_id_;
    }

    // Whole-target clears become the render pass's load action, which is the
    // cheap path and the one a real device takes.
    bool whole = count == 0;
    if (count == 1 && rects) {
        whole =
            rects[0] <= 0 && rects[1] <= 0 && rects[2] >= guest_width_ && rects[3] >= guest_height_;
    }
    if (whole && !encoder_) {
        if (clear_color) {
            pending_clear_color_ = true;
            pending_color_ = MTLClearColorMake(r, g, b, a);
            // A whole-target colour clear replaces the surface's contents, so
            // there is nothing left to read in from it.
            needs_upload_ = false;
        }
        if (clear_depth) {
            pending_clear_depth_ = true;
            pending_depth_ = depth;
        }
        return;
    }

    id<MTLRenderCommandEncoder> enc = [self encoder];
    if (!enc)
        return;
    id<MTLRenderPipelineState> pipeline = [self pipelineForBlend:NO
                                                             src:MTLBlendFactorOne
                                                             dst:MTLBlendFactorZero
                                                      writeColor:clear_color ? YES : NO];
    if (!pipeline)
        return;
    [enc setRenderPipelineState:pipeline];
    [enc setDepthStencilState:[self depthStateWithCompare:MTLCompareFunctionAlways
                                                    write:clear_depth ? YES : NO]];
    [enc setCullMode:MTLCullModeNone];
    [enc setViewport:(MTLViewport){0, 0, (double)width_, (double)height_, 0, 1}];

    Uniforms u;
    memset(&u, 0, sizeof u);
    identity(u.mvp);
    u.pretransformed = 1;
    u.pointsize = 1.0f;
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentTexture:white_ atIndex:0];
    [enc setFragmentSamplerState:[self samplerU:MTLSamplerAddressModeClampToEdge
                                              v:MTLSamplerAddressModeClampToEdge
                                            mag:MTLSamplerMinMagFilterNearest
                                            min:MTLSamplerMinMagFilterNearest
                                            mip:MTLSamplerMipFilterNotMipmapped
                                     anisotropy:1]
                         atIndex:0];

    uint32_t n = count ? count : 1;
    auto &quad = vertex_scratch_;
    quad.clear();
    if (quad.capacity() < (size_t)n * 6) {
        quad.reserve((size_t)n * 6);
        ++storage_stats_.cpu_growths;
    }
    for (uint32_t i = 0; i < n; ++i) {
        double x0 = 0, y0 = 0, x1 = width_, y1 = height_;
        if (count && rects) {
            x0 = rects[i * 4 + 0] * (double)width_ / guest_width_;
            y0 = rects[i * 4 + 1] * (double)height_ / guest_height_;
            x1 = rects[i * 4 + 2] * (double)width_ / guest_width_;
            y1 = rects[i * 4 + 3] * (double)height_ / guest_height_;
        }
        float nx0 = (float)(x0 / (double)width_ * 2.0 - 1.0);
        float nx1 = (float)(x1 / (double)width_ * 2.0 - 1.0);
        float ny0 = (float)(1.0 - y0 / (double)height_ * 2.0);
        float ny1 = (float)(1.0 - y1 / (double)height_ * 2.0);
        HostD3DVertex v;
        memset(&v, 0, sizeof v);
        v.w = 1;
        v.z = depth;
        v.r = r;
        v.g = g;
        v.b = b;
        v.a = a;
        const float xs[6] = {nx0, nx1, nx0, nx0, nx1, nx1};
        const float ys[6] = {ny0, ny0, ny1, ny1, ny0, ny1};
        for (int k = 0; k < 6; ++k) {
            v.x = xs[k];
            v.y = ys[k];
            quad.push_back(v);
        }
    }
    if (quad.empty())
        return;
    [self bindVertices:quad encoder:enc];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:quad.size()];
}

- (void)draw:(const HostD3DDraw *)cmd {
    [self draw:cmd revision:0];
}

- (void)draw:(const HostD3DDraw *)cmd revision:(uint32_t)revision {
    if (!cmd || !color_)
        return;
    ++g_total_draws;
    ++g_draws_since_present;

    // Expand the logical viewport BEFORE TL vertices become clip coordinates.
    // Changing only the Metal viewport would still clip the widened band at +w.
    int domain = guest_width_;
    HostD3DDraw mapped;
    if (active_slot_ >= 0 && !overlay_rendering_ && !replaying_ && !mods_display_classic() &&
        !slots_[active_slot_].materializedLayers &&
        !host_frame_legacy({slots_[active_slot_].frame})) {
        domain = mods_display_scene_width(guest_width_, guest_height_);
        slots_[active_slot_].scene_domain_w = domain;
        if (domain != guest_width_) {
            mapped = *cmd;
            mapped.viewport[2] += domain - guest_width_;
            cmd = &mapped;
        }
    }
    int count = host_d3d_expand(cmd, nullptr, 0);
    if (count < 0) {
        log_once("d3d.vtype",
                 "d3d: vertex type %u with primitive %u is not one this "
                 "renderer draws",
                 cmd->vertex_type, cmd->primitive_type);
        return;
    }
    if (count == 0)
        return;
    auto &vertices = vertex_scratch_;
    if (vertices.capacity() < (size_t)count) {
        vertices.reserve(count);
        ++storage_stats_.cpu_growths;
    }
    vertices.resize(count);
    host_d3d_expand(cmd, vertices.data(), count);

    id<MTLRenderCommandEncoder> enc = [self encoder];
    if (!enc)
        return;
    gpu_dirty_ = true;
    pending_id_ = target_id_;

    // The first draw, in full: what the game asked for, so it can be put beside
    // what the original asked for in the Wine trace.
    static int draw_trace = -1;
    if (draw_trace < 0)
        draw_trace = getenv("POP_HOST_TRACE_D3D") ? 1 : 0;
    static bool told_draw = false;
    if (draw_trace && !told_draw) {
        told_draw = true;
        fprintf(stderr,
                "[host] first draw: vtype %u prim %u verts %u idx %u tex %u\n"
                "       SHADEMODE %u TEXTUREMAPBLEND %u SPECULARENABLE %u "
                "COLORKEYENABLE %u DITHERENABLE %u\n"
                "       ALPHABLENDENABLE %u SRCBLEND %u DESTBLEND %u "
                "ALPHATESTENABLE %u ALPHAFUNC %u ALPHAREF %u\n"
                "       ZENABLE %u ZWRITEENABLE %u ZFUNC %u CULLMODE %u "
                "FILLMODE %u\n"
                "       FOGENABLE %u FOGTABLEMODE %u FOGCOLOR %08x "
                "TEXTUREMAG %u TEXTUREMIN %u\n"
                "       viewport %d,%d %dx%d z %.3f..%.3f  world %s view %s proj %s\n",
                cmd->vertex_type, cmd->primitive_type, cmd->vertex_count, cmd->index_count,
                cmd->texture_handle, rs_raw(cmd, RS_SHADEMODE), rs_raw(cmd, RS_TEXTUREMAPBLEND),
                rs_raw(cmd, RS_SPECULARENABLE), rs_raw(cmd, 41 + 0), rs_raw(cmd, 26),
                rs_raw(cmd, RS_ALPHABLENDENABLE), rs_raw(cmd, RS_SRCBLEND),
                rs_raw(cmd, RS_DESTBLEND), rs_raw(cmd, RS_ALPHATESTENABLE),
                rs_raw(cmd, RS_ALPHAFUNC), rs_raw(cmd, RS_ALPHAREF), rs_raw(cmd, RS_ZENABLE),
                rs_raw(cmd, RS_ZWRITEENABLE), rs_raw(cmd, RS_ZFUNC), rs_raw(cmd, RS_CULLMODE),
                rs_raw(cmd, RS_FILLMODE), rs_raw(cmd, RS_FOGENABLE), rs_raw(cmd, RS_FOGTABLEMODE),
                rs_raw(cmd, RS_FOGCOLOR), rs_raw(cmd, RS_TEXTUREMAG), rs_raw(cmd, RS_TEXTUREMIN),
                cmd->viewport[0], cmd->viewport[1], cmd->viewport[2], cmd->viewport[3],
                cmd->viewport_minz, cmd->viewport_maxz, cmd->world ? "set" : "unset",
                cmd->view ? "set" : "unset", cmd->projection ? "set" : "unset");
        // The raw vertices as the guest wrote them, beside what they became.
        // Screen coordinates that all land on the same spot are a mesh that
        // will not be seen however brightly it is shaded.
        for (uint32_t i = 0; i < 3 && i < cmd->vertex_count; ++i) {
            const uint8_t *raw = (const uint8_t *)cmd->vertices + i * cmd->vertex_stride;
            fprintf(stderr,
                    "       raw%u sx %.2f sy %.2f sz %.4f rhw %.4f "
                    "colour %08x specular %08x tu %.4f tv %.4f\n",
                    i, f32(raw, 0), f32(raw, 4), f32(raw, 8), f32(raw, 12), u32(raw, 16),
                    u32(raw, 20), f32(raw, 24), f32(raw, 28));
        }
        for (int i = 0; i < 3 && i < (int)vertices.size(); ++i) {
            const HostD3DVertex &v = vertices[i];
            fprintf(stderr,
                    "       v%d clip %.4f,%.4f,%.4f w %.4f -> ndc %.3f,%.3f "
                    "uv %.3f,%.3f diffuse %.2f,%.2f,%.2f,%.2f\n",
                    i, v.x, v.y, v.z, v.w, v.w != 0 ? v.x / v.w : 0.0f, v.w != 0 ? v.y / v.w : 0.0f,
                    v.u, v.v, v.r, v.g, v.b, v.a);
        }
        fflush(stderr);
    }

    // --- blending ---
    bool blend_on = rs_raw(cmd, RS_ALPHABLENDENABLE) != 0;
    bool both_src = false, ignored = false;
    uint32_t src_state = rs(cmd, RS_SRCBLEND, 2);
    uint32_t dst_state = rs(cmd, RS_DESTBLEND, 1);
    MTLBlendFactor src = blend_factor(src_state, false, &both_src);
    MTLBlendFactor dst = blend_factor(dst_state, true, &ignored);
    // D3DBLEND_BOTHSRCALPHA names both factors at once, so the destination
    // follows the source and whatever the guest left in DESTBLEND is ignored.
    if (both_src) {
        bool b;
        dst = blend_factor(src_state, true, &b);
    }
    id<MTLRenderPipelineState> pipeline = [self pipelineForBlend:blend_on
                                                             src:src
                                                             dst:dst
                                                      writeColor:YES];
    if (!pipeline)
        return;
    [enc setRenderPipelineState:pipeline];

    // --- depth ---
    // D3DRENDERSTATE_ZENABLE off means no test and no write: a device with its
    // depth buffer disabled does not quietly keep filling it in.
    bool ztest = rs_raw(cmd, RS_ZENABLE) != 0;
    bool zwrite = ztest && rs_raw(cmd, RS_ZWRITEENABLE) != 0;
    MTLCompareFunction zfunc =
        ztest ? compare_function(rs(cmd, RS_ZFUNC, 4)) : MTLCompareFunctionAlways;
    [enc setDepthStencilState:[self depthStateWithCompare:zfunc write:zwrite ? YES : NO]];

    // --- culling ---
    MTLCullMode cull = MTLCullModeNone;
    if (cull_enabled_) {
        switch (rs(cmd, RS_CULLMODE, 1)) {
        case 2:
            cull = MTLCullModeFront;
            break; // D3DCULL_CW
        case 3:
            cull = MTLCullModeBack;
            break; // D3DCULL_CCW
        default:
            cull = MTLCullModeNone;
            break;
        }
    }
    [enc setCullMode:cull];

    // --- viewport ---
    bool pretransformed = cmd->vertex_type == VT_TLVERTEX;
    double vx = cmd->viewport[0], vy = cmd->viewport[1];
    double vw = cmd->viewport[2], vh = cmd->viewport[3];
    if (vw <= 0 || vh <= 0) {
        vx = vy = 0;
        vw = guest_width_;
        vh = guest_height_;
    }
    vx *= (double)width_ / domain;
    vw *= (double)width_ / domain;
    vy *= (double)height_ / guest_height_;
    vh *= (double)height_ / guest_height_;
    // A pre-transformed vertex already carries the depth the viewport would
    // have produced, so applying the range again would squeeze it twice.
    double near_z = pretransformed ? 0.0 : cmd->viewport_minz;
    double far_z = pretransformed ? 1.0 : cmd->viewport_maxz;
    if (!pretransformed && far_z <= near_z) {
        near_z = 0.0;
        far_z = 1.0;
    }
    [enc setViewport:(MTLViewport){vx, vy, vw, vh, near_z, far_z}];

    // --- uniforms ---
    Uniforms u;
    memset(&u, 0, sizeof u);
    u.pretransformed = pretransformed ? 1 : 0;
    u.pointsize = 1.0f;
    if (!pretransformed) {
        float world[16], view[16], proj[16], wv[16], mvp[16];
        if (cmd->world)
            d3d_matrix_to_metal(cmd->world, world);
        else
            identity(world);
        if (cmd->view)
            d3d_matrix_to_metal(cmd->view, view);
        else
            identity(view);
        if (cmd->projection)
            d3d_matrix_to_metal(cmd->projection, proj);
        else
            identity(proj);
        matrix_multiply(view, world, wv);
        matrix_multiply(proj, wv, mvp);
        memcpy(u.mvp, mvp, sizeof mvp);
        if (cmd->vertex_type == VT_VERTEX)
            log_once("d3d.lit", "d3d: an unlit D3DVT_VERTEX draw arrived; the command "
                                "list carries no material or light, so it draws white");
    } else {
        identity(u.mvp);
    }
    u.textured = cmd->texture_handle ? 1 : 0;
    u.texblend = rs(cmd, RS_TEXTUREMAPBLEND, 2);
    u.alphatest = rs_raw(cmd, RS_ALPHATESTENABLE) ? 1 : 0;
    u.alphafunc = rs(cmd, RS_ALPHAFUNC, 8);
    u.alpharef = (float)rs_raw(cmd, RS_ALPHAREF) / 255.0f;
    u.specular = rs_raw(cmd, RS_SPECULARENABLE) ? 1 : 0;

    // --- fog ---
    if (rs_raw(cmd, RS_FOGENABLE)) {
        uint32_t table = rs_raw(cmd, RS_FOGTABLEMODE);
        // D3DFOG_NONE means the fog factor rides in the specular alpha, which
        // is how a device with a hardware transform is told about vertex fog.
        u.fogmode = table == 1 ? 2 : (table == 2 ? 3 : (table == 3 ? 4 : 1));
        float fr, fg, fb, fa;
        unpack(rs_raw(cmd, RS_FOGCOLOR), &fr, &fg, &fb, &fa);
        u.fogr = fr;
        u.fogg = fg;
        u.fogb = fb;
        u.fogstart = rs_value(cmd, RS_FOGSTART, 0.0f);
        u.fogend = rs_value(cmd, RS_FOGEND, 1.0f);
        u.fogdensity = rs_value(cmd, RS_FOGDENSITY, 1.0f);
    }

    // --- texture ---
    id<MTLTexture> texture = white_;
    bool smallOpaqueTile = false;
    if (cmd->texture_handle) {
        // The revision this draw named, and only that one, when it named one.
        // Falling back to the current revision for a named-but-missing one
        // would sample pixels from the guest's future, which is the whole
        // defect the revisions exist to prevent.
        uint32_t want = revision;
        if (!want) {
            auto cur = texture_current_->find(cmd->texture_handle);
            want = cur == texture_current_->end() ? 0u : cur->second;
        }
        auto it = textures_->find(tex_key(cmd->texture_handle, want));
        if (it != textures_->end() && it->second.texture) {
            smallOpaqueTile = it->second.smallOpaqueTile;
            texture = it->second.texture;
            u.texture_has_alpha = it->second.alpha ? 1 : 0;
            // Classic and legacy replay always sample the original image.
            // A replacement must never contaminate the CPU compatibility path.
            if (it->second.enhanced && active_slot_ >= 0 && !mods_display_classic() &&
                mods_display_textures() && !replaying_ && !overlay_rendering_ &&
                !slots_[active_slot_].materializedLayers &&
                !host_frame_legacy({slots_[active_slot_].frame})) {
                texture = it->second.enhanced->texture;
                u.texture_has_alpha = it->second.alpha || it->second.enhanced->alpha;
                ++hd_.draws;
                it->second.enhanced->lastUse = command_;
                it->second.enhanced->stamp = ++hd_.clock;
            }
        } else {
            u.textured = 0;
            log_once("d3d.notex",
                     "d3d: draw names texture handle %u, which was never "
                     "uploaded; it draws untextured",
                     cmd->texture_handle);
        }
    }
    uint32_t addr = rs(cmd, RS_TEXTUREADDRESS, 1);
    uint32_t addr_u = rs(cmd, RS_TEXTUREADDRESSU, addr);
    uint32_t addr_v = rs(cmd, RS_TEXTUREADDRESSV, addr);
    // WRAPU/WRAPV are the older, coarser control and only turn wrapping on.
    if (rs_raw(cmd, RS_WRAPU))
        addr_u = 1;
    if (rs_raw(cmd, RS_WRAPV))
        addr_v = 1;
    MTLSamplerMinMagFilter mag = rs(cmd, RS_TEXTUREMAG, 1) == 1 ? MTLSamplerMinMagFilterNearest
                                                                : MTLSamplerMinMagFilterLinear;
    MTLSamplerMinMagFilter minf;
    MTLSamplerMipFilter mip;
    min_filter(rs(cmd, RS_TEXTUREMIN, 1), &minf, &mip);
    if (texture.mipmapLevelCount <= 1)
        mip = MTLSamplerMipFilterNotMipmapped;
    // The game stores each terrain tile as a separate complete texture but
    // leaves the device's default REPEAT address mode in place. Bilinear taps
    // at UV 0/1 then pull colour from the tile's opposite edge, drawing seams
    // at native resolution. Clamp complete opaque tiles only; atlas regions,
    // explicit wrapping, repeating coordinates, sprites and legacy replay keep
    // the guest sampler. This does not blur the UI or substitute new textures.
    NSUInteger anisotropy = 1;
    if (active_slot_ >= 0 && !mods_display_classic() && !overlay_rendering_ && !replaying_ &&
        !slots_[active_slot_].materializedLayers &&
        !host_frame_legacy({slots_[active_slot_].frame}) && pretransformed && ztest && zwrite &&
        !blend_on && !u.alphatest && !u.texture_has_alpha && u.textured && addr_u == 1 &&
        addr_v == 1 && !rs_raw(cmd, RS_WRAPU) && !rs_raw(cmd, RS_WRAPV) &&
        (vertices.size() == 3 || vertices.size() == 6)) {
        bool tile = true, lo_u = false, hi_u = false, lo_v = false, hi_v = false;
        for (const auto &v : vertices) {
            tile &= (v.u == 0 || v.u == 1) && (v.v == 0 || v.v == 1) && v.a == 1;
            lo_u |= v.u == 0;
            hi_u |= v.u == 1;
            lo_v |= v.v == 0;
            hi_v |= v.v == 1;
        }
        if (tile && lo_u && hi_u && lo_v && hi_v) {
            addr_u = addr_v = 3;
            // Larger artist replacements already contain their own detail.
            if (smallOpaqueTile && texture.width <= 128 && terrain_detail_ &&
                mods_display_textures()) {
                u.terrain_detail = 1;
                ++hd_.detail_draws;
                terrain_detail_->lastUse = command_;
            }
            const int filter = mods_display_filtering();
            if (filter) {
                mag = minf = MTLSamplerMinMagFilterLinear;
                mip = texture.mipmapLevelCount > 1 ? MTLSamplerMipFilterLinear
                                                   : MTLSamplerMipFilterNotMipmapped;
                anisotropy = filter > 1 ? (1u << filter) : 1;
            }
        }
    }

    [enc setFragmentTexture:texture atIndex:0];
    [enc setFragmentTexture:u.terrain_detail ? terrain_detail_->texture : white_ atIndex:1];
    [enc setFragmentSamplerState:[self samplerU:MTLSamplerAddressModeRepeat
                                              v:MTLSamplerAddressModeRepeat
                                            mag:MTLSamplerMinMagFilterLinear
                                            min:MTLSamplerMinMagFilterLinear
                                            mip:MTLSamplerMipFilterLinear
                                     anisotropy:std::max(NSUInteger(4), anisotropy)]
                         atIndex:1];
    [enc setFragmentSamplerState:[self samplerU:address_mode(addr_u)
                                              v:address_mode(addr_v)
                                            mag:mag
                                            min:minf
                                            mip:mip
                                     anisotropy:anisotropy]
                         atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];

    [self bindVertices:vertices encoder:enc];
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    switch (host_d3d_primitive_kind(cmd)) {
    case HOST_D3D_POINTS:
        primitive = MTLPrimitiveTypePoint;
        break;
    case HOST_D3D_LINES:
        primitive = MTLPrimitiveTypeLine;
        break;
    default:
        primitive = MTLPrimitiveTypeTriangle;
        break;
    }
    [enc drawPrimitives:primitive vertexStart:0 vertexCount:vertices.size()];
    // Start native rendering while the CPU encodes the rest of the scene.
    // At most two early prefixes retain the normal attachment stores, command
    // tracking and resource ownership, then resume with load actions. Classic,
    // guest-resolution mirrors, replay and CPU overlays keep their old path.
    if (submit_draws_ && in_scene_ && !replaying_ && !overlay_rendering_ && incremental_ &&
        active_slot_ >= 0 && !mods_display_classic() && width_ > guest_width_ &&
        early_submissions_ < 2 && ++encoded_draws_ >= submit_draws_) {
        [self flush];
        encoded_draws_ = 0;
        ++early_submissions_;
        ++storage_stats_.early_submissions;
    }
}

- (id<MTLTexture>)makeTexture:(const HostD3DTexture *)t alpha:(bool *)alpha {
    auto &rgba = upload_scratch_;
    if (!t || !texture_decode_rgba(*t, rgba, alpha))
        return nil;
    if (!t->original)
        hd_.pack.capture(*t, rgba);

    // POP_HOST_TRACE_D3D=1 says what the texels actually are. A texture that
    // arrives near-black makes a black scene however the renderer shades it,
    // and that is a different fault in a different file from one that arrives
    // bright and is drawn dark. The first few are written out so they can be
    // looked at rather than only summarised.
    static int trace = -1;
    if (trace < 0)
        trace = getenv("POP_HOST_TRACE_D3D") ? 1 : 0;
    if (trace) {
        static uint32_t seen = 0;
        uint32_t peak = 0;
        uint64_t sum = 0;
        size_t n = (size_t)t->width * (size_t)t->height;
        for (size_t i = 0; i < n; ++i) {
            uint32_t v = rgba[i * 4 + 0];
            if (rgba[i * 4 + 1] > v)
                v = rgba[i * 4 + 1];
            if (rgba[i * 4 + 2] > v)
                v = rgba[i * 4 + 2];
            if (v > peak)
                peak = v;
            sum += v;
        }
        fprintf(stderr, "[host] texture %u: %dx%d %dbpp %s key=%d peak=%u mean=%u\n", t->handle,
                t->width, t->height, t->bpp, t->palette ? "palettised" : "direct", t->has_colorkey,
                peak, n ? (uint32_t)(sum / n) : 0u);
        if (seen < 8) {
            std::vector<uint8_t> rgb(n * 3);
            for (size_t i = 0; i < n; ++i) {
                rgb[i * 3 + 0] = rgba[i * 4 + 0];
                rgb[i * 3 + 1] = rgba[i * 4 + 1];
                rgb[i * 3 + 2] = rgba[i * 4 + 2];
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/texture_%03u_h%u.ppm", host_dump_dir(), seen,
                     t->handle);
            host_write_ppm(path, rgb.data(), t->width, t->height);
        }
        ++seen;
    }

    // A new texture every time, never a rewrite of the old one's bytes: draws
    // already encoded into an open command buffer still reference the old
    // texture, and Metal keeps it alive until they finish. Replacing the bytes
    // in place would change what those earlier draws sample.
    int levels = 1;
    for (int d = t->width > t->height ? t->width : t->height; d > 1; d >>= 1)
        ++levels;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:t->width
                                                          height:t->height
                                                       mipmapped:levels > 1];
    d.mipmapLevelCount = levels;
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:d];
    if (!texture)
        return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, t->width, t->height)
               mipmapLevel:0
                 withBytes:rgba.data()
               bytesPerRow:(NSUInteger)t->width * 4];
    if (levels > 1) {
        // The device advertises the mip filters, so the levels have to exist.
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit generateMipmapsForTexture:texture];
        [blit endEncoding];
        track_native_command(cb);
        [cb commit];
    }
    ++g_total_textures;
    host_stats_note_upload((uint32_t)rgba.size());
    return texture;
}

- (std::shared_ptr<HDTexture>)packTexture:(uint64_t)hash width:(int)width height:(int)height {
    auto file = hd_.pack.files.find(hash);
    if (file == hd_.pack.files.end())
        return {};
    const auto &f = file->second;
    if (width && int64_t(width) * f.height != int64_t(height) * f.width)
        return {};
    auto hit = hd_.entries.find({hash, 0});
    if (hit != hd_.entries.end()) {
        ++hd_.hits;
        hit->second->stamp = ++hd_.clock;
        return hit->second;
    }
    if (!hd_.reserve(f.bytes))
        return {};
    // File dimensions/length were validated at indexing, but revalidate at
    // use too: a pack may have been replaced between startup and level load.
    pop_hd::File fresh;
    if (!pop_hd::inspect(f.path, fresh) || fresh.hash != hash || fresh.bytes != f.bytes ||
        fresh.width != f.width || fresh.height != f.height || fresh.flags != f.flags)
        return {};
    std::ifstream input(f.path, std::ios::binary);
    input.seekg(32);
    auto descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:f.width
                                                          height:f.height
                                                       mipmapped:f.levels > 1];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    auto resource = std::make_shared<HDTexture>();
    resource->texture = [device_ newTextureWithDescriptor:descriptor];
    if (!resource->texture)
        return {};
    std::vector<uint8_t> level;
    int w = f.width, h = f.height;
    for (uint32_t i = 0; i < f.levels; ++i) {
        level.resize(size_t(w) * h * 4);
        if (!input.read(reinterpret_cast<char *>(level.data()), level.size()))
            return {};
        [resource->texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                             mipmapLevel:i
                               withBytes:level.data()
                             bytesPerRow:w * 4];
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    resource->bytes = resource->texture.allocatedSize;
    if (!hd_.reserve(resource->bytes))
        return {};
    resource->alpha = f.flags & 1;
    resource->stamp = ++hd_.clock;
    hd_.entries[{hash, 0}] = resource;
    hd_.used += resource->bytes;
    ++hd_.loads;
    host_stats_note_upload(uint32_t(f.bytes));
    return resource;
}

// Upload a validated texture revision while retaining the original and optional replacement.
// Existing leased revisions are immutable; material detail is restricted to eligible terrain.
- (void)uploadTexture:(const HostD3DTexture *)t {
    if (!t || !texture_layout_valid(*t) || (t->original && !texture_layout_valid(*t->original)))
        return;
    auto existing = textures_->find(tex_key(t->handle, t->revision));
    if (existing != textures_->end() && (t->revision || existing->second.leases))
        return;
    bool alpha = false;
    id<MTLTexture> base = [self makeTexture:t->original ? t->original : t alpha:&alpha];
    if (!base)
        return;
    TexEntry &e = (*textures_)[tex_key(t->handle, t->revision)];
    e.texture = base;
    e.alpha =
        (t->original ? t->original : t)->amask || (t->original ? t->original : t)->has_colorkey;
    e.enhanced.reset();
    const auto &source = t->original ? *t->original : *t;
    // Original landscape cache entries are complete opaque 16/32-square
    // RGB565 tiles. The draw gate additionally requires depth-writing world
    // triangles with full 0..1 UVs: skies, atlases and sprites are excluded.
    // A provider owns its artwork; do not add host detail on top of it.
    e.smallOpaqueTile = !t->original && !e.alpha && source.width == source.height &&
                        (source.width == 16 || source.width == 32) && source.bpp == 16 &&
                        source.rmask == 0xf800 && source.gmask == 0x07e0 && source.bmask == 0x001f;
    if (t->original) {
        const uint64_t bytes =
            pop_hd::mip_bytes(t->width, t->height, pop_hd::mip_levels(t->width, t->height));
        if (hd_.reserve(bytes)) {
            auto resource = std::make_shared<HDTexture>();
            resource->texture = [self makeTexture:t alpha:&resource->alpha];
            if (resource->texture) {
                resource->bytes = resource->texture.allocatedSize;
                if (!hd_.reserve(resource->bytes))
                    resource->texture = nil;
                resource->stamp = ++hd_.clock;
                // Provider revisions are independent; never cache them under
                // the original content hash a mod may reinterpret next time.
                const auto key = std::make_pair(tex_key(t->handle, t->revision), ++hd_.clock);
                if (resource->texture) {
                    hd_.entries[key] = resource;
                    hd_.used += resource->bytes;
                    ++hd_.loads;
                    e.enhanced = resource;
                }
            }
        }
    } else if (t->content_hash)
        e.enhanced = [self packTexture:t->content_hash width:t->width height:t->height];
    uint32_t was = 0;
    auto cur = texture_current_->find(t->handle);
    if (cur != texture_current_->end())
        was = cur->second;
    (*texture_current_)[t->handle] = t->revision;
    // The revision this one replaces goes as soon as nothing holds it. Every
    // upload would otherwise add a texture the renderer never drops.
    if (was != t->revision)
        [self dropTexture:t->handle revision:was];
}

// Drops one (handle, revision) if it is neither current nor leased.
- (void)dropTexture:(uint32_t)handle revision:(uint32_t)revision {
    auto it = textures_->find(tex_key(handle, revision));
    if (it == textures_->end())
        return;
    if (it->second.leases)
        return;
    auto cur = texture_current_->find(handle);
    if (cur != texture_current_->end() && cur->second == revision)
        return;
    textures_->erase(it);
}

- (void)destroyTexture:(uint32_t)handle {
    // The handle is gone, but a frame still holding one of its revisions is
    // not: those stay until the frame retires, and only then are dropped.
    texture_current_->erase(handle);
    for (auto it = textures_->begin(); it != textures_->end();) {
        if ((uint32_t)(it->first >> 32) == handle && !it->second.leases)
            it = textures_->erase(it);
        else
            ++it;
    }
}

- (BOOL)retainTexture:(uint32_t)handle revision:(uint32_t)revision {
    if (!handle)
        return YES;
    auto it = textures_->find(tex_key(handle, revision));
    // NO means "I never received that revision". The shim uploads and asks
    // again rather than leaving a draw naming pixels the renderer does not
    // have: a silent no-op here drew untextured where the pre-revision code
    // drew the handle's contents.
    if (it == textures_->end())
        return NO;
    ++it->second.leases;
    return YES;
}

- (void)releaseTexture:(uint32_t)handle revision:(uint32_t)revision {
    if (!handle)
        return;
    auto it = textures_->find(tex_key(handle, revision));
    if (it == textures_->end())
        return;
    if (it->second.leases)
        --it->second.leases;
    if (!it->second.leases)
        [self dropTexture:handle revision:revision];
}

- (BOOL)hasTexture:(uint32_t)handle revision:(uint32_t)revision {
    auto it = textures_->find(tex_key(handle, revision));
    return it != textures_->end() && it->second.texture != nil;
}

- (void)forgetTexturesForTest {
    textures_->clear();
    texture_current_->clear();
}

// Look up the layout mapping recorded for a draw within its sealed frame.
// Unclassified draws use the scene mapping.
- (HostDrawMapping)mappingForFrame:(uint64_t)f seq:(uint32_t)seq {
    for (auto &slot : slots_)
        if (slot.frame == f)
            for (auto &step : slot.replay.steps)
                if (step.draw && step.seq == seq)
                    return step.mapping;
    return HOST_MAPPING_SCENE;
}
- (void)replayBarrier:(const HostD3DSurface *)surface generation:(uint32_t)g seq:(uint32_t)seq {
    int index = [self slotFor:surface->id generation:g];
    if (index < 0 || !slots_[index].frame || replaying_)
        return;
    ReplayStep step;
    step.seq = seq;
    step.barrier = true;
    slots_[index].replay.steps.push_back(std::move(step));
}

// Preserve the entry state before this frame's first GPU/CPU operation.
- (void)captureReplaySeed {
    if (active_slot_ < 0)
        return;
    auto &j = slots_[active_slot_].replay;
    if (!j.seed.empty())
        return;
    j.seed.resize((size_t)target_pitch_ * guest_height_);
    memcpy(j.seed.data(), target_pixels_, j.seed.size());
    j.hasPalette = target_has_palette_;
    memcpy(j.palette, target_palette_, sizeof j.palette);
    j.pendingCheckpoint = !needs_upload_;
}
- (void)captureReplayCheckpoint:(BOOL)needed {
    if (active_slot_ < 0)
        return;
    auto &j = slots_[active_slot_].replay;
    if (!j.pendingCheckpoint)
        return;
    j.pendingCheckpoint = false;
    // A full colour+depth Clear supersedes every byte of the predecessor.
    // Most gameplay frames start here and need no GPU checkpoint at all.
    if (!needed)
        return;
    [self flush];
    j.width = width_;
    j.height = height_;
    if (!command_)
        command_ = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
    id<MTLTexture> textures[] = {color_, depth_, coverage_};
    for (int k = 0; k < 3; ++k) {
        j.pitch[k] = ((NSUInteger)width_ * (k == 2 ? 1 : 4) + 255) & ~(NSUInteger)255;
        j.checkpoint[k] = [device_ newBufferWithLength:j.pitch[k] * height_
                                               options:MTLResourceStorageModePrivate];
        if (!j.checkpoint[k]) {
            fprintf(stderr, "[host] replay checkpoint allocation failed\n");
            abort();
        }
        [blit copyFromTexture:textures[k]
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width_, height_, 1)
                            toBuffer:j.checkpoint[k]
                   destinationOffset:0
              destinationBytesPerRow:j.pitch[k]
            destinationBytesPerImage:j.pitch[k] * height_];
    }
    [blit endEncoding];
}

// This runs on the guest thread at seal, before the frame can be claimed. It
// restarts from the entry seed and merges records/Clear/primitives by seq into
// a guest-resolution target. No presenter or AppKit service is needed.
- (BOOL)replayLegacyFrame:(uint64_t)frame {
    [self flush];
    [self saveSlot];
    int previous = active_slot_;
    bool found = false;
    uint32_t draws = g_total_draws, since = g_draws_since_present;
    for (int index = 0; index < 4; ++index) {
        SceneSlot &slot = slots_[index];
        if (slot.frame != frame)
            continue;
        found = true;
        auto &j = slot.replay;
        if (j.done)
            continue;
        if (j.seed.empty())
            return NO;
        [self loadSlot:index];
        [self captureReplayCheckpoint:YES];
        [self flush];
        [self saveSlot];
        id<MTLBuffer> entry[3] = {nil, nil, nil};
        id<MTLCommandBuffer> ready = slot.last;
        if (j.checkpoint[0]) {
            ready = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [ready blitCommandEncoder];
            for (int k = 0; k < 3; ++k) {
                entry[k] = [device_ newBufferWithLength:j.checkpoint[k].length
                                                options:MTLResourceStorageModeShared];
                if (!entry[k]) {
                    fprintf(stderr, "[host] legacy readback allocation failed\n");
                    abort();
                }
                [blit copyFromBuffer:j.checkpoint[k]
                         sourceOffset:0
                             toBuffer:entry[k]
                    destinationOffset:0
                                 size:j.checkpoint[k].length];
            }
            [blit endEncoding];
            track_native_command(ready);
            [ready commit];
        }
        assert(!host_d3d_appkit_pending());
        [ready waitUntilCompleted];
        if (ready.status == MTLCommandBufferStatusError)
            return NO;
        HostD3DSurface actual = slot.surface;
        std::vector<uint8_t> pixels = j.seed;
        target_pixels_ = pixels.data();
        target_has_palette_ = j.hasPalette;
        memcpy(target_palette_, j.palette, sizeof target_palette_);
        index_cache_valid_ = false;
        // Discard the speculative native result only after its GPU work has
        // completed. The four frame slots and their lifetime do not change.
        color_ = nil;
        [self allocateMirror:guest_width_ height:guest_height_];
        pending_id_ = target_id_;
        replaying_ = true;
        if (j.checkpoint[0]) {
            // Guest-centre sampling agrees with the coherence downsample.
            [self uploadSurface];
            pending_clear_depth_ = false;
            coverage_reset_pending_ = false;
            [self endEncoding];
            if (!command_)
                command_ = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
            id<MTLTexture> textures[] = {color_, depth_, coverage_};
            for (int k = 0; k < 3; ++k) {
                size_t bpp = k == 2 ? 1 : 4, pitch = ((size_t)width_ * bpp + 255) & ~(size_t)255;
                std::vector<uint8_t> data(pitch * height_);
                auto *from = (const uint8_t *)entry[k].contents;
                for (int y = 0; y < height_; ++y)
                    for (int x = 0; x < width_; ++x) {
                        int sx = (int)((int64_t)(2 * x + 1) * j.width / (2 * width_));
                        int sy = (int)((int64_t)(2 * y + 1) * j.height / (2 * height_));
                        memcpy(data.data() + y * pitch + x * bpp, from + sy * j.pitch[k] + sx * bpp,
                               bpp);
                    }
                id<MTLBuffer> buffer = [self argumentBytes:data.data() length:data.size()];
                [blit copyFromBuffer:buffer
                           sourceOffset:0
                      sourceBytesPerRow:pitch
                    sourceBytesPerImage:data.size()
                             sourceSize:MTLSizeMake(width_, height_, 1)
                              toTexture:textures[k]
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
            [blit endEncoding];
            needs_upload_ = false;
            gpu_dirty_ = true;
            [self legacyWriteBackPending];
        }
        std::stable_sort(j.steps.begin(), j.steps.end(),
                         [](const ReplayStep &a, const ReplayStep &b) { return a.seq < b.seq; });
        for (auto &step : j.steps) {
            if (step.barrier) {
                [self legacyWriteBackPending];
                continue;
            }
            if (step.draw) {
                target_has_palette_ = step.hasPalette;
                if (memcmp(target_palette_, step.palette, sizeof target_palette_))
                    index_cache_valid_ = false;
                memcpy(target_palette_, step.palette, sizeof target_palette_);
                [self drawSnapshot:&step.snapshot];
                continue;
            }
            [self legacyWriteBackPending];
            target_has_palette_ = step.hasPalette;
            memcpy(target_palette_, step.palette, sizeof target_palette_);
            index_cache_valid_ = false;
            auto r = step.record;
            r.cpu_pixels = step.pixels.data();
            r.coverage = step.coverage.data();
            int outBytes = texture_storage_bytes(target_bpp_),
                inBytes = texture_storage_bytes(r.cpu_bpp);
            int rs = 0, gs = 0, bs = 0;
            uint32_t rm = 0, gm = 0, bm = 0;
            mask_shape(target_rmask_, &rs, &rm);
            mask_shape(target_gmask_, &gs, &gm);
            mask_shape(target_bmask_, &bs, &bm);
            if (target_bpp_ == 8 && target_has_palette_ && !index_cache_valid_)
                [self rebuildIndexCache];
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x) {
                    int dx = r.dst_x + x, dy = r.dst_y + y;
                    if (!r.coverage[y * r.w + x] || dx < 0 || dy < 0 || dx >= guest_width_ ||
                        dy >= guest_height_)
                        continue;
                    const uint8_t *src = r.cpu_pixels + y * r.cpu_pitch + x * inBytes;
                    uint8_t *out = pixels.data() + dy * target_pitch_ + dx * outBytes;
                    if (inBytes == outBytes) {
                        memcpy(out, src, outBytes);
                        continue;
                    }
                    uint32_t red, green, blue;
                    if (inBytes == 1) {
                        uint32_t c =
                            step.hasPalette ? step.palette[*src] : (uint32_t)*src * 0x010101u;
                        red = (c >> 16) & 255;
                        green = (c >> 8) & 255;
                        blue = c & 255;
                    } else {
                        uint32_t raw = texture_raw(src, 0, r.cpu_bpp);
                        if (inBytes == 4) {
                            red = (raw >> 16) & 255;
                            green = (raw >> 8) & 255;
                            blue = raw & 255;
                        } else {
                            red = ((raw >> 11) & 31) * 255 / 31;
                            green = ((raw >> 5) & 63) * 255 / 63;
                            blue = (raw & 31) * 255 / 31;
                        }
                    }
                    if (outBytes == 1) {
                        if (!target_has_palette_) {
                            fprintf(stderr,
                                    "[host] legacy CPU payload needs a destination palette\n");
                            abort();
                        }
                        *out =
                            (*index_cache_)[((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3)];
                    } else {
                        uint32_t raw = rm ? ((red * rm / 255) << rs) | ((green * gm / 255) << gs) |
                                                ((blue * bm / 255) << bs)
                                          : ((red * 31 / 255) << 11) | ((green * 63 / 255) << 5) |
                                                (blue * 31 / 255);
                        if (!rm && outBytes == 4)
                            raw = (red << 16) | (green << 8) | blue;
                        for (int k = 0; k < outBytes; ++k)
                            out[k] = uint8_t(raw >> (k * 8));
                    }
                }
            needs_upload_ = true;
            coverage_reset_pending_ = true;
        }
        [self legacyWriteBackPending];
        // Publish the quantized legacy picture, including final CPU-only
        // writes, not the unquantized colour left by the last primitive.
        [self uploadSurface];
        needs_upload_ = false;
        [self flush];
        assert(!host_d3d_appkit_pending());
        [last_command_ waitUntilCompleted];
        if (last_command_.status == MTLCommandBufferStatusError) {
            replaying_ = false;
            return NO;
        }
        memcpy(actual.pixels, pixels.data(), pixels.size());
        target_pixels_ = actual.pixels;
        gpu_dirty_ = false;
        pending_id_ = 0;
        host_d3d_clean_pixels(actual.id, slot.generation, {0, 0, actual.width, actual.height});
        replaying_ = false;
        j.done = true;
        [self saveSlot];
    }
    g_total_draws = draws;
    g_draws_since_present = since; // replay is not another guest frame
    if (previous >= 0)
        [self loadSlot:previous];
    return found;
}

// Replay an immutable draw snapshot with its captured transforms and texture revision.
// Incremental rendering also records the operation so CPU/GPU barriers can rebuild ordering.
- (void)drawSnapshot:(const HostD3DDrawSnapshot *)d {
    if (!d || (frame_dropped_ && !replaying_))
        return;
    if (incremental_ && active_slot_ >= 0 && !replaying_) {
        [self captureReplayCheckpoint:!(d->kind == HOST_DRAW_CLEAR && (d->clear_flags & 3) == 3 &&
                                        !d->clear_rect_count)];
        ReplayStep step;
        step.seq = d->seq;
        step.draw = true;
        step.snapshot = *d;
        step.mapping = host_frame_draw_mapping({slots_[active_slot_].frame}, d->seq);
        step.hasPalette = target_has_palette_;
        memcpy(step.palette, target_palette_, sizeof step.palette);
        slots_[active_slot_].replay.done = false;
        slots_[active_slot_].replay.steps.push_back(std::move(step));
    }
    if (d->kind == HOST_DRAW_CLEAR) {
        [self clearFlags:d->clear_flags
                   rects:d->clear_rects
                   count:d->clear_rect_count
                   color:d->clear_color
                   depth:d->clear_z];
        return;
    }
    HostD3DDraw cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.primitive_type = d->primitive_type;
    cmd.vertex_type = d->fvf;
    cmd.vertex_stride = d->vertex_stride;
    cmd.vertex_count = d->vertex_count;
    cmd.vertices = d->vertices;
    cmd.indices = d->indices;
    cmd.index_count = d->index_count;
    cmd.texture_handle = d->texture_handle;
    cmd.render_state = d->state.render_state;
    cmd.render_state_count = HOST_D3D_RENDERSTATE_MAX;
    // Indexed by D3DTRANSFORMSTATE, which is what the snapshot stores: WORLD
    // is 1, VIEW 2, PROJECTION 3, and slot 0 is never used. Reading 0, 1, 2
    // here gave every untransformed draw an absent world, the world matrix as
    // its view and the view as its projection - a wrong MVP on every one of
    // them, and invisible to the dx suite because its test double indexes by
    // the enum.
    cmd.world = d->state.transform_set[HOST_D3D_TRANSFORM_WORLD]
                    ? d->state.transform[HOST_D3D_TRANSFORM_WORLD]
                    : nullptr;
    cmd.view = d->state.transform_set[HOST_D3D_TRANSFORM_VIEW]
                   ? d->state.transform[HOST_D3D_TRANSFORM_VIEW]
                   : nullptr;
    cmd.projection = d->state.transform_set[HOST_D3D_TRANSFORM_PROJECTION]
                         ? d->state.transform[HOST_D3D_TRANSFORM_PROJECTION]
                         : nullptr;
    for (int i = 0; i < 4; ++i)
        cmd.viewport[i] = d->state.viewport[i];
    cmd.viewport_minz = d->state.viewport_minz;
    cmd.viewport_maxz = d->state.viewport_maxz;
    bool overlay =
        !mods_display_classic() && active_slot_ >= 0 && !replaying_ &&
        slots_[active_slot_].presentTarget.overlay && !slots_[active_slot_].materializedLayers &&
        host_frame_draw_mapping({slots_[active_slot_].frame}, d->seq) == HOST_MAPPING_UI &&
        !host_frame_legacy({slots_[active_slot_].frame});
    if (overlay) {
        slots_[active_slot_].separateLayers = true;
        [self endEncoding];
        auto &slot = slots_[active_slot_];
        id<MTLTexture> world = color_, depth = depth_, coverage = coverage_;
        int w = width_, h = height_;
        bool upload = needs_upload_, cc = pending_clear_color_, cd = pending_clear_depth_,
             cr = coverage_reset_pending_;
        color_ = present_texture(slot.presentTarget.overlay);
        depth_ = slot.overlayDepth;
        coverage_ = slot.overlayCoverage;
        width_ = (int)color_.width;
        height_ = (int)color_.height;
        needs_upload_ = pending_clear_color_ = pending_clear_depth_ = coverage_reset_pending_ =
            false;
        overlay_rendering_ = true;
        [self draw:&cmd revision:d->texture_revision];
        [self endEncoding];
        overlay_rendering_ = false;
        color_ = world;
        depth_ = depth;
        coverage_ = coverage;
        width_ = w;
        height_ = h;
        needs_upload_ = upload;
        pending_clear_color_ = cc;
        pending_clear_depth_ = cd;
        coverage_reset_pending_ = cr;
    } else
        [self draw:&cmd revision:d->texture_revision];
}

- (BOOL)readPixels:(void *)out width:(int *)width height:(int *)height {
    [self flush];
    if (!color_ || !staging_ || !out)
        return NO;
    if (width)
        *width = width_;
    if (height)
        *height = height_;

    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:color_
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width_, height_, 1)
                toTexture:staging_
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    track_native_command(cb);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError)
        return NO;
    [staging_ getBytes:out
           bytesPerRow:(NSUInteger)width_ * 4
            fromRegion:MTLRegionMake2D(0, 0, width_, height_)
           mipmapLevel:0];
    size_t n = (size_t)width_ * height_;
    note_scene_nonblack(count_nonblack((const uint8_t *)out, n), n);
    return YES;
}
@end

// ---------------------------------------------------------------------------
// The shim callbacks. Strong definitions that displace the weak no-ops in
// src/recomp/dx/host_api.cpp.
// ---------------------------------------------------------------------------
extern "C" void host_d3d_begin_scene(void) {
    [[PopD3DRenderer shared] beginScene];
}
extern "C" void host_d3d_end_scene(void) {
    [[PopD3DRenderer shared] endScene];
}
// The snapshot is the frame's own copy of the draw: every pointer in it is
// arena-owned and outlives the guest call that made it. The renderer still
// reads the older command shape, so this is a view over those copies - not a
// second copy, and not a guest pointer.
extern "C" void host_d3d_draw(const HostD3DDrawSnapshot *d) {
    [[PopD3DRenderer shared] drawSnapshot:d];
}

extern "C" int host_d3d_texture_retain(uint32_t handle, uint32_t revision) {
    return [[PopD3DRenderer shared] retainTexture:handle revision:revision] ? 1 : 0;
}
extern "C" void host_d3d_texture_release(uint32_t handle, uint32_t revision) {
    [[PopD3DRenderer shared] releaseTexture:handle revision:revision];
}
extern "C" int host_render_texture_revision_alive_for_test(uint32_t handle, uint32_t revision) {
    return [[PopD3DRenderer shared] hasTexture:handle revision:revision] ? 1 : 0;
}
extern "C" void host_render_reset_for_test(void) {
    [[PopD3DRenderer shared] forgetTexturesForTest];
}
extern "C" void host_d3d_clear(uint32_t flags, const int32_t *rects, uint32_t count, uint32_t color,
                               float depth) {
    [[PopD3DRenderer shared] clearFlags:flags rects:rects count:count color:color depth:depth];
}
extern "C" void host_d3d_set_render_target(const HostD3DSurface *target) {
    [[PopD3DRenderer shared] setRenderTarget:target];
}
extern "C" void host_d3d_flush_surface(const HostD3DSurface *surface, const char *why) {
    if (host_d3d_legacy_writeback())
        [[PopD3DRenderer shared] flushSurface:surface why:why];
}
extern "C" void host_d3d_discard(void) {
    [[PopD3DRenderer shared] discard];
}
extern "C" void host_d3d_texture(const HostD3DTexture *tex) {
    [[PopD3DRenderer shared] uploadTexture:tex];
}
extern "C" void host_d3d_texture_destroyed(uint32_t handle) {
    [[PopD3DRenderer shared] destroyTexture:handle];
}

void host_d3d_seal_commands(void) {
    [[PopD3DRenderer shared] sealCommands];
}
extern "C" void host_d3d_bind_generation(const HostD3DSurface *s, uint32_t g, uint64_t f) {
    if (host_d3d_legacy_writeback()) {
        [[PopD3DRenderer shared] setRenderTarget:s];
        return;
    }
    [[PopD3DRenderer shared] bindSurface:s generation:g frame:f];
}
extern "C" int host_d3d_readback_rects(const HostD3DSurface *s, uint32_t g, const HostDirtyRect *r,
                                       uint32_t n) {
    if (host_d3d_legacy_writeback())
        return 1; // old flush already serviced it
    return [[PopD3DRenderer shared] coherentSurface:s generation:g rects:r count:n];
}
extern "C" void host_d3d_apply_cpu(const HostD3DSurface *s, const HostBlitRecord *r) {
    if (!host_d3d_legacy_writeback())
        [[PopD3DRenderer shared] applyCPU:s record:r];
}
extern "C" void host_d3d_swap_generations(uint32_t a, uint32_t ag, uint32_t b, uint32_t bg) {
    [[PopD3DRenderer shared] swapSurface:a generation:ag other:b generation:bg];
}
extern "C" void host_d3d_seal_frame(uint64_t f) {
    [[PopD3DRenderer shared] sealFrame:f];
}
extern "C" void host_d3d_retire_frame(uint64_t f) {
    [[PopD3DRenderer shared] retireFrame:f];
}

extern "C" void host_d3d_note_readback(HostReadReason reason) {
    const HostReadbackReason reasons[] = {HOST_READBACK_LOCK,        HOST_READBACK_LOCK,
                                          HOST_READBACK_GETDC,       HOST_READBACK_BLT_SOURCE,
                                          HOST_READBACK_DSTKEY,      HOST_READBACK_DUPLICATE,
                                          HOST_READBACK_TEXTURE_LOAD};
    if (reason >= 0 && reason < HOST_READ_REASON_COUNT)
        host_stats_note_readback(reasons[reason]);
}

extern "C" void host_d3d_prepare_cpu_write(const HostD3DSurface *s, uint32_t g, uint64_t f) {
    PopD3DRenderer *renderer = [PopD3DRenderer shared];
    if (!host_d3d_legacy_writeback() && renderer && [renderer slotFor:s->id generation:g] >= 0)
        [renderer bindSurface:s generation:g frame:f];
}

extern "C" int host_render_legacy_frame_for_test(HostFrameHandle f) {
    return [[PopD3DRenderer shared] replayLegacyFrame:f.id];
}

extern "C" void host_d3d_replay_barrier(const HostD3DSurface *s, uint32_t generation,
                                        uint32_t seq) {
    [[PopD3DRenderer shared] replayBarrier:s generation:generation seq:seq];
}
extern "C" HostDrawMapping host_render_draw_mapping_for_test(HostFrameHandle f, uint32_t seq) {
    return [[PopD3DRenderer shared] mappingForFrame:f.id seq:seq];
}

extern "C" int host_d3d_accepts_draw() {
    return ![PopD3DRenderer shared] || [[PopD3DRenderer shared] acceptsDraw];
}
void host_d3d_collect_present_targets() {
    [[PopD3DRenderer shared] collectCleanTargets];
}
