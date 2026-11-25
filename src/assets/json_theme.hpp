#pragma once

#include "assets/mnu_parser.hpp"
#include "gfx/proc_luts.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xmb::assets {

// Open MIT-licensed JSON theme bundle ("modern-moyou-theme/v1") — a clean,
// human-readable replacement for proprietary Sony .qrc archives.
struct JsonThemeBundle {
    std::string name;
    TimeOfDayRig tod_rig;
    std::map<std::string, XmbThemeConfig, std::less<>> named_overrides;

    // Optional 12-month sky gradient stops (hex RGB normalized [0..1]: top_rgb, bot_rgb)
    bool has_custom_sky = false;
    std::array<std::array<float, 6>, 12> sky_day{};
    std::array<std::array<float, 6>, 12> sky_night{};

    // Optional solid particle iridescent tint (when unset, uses procedural pearl LUT)
    bool has_iridescent_tint = false;
    std::array<float, 3> iridescent_tint{1.0f, 1.0f, 1.0f};

    bool load_from_file(const std::filesystem::path& path, std::string* out_error = nullptr);
    bool load_from_string(std::string_view json_text, std::string* out_error = nullptr);

    [[nodiscard]] std::vector<std::string> list_overrides() const;
    [[nodiscard]] bool get_override(std::string_view ov_name, XmbThemeConfig& out_cfg) const;

    // Generates a 64x32 RGBA8 sky LUT for the given 1-based month (1..12).
    [[nodiscard]] gfx::Rgba8Image build_sky_lut(int month, bool is_night) const;
    // Generates the 128x128 RGBA8 iridescent LUT (custom tint or procedural pearl).
    [[nodiscard]] gfx::Rgba8Image build_iridescent_lut() const;
};

} // namespace xmb::assets
