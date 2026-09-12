// gpu_factory.h - the one place that knows which backend this platform has.
#pragma once
#include "gpu.h"

#include <memory>

namespace gpu {
// Metal on Apple platforms; nullptr where no backend exists yet.
std::unique_ptr<Device> create_default_device();
const char *default_backend_name();
// A native surface create_swapchain() accepts, for tests that need one without
// a window: a CAMetalLayer on Apple, nullptr elsewhere.
void *test_native_surface(int w, int h);
} // namespace gpu
