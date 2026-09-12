// vulkan_device.h - gpu::Device over Vulkan. One queue; a reaper thread
// retires fences in submission order so command buffers complete in commit
// order. Included only by files under gpu/vulkan/.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../../../../../third_party/volk/volk.h"
#include "../gpu.h"

namespace gpu {
bool vulkan_load();                    // volk initialised; false when no loader exists
const char *vulkan_loader_path_impl(); // path dlopen'ed, or nullptr for the default
} // namespace gpu
