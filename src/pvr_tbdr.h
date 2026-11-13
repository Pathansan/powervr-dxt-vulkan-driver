#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Sanitizes VkRenderingInfo for PowerVR TBDR drivers that lack
// VK_EXT_dynamic_rendering_unused_attachments.
void pvr_tbdr_begin_rendering(
    PFN_vkCmdBeginRendering next_fn,
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo);
