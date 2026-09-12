#pragma once
#include "frame_pacing.h"
#import <Metal/Metal.h>
#import <CoreText/CoreText.h>
#include <cstdio>

// Presenter-thread only. Small immutable texture refreshed four times a
// second; drawn after the game copy, never into cached/guest game pixels.
class PerformanceOverlay {
    id<MTLTexture> texture = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    double refreshed = -1;
    int last_mode = 0;
    static constexpr int width = 330, height = 136;
    // Rasterize a pacing snapshot into the overlay texture. The graph uses a fixed 50 ms
    // scale and marks samples exceeding the selected frame budget.
    void update(id<MTLDevice> device, const FramePacingSnapshot &s, int mode, int limit) {
        const int used = mode == 2 ? height : 88;
        std::vector<uint8_t> pixels(width * used * 4);
        auto cs = CGColorSpaceCreateDeviceRGB();
        auto ctx = CGBitmapContextCreate(pixels.data(), width, used, 8, width * 4, cs,
                                         uint32_t(kCGImageAlphaPremultipliedLast) |
                                             uint32_t(kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(cs);
        if (!ctx)
            return;
        CGContextSetRGBFillColor(ctx, .025, .035, .06, .88);
        CGContextFillRect(ctx, CGRectMake(0, 0, width, mode == 2 ? height : 88));
        auto font = CTFontCreateWithName(CFSTR("Menlo"), 11, nullptr);
        auto text = [&](int y, const char *str, double r, double g, double b) {
            auto string = CFStringCreateWithCString(nullptr, str, kCFStringEncodingUTF8);
            CGFloat components[] = {CGFloat(r), CGFloat(g), CGFloat(b), 1};
            auto space = CGColorSpaceCreateDeviceRGB();
            auto color = CGColorCreate(space, components);
            const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
            const void *values[] = {font, color};
            auto attrs =
                CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
            auto attributed = CFAttributedStringCreate(nullptr, string, attrs);
            auto line = CTLineCreateWithAttributedString(attributed);
            CGContextSetTextPosition(ctx, 10, y);
            CTLineDraw(line, ctx);
            CFRelease(line);
            CFRelease(attributed);
            CFRelease(attrs);
            CGColorRelease(color);
            CGColorSpaceRelease(space);
            CFRelease(string);
        };
        const int top = mode == 2 ? height : 88;
        char line[128];
        snprintf(line, sizeof line, "NEW %5.1f FPS   DISPLAY %5.1f", s.new_fps, s.display_fps);
        text(top - 19, line, .45, 1, .77);
        snprintf(line, sizeof line, "FRAME %5.1f ms   P95 %5.1f ms", s.median_ms, s.p95_ms);
        text(top - 36, line, .92, .95, 1);
        snprintf(line, sizeof line, "COMPOSE GPU %4.1f ms   AGE %4.1f ms", s.gpu_ms, s.age_ms);
        text(top - 53, line, .8, .85, .93);
        char cap[24];
        if (limit)
            snprintf(cap, sizeof cap, "CAP %d", limit);
        else
            snprintf(cap, sizeof cap, "ORIGINAL");
        snprintf(line, sizeof line, "REPEAT %3.0f%%  DROP %llu  %s", s.repeat_percent,
                 (unsigned long long)s.drops, cap);
        text(top - 70, line, .8, .85, .93);
        if (mode == 2) {
            // Fixed 0..50ms axis. A red bar means a missed configured budget.
            const double budget = 1000.0 / (limit ? limit : 60);
            CGContextSetRGBFillColor(ctx, .2, .28, .36, 1);
            CGContextFillRect(ctx, CGRectMake(10, 12, width - 20, 1));
            CGContextSetRGBFillColor(ctx, .4, .48, .55, .8);
            CGContextFillRect(ctx, CGRectMake(10, 12 + std::min(50.0, budget) * .7, width - 20, 1));
            for (size_t i = 0; i < s.intervals_ms.size(); ++i) {
                double ms = s.intervals_ms[i];
                const bool late = ms > budget * 1.2;
                CGContextSetRGBFillColor(ctx, late ? 1 : .3, late ? .4 : .85, late ? .3 : 1, 1);
                CGContextFillRect(ctx, CGRectMake(10 + i * 2.55, 13, 2, std::min(50.0, ms) * .7));
            }
        }
        CFRelease(font);
        CGContextRelease(ctx);
        // CGBitmapContext storage already has the row order sampled by Metal.
        auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                    width:width
                                                                   height:used
                                                                mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead;
        texture = [device newTextureWithDescriptor:d];
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, used)
                   mipmapLevel:0
                     withBytes:pixels.data()
                   bytesPerRow:width * 4];
    }

  public:
    void draw(id<MTLTexture> target, id<MTLCommandBuffer> cb, const FramePacingSnapshot &s,
              double now, int mode, int limit) {
        if (!mode || !target || !cb)
            return;
        if (!pipeline || format != target.pixelFormat) {
            NSString *source =
                @"#include <metal_stdlib>\nusing namespace metal; struct V{float4 p "
                @"[[position]];float2 uv;}; vertex V hud_v(uint i [[vertex_id]],constant float4& r "
                @"[[buffer(0)]]) {float2 q=float2(i&1,i>>1);V "
                @"v;v.p=float4(r.xy+q*r.zw,0,1);v.uv=q;return v;} fragment float4 hud_f(V v "
                @"[[stage_in]],texture2d<float> t [[texture(0)]]) {constexpr sampler "
                @"s(filter::linear);return t.sample(s,v.uv);}";
            NSError *error = nil;
            auto lib = [target.device newLibraryWithSource:source options:nil error:&error];
            auto desc = [MTLRenderPipelineDescriptor new];
            desc.vertexFunction = [lib newFunctionWithName:@"hud_v"];
            desc.fragmentFunction = [lib newFunctionWithName:@"hud_f"];
            auto a = desc.colorAttachments[0];
            a.pixelFormat = target.pixelFormat;
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOne;
            a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
            pipeline = [target.device newRenderPipelineStateWithDescriptor:desc error:&error];
            format = target.pixelFormat;
            if (!pipeline) {
                fprintf(stderr, "performance overlay: %s\n", error.localizedDescription.UTF8String);
                return;
            }
        }
        if (!texture || mode != last_mode || now - refreshed >= .25) {
            update(target.device, s, mode, limit);
            refreshed = now;
            last_mode = mode;
        }
        if (!texture)
            return;
        const double scale = std::clamp(double(target.height) / 900.0, 1.0, 3.0);
        const double w = std::min(double(target.width) - 20, width * scale),
                     h = texture.height * (w / width);
        float rect[] = {float(1 - 2 * (w + 10) / target.width), float(1 - 20.0 / target.height),
                        float(2 * w / target.width), float(-2 * h / target.height)};
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        auto enc = [cb renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:pipeline];
        [enc setVertexBytes:rect length:sizeof rect atIndex:0];
        [enc setFragmentTexture:texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
    }
};
