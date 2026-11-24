#include "assets/mnu_parser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace xmb::assets {

namespace {

std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool parse_int(std::string_view s, int32_t& out) {
    s = trim(s);
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{};
}

bool parse_float(std::string_view s, float& out) {
    s = trim(s);
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end != tmp.c_str();
}

} // namespace

bool MnuDocument::parse(std::string_view text, std::string* out_error) {
    std::size_t pos = 0;
    bool saw_header = false;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;

        if (line.empty()) {
            continue;
        }
        if (line.starts_with('#')) {
            if (line.starts_with("#MNU_1.0")) {
                saw_header = true;
            }
            continue;
        }

        const std::size_t c1 = line.find(':');
        if (c1 == std::string_view::npos) {
            continue;
        }
        const std::size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string_view::npos) {
            continue;
        }

        std::string key(trim(line.substr(0, c1)));
        std::string_view type = trim(line.substr(c1 + 1, c2 - c1 - 1));
        std::string_view val_str = trim(line.substr(c2 + 1));

        if (type == "int") {
            int32_t v = 0;
            if (!parse_int(val_str, v)) {
                if (out_error) *out_error = "Invalid int for key: " + key;
                return false;
            }
            entries.insert_or_assign(std::move(key), v);
        } else if (type == "float") {
            float v = 0.0f;
            if (!parse_float(val_str, v)) {
                if (out_error) *out_error = "Invalid float for key: " + key;
                return false;
            }
            entries.insert_or_assign(std::move(key), v);
        } else {
            entries.insert_or_assign(std::move(key), std::string(val_str));
        }
    }

    if (!saw_header && entries.empty()) {
        if (out_error) *out_error = "Empty or non-MNU_1.0 document";
        return false;
    }
    return true;
}

void MnuDocument::merge_from(const MnuDocument& override_doc) {
    for (const auto& [k, v] : override_doc.entries) {
        entries.insert_or_assign(k, v);
    }
}

float MnuDocument::get_float(std::string_view key, float fallback) const {
    auto it = entries.find(key);
    if (it == entries.end()) return fallback;
    if (const auto* f = std::get_if<float>(&it->second)) return *f;
    if (const auto* i = std::get_if<int32_t>(&it->second)) return static_cast<float>(*i);
    return fallback;
}

int32_t MnuDocument::get_int(std::string_view key, int32_t fallback) const {
    auto it = entries.find(key);
    if (it == entries.end()) return fallback;
    if (const auto* i = std::get_if<int32_t>(&it->second)) return *i;
    if (const auto* f = std::get_if<float>(&it->second)) return static_cast<int32_t>(*f);
    return fallback;
}

BackgroundConfig BackgroundConfig::from_document(const MnuDocument& d) {
    BackgroundConfig c;
    c.color1 = {d.get_float("1 RED", c.color1[0]), d.get_float("1 GREEN", c.color1[1]), d.get_float("1 BLUE", c.color1[2])};
    c.color2 = {d.get_float("2 RED", c.color2[0]), d.get_float("2 GREEN", c.color2[1]), d.get_float("2 BLUE", c.color2[2])};
    c.color3 = {d.get_float("3 RED", c.color3[0]), d.get_float("3 GREEN", c.color3[1]), d.get_float("3 BLUE", c.color3[2])};
    c.color4 = {d.get_float("4 RED", c.color4[0]), d.get_float("4 GREEN", c.color4[1]), d.get_float("4 BLUE", c.color4[2])};
    c.fovy             = d.get_float("FOVY", c.fovy);
    c.colour_shader    = d.get_int("COLOUR SHADER", c.colour_shader);
    c.night_blend      = d.get_float("NIGHT BLEND", c.night_blend);
    c.night2day_begin  = d.get_float("NIGHT2DAY BEGIN", c.night2day_begin);
    c.night2day_end    = d.get_float("NIGHT2DAY END", c.night2day_end);
    c.day2night_begin  = d.get_float("DAY2NIGHT BEGIN", c.day2night_begin);
    c.day2night_end    = d.get_float("DAY2NIGHT END", c.day2night_end);
    c.dayspread        = d.get_float("DAYSPREAD", c.dayspread);
    c.night_white_bias = d.get_float("NIGHT WHIT BIAS", c.night_white_bias);
    c.neutrinos_x      = d.get_float("NEUTRINOS X", c.neutrinos_x);
    return c;
}

LineConfig LineConfig::from_document(const MnuDocument& d) {
    LineConfig c;
    c.state        = d.get_int("STATE", c.state);
    c.shader       = d.get_int("SHADER", c.shader);
    c.damping      = d.get_float("DAMPING", c.damping);
    c.length       = d.get_float("LENGTH", c.length);
    c.tension      = d.get_float("TENSION", c.tension);
    c.spacing      = d.get_float("SPACING", c.spacing);
    c.thinness     = d.get_float("THINNESS", c.thinness);
    c.brightness   = d.get_float("BRIGHTNESS", c.brightness);
    c.mipmap_bias  = d.get_float("MIPMAP BIAS", c.mipmap_bias);
    c.fresnel      = d.get_float("FRESNEL", c.fresnel);
    c.falloff      = d.get_float("FALLOFF", c.falloff);
    c.timestep     = d.get_float("TIMESTEP", c.timestep);
    c.perturbation = d.get_float("PERTURBATION", c.perturbation);
    c.pos        = {d.get_float("POS X", c.pos[0]), d.get_float("POS Y", c.pos[1]), d.get_float("POS Z", c.pos[2])};
    c.ang        = {d.get_float("ANG X", c.ang[0]), d.get_float("ANG Y", c.ang[1]), d.get_float("ANG Z", c.ang[2])};
    c.ang_rot    = d.get_float("ANG ROT", c.ang_rot);
    c.end        = {d.get_float("END X", c.end[0]), d.get_float("END Y", c.end[1]), d.get_float("END Z", c.end[2])};
    c.ffd_scale1 = {d.get_float("FFD SCALE1 X", c.ffd_scale1[0]), d.get_float("FFD SCALE1 Y", c.ffd_scale1[1]), d.get_float("FFD SCALE1 Z", c.ffd_scale1[2])};
    c.ffd_scale2 = {d.get_float("FFD SCALE2 X", c.ffd_scale2[0]), d.get_float("FFD SCALE2 Y", c.ffd_scale2[1]), d.get_float("FFD SCALE2 Z", c.ffd_scale2[2])};
    c.ffd_offset = {d.get_float("FFD OFFSET X", c.ffd_offset[0]), d.get_float("FFD OFFSET Y", c.ffd_offset[1]), d.get_float("FFD OFFSET Z", c.ffd_offset[2])};
    c.ffd_param1 = d.get_float("FFD PARAM 1", c.ffd_param1);
    c.show_ffd   = d.get_int("SHOW FFD", c.show_ffd);
    c.ffd_shader = d.get_int("FFD SHADER", c.ffd_shader);
    return c;
}

HdrConfig HdrConfig::from_document(const MnuDocument& d) {
    HdrConfig c;
    c.enabled      = d.get_int("ENABLED", c.enabled);
    c.exposure     = d.get_float("EXPOSURE", c.exposure);
    c.white_level  = d.get_float("WHITE LEVEL", c.white_level);
    c.glare_level  = d.get_float("GLARE LEVEL", c.glare_level);
    c.glare_thresh = d.get_float("GLARE THRESH", c.glare_thresh);
    c.gaussian_rad = {
        d.get_float("GAUSSIAN RAD R", c.gaussian_rad[0]),
        d.get_float("GAUSSIAN RAD G", c.gaussian_rad[1]),
        d.get_float("GAUSSIAN RAD B", c.gaussian_rad[2])};
    c.glare_sum_pow = d.get_float("GLARE SUM POW", c.glare_sum_pow);
    c.tex_size      = d.get_int("TEX SIZE", c.tex_size);
    c.tex_max_mip   = d.get_int("TEX MAX MIP", c.tex_max_mip);
    c.glare         = d.get_int("GLARE", c.glare);
    c.glare_only    = d.get_int("GLARE_ONLY", c.glare_only);
    c.tonebefore    = d.get_int("TONEBEFORE", c.tonebefore);
    c.blur          = d.get_int("BLUR", c.blur);
    c.gamma         = d.get_float("GAMMA", c.gamma);
    c.dither        = d.get_float("DITHER", c.dither);
    return c;
}

ParticlesConfig ParticlesConfig::from_document(const MnuDocument& d) {
    ParticlesConfig c;
    c.emit_vel_min    = d.get_float("emit vel min", c.emit_vel_min);
    c.emit_vel_mul    = d.get_float("emit vel mul", c.emit_vel_mul);
    c.emit_vel_var    = d.get_float("emit vel var", c.emit_vel_var);
    c.emit_cone_angle = d.get_float("emit cone angle", c.emit_cone_angle);
    c.emit_neg_prob   = d.get_float("emit neg prob", c.emit_neg_prob);
    c.emit_vel_zscale = d.get_float("emit vel zscale", c.emit_vel_zscale);
    c.emit_per_frame  = d.get_float("emit per frame", c.emit_per_frame);
    c.emit_prob       = d.get_float("emit prob", c.emit_prob);
    c.aging_speed     = d.get_float("aging speed", c.aging_speed);
    c.aging_variance  = d.get_float("aging variance", c.aging_variance);
    c.friction        = d.get_float("friction", c.friction);
    c.spin_time_scale = d.get_float("spin time scale", c.spin_time_scale);
    c.delta_time      = d.get_float("delta time", c.delta_time);
    c.gravity         = d.get_float("gravity", c.gravity);
    c.wind_dir        = {d.get_float("wind dir x", c.wind_dir[0]), d.get_float("wind dir y", c.wind_dir[1]), d.get_float("wind dir z", c.wind_dir[2])};
    c.wind_scale      = d.get_float("wind scale", c.wind_scale);
    c.wind_scale_10   = d.get_float("wind scale 10", c.wind_scale_10);
    c.brownian_scale  = d.get_float("brownian scale", c.brownian_scale);
    c.spot_pos        = {d.get_float("spot pos x", c.spot_pos[0]), d.get_float("spot pos y", c.spot_pos[1]), d.get_float("spot pos z", c.spot_pos[2])};
    c.spot_attn       = {d.get_float("spot attn x", c.spot_attn[0]), d.get_float("spot attn y", c.spot_attn[1]), d.get_float("spot attn z", c.spot_attn[2])};
    c.specular_power  = d.get_float("specular power", c.specular_power);
    c.specular_coeff  = d.get_float("specular coeff", c.specular_coeff);
    c.lambert_coeff   = d.get_float("lambert coeff", c.lambert_coeff);
    c.exposure        = d.get_float("exposure", c.exposure);
    c.fresnel         = d.get_float("fresnel", c.fresnel);
    c.color_control   = d.get_float("color_control", c.color_control);
    c.iridescent_exp  = d.get_float("iridescent exp", c.iridescent_exp);
    c.global_alpha    = d.get_float("global alpha", c.global_alpha);
    c.size_middle     = d.get_float("size middle", c.size_middle);
    c.size_near       = d.get_float("size near", c.size_near);
    c.size_far        = d.get_float("size far", c.size_far);
    c.near_focus      = d.get_float("near focus", c.near_focus);
    c.near_focus_dist = d.get_float("near focus_dist", c.near_focus_dist);
    c.near_focus_pow  = d.get_float("near focus_pow", c.near_focus_pow);
    c.near_darkness   = d.get_float("near darkness", c.near_darkness);
    c.near_fuzziness  = d.get_float("near fuzziness", c.near_fuzziness);
    c.far_focus       = d.get_float("far focus", c.far_focus);
    c.far_focus_dist  = d.get_float("far focus_dist", c.far_focus_dist);
    c.far_focus_pow   = d.get_float("far focus_pow", c.far_focus_pow);
    c.far_darkness    = d.get_float("far darkness", c.far_darkness);
    c.near_align      = d.get_float("near align", c.near_align);
    c.size_align      = d.get_float("size align", c.size_align);
    c.glare           = d.get_float("glare", c.glare);
    c.glare_scale     = d.get_float("glare scale", c.glare_scale);
    c.glare_p1        = d.get_float("glare p1", c.glare_p1);
    c.glare_p2        = d.get_float("glare p2", c.glare_p2);
    return c;
}

bool XmbThemeConfig::load_from_qrc(
    const QrcArchive& archive,
    XmbThemeConfig& out_config,
    std::string_view override_name,
    std::string* out_error) {

    auto load_merged = [&](std::string_view filename, MnuDocument& doc) -> bool {
        if (archive.has(filename)) {
            if (!doc.parse(archive.read_text(filename), out_error)) {
                return false;
            }
        }
        if (!override_name.empty()) {
            std::string ov_path = "override/";
            ov_path += override_name;
            ov_path += "/";
            ov_path += filename;
            if (archive.has(ov_path)) {
                MnuDocument ov_doc;
                if (!ov_doc.parse(archive.read_text(ov_path), out_error)) {
                    return false;
                }
                doc.merge_from(ov_doc);
            }
        }
        return true;
    };

    MnuDocument bg_doc, line_doc, hdr_doc, part_doc;
    if (!load_merged("BACKGROUND.mnu", bg_doc) ||
        !load_merged("LINE1.mnu", line_doc) ||
        !load_merged("HDR.mnu", hdr_doc) ||
        !load_merged("PARTICLES.mnu", part_doc)) {
        return false;
    }

    out_config.background = BackgroundConfig::from_document(bg_doc);
    out_config.line1 = LineConfig::from_document(line_doc);
    out_config.hdr = HdrConfig::from_document(hdr_doc);
    out_config.particles = ParticlesConfig::from_document(part_doc);
    out_config.active_override = std::string(override_name);
    return true;
}

XmbThemeConfig XmbThemeConfig::lerp(const XmbThemeConfig& a, const XmbThemeConfig& b, float t) {
    const float u = std::clamp(t, 0.0f, 1.0f);
    auto lf = [u](float x, float y) noexcept { return x + (y - x) * u; };
    auto l3 = [u](const std::array<float, 3>& x, const std::array<float, 3>& y) noexcept {
        return std::array<float, 3>{
            x[0] + (y[0] - x[0]) * u,
            x[1] + (y[1] - x[1]) * u,
            x[2] + (y[2] - x[2]) * u,
        };
    };

    XmbThemeConfig out = (u < 0.5f) ? a : b;

    // BackgroundConfig
    out.background.color1 = l3(a.background.color1, b.background.color1);
    out.background.color2 = l3(a.background.color2, b.background.color2);
    out.background.color3 = l3(a.background.color3, b.background.color3);
    out.background.color4 = l3(a.background.color4, b.background.color4);
    out.background.fovy = lf(a.background.fovy, b.background.fovy);
    out.background.night_blend = lf(a.background.night_blend, b.background.night_blend);
    out.background.night2day_begin = lf(a.background.night2day_begin, b.background.night2day_begin);
    out.background.night2day_end = lf(a.background.night2day_end, b.background.night2day_end);
    out.background.day2night_begin = lf(a.background.day2night_begin, b.background.day2night_begin);
    out.background.day2night_end = lf(a.background.day2night_end, b.background.day2night_end);
    out.background.dayspread = lf(a.background.dayspread, b.background.dayspread);
    out.background.night_white_bias = lf(a.background.night_white_bias, b.background.night_white_bias);
    out.background.neutrinos_x = lf(a.background.neutrinos_x, b.background.neutrinos_x);

    // LineConfig
    out.line1.damping = lf(a.line1.damping, b.line1.damping);
    out.line1.length = lf(a.line1.length, b.line1.length);
    out.line1.tension = lf(a.line1.tension, b.line1.tension);
    out.line1.spacing = lf(a.line1.spacing, b.line1.spacing);
    out.line1.thinness = lf(a.line1.thinness, b.line1.thinness);
    out.line1.brightness = lf(a.line1.brightness, b.line1.brightness);
    out.line1.mipmap_bias = lf(a.line1.mipmap_bias, b.line1.mipmap_bias);
    out.line1.fresnel = lf(a.line1.fresnel, b.line1.fresnel);
    out.line1.falloff = lf(a.line1.falloff, b.line1.falloff);
    out.line1.timestep = lf(a.line1.timestep, b.line1.timestep);
    out.line1.perturbation = lf(a.line1.perturbation, b.line1.perturbation);
    out.line1.pos = l3(a.line1.pos, b.line1.pos);
    out.line1.ang = l3(a.line1.ang, b.line1.ang);
    out.line1.ang_rot = lf(a.line1.ang_rot, b.line1.ang_rot);
    out.line1.end = l3(a.line1.end, b.line1.end);
    out.line1.ffd_scale1 = l3(a.line1.ffd_scale1, b.line1.ffd_scale1);
    out.line1.ffd_scale2 = l3(a.line1.ffd_scale2, b.line1.ffd_scale2);
    out.line1.ffd_offset = l3(a.line1.ffd_offset, b.line1.ffd_offset);
    out.line1.ffd_param1 = lf(a.line1.ffd_param1, b.line1.ffd_param1);

    // HdrConfig
    out.hdr.exposure = lf(a.hdr.exposure, b.hdr.exposure);
    out.hdr.white_level = lf(a.hdr.white_level, b.hdr.white_level);
    out.hdr.glare_level = lf(a.hdr.glare_level, b.hdr.glare_level);
    out.hdr.glare_thresh = lf(a.hdr.glare_thresh, b.hdr.glare_thresh);
    out.hdr.gaussian_rad = l3(a.hdr.gaussian_rad, b.hdr.gaussian_rad);
    out.hdr.glare_sum_pow = lf(a.hdr.glare_sum_pow, b.hdr.glare_sum_pow);
    out.hdr.gamma = lf(a.hdr.gamma, b.hdr.gamma);
    out.hdr.dither = lf(a.hdr.dither, b.hdr.dither);

    // ParticlesConfig
    out.particles.emit_vel_min = lf(a.particles.emit_vel_min, b.particles.emit_vel_min);
    out.particles.emit_vel_mul = lf(a.particles.emit_vel_mul, b.particles.emit_vel_mul);
    out.particles.emit_vel_var = lf(a.particles.emit_vel_var, b.particles.emit_vel_var);
    out.particles.emit_cone_angle = lf(a.particles.emit_cone_angle, b.particles.emit_cone_angle);
    out.particles.emit_neg_prob = lf(a.particles.emit_neg_prob, b.particles.emit_neg_prob);
    out.particles.emit_vel_zscale = lf(a.particles.emit_vel_zscale, b.particles.emit_vel_zscale);
    out.particles.emit_per_frame = lf(a.particles.emit_per_frame, b.particles.emit_per_frame);
    out.particles.emit_prob = lf(a.particles.emit_prob, b.particles.emit_prob);
    out.particles.aging_speed = lf(a.particles.aging_speed, b.particles.aging_speed);
    out.particles.aging_variance = lf(a.particles.aging_variance, b.particles.aging_variance);
    out.particles.friction = lf(a.particles.friction, b.particles.friction);
    out.particles.spin_time_scale = lf(a.particles.spin_time_scale, b.particles.spin_time_scale);
    out.particles.delta_time = lf(a.particles.delta_time, b.particles.delta_time);
    out.particles.gravity = lf(a.particles.gravity, b.particles.gravity);
    out.particles.wind_dir = l3(a.particles.wind_dir, b.particles.wind_dir);
    out.particles.wind_scale = lf(a.particles.wind_scale, b.particles.wind_scale);
    out.particles.wind_scale_10 = lf(a.particles.wind_scale_10, b.particles.wind_scale_10);
    out.particles.brownian_scale = lf(a.particles.brownian_scale, b.particles.brownian_scale);
    out.particles.spot_pos = l3(a.particles.spot_pos, b.particles.spot_pos);
    out.particles.spot_attn = l3(a.particles.spot_attn, b.particles.spot_attn);
    out.particles.specular_power = lf(a.particles.specular_power, b.particles.specular_power);
    out.particles.specular_coeff = lf(a.particles.specular_coeff, b.particles.specular_coeff);
    out.particles.lambert_coeff = lf(a.particles.lambert_coeff, b.particles.lambert_coeff);
    out.particles.exposure = lf(a.particles.exposure, b.particles.exposure);
    out.particles.fresnel = lf(a.particles.fresnel, b.particles.fresnel);
    out.particles.color_control = lf(a.particles.color_control, b.particles.color_control);
    out.particles.iridescent_exp = lf(a.particles.iridescent_exp, b.particles.iridescent_exp);
    out.particles.global_alpha = lf(a.particles.global_alpha, b.particles.global_alpha);
    out.particles.size_middle = lf(a.particles.size_middle, b.particles.size_middle);
    out.particles.size_near = lf(a.particles.size_near, b.particles.size_near);
    out.particles.size_far = lf(a.particles.size_far, b.particles.size_far);
    out.particles.near_focus = lf(a.particles.near_focus, b.particles.near_focus);
    out.particles.near_focus_dist = lf(a.particles.near_focus_dist, b.particles.near_focus_dist);
    out.particles.near_focus_pow = lf(a.particles.near_focus_pow, b.particles.near_focus_pow);
    out.particles.near_darkness = lf(a.particles.near_darkness, b.particles.near_darkness);
    out.particles.near_fuzziness = lf(a.particles.near_fuzziness, b.particles.near_fuzziness);
    out.particles.far_focus = lf(a.particles.far_focus, b.particles.far_focus);
    out.particles.far_focus_dist = lf(a.particles.far_focus_dist, b.particles.far_focus_dist);
    out.particles.far_focus_pow = lf(a.particles.far_focus_pow, b.particles.far_focus_pow);
    out.particles.far_darkness = lf(a.particles.far_darkness, b.particles.far_darkness);
    out.particles.near_align = lf(a.particles.near_align, b.particles.near_align);
    out.particles.size_align = lf(a.particles.size_align, b.particles.size_align);
    out.particles.glare = lf(a.particles.glare, b.particles.glare);
    out.particles.glare_scale = lf(a.particles.glare_scale, b.particles.glare_scale);
    out.particles.glare_p1 = lf(a.particles.glare_p1, b.particles.glare_p1);
    out.particles.glare_p2 = lf(a.particles.glare_p2, b.particles.glare_p2);

    return out;
}

bool TimeOfDayRig::load_from_qrc(const QrcArchive& archive, std::string* out_error) {
    loaded = false;
    if (!XmbThemeConfig::load_from_qrc(archive, base, "", out_error)) {
        return false;
    }
    auto load_slot = [&](std::string_view ov, XmbThemeConfig& dst) {
        if (!XmbThemeConfig::load_from_qrc(archive, dst, ov, nullptr)) {
            dst = base;
        }
    };
    load_slot("night", night);
    load_slot("yoake", yoake);
    load_slot("day", day);
    load_slot("higure", higure);
    loaded = true;
    return true;
}

XmbThemeConfig TimeOfDayRig::evaluate(float day_time_0_2400) const {
    if (!loaded) return base;
    // Wrap day_time cleanly into [0, 24) hours
    float h = std::fmod(day_time_0_2400 * 0.01f, 24.0f);
    if (h < 0.0f) h += 24.0f;

    auto smooth = [](float e0, float e1, float x) noexcept {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    // Sony PS3 time-of-day windows:
    //   00:00..04:30 night
    //   04:30..06:30 night -> yoake (dawn)
    //   06:30..08:30 yoake -> day
    //   08:30..17:00 day
    //   17:00..19:00 day -> higure (dusk)
    //   19:00..21:00 higure -> night
    //   21:00..24:00 night
    XmbThemeConfig out;
    if (h < 4.5f) {
        out = night;
        out.active_override = "tod:night";
    } else if (h < 6.5f) {
        out = XmbThemeConfig::lerp(night, yoake, smooth(4.5f, 6.5f, h));
        out.active_override = "tod:night->yoake";
    } else if (h < 8.5f) {
        out = XmbThemeConfig::lerp(yoake, day, smooth(6.5f, 8.5f, h));
        out.active_override = "tod:yoake->day";
    } else if (h < 17.0f) {
        out = day;
        out.active_override = "tod:day";
    } else if (h < 19.0f) {
        out = XmbThemeConfig::lerp(day, higure, smooth(17.0f, 19.0f, h));
        out.active_override = "tod:day->higure";
    } else if (h < 21.0f) {
        out = XmbThemeConfig::lerp(higure, night, smooth(19.0f, 21.0f, h));
        out.active_override = "tod:higure->night";
    } else {
        out = night;
        out.active_override = "tod:night";
    }
    return out;
}

} // namespace xmb::assets
