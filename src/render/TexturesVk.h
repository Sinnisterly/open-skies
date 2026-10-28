#pragma once
#include <vulkan/vulkan.h>

// Vulkan side of the texture cache, driven by the present hook.
namespace os::render::tex {

struct DeviceInfo
{
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkGetInstanceProcAddr gipa;
};

// Inside the frame, after its fence wait and before the render pass.
void Record(const DeviceInfo& d, VkCommandBuffer cmd, uint32_t frameSlot, uint32_t frameCount);
void Release(); // device is going away
uint32_t DescriptorBudget(); // combined image samplers to reserve in ImGui's pool

}
