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

    // Also sanitize color attachments if any are null, using stack storage on the hot path.
    VkRenderingAttachmentInfo stack_atts[8];
    std::vector<VkRenderingAttachmentInfo> heap_atts;
    if (info.colorAttachmentCount > 0 && info.pColorAttachments) {
        bool has_null = false;
        for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
            if (info.pColorAttachments[i].imageView == VK_NULL_HANDLE) {
                has_null = true;
                break;
            }
        }
        if (has_null) {
            VkRenderingAttachmentInfo* dst = stack_atts;
            if (info.colorAttachmentCount > 8) {
                heap_atts.resize(info.colorAttachmentCount);
                dst = heap_atts.data();
            }
            for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
                dst[i] = info.pColorAttachments[i];
                if (dst[i].imageView == VK_NULL_HANDLE) {
                    dst[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    dst[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    dst[i].resolveMode = VK_RESOLVE_MODE_NONE;
                    dst[i].resolveImageView = VK_NULL_HANDLE;
                }
            }
            info.pColorAttachments = dst;
        }
    }

    PvrX18Guard guard;
    next_fn(commandBuffer, &info);
}
