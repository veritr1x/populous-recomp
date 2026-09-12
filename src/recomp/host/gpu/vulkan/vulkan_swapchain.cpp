#include "vulkan_device.h"

namespace gpu {
void *vulkan_test_native_surface(int, int) {
    return nullptr; // Task 5
}
void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}
} // namespace gpu
