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
// human-readable replacement for proprietary Sony .qrc archives with full
// parameter and LUT expressiveness.
struct JsonThemeBundle {
    using ColorStops = std::vector<std::array<float, 3>>;

    struct SkyMonth {
        ColorStops day;
        ColorStops night;
        ColorStops dawn; // optional: enables hue-shifting sunrise along X
        ColorStops dusk; // optional: enables hue-shifting sunset along X
    };

    struct FresnelCurve {
        bool enabled = false;
        // Either custom 1D stops [0..1] along U, or parametric pow(1-u, power)*scale + bias
        std::vector<float> stops;
        float power = 2.5f;
        float bias = 0.0f;
        float scale = 1.0f;
        bool invert = false;
    };

    std::string name;
    TimeOfDayRig tod_rig;
    std::map<std::string, XmbThemeConfig, std::less<>> named_overrides;

    // Optional 12-month multi-stop 2D sky LUTs (X = time of day, Y = vertical stops)
    bool has_custom_sky = false;
    std::array<SkyMonth, 12> sky_months{};

    // Optional custom 1D Fresnel edge LUT (line1.fresnel_curve)
    FresnelCurve fresnel_curve{};

    // Optional particle iridescent LUT: single flat tint OR multi-stop 2D shimmer gradient
    bool has_iridescent_tint = false;
    std::array<float, 3> iridescent_tint{1.0f, 1.0f, 1.0f};
    ColorStops iridescent_gradient;

    bool load_from_file(const std::filesystem::path& path, std::string* out_error = nullptr);
    bool load_from_string(std::string_view json_text, std::string* out_error = nullptr);

    [[nodiscard]] std::vector<std::string> list_overrides() const;
    [[nodiscard]] bool get_override(std::string_view ov_name, XmbThemeConfig& out_cfg) const;

    // Generates a 64x32 RGBA8 sky LUT for the given 1-based month (1..12).
    [[nodiscard]] gfx::Rgba8Image build_sky_lut(int month, bool is_night) const;
    // Generates the 512x1 RGBA8 Fresnel edge LUT (custom curve or procedural default).
    [[nodiscard]] gfx::Rgba8Image build_fresnel_lut() const;
    // Generates the 128x128 (or 16x16 flat tint) RGBA8 iridescent LUT.
    [[nodiscard]] gfx::Rgba8Image build_iridescent_lut() const;
};

} // namespace xmb::assets
