#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stddef.h>

// PowerVR DXT-48-1536 MC1 lacks native BC1-BC7 (DXT) hardware decompression.
// This module provides format mapping and block decompression (BC1/BC2/BC3/BC4/BC5/BC7 -> RGBA8,
// BC6H -> RGBA16F) for both Vulkan 1.0 vkCmdCopyBufferToImage and Vulkan 1.3 vkCmdCopyBufferToImage2.

bool pvr_is_bcn_format(VkFormat fmt);
VkFormat pvr_bcn_fallback_format(VkFormat fmt);
bool pvr_bcn_is_srgb(VkFormat fmt);
uint32_t pvr_bcn_block_bytes(VkFormat fmt);
uint32_t pvr_bcn_dst_pixel_bytes(VkFormat fmt);

void pvr_bcn_decode_region(
    VkFormat src_fmt,
    const uint8_t* src,
    uint32_t row_pitch_blocks,
    uint32_t width,
    uint32_t height,
    uint8_t* dst,
    uint32_t dst_row_pitch_bytes);
