// vulkan_pipeline.cpp - Task 4: pipelines, passes, bindings, draws, transfers.
// Stubs until then so the core can be tested alone.
#include "vulkan_device.h"

namespace gpu {

bool VulkanDevice::init_pipeline_layout() {
    return true;
}
void VulkanDevice::create_descriptor_pool(Cmd &) {}
void VulkanDevice::end_passes(Cmd &c) {
    c.in_render = c.in_compute = false;
}
void VulkanDevice::queue_present(Cmd &) {}
VkShaderModule VulkanDevice::module(const std::string &) {
    return VK_NULL_HANDLE;
}
VkPipeline VulkanDevice::variant_for(Cmd &) {
    return VK_NULL_HANDLE;
}
VkSampler VulkanDevice::sampler_for(const SamplerState &) {
    return VK_NULL_HANDLE;
}
void VulkanDevice::bind_descriptors(Cmd &, VkPipelineBindPoint) {}
Pipeline VulkanDevice::render_pipeline(const std::string &, const RenderState &) {
    return {};
}
Pipeline VulkanDevice::compute_pipeline(const std::string &) {
    return {};
}
int VulkanDevice::thread_execution_width(Pipeline) {
    return 1;
}
void VulkanDevice::begin_render_pass(CommandBuffer, const RenderPass &) {}
void VulkanDevice::set_pipeline(CommandBuffer, Pipeline) {}
void VulkanDevice::set_depth(CommandBuffer, const DepthState &) {}
void VulkanDevice::set_cull(CommandBuffer, Cull) {}
void VulkanDevice::set_viewport(CommandBuffer, const Viewport &) {}
void VulkanDevice::set_vertex_buffer(CommandBuffer, int, Buffer, uint64_t) {}
void VulkanDevice::set_bytes(CommandBuffer, Stage, int, const void *, uint64_t) {}
void VulkanDevice::set_buffer(CommandBuffer, Stage, int, Buffer, uint64_t) {}
void VulkanDevice::set_texture(CommandBuffer, Stage, int, Texture) {}
void VulkanDevice::set_sampler(CommandBuffer, Stage, int, const SamplerState &) {}
void VulkanDevice::draw(CommandBuffer, Primitive, int, int) {}
void VulkanDevice::end_render_pass(CommandBuffer) {}
void VulkanDevice::begin_compute_pass(CommandBuffer) {}
void VulkanDevice::dispatch_threads(CommandBuffer, int, int, int, int) {}
void VulkanDevice::dispatch_groups(CommandBuffer, int, int, int, int) {}
void VulkanDevice::end_compute_pass(CommandBuffer) {}
void VulkanDevice::blit(CommandBuffer, Texture, Region, Texture, int, int) {}
void VulkanDevice::copy_buffer_to_texture(CommandBuffer, Buffer, uint64_t, int, Texture, Region) {}
void VulkanDevice::copy_texture_to_buffer(CommandBuffer, Texture, Region, Buffer, uint64_t, int) {}
void VulkanDevice::generate_mipmaps(CommandBuffer, Texture) {}

} // namespace gpu
