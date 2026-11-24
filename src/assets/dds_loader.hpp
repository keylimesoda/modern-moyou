#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xmb::assets {

enum class DdsPixelFormat {
    Unknown,
    RGBA8,
    RGBA16F,
};

struct DdsMipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA8 (4 bytes/px) or RGBA16F (8 bytes/px)
};

struct DdsImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    DdsPixelFormat format = DdsPixelFormat::Unknown;
    std::vector<DdsMipLevel> mips;
};

bool load_dds_from_memory(
    std::span<const uint8_t> bytes,
    DdsImage& out_image,
    std::string* out_error = nullptr);

const char* dds_format_name(DdsPixelFormat fmt) noexcept;

} // namespace xmb::assets
