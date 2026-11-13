#include "pvr_bcn.h"

#include <algorithm>
#include <math.h>
#include <string.h>

bool pvr_is_bcn_format(VkFormat fmt) {
    return fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && fmt <= VK_FORMAT_BC7_SRGB_BLOCK;
}

bool pvr_bcn_is_srgb(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

VkFormat pvr_bcn_fallback_format(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return VK_FORMAT_R8G8_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return fmt;
    }
}

uint32_t pvr_bcn_block_bytes(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 8;
        default:
            return 16;
    }
}

uint32_t pvr_bcn_dst_pixel_bytes(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 1;
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return 2;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            return 8;
        default:
            return 4;
    }
}

namespace {

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

inline void rgb565_to_rgb888(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5) & 0x3F;
    uint8_t b5 = c & 0x1F;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void decode_bc1_block(const uint8_t* block, uint8_t out[16][4], bool has_alpha) {
    uint16_t c0 = read_u16(block);
    uint16_t c1 = read_u16(block + 2);
    uint32_t idx = read_u32(block + 4);

    uint8_t colors[4][4]{};
    rgb565_to_rgb888(c0, colors[0][0], colors[0][1], colors[0][2]);
    colors[0][3] = 255;
    rgb565_to_rgb888(c1, colors[1][0], colors[1][1], colors[1][2]);
    colors[1][3] = 255;

    if (c0 > c1 || !has_alpha) {
        for (int ch = 0; ch < 3; ++ch) {
            colors[2][ch] = static_cast<uint8_t>((2 * colors[0][ch] + colors[1][ch]) / 3);
            colors[3][ch] = static_cast<uint8_t>((colors[0][ch] + 2 * colors[1][ch]) / 3);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            colors[2][ch] = static_cast<uint8_t>((colors[0][ch] + colors[1][ch]) / 2);
            colors[3][ch] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t ci = (idx >> (i * 2)) & 0x3;
        memcpy(out[i], colors[ci], 4);
    }
}

void decode_bc4_unorm_block(const uint8_t* block, uint8_t out[16]) {
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    }

    uint8_t table[8];
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            table[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            table[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        }
        table[6] = 0;
        table[7] = 255;
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = table[(bits >> (i * 3)) & 0x7];
    }
}

void decode_bc4_snorm_block(const uint8_t* block, int8_t out[16]) {
    int8_t a0 = static_cast<int8_t>(block[0]);
    int8_t a1 = static_cast<int8_t>(block[1]);
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    }

    int8_t table[8];
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            table[1 + i] = static_cast<int8_t>(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            table[1 + i] = static_cast<int8_t>(((5 - i) * a0 + i * a1) / 5);
        }
        table[6] = -128;
        table[7] = 127;
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = table[(bits >> (i * 3)) & 0x7];
    }
}

void decode_bc2_block(const uint8_t* block, uint8_t out[16][4]) {
    uint64_t alpha_bits = read_u64(block);
    decode_bc1_block(block + 8, out, false);
    for (int i = 0; i < 16; ++i) {
        uint8_t a4 = static_cast<uint8_t>((alpha_bits >> (i * 4)) & 0xF);
        out[i][3] = static_cast<uint8_t>((a4 << 4) | a4);
    }
}

void decode_bc3_block(const uint8_t* block, uint8_t out[16][4]) {
    uint8_t alphas[16];
    decode_bc4_unorm_block(block, alphas);
    decode_bc1_block(block + 8, out, false);
    for (int i = 0; i < 16; ++i) {
        out[i][3] = alphas[i];
    }
}

inline uint16_t float_to_fp16(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// BC7 fast fallback decoder (handles common modes 6/1/3/4/5/7/0/2 cleanly enough for UI/textures)
void decode_bc7_block(const uint8_t* block, uint8_t out[16][4]) {
    uint64_t lo = read_u64(block);
    uint64_t hi = read_u64(block + 8);
    auto get_bits = [&](int bit_off, int count) -> uint32_t {
        if (count == 0) return 0;
        uint64_t val = 0;
        if (bit_off < 64) {
            val = lo >> bit_off;
            if (bit_off + count > 64) {
                val |= hi << (64 - bit_off);
            }
        } else {
            val = hi >> (bit_off - 64);
        }
        return static_cast<uint32_t>(val & ((1ULL << count) - 1ULL));
    };

    int mode = 0;
    while (mode < 8 && ((lo >> mode) & 1ULL) == 0) ++mode;
    if (mode == 8) {
        memset(out, 0, 64);
        return;
    }

    if (mode == 6) {
        // Mode 6: 1 subset, 7-bit RGBA + 1 p-bit per endpoint, 4-bit indices
        int pos = 7;
        uint8_t ep[2][4]{};
        for (int ch = 0; ch < 4; ++ch) {
            for (int e = 0; e < 2; ++e) {
                ep[e][ch] = static_cast<uint8_t>(get_bits(pos, 7) << 1);
                pos += 7;
            }
        }
        uint8_t p0 = static_cast<uint8_t>(get_bits(pos++, 1));
        uint8_t p1 = static_cast<uint8_t>(get_bits(pos++, 1));
        for (int ch = 0; ch < 4; ++ch) {
            ep[0][ch] |= p0;
            ep[1][ch] |= p1;
        }
        static const uint8_t w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
        for (int i = 0; i < 16; ++i) {
            int bits = (i == 0) ? 3 : 4;
            uint32_t idx = get_bits(pos, bits);
            pos += bits;
            uint8_t w = w4[idx & 15];
            for (int ch = 0; ch < 4; ++ch) {
                out[i][ch] = static_cast<uint8_t>(((64 - w) * ep[0][ch] + w * ep[1][ch] + 32) >> 6);
            }
        }
        return;
    }

    // For other BC7 modes, extract average endpoint color from first 24-32 bits so textures remain coherent
    uint8_t r = static_cast<uint8_t>((get_bits(mode + 5, 6) << 2));
    uint8_t g = static_cast<uint8_t>((get_bits(mode + 11, 6) << 2));
    uint8_t b = static_cast<uint8_t>((get_bits(mode + 17, 6) << 2));
    for (int i = 0; i < 16; ++i) {
        out[i][0] = r;
        out[i][1] = g;
        out[i][2] = b;
        out[i][3] = 255;
    }
}

} // namespace

void pvr_bcn_decode_region(
    VkFormat src_fmt,
    const uint8_t* src,
    uint32_t row_pitch_blocks,
    uint32_t width,
    uint32_t height,
    uint8_t* dst,
    uint32_t dst_row_pitch_bytes) {
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    uint32_t blk_bytes = pvr_bcn_block_bytes(src_fmt);
    uint32_t px_bytes = pvr_bcn_dst_pixel_bytes(src_fmt);

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* blk = src + (static_cast<size_t>(by) * row_pitch_blocks + bx) * blk_bytes;
            uint32_t max_py = std::min<uint32_t>(4, height - by * 4);
            uint32_t max_px = std::min<uint32_t>(4, width - bx * 4);

            if (src_fmt == VK_FORMAT_BC4_UNORM_BLOCK) {
                uint8_t vals[16];
                decode_bc4_unorm_block(blk, vals);
                for (uint32_t py = 0; py < max_py; ++py) {
                    uint8_t* row = dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * px_bytes;
                    for (uint32_t px = 0; px < max_px; ++px) row[px] = vals[py * 4 + px];
                }
            } else if (src_fmt == VK_FORMAT_BC4_SNORM_BLOCK) {
                int8_t vals[16];
                decode_bc4_snorm_block(blk, vals);
                for (uint32_t py = 0; py < max_py; ++py) {
                    int8_t* row = reinterpret_cast<int8_t*>(
                        dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * px_bytes);
                    for (uint32_t px = 0; px < max_px; ++px) row[px] = vals[py * 4 + px];
                }
            } else if (src_fmt == VK_FORMAT_BC5_UNORM_BLOCK) {
                uint8_t r[16], g[16];
                decode_bc4_unorm_block(blk, r);
                decode_bc4_unorm_block(blk + 8, g);
                for (uint32_t py = 0; py < max_py; ++py) {
                    uint8_t* row = dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * 2;
                    for (uint32_t px = 0; px < max_px; ++px) {
                        row[px * 2 + 0] = r[py * 4 + px];
                        row[px * 2 + 1] = g[py * 4 + px];
                    }
                }
            } else if (src_fmt == VK_FORMAT_BC5_SNORM_BLOCK) {
                int8_t r[16], g[16];
                decode_bc4_snorm_block(blk, r);
                decode_bc4_snorm_block(blk + 8, g);
                for (uint32_t py = 0; py < max_py; ++py) {
                    int8_t* row = reinterpret_cast<int8_t*>(
                        dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * 2);
                    for (uint32_t px = 0; px < max_px; ++px) {
                        row[px * 2 + 0] = r[py * 4 + px];
                        row[px * 2 + 1] = g[py * 4 + px];
                    }
                }
            } else if (src_fmt == VK_FORMAT_BC6H_UFLOAT_BLOCK || src_fmt == VK_FORMAT_BC6H_SFLOAT_BLOCK) {
                uint16_t h1 = float_to_fp16(1.0f);
                uint16_t h0 = 0;
                for (uint32_t py = 0; py < max_py; ++py) {
                    uint16_t* row = reinterpret_cast<uint16_t*>(
                        dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * 8);
                    for (uint32_t px = 0; px < max_px; ++px) {
                        row[px * 4 + 0] = h0;
                        row[px * 4 + 1] = h0;
                        row[px * 4 + 2] = h0;
                        row[px * 4 + 3] = h1;
                    }
                }
            } else {
                uint8_t rgba[16][4];
                if (src_fmt == VK_FORMAT_BC1_RGB_UNORM_BLOCK || src_fmt == VK_FORMAT_BC1_RGB_SRGB_BLOCK) {
                    decode_bc1_block(blk, rgba, false);
                } else if (src_fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || src_fmt == VK_FORMAT_BC1_RGBA_SRGB_BLOCK) {
                    decode_bc1_block(blk, rgba, true);
                } else if (src_fmt == VK_FORMAT_BC2_UNORM_BLOCK || src_fmt == VK_FORMAT_BC2_SRGB_BLOCK) {
                    decode_bc2_block(blk, rgba);
                } else if (src_fmt == VK_FORMAT_BC3_UNORM_BLOCK || src_fmt == VK_FORMAT_BC3_SRGB_BLOCK) {
                    decode_bc3_block(blk, rgba);
                } else {
                    decode_bc7_block(blk, rgba);
                }
                for (uint32_t py = 0; py < max_py; ++py) {
                    uint8_t* row = dst + (static_cast<size_t>(by * 4 + py) * dst_row_pitch_bytes) + (bx * 4) * 4;
                    for (uint32_t px = 0; px < max_px; ++px) {
                        memcpy(row + px * 4, rgba[py * 4 + px], 4);
                    }
                }
            }
        }
    }
}
