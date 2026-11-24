#include "assets/dds_loader.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace xmb::assets {

namespace {

constexpr uint32_t kDdpfAlphaPixels = 0x1u;
constexpr uint32_t kDdpfFourCC      = 0x4u;
constexpr uint32_t kDdpfRgb         = 0x40u;

constexpr uint32_t kD3dFmtA16B16G16R16F = 113u;
constexpr uint32_t kDxgiFormatR16G16B16A16Float = 10u;
constexpr uint32_t kDxgiFormatR8G8B8A8Unorm     = 28u;
constexpr uint32_t kDxgiFormatB8G8R8A8Unorm     = 87u;

uint32_t read_le32(std::span<const uint8_t> buf, std::size_t off) {
    return static_cast<uint32_t>(buf[off + 0]) |
           (static_cast<uint32_t>(buf[off + 1]) << 8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

uint8_t extract_channel(uint32_t pixel, uint32_t mask, uint8_t default_val) {
    if (mask == 0) return default_val;
    const int shift = std::countr_zero(mask);
    const uint32_t max_val = mask >> shift;
    const uint32_t v = (pixel & mask) >> shift;
    if (max_val == 255) return static_cast<uint8_t>(v);
    return static_cast<uint8_t>((v * 255u + max_val / 2u) / max_val);
}

void unpack_rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1Fu);
    const uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3Fu);
    const uint8_t b5 = static_cast<uint8_t>(c & 0x1Fu);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void decode_bc_block(const uint8_t* block, int bc_mode, uint8_t out_rgba[16][4]) {
    const uint8_t* color_blk = (bc_mode == 1) ? block : (block + 8);
    const uint16_t c0 = static_cast<uint16_t>(color_blk[0] | (color_blk[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(color_blk[2] | (color_blk[3] << 8));
    const uint32_t bits = static_cast<uint32_t>(color_blk[4]) |
                          (static_cast<uint32_t>(color_blk[5]) << 8) |
                          (static_cast<uint32_t>(color_blk[6]) << 16) |
                          (static_cast<uint32_t>(color_blk[7]) << 24);

    uint8_t pal[4][4]{};
    unpack_rgb565(c0, pal[0][0], pal[0][1], pal[0][2]); pal[0][3] = 255;
    unpack_rgb565(c1, pal[1][0], pal[1][1], pal[1][2]); pal[1][3] = 255;

    if (bc_mode != 1 || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = static_cast<uint8_t>((2 * pal[0][ch] + pal[1][ch] + 1) / 3);
            pal[3][ch] = static_cast<uint8_t>((pal[0][ch] + 2 * pal[1][ch] + 1) / 3);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = static_cast<uint8_t>((pal[0][ch] + pal[1][ch]) / 2);
            pal[3][ch] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }

    for (int i = 0; i < 16; ++i) {
        const uint32_t code = (bits >> (2 * i)) & 0x3u;
        out_rgba[i][0] = pal[code][0];
        out_rgba[i][1] = pal[code][1];
        out_rgba[i][2] = pal[code][2];
        out_rgba[i][3] = pal[code][3];
    }

    if (bc_mode == 2) { // DXT3: 4-bit explicit alpha
        for (int i = 0; i < 16; ++i) {
            const uint8_t nibble = (i & 1) ? static_cast<uint8_t>(block[i >> 1] >> 4)
                                           : static_cast<uint8_t>(block[i >> 1] & 0x0Fu);
            out_rgba[i][3] = static_cast<uint8_t>((nibble << 4) | nibble);
        }
    } else if (bc_mode == 3) { // DXT5: 8-bit interpolated alpha
        uint8_t apal[8]{};
        apal[0] = block[0];
        apal[1] = block[1];
        if (apal[0] > apal[1]) {
            for (int k = 1; k <= 6; ++k) {
                apal[k + 1] = static_cast<uint8_t>(((7 - k) * apal[0] + k * apal[1] + 3) / 7);
            }
        } else {
            for (int k = 1; k <= 4; ++k) {
                apal[k + 1] = static_cast<uint8_t>(((5 - k) * apal[0] + k * apal[1] + 2) / 5);
            }
            apal[6] = 0;
            apal[7] = 255;
        }
        uint64_t abits = 0;
        for (int b = 0; b < 6; ++b) abits |= static_cast<uint64_t>(block[2 + b]) << (8 * b);
        for (int i = 0; i < 16; ++i) {
            out_rgba[i][3] = apal[(abits >> (3 * i)) & 0x7u];
        }
    }
}

void decode_bc_mip(const uint8_t* src, uint32_t w, uint32_t h, int bc_mode, uint8_t* dst) {
    const uint32_t bx_count = std::max(1u, (w + 3u) / 4u);
    const uint32_t by_count = std::max(1u, (h + 3u) / 4u);
    const std::size_t blk_sz = (bc_mode == 1) ? 8u : 16u;
    uint8_t blk_rgba[16][4];
    for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            decode_bc_block(src + (static_cast<std::size_t>(by) * bx_count + bx) * blk_sz, bc_mode, blk_rgba);
            for (uint32_t py = 0; py < 4; ++py) {
                const uint32_t y = by * 4 + py;
                if (y >= h) continue;
                for (uint32_t px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4 + px;
                    if (x >= w) continue;
                    uint8_t* o = dst + (static_cast<std::size_t>(y) * w + x) * 4u;
                    const uint8_t* c = blk_rgba[py * 4 + px];
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
                }
            }
        }
    }
}

} // namespace

const char* dds_format_name(DdsPixelFormat fmt) noexcept {
    switch (fmt) {
        case DdsPixelFormat::RGBA8:   return "RGBA8";
        case DdsPixelFormat::RGBA16F: return "RGBA16F";
        default:                      return "Unknown";
    }
}

bool load_dds_from_memory(
    std::span<const uint8_t> bytes,
    DdsImage& out_image,
    std::string* out_error) {

    out_image = {};
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        if (out_error) *out_error = "Invalid DDS magic or header too short";
        return false;
    }

    const uint32_t header_size = read_le32(bytes, 4);
    if (header_size != 124) {
        if (out_error) *out_error = "Unexpected DDS header size";
        return false;
    }

    const uint32_t height    = read_le32(bytes, 12);
    const uint32_t width     = read_le32(bytes, 16);
    const uint32_t mip_count = std::max(1u, read_le32(bytes, 28));

    const uint32_t pf_flags  = read_le32(bytes, 80);
    const uint32_t pf_fourcc = read_le32(bytes, 84);
    const uint32_t pf_bpp    = read_le32(bytes, 88);
    const uint32_t pf_rmask  = read_le32(bytes, 92);
    const uint32_t pf_gmask  = read_le32(bytes, 96);
    const uint32_t pf_bmask  = read_le32(bytes, 100);
    const uint32_t pf_amask  = read_le32(bytes, 104);

    std::size_t data_offset = 128;
    bool is_fp16 = false;
    bool is_bgra8 = false;
    bool is_rgba8_direct = false;
    int bc_mode = 0; // 1 = DXT1/BC1, 2 = DXT3/BC2, 3 = DXT5/BC3
    uint32_t src_bpp = pf_bpp;
    uint32_t rmask = pf_rmask, gmask = pf_gmask, bmask = pf_bmask, amask = pf_amask;

    if ((pf_flags & kDdpfFourCC) != 0) {
        if (pf_fourcc == kD3dFmtA16B16G16R16F) {
            is_fp16 = true;
            src_bpp = 64;
        } else if (std::memcmp(bytes.data() + 84, "DXT1", 4) == 0) {
            bc_mode = 1;
        } else if (std::memcmp(bytes.data() + 84, "DXT3", 4) == 0) {
            bc_mode = 2;
        } else if (std::memcmp(bytes.data() + 84, "DXT5", 4) == 0) {
            bc_mode = 3;
        } else if (std::memcmp(bytes.data() + 84, "DX10", 4) == 0) {
            if (bytes.size() < 148) {
                if (out_error) *out_error = "Truncated DDS DX10 header";
                return false;
            }
            const uint32_t dxgi_fmt = read_le32(bytes, 128);
            data_offset = 148;
            if (dxgi_fmt == kDxgiFormatR16G16B16A16Float) {
                is_fp16 = true;
                src_bpp = 64;
            } else if (dxgi_fmt == kDxgiFormatR8G8B8A8Unorm) {
                is_rgba8_direct = true;
                src_bpp = 32;
            } else if (dxgi_fmt == kDxgiFormatB8G8R8A8Unorm) {
                is_bgra8 = true;
                src_bpp = 32;
            } else {
                if (out_error) *out_error = "Unsupported DXGI format: " + std::to_string(dxgi_fmt);
                return false;
            }
        } else {
            if (out_error) *out_error = "Unsupported DDS FourCC";
            return false;
        }
    } else if ((pf_flags & kDdpfRgb) != 0) {
        if (src_bpp != 24 && src_bpp != 32) {
            if (out_error) *out_error = "Unsupported uncompressed DDS bpp: " + std::to_string(src_bpp);
            return false;
        }
        if ((pf_flags & kDdpfAlphaPixels) == 0) {
            amask = 0;
        }
        if (src_bpp == 32 && rmask == 0x00FF0000u && gmask == 0x0000FF00u && bmask == 0x000000FFu && amask == 0xFF000000u) {
            is_bgra8 = true;
        } else if (src_bpp == 32 && rmask == 0x000000FFu && gmask == 0x0000FF00u && bmask == 0x00FF0000u && amask == 0xFF000000u) {
            is_rgba8_direct = true;
        }
    } else {
        if (out_error) *out_error = "Unsupported DDS pixel format flags";
        return false;
    }

    out_image.width = width;
    out_image.height = height;
    out_image.format = is_fp16 ? DdsPixelFormat::RGBA16F : DdsPixelFormat::RGBA8;

    const std::size_t src_bytes_per_px = src_bpp / 8;
    const std::size_t dst_bytes_per_px = is_fp16 ? 8 : 4;

    uint32_t mw = width;
    uint32_t mh = height;
    std::size_t cursor = data_offset;

    for (uint32_t mip = 0; mip < mip_count; ++mip) {
        const std::size_t pixel_count = static_cast<std::size_t>(mw) * mh;
        const std::size_t src_mip_bytes = (bc_mode != 0)
            ? static_cast<std::size_t>(std::max(1u, (mw + 3u) / 4u)) *
              static_cast<std::size_t>(std::max(1u, (mh + 3u) / 4u)) *
              ((bc_mode == 1) ? 8u : 16u)
            : pixel_count * src_bytes_per_px;
        if (cursor + src_mip_bytes > bytes.size()) {
            if (mip == 0) {
                if (out_error) *out_error = "DDS mip 0 exceeds file size";
                return false;
            }
            break;
        }

        DdsMipLevel level;
        level.width = mw;
        level.height = mh;
        level.pixels.resize(pixel_count * dst_bytes_per_px);

        const uint8_t* src = bytes.data() + cursor;
        uint8_t* dst = level.pixels.data();

        if (bc_mode != 0) {
            decode_bc_mip(src, mw, mh, bc_mode, dst);
        } else if (is_fp16 || is_rgba8_direct) {
            std::memcpy(dst, src, src_mip_bytes);
        } else if (is_bgra8) {
            for (std::size_t i = 0; i < pixel_count; ++i) {
                dst[i * 4 + 0] = src[i * 4 + 2]; // R
                dst[i * 4 + 1] = src[i * 4 + 1]; // G
                dst[i * 4 + 2] = src[i * 4 + 0]; // B
                dst[i * 4 + 3] = src[i * 4 + 3]; // A
            }
        } else {
            for (std::size_t i = 0; i < pixel_count; ++i) {
                uint32_t px = 0;
                std::memcpy(&px, src + i * src_bytes_per_px, src_bytes_per_px);
                dst[i * 4 + 0] = extract_channel(px, rmask, 0);
                dst[i * 4 + 1] = extract_channel(px, gmask, 0);
                dst[i * 4 + 2] = extract_channel(px, bmask, 0);
                dst[i * 4 + 3] = extract_channel(px, amask, 255);
            }
        }

        out_image.mips.push_back(std::move(level));
        cursor += src_mip_bytes;
        mw = std::max(1u, mw >> 1);
        mh = std::max(1u, mh >> 1);
    }

    out_image.mip_levels = static_cast<uint32_t>(out_image.mips.size());
    return !out_image.mips.empty();
}

} // namespace xmb::assets
