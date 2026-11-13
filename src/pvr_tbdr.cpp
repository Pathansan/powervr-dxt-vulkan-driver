#include "pvr_tbdr.h"
#include "x18_guard.h"

#include <vector>

void pvr_tbdr_begin_rendering(
    PFN_vkCmdBeginRendering next_fn,
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo) {
    if (!next_fn) return;
    if (!pRenderingInfo) {
        PvrX18Guard guard;
        next_fn(commandBuffer, pRenderingInfo);
        return;
    }

    VkRenderingInfo info = *pRenderingInfo;
    if (info.layerCount == 0) {
        info.layerCount = 1;
    }

    // PowerVR DXT lacks VK_EXT_dynamic_rendering_unused_attachments:
    // strip null depth/stencil attachments instead of passing VK_NULL_HANDLE imageViews.
    if (info.pDepthAttachment && info.pDepthAttachment->imageView == VK_NULL_HANDLE) {
        info.pDepthAttachment = nullptr;
    }
    if (info.pStencilAttachment && info.pStencilAttachment->imageView == VK_NULL_HANDLE) {
        info.pStencilAttachment = nullptr;
    }

    // Also sanitize color attachments if any trailing ones are null (or keep indices intact with DONT_CARE).
    std::vector<VkRenderingAttachmentInfo> color_atts;
    if (info.colorAttachmentCount > 0 && info.pColorAttachments) {
        bool has_null = false;
        for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
            if (info.pColorAttachments[i].imageView == VK_NULL_HANDLE) {
                has_null = true;
                break;
            }
        }
        if (has_null) {
            color_atts.assign(info.pColorAttachments, info.pColorAttachments + info.colorAttachmentCount);
            for (auto& att : color_atts) {
                if (att.imageView == VK_NULL_HANDLE) {
                    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    att.resolveMode = VK_RESOLVE_MODE_NONE;
                    att.resolveImageView = VK_NULL_HANDLE;
                }
            }
            info.pColorAttachments = color_atts.data();
        }
    }

    PvrX18Guard guard;
    next_fn(commandBuffer, &info);
}
