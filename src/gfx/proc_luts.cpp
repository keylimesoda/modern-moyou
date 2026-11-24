#include "gfx/proc_luts.hpp"

#include <algorithm>
#include <cmath>

namespace xmb::gfx {

namespace {

uint8_t to_u8(float v) {
    const float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

} // namespace

Rgba8Image generate_stripes_lut() {
    // Exact Sony Lines::buildStripesLUT (custom_render_plugin.prx 0x04bad0): 16x4 RGBA8
    Rgba8Image img;
    img.width = 16;
    img.height = 4;
    img.pixels.resize(16u * 4u * 4u);

    for (uint32_t y = 0; y < 4; ++y) {
        const float fy = static_cast<float>(y) / 3.0f;
        const float exp_y = 20.0f * fy * fy * fy;
        const float scale_y = fy * 0.5f + 0.5f;
        for (uint32_t x = 0; x < 16; ++x) {
            const float fx = static_cast<float>(x) / 15.0f;
            const float d = fx - 0.5f;
            const float base = std::max(-4.0f * d * d + 1.01f, 0.0f);
            const float v = std::min(std::pow(base, exp_y) * scale_y, 1.0f);
            const uint8_t b = to_u8(v);

            const size_t idx = (static_cast<size_t>(y) * 16u + x) * 4u;
            img.pixels[idx + 0] = b;
            img.pixels[idx + 1] = b;
            img.pixels[idx + 2] = b;
            img.pixels[idx + 3] = b;
        }
    }
    return img;
}

Rgba8Image generate_encode_lut() {
    // Exact Sony Lines::buildEncodeLUT (custom_render_plugin.prx 0x04b928): 4096x1 RGBA8
    Rgba8Image img;
    img.width = 4096;
    img.height = 1;
    img.pixels.resize(4096u * 4u);

    for (uint32_t i = 0; i < 4096; ++i) {
        const float x = (static_cast<float>(i) / 4095.0f) * 128.0f;
        float intpart = 0.0f;
        const float frac = std::modf(x, &intpart);
        const uint8_t c0 = to_u8(frac * 0.25f);
        const uint8_t c1 = to_u8(intpart * (1.0f / 64.0f));
        const size_t idx = static_cast<size_t>(i) * 4u;
        img.pixels[idx + 0] = c0;
        img.pixels[idx + 1] = c1;
        img.pixels[idx + 2] = c0;
        img.pixels[idx + 3] = c1;
    }
    return img;
}

Rgba8Image generate_fresnel_lut(uint32_t width, uint32_t height) {
    Rgba8Image img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<size_t>(width) * height * 4u);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float u = (width > 1) ? (static_cast<float>(x) / static_cast<float>(width - 1)) : 0.5f;
            const float r = std::pow(1.0f - u, 2.5f);
            const float g = 0.25f + 0.75f * std::pow(1.0f - u * 0.5f, 1.5f);
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4u;
            img.pixels[idx + 0] = to_u8(r);
            img.pixels[idx + 1] = to_u8(g);
            img.pixels[idx + 2] = 0;
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

Rgba8Image generate_iridescent_lut(uint32_t width, uint32_t height) {
    Rgba8Image img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<size_t>(width) * height * 4u);

    for (uint32_t y = 0; y < height; ++y) {
        const float ny = (static_cast<float>(y) / static_cast<float>(height - 1)) * 2.0f - 1.0f;
        for (uint32_t x = 0; x < width; ++x) {
            const float nx = (static_cast<float>(x) / static_cast<float>(width - 1)) * 2.0f - 1.0f;
            const float r = std::min(1.0f, std::sqrt(nx * nx + ny * ny));
            const float angle = std::atan2(ny, nx);
            const float phase = r * 4.2f + angle * 0.35f;
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4u;
            img.pixels[idx + 0] = to_u8(0.78f + 0.22f * std::cos(phase));
            img.pixels[idx + 1] = to_u8(0.78f + 0.22f * std::cos(phase - 2.0943951f));
            img.pixels[idx + 2] = to_u8(0.78f + 0.22f * std::cos(phase - 4.1887902f));
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

bool load_tga_rgba8(std::span<const uint8_t> bytes, Rgba8Image& out, std::string* out_error) {
    if (bytes.size() < 18) {
        if (out_error) *out_error = "TGA too small";
        return false;
    }
    const uint8_t id_len = bytes[0];
    const uint8_t cmap_type = bytes[1];
    const uint8_t img_type = bytes[2];
    const uint16_t width = static_cast<uint16_t>(bytes[12] | (bytes[13] << 8));
    const uint16_t height = static_cast<uint16_t>(bytes[14] | (bytes[15] << 8));
    const uint8_t bpp = bytes[16];
    const uint8_t desc = bytes[17];

    if (cmap_type != 0 || img_type != 2 || (bpp != 24 && bpp != 32) || width == 0 || height == 0) {
        if (out_error) *out_error = "Unsupported TGA format (expected uncompressed 24/32-bit RGB/RGBA)";
        return false;
    }

    const size_t bpp_bytes = bpp / 8u;
    const size_t data_off = 18u + id_len;
    const size_t needed = data_off + static_cast<size_t>(width) * height * bpp_bytes;
    if (bytes.size() < needed) {
        if (out_error) *out_error = "TGA pixel data truncated";
        return false;
    }

    const bool top_origin = (desc & 0x20u) != 0;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 4u);

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t src_y = top_origin ? y : (height - 1u - y);
        for (uint32_t x = 0; x < width; ++x) {
            const size_t s = data_off + (static_cast<size_t>(src_y) * width + x) * bpp_bytes;
            const size_t d = (static_cast<size_t>(y) * width + x) * 4u;
            out.pixels[d + 0] = bytes[s + 2]; // R
            out.pixels[d + 1] = bytes[s + 1]; // G
            out.pixels[d + 2] = bytes[s + 0]; // B
            out.pixels[d + 3] = (bpp_bytes == 4u) ? bytes[s + 3] : 255u;
        }
    }
    return true;
}

Rgba8Image generate_debug_font_atlas() {
    // Compact 5x7 ASCII font (chars 32..127) unpacked into a 128x48 RGBA8 atlas (16 cols x 6 rows of 8x8 cells).
    static constexpr uint8_t kFont5x7[96][7] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' 32
        {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // '!'
        {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // '"'
        {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, // '#'
        {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // '$'
        {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // '%'
        {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // '&'
        {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, // '''
        {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // '('
        {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // ')'
        {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, // '*'
        {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // '+'
        {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, // ','
        {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // '-'
        {0x00,0x00,0x00,0x00,0x00,0x04,0x00}, // '.'
        {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, // '/'
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // '0' 48
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // '1'
        {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // '2'
        {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E}, // '3'
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // '4'
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // '5'
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // '6'
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // '7'
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // '8'
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // '9'
        {0x00,0x04,0x00,0x00,0x00,0x04,0x00}, // ':'
        {0x00,0x04,0x00,0x00,0x00,0x04,0x08}, // ';'
        {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // '<'
        {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // '='
        {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // '>'
        {0x0E,0x11,0x02,0x04,0x04,0x00,0x04}, // '?'
        {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, // '@' 64
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'A'
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 'B'
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 'C'
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // 'D'
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 'E'
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 'F'
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // 'G'
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'H'
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // 'I'
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // 'J'
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 'K'
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 'L'
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 'M'
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // 'N'
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'O'
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 'P' 80
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 'Q'
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 'R'
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 'S'
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 'T'
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'U'
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 'V'
        {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // 'W'
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 'X'
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 'Y'
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 'Z'
        {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // '['
        {0x10,0x08,0x08,0x04,0x02,0x02,0x01}, // '\'
        {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ']'
        {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // '^'
        {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // '_'
        {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, // '`' 96
        {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // 'a'
        {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, // 'b'
        {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, // 'c'
        {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, // 'd'
        {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // 'e'
        {0x06,0x08,0x1E,0x08,0x08,0x08,0x08}, // 'f'
        {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, // 'g'
        {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, // 'h'
        {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // 'i'
        {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, // 'j'
        {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // 'k'
        {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, // 'l'
        {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, // 'm'
        {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, // 'n'
        {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // 'o'
        {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, // 'p' 112
        {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}, // 'q'
        {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, // 'r'
        {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, // 's'
        {0x08,0x08,0x1E,0x08,0x08,0x09,0x06}, // 't'
        {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // 'u'
        {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, // 'v'
        {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // 'w'
        {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, // 'x'
        {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, // 'y'
        {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, // 'z'
        {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, // '{'
        {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, // '|'
        {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, // '}'
        {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, // '~'
        {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, // DEL
    };

    Rgba8Image img;
    img.width = 128;
    img.height = 48;
    img.pixels.assign(128u * 48u * 4u, 0);

    for (uint32_t g = 0; g < 96; ++g) {
        const uint32_t gx = (g % 16u) * 8u;
        const uint32_t gy = (g / 16u) * 8u;
        for (uint32_t row = 0; row < 7; ++row) {
            const uint8_t bits = kFont5x7[g][row];
            for (uint32_t col = 0; col < 5; ++col) {
                if ((bits >> (4u - col)) & 1u) {
                    const size_t idx = (static_cast<size_t>(gy + row) * 128u + (gx + col + 1u)) * 4u;
                    img.pixels[idx + 0] = 255;
                    img.pixels[idx + 1] = 255;
                    img.pixels[idx + 2] = 255;
                    img.pixels[idx + 3] = 255;
                }
            }
        }
    }
    return img;
}

Rgba8Image generate_procedural_sky_lut(int month, bool is_night) {
    // 12 seasonal palettes (Jan..Dec): {top_rgb, bottom_rgb} in [0..1]
    static constexpr float kDayPal[12][6] = {
        {0.18f, 0.28f, 0.42f,  0.48f, 0.62f, 0.78f}, // Jan: crisp winter slate-blue
        {0.52f, 0.44f, 0.14f,  0.86f, 0.74f, 0.28f}, // Feb: warm late-winter gold
        {0.18f, 0.46f, 0.16f,  0.46f, 0.78f, 0.34f}, // Mar: early spring green
        {0.62f, 0.28f, 0.44f,  0.90f, 0.56f, 0.70f}, // Apr: sakura rose-pink
        {0.08f, 0.42f, 0.14f,  0.24f, 0.74f, 0.28f}, // May: vivid emerald green
        {0.26f, 0.22f, 0.56f,  0.54f, 0.48f, 0.86f}, // Jun: hydrangea periwinkle
        {0.06f, 0.36f, 0.58f,  0.20f, 0.70f, 0.86f}, // Jul: summer turquoise-cyan
        {0.08f, 0.20f, 0.54f,  0.22f, 0.48f, 0.86f}, // Aug: deep midsummer cobalt
        {0.34f, 0.16f, 0.50f,  0.64f, 0.38f, 0.80f}, // Sep: twilight violet
        {0.58f, 0.30f, 0.08f,  0.90f, 0.56f, 0.20f}, // Oct: autumn amber-orange
        {0.38f, 0.18f, 0.08f,  0.66f, 0.36f, 0.18f}, // Nov: warm chestnut-sienna
        {0.52f, 0.10f, 0.12f,  0.84f, 0.24f, 0.22f}, // Dec: winter crimson
    };

    const int m = std::clamp(month, 1, 12) - 1;
    const float* p = kDayPal[m];
    const float scale_top = is_night ? 0.16f : 0.82f;
    const float scale_bot = is_night ? 0.30f : 1.00f;

    Rgba8Image img;
    img.width = 64;
    img.height = 32;
    img.pixels.resize(64u * 32u * 4u);

    for (uint32_t y = 0; y < img.height; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(img.height - 1);
        for (uint32_t x = 0; x < img.width; ++x) {
            // Subtle diurnal brightness curve along U (0=midnight, 0.5=noon, 1=midnight)
            const float u = static_cast<float>(x) / static_cast<float>(img.width - 1);
            const float diurnal = is_night
                ? (0.75f + 0.25f * std::sin(u * 3.14159265f))
                : (0.65f + 0.35f * std::sin(u * 3.14159265f));
            const float r = (p[0] * scale_top * (1.0f - v) + p[3] * scale_bot * v) * diurnal;
            const float g = (p[1] * scale_top * (1.0f - v) + p[4] * scale_bot * v) * diurnal;
            const float b = (p[2] * scale_top * (1.0f - v) + p[5] * scale_bot * v) * diurnal;
            const size_t idx = (static_cast<size_t>(y) * img.width + x) * 4u;
            img.pixels[idx + 0] = to_u8(r);
            img.pixels[idx + 1] = to_u8(g);
            img.pixels[idx + 2] = to_u8(b);
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

} // namespace xmb::gfx
