#pragma once

#include "assets/qrc_loader.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace xmb::assets {

using MnuValue = std::variant<int32_t, float, std::string>;

struct MnuDocument {
    std::map<std::string, MnuValue, std::less<>> entries;

    bool parse(std::string_view text, std::string* out_error = nullptr);
    void merge_from(const MnuDocument& override_doc);

    [[nodiscard]] float get_float(std::string_view key, float fallback = 0.0f) const;
    [[nodiscard]] int32_t get_int(std::string_view key, int32_t fallback = 0) const;
};

struct BackgroundConfig {
    std::array<float, 3> color1{1.0f, 1.0f, 1.0f};
    std::array<float, 3> color2{1.0f, 1.0f, 1.0f};
    std::array<float, 3> color3{0.85f, 0.85f, 0.85f};
    std::array<float, 3> color4{0.92f, 0.92f, 0.92f};
    float fovy = 71.846f;
    int32_t colour_shader = 0;
    float night_blend = 0.0f;
    float night2day_begin = 0.0f;
    float night2day_end = 5.166f;
    float day2night_begin = 18.498f;
    float day2night_end = 20.331f;
    float dayspread = 2.68f;
    float night_white_bias = 0.486f;
    float neutrinos_x = 0.0f;

    static BackgroundConfig from_document(const MnuDocument& doc);
};

struct LineConfig {
    int32_t state = 1;
    int32_t shader = 1;
    float damping = 0.0001f;
    float length = 0.306f;
    float tension = 0.25f;
    float spacing = 407.658f;
    float thinness = 1.0f;
    float brightness = 0.702f;
    float mipmap_bias = 1.867f;
    float fresnel = 0.639f;
    float falloff = 1.003f;
    float timestep = 4.0f;
    float perturbation = 0.1f;
    std::array<float, 3> pos{-7.679f, -1.088f, -6.403f};
    std::array<float, 3> ang{-0.994f, 0.087f, 0.065f};
    float ang_rot = 18.121f;
    std::array<float, 3> end{3.0f, 0.4f, 0.2f};
    std::array<float, 3> ffd_scale1{5.677f, 1.001f, 1.0f};
    std::array<float, 3> ffd_scale2{2.828f, 1.276f, 2.888f};
    std::array<float, 3> ffd_offset{0.0f, -0.47f, 0.0f};
    float ffd_param1 = -1.335f;
    int32_t show_ffd = 0;
    int32_t ffd_shader = 1;

    static LineConfig from_document(const MnuDocument& doc);
};

struct HdrConfig {
    int32_t enabled = 1;
    float exposure = 1.05f;
    float white_level = 0.899f;
    float glare_level = 1.102f;
    float glare_thresh = 0.739f;
    std::array<float, 3> gaussian_rad{1.239f, 1.432f, 1.558f};
    float glare_sum_pow = 0.557f;
    int32_t tex_size = 8;
    int32_t tex_max_mip = 8;
    int32_t glare = 1;
    int32_t glare_only = 0;
    int32_t tonebefore = 1;
    int32_t blur = 0;
    float gamma = 2.2f;
    float dither = 0.0f;

    static HdrConfig from_document(const MnuDocument& doc);
};

struct ParticlesConfig {
    float emit_vel_min = 0.151f;
    float emit_vel_mul = 0.19f;
    float emit_vel_var = 0.283f;
    float emit_cone_angle = 51.87f;
    float emit_neg_prob = 0.174f;
    float emit_vel_zscale = 0.0f;
    float emit_per_frame = 16.654f;
    float emit_prob = 0.479f;
    float aging_speed = 0.00285f;
    float aging_variance = 0.493f;
    float friction = 0.0306f;
    float spin_time_scale = 2.74f;
    float delta_time = 0.00889f;
    float gravity = -6.8e-5f;
    std::array<float, 3> wind_dir{0.340f, 0.0f, 0.35f};
    float wind_scale = 0.0f;
    float wind_scale_10 = 0.0f;
    float brownian_scale = 0.225f;
    std::array<float, 3> spot_pos{4.16f, 2.63f, -7.6f};
    std::array<float, 3> spot_attn{1.0f, 0.0f, 0.0f};
    float specular_power = 35.29f;
    float specular_coeff = 74.74f;
    float lambert_coeff = 8.0f;
    float exposure = 0.039f;
    float fresnel = 1.333f;
    float color_control = 1.0f;
    float iridescent_exp = 1.0f;
    float global_alpha = 1.0f;
    float size_middle = 0.0536f;
    float size_near = 0.0772f;
    float size_far = 0.874f;
    float near_focus = 6.124f;
    float near_focus_dist = 1.319f;
    float near_focus_pow = 2.472f;
    float near_darkness = 6.24f;
    float near_fuzziness = 3.79f;
    float far_focus = 12.833f;
    float far_focus_dist = 3.541f;
    float far_focus_pow = 1.833f;
    float far_darkness = 13.9f;
    float near_align = 0.722f;
    float size_align = 13.19f;
    float glare = 0.160f;
    float glare_scale = 5.444f;
    float glare_p1 = 0.99f;
    float glare_p2 = 4.44f;

    static ParticlesConfig from_document(const MnuDocument& doc);
};

struct XmbThemeConfig {
    BackgroundConfig background;
    LineConfig line1;
    HdrConfig hdr;
    ParticlesConfig particles;
    std::string active_override;

    static bool load_from_qrc(
        const QrcArchive& archive,
        XmbThemeConfig& out_config,
        std::string_view override_name = "",
        std::string* out_error = nullptr);

    static XmbThemeConfig lerp(const XmbThemeConfig& a, const XmbThemeConfig& b, float t);
};

// Preloaded time-of-day override rig (night / yoake / day / higure) for smooth
// real-time clock morphing without disk I/O or simulation resets.
struct TimeOfDayRig {
    XmbThemeConfig base;
    XmbThemeConfig night;
    XmbThemeConfig yoake;
    XmbThemeConfig day;
    XmbThemeConfig higure;
    bool loaded = false;

    bool load_from_qrc(const QrcArchive& archive, std::string* out_error = nullptr);
    [[nodiscard]] XmbThemeConfig evaluate(float day_time_0_2400) const;
};

} // namespace xmb::assets
