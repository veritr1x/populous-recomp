// metal_bridge.h - TEMPORARY. While the renderer is still Metal and the window
// is still AppKit (plan Tasks 4-6), they exchange native objects with the
// portable layers through these calls. Deleted in Task 7.
#pragma once
#import <Metal/Metal.h>
#include "../gpu.h"

namespace gpu::metal {
// Registers a foreign texture; destroy() on the handle drops our reference only.
gpu::Texture import_texture(gpu::Device *d, id<MTLTexture> t);
id<MTLTexture> export_texture(gpu::Device *d, gpu::Texture t); // nil for unknown ids
id<MTLCommandBuffer> export_command(gpu::Device *d, gpu::CommandBuffer cb);
// Lets portable code encode into a renderer-owned, uncommitted command buffer.
// The caller commits it itself and calls forget_command afterwards.
gpu::CommandBuffer import_command(gpu::Device *d, id<MTLCommandBuffer> cb);
void forget_command(gpu::Device *d, gpu::CommandBuffer cb);
id<MTLCommandQueue> queue(gpu::Device *d);
id<MTLDevice> device(gpu::Device *d);
} // namespace gpu::metal
