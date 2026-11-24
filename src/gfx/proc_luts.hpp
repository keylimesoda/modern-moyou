#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xmb::gfx {

struct Rgba8Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width * height * 4 (RGBA8_UNORM)
};

// Exact Sony custom_render_plugin.prx 0x04bad0: 16x4 RGBA8 _Stripes LUT
Rgba8Image generate_stripes_lut();

// Exact Sony custom_render_plugin.prx 0x04b928: 4096x1 RGBA8 _Encode LUT
Rgba8Image generate_encode_lut();

// Procedural fallbacks when a modded .qrc omits freslut1.tga or proc_iridescent.tga
Rgba8Image generate_fresnel_lut(uint32_t width = 512, uint32_t height = 1);
Rgba8Image generate_iridescent_lut(uint32_t width = 128, uint32_t height = 128);

// Uncompressed 24-bit BGR / 32-bit BGRA TGA loader (for freslut1.tga and proc_iridescent.tga)
bool load_tga_rgba8(std::span<const uint8_t> bytes, Rgba8Image& out, std::string* out_error = nullptr);

// 128x48 RGBA8 bitmap font atlas (16x6 grid of 8x8 glyphs for ASCII 32..127) for on-screen debug HUD
Rgba8Image generate_debug_font_atlas();

// Built-in 12-month procedural sky LUT (64x32 RGBA8) so Modern-XMB works out of the box
// with zero external Sony OFW assets.
Rgba8Image generate_procedural_sky_lut(int month, bool is_night);

} // namespace xmb::gfx
