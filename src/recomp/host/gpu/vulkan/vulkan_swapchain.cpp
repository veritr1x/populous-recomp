// vulkan_swapchain.cpp - Task 5: surface, swapchain, acquire and present.
#include "vulkan_device.h"

namespace gpu {

Swapchain VulkanDevice::create_swapchain(void *, int, int) {
    return {};
}
void VulkanDevice::resize(Swapchain, int, int) {}
Format VulkanDevice::swapchain_format(Swapchain) {
    return Format::BGRA8;
}
Texture VulkanDevice::acquire(Swapchain) {
    return {};
}
void VulkanDevice::release_drawable(Swapchain, Texture) {}
void VulkanDevice::present(CommandBuffer, Swapchain, Texture, double, std::function<void(double)>) {
}
double VulkanDevice::refresh_period(Swapchain) {
    return 1.0 / 60;
}
void VulkanDevice::destroy(Swapchain) {}
void VulkanDevice::queue_present(Cmd &) {}

void *vulkan_test_native_surface(int, int) {
    return nullptr;
}
void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}

} // namespace gpu
