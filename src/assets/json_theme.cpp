#include "assets/json_theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <variant>

namespace xmb::assets {

namespace {

// Minimal, zero-dependency RFC 8259 JSON DOM parser
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{nullptr};

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<JsonArray>(data); }
    bool is_object() const { return std::holds_alternative<JsonObject>(data); }

    double as_double(double fb = 0.0) const {
        if (auto* v = std::get_if<double>(&data)) return *v;
        if (auto* b = std::get_if<bool>(&data)) return *b ? 1.0 : 0.0;
        return fb;
    }
    int32_t as_int(int32_t fb = 0) const {
        if (auto* v = std::get_if<double>(&data)) return static_cast<int32_t>(std::lround(*v));
        if (auto* b = std::get_if<bool>(&data)) return *b ? 1 : 0;
        return fb;
    }
    const std::string* as_string() const { return std::get_if<std::string>(&data); }
    const JsonArray* as_array() const { return std::get_if<JsonArray>(&data); }
    const JsonObject* as_object() const { return std::get_if<JsonObject>(&data); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : src_(src) {}

    bool parse(JsonValue& out, std::string* err) {
        skip_ws();
        if (!parse_value(out, err)) return false;
        skip_ws();
        if (pos_ != src_.size()) {
            set_err(err, "Trailing characters after JSON root");
            return false;
        }
        return true;
    }

private:
    void set_err(std::string* err, const std::string& msg) const {
        if (err) *err = msg + " at offset " + std::to_string(pos_);
    }

    void skip_ws() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                // Allow // comments for human-edited theme files
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, std::string* err) {
        skip_ws();
        if (pos_ >= src_.size()) { set_err(err, "Unexpected EOF"); return false; }
        const char c = src_[pos_];
        if (c == '{') return parse_object(out, err);
        if (c == '[') return parse_array(out, err);
        if (c == '"') {
            std::string s;
            if (!parse_string(s, err)) return false;
            out.data = std::move(s);
            return true;
        }
        if (c == 't' && src_.substr(pos_, 4) == "true")  { pos_ += 4; out.data = true; return true; }
        if (c == 'f' && src_.substr(pos_, 5) == "false") { pos_ += 5; out.data = false; return true; }
        if (c == 'n' && src_.substr(pos_, 4) == "null")  { pos_ += 4; out.data = nullptr; return true; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number(out, err);
        set_err(err, std::string("Unexpected token '") + c + "'");
        return false;
    }

    bool parse_string(std::string& out, std::string* err) {
        if (pos_ >= src_.size() || src_[pos_] != '"') { set_err(err, "Expected '\"'"); return false; }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) { set_err(err, "Truncated escape"); return false; }
                const char e = src_[pos_++];
                switch (e) {
                    case '"': case '\\': case '/': out.push_back(e); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        set_err(err, "Unterminated string");
        return false;
    }

    bool parse_number(JsonValue& out, std::string* err) {
        const size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string tmp(src_.substr(start, pos_ - start));
        char* end = nullptr;
        const double val = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) { set_err(err, "Invalid number"); return false; }
        out.data = val;
        return true;
    }

    bool parse_array(JsonValue& out, std::string* err) {
        ++pos_; // '['
        JsonArray arr;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; out.data = std::move(arr); return true; }
        while (pos_ < src_.size()) {
            JsonValue elem;
            if (!parse_value(elem, err)) return false;
            arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; out.data = std::move(arr); return true; }
            set_err(err, "Expected ',' or ']' in array");
            return false;
        }
        set_err(err, "Unterminated array");
        return false;
    }

    bool parse_object(JsonValue& out, std::string* err) {
        ++pos_; // '{'
        JsonObject obj;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; out.data = std::move(obj); return true; }
        while (pos_ < src_.size()) {
            skip_ws();
            std::string key;
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') { set_err(err, "Expected ':'"); return false; }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err)) return false;
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; out.data = std::move(obj); return true; }
            set_err(err, "Expected ',' or '}' in object");
            return false;
        }
        set_err(err, "Unterminated object");
        return false;
    }

    std::string_view src_;
    size_t pos_ = 0;
};

bool parse_hex_color(std::string_view s, std::array<float, 3>& out) {
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    if (s.size() != 6) return false;
    auto hex_byte = [](char hi, char lo) -> int {
        auto h = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        const int a = h(hi), b = h(lo);
        return (a < 0 || b < 0) ? -1 : ((a << 4) | b);
    };
    const int r = hex_byte(s[0], s[1]);
    const int g = hex_byte(s[2], s[3]);
    const int b = hex_byte(s[4], s[5]);
    if (r < 0 || g < 0 || b < 0) return false;
    out = {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f};
    return true;
}

bool parse_color3(const JsonValue& v, std::array<float, 3>& out) {
    if (auto* s = v.as_string()) return parse_hex_color(*s, out);
    if (auto* a = v.as_array(); a && a->size() >= 3) {
        out = {
            static_cast<float>((*a)[0].as_double()),
            static_cast<float>((*a)[1].as_double()),
            static_cast<float>((*a)[2].as_double()),
        };
        return true;
    }
    return false;
}

void read_f(const JsonObject& obj, std::string_view key, float& dst) {
    if (auto it = obj.find(key); it != obj.end() && it->second.is_number()) {
        dst = static_cast<float>(it->second.as_double(dst));
    }
}

void read_i(const JsonObject& obj, std::string_view key, int32_t& dst) {
    if (auto it = obj.find(key); it != obj.end() && (it->second.is_number() || it->second.is_bool())) {
        dst = it->second.as_int(dst);
    }
}

void read_v3(const JsonObject& obj, std::string_view key, std::array<float, 3>& dst) {
    if (auto it = obj.find(key); it != obj.end()) {
        parse_color3(it->second, dst);
    }
}

void apply_theme_patch(const JsonObject& root, XmbThemeConfig& cfg) {
    if (auto it = root.find("background"); it != root.end() && it->second.is_object()) {
        const auto& b = *it->second.as_object();
        read_v3(b, "color1", cfg.background.color1);
        read_v3(b, "color2", cfg.background.color2);
        read_v3(b, "color3", cfg.background.color3);
        read_v3(b, "color4", cfg.background.color4);
        read_f(b, "fovy", cfg.background.fovy);
        read_i(b, "colour_shader", cfg.background.colour_shader);
        read_f(b, "night_blend", cfg.background.night_blend);
        read_f(b, "night2day_begin", cfg.background.night2day_begin);
        read_f(b, "night2day_end", cfg.background.night2day_end);
        read_f(b, "day2night_begin", cfg.background.day2night_begin);
        read_f(b, "day2night_end", cfg.background.day2night_end);
        read_f(b, "dayspread", cfg.background.dayspread);
        read_f(b, "night_white_bias", cfg.background.night_white_bias);
        read_f(b, "neutrinos_x", cfg.background.neutrinos_x);
    }
    if (auto it = root.find("line1"); it != root.end() && it->second.is_object()) {
        const auto& l = *it->second.as_object();
        read_i(l, "state", cfg.line1.state);
        read_i(l, "shader", cfg.line1.shader);
        read_f(l, "damping", cfg.line1.damping);
        read_f(l, "length", cfg.line1.length);
        read_f(l, "tension", cfg.line1.tension);
        read_f(l, "spacing", cfg.line1.spacing);
        read_f(l, "thinness", cfg.line1.thinness);
        read_f(l, "brightness", cfg.line1.brightness);
        read_f(l, "mipmap_bias", cfg.line1.mipmap_bias);
        read_f(l, "fresnel", cfg.line1.fresnel);
        read_f(l, "falloff", cfg.line1.falloff);
        read_f(l, "timestep", cfg.line1.timestep);
        read_f(l, "perturbation", cfg.line1.perturbation);
        read_v3(l, "pos", cfg.line1.pos);
        read_v3(l, "ang", cfg.line1.ang);
        read_f(l, "ang_rot", cfg.line1.ang_rot);
        read_v3(l, "end", cfg.line1.end);
        read_v3(l, "ffd_scale1", cfg.line1.ffd_scale1);
        read_v3(l, "ffd_scale2", cfg.line1.ffd_scale2);
        read_v3(l, "ffd_offset", cfg.line1.ffd_offset);
        read_f(l, "ffd_param1", cfg.line1.ffd_param1);
        read_i(l, "show_ffd", cfg.line1.show_ffd);
        read_i(l, "ffd_shader", cfg.line1.ffd_shader);
    }
    if (auto it = root.find("hdr"); it != root.end() && it->second.is_object()) {
        const auto& h = *it->second.as_object();
        read_i(h, "enabled", cfg.hdr.enabled);
        read_f(h, "exposure", cfg.hdr.exposure);
        read_f(h, "white_level", cfg.hdr.white_level);
        read_f(h, "glare_level", cfg.hdr.glare_level);
        read_f(h, "glare_thresh", cfg.hdr.glare_thresh);
        read_v3(h, "gaussian_rad", cfg.hdr.gaussian_rad);
        read_f(h, "glare_sum_pow", cfg.hdr.glare_sum_pow);
        read_i(h, "glare", cfg.hdr.glare);
        read_i(h, "glare_only", cfg.hdr.glare_only);
        read_i(h, "tonebefore", cfg.hdr.tonebefore);
        read_i(h, "blur", cfg.hdr.blur);
        read_i(h, "tex_size", cfg.hdr.tex_size);
        read_i(h, "tex_max_mip", cfg.hdr.tex_max_mip);
        read_f(h, "gamma", cfg.hdr.gamma);
        read_f(h, "dither", cfg.hdr.dither);
    }
    if (auto it = root.find("particles"); it != root.end() && it->second.is_object()) {
        const auto& p = *it->second.as_object();
        read_f(p, "emit_vel_min", cfg.particles.emit_vel_min);
        read_f(p, "emit_vel_mul", cfg.particles.emit_vel_mul);
        read_f(p, "emit_vel_var", cfg.particles.emit_vel_var);
        read_f(p, "emit_cone_angle", cfg.particles.emit_cone_angle);
        read_f(p, "emit_neg_prob", cfg.particles.emit_neg_prob);
        read_f(p, "emit_vel_zscale", cfg.particles.emit_vel_zscale);
        read_f(p, "emit_per_frame", cfg.particles.emit_per_frame);
        read_f(p, "emit_prob", cfg.particles.emit_prob);
        read_f(p, "aging_speed", cfg.particles.aging_speed);
        read_f(p, "aging_variance", cfg.particles.aging_variance);
        read_f(p, "friction", cfg.particles.friction);
        read_f(p, "spin_time_scale", cfg.particles.spin_time_scale);
        read_f(p, "delta_time", cfg.particles.delta_time);
        read_f(p, "gravity", cfg.particles.gravity);
        read_v3(p, "wind_dir", cfg.particles.wind_dir);
        read_f(p, "wind_scale", cfg.particles.wind_scale);
        read_f(p, "wind_scale_10", cfg.particles.wind_scale_10);
        read_f(p, "brownian_scale", cfg.particles.brownian_scale);
        read_v3(p, "spot_pos", cfg.particles.spot_pos);
        read_v3(p, "spot_attn", cfg.particles.spot_attn);
        read_f(p, "specular_power", cfg.particles.specular_power);
        read_f(p, "specular_coeff", cfg.particles.specular_coeff);
        read_f(p, "lambert_coeff", cfg.particles.lambert_coeff);
        read_f(p, "exposure", cfg.particles.exposure);
        read_f(p, "fresnel", cfg.particles.fresnel);
        read_f(p, "color_control", cfg.particles.color_control);
        read_f(p, "iridescent_exp", cfg.particles.iridescent_exp);
        read_f(p, "global_alpha", cfg.particles.global_alpha);
        read_f(p, "size_middle", cfg.particles.size_middle);
        read_f(p, "size_near", cfg.particles.size_near);
        read_f(p, "size_far", cfg.particles.size_far);
        read_f(p, "near_focus", cfg.particles.near_focus);
        read_f(p, "near_focus_dist", cfg.particles.near_focus_dist);
        read_f(p, "near_focus_pow", cfg.particles.near_focus_pow);
        read_f(p, "near_darkness", cfg.particles.near_darkness);
        read_f(p, "near_fuzziness", cfg.particles.near_fuzziness);
        read_f(p, "far_focus", cfg.particles.far_focus);
        read_f(p, "far_focus_dist", cfg.particles.far_focus_dist);
        read_f(p, "far_focus_pow", cfg.particles.far_focus_pow);
        read_f(p, "far_darkness", cfg.particles.far_darkness);
        read_f(p, "near_align", cfg.particles.near_align);
        read_f(p, "size_align", cfg.particles.size_align);
        read_f(p, "glare", cfg.particles.glare);
        read_f(p, "glare_scale", cfg.particles.glare_scale);
        read_f(p, "glare_p1", cfg.particles.glare_p1);
        read_f(p, "glare_p2", cfg.particles.glare_p2);
    }
}

bool parse_color_stops(const JsonValue& v, JsonThemeBundle::ColorStops& out) {
    out.clear();
    if (auto* arr = v.as_array(); arr && !arr->empty()) {
        out.reserve(arr->size());
        for (const auto& item : *arr) {
            std::array<float, 3> c{};
            if (!parse_color3(item, c)) return false;
            out.push_back(c);
        }
        return !out.empty();
    }
    std::array<float, 3> single{};
    if (parse_color3(v, single)) {
        out.push_back(single);
        return true;
    }
    return false;
}

JsonThemeBundle::ColorStops scale_stops(const JsonThemeBundle::ColorStops& src, float factor) {
    JsonThemeBundle::ColorStops out = src;
    for (auto& c : out) {
        c[0] *= factor;
        c[1] *= factor;
        c[2] *= factor;
    }
    return out;
}

std::array<float, 3> sample_stops(const JsonThemeBundle::ColorStops& stops, float t) {
    if (stops.empty()) return {0.0f, 0.0f, 0.0f};
    if (stops.size() == 1) return stops[0];
    const float u = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(stops.size() - 1);
    const size_t i0 = std::min<size_t>(static_cast<size_t>(std::floor(u)), stops.size() - 2);
    const float f = u - static_cast<float>(i0);
    const auto& a = stops[i0];
    const auto& b = stops[i0 + 1];
    return {
        a[0] + (b[0] - a[0]) * f,
        a[1] + (b[1] - a[1]) * f,
        a[2] + (b[2] - a[2]) * f,
    };
}

float sample_scalar_stops(const std::vector<float>& stops, float t) {
    if (stops.empty()) return 0.0f;
    if (stops.size() == 1) return stops[0];
    const float u = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(stops.size() - 1);
    const size_t i0 = std::min<size_t>(static_cast<size_t>(std::floor(u)), stops.size() - 2);
    const float f = u - static_cast<float>(i0);
    return stops[i0] + (stops[i0 + 1] - stops[i0]) * f;
}

bool parse_sky_month(const JsonObject& obj, JsonThemeBundle::SkyMonth& out) {
    bool got_d = false, got_n = false;
    if (auto it = obj.find("day"); it != obj.end()) got_d = parse_color_stops(it->second, out.day);
    if (auto it = obj.find("night"); it != obj.end()) got_n = parse_color_stops(it->second, out.night);
    if (auto it = obj.find("dawn"); it != obj.end()) parse_color_stops(it->second, out.dawn);
    if (auto it = obj.find("dusk"); it != obj.end()) parse_color_stops(it->second, out.dusk);
    if (!got_d && !got_n) return false;
    if (!got_n) out.night = scale_stops(out.day, 0.22f);
    if (!got_d) out.day = out.night;
    return true;
}

uint8_t to_u8(float v) {
    return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

} // namespace

bool JsonThemeBundle::load_from_file(const std::filesystem::path& path, std::string* out_error) {
    std::ifstream ifs(path);
    if (!ifs) {
        if (out_error) *out_error = "Failed to open JSON theme: " + path.string();
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    const bool ok = load_from_string(ss.str(), out_error);
    if (ok && name.empty()) name = path.stem().string();
    return ok;
}

bool JsonThemeBundle::load_from_string(std::string_view json_text, std::string* out_error) {
    *this = {};
    JsonValue root_val;
    JsonParser parser(json_text);
    if (!parser.parse(root_val, out_error)) return false;
    if (!root_val.is_object()) {
        if (out_error) *out_error = "JSON theme root must be an object";
        return false;
    }
    const auto& root = *root_val.as_object();

    if (auto it = root.find("name"); it != root.end() && it->second.is_string()) {
        name = *it->second.as_string();
    }

    XmbThemeConfig base_cfg;
    apply_theme_patch(root, base_cfg);

    // Optional custom 1D Fresnel edge LUT under line1.fresnel_curve
    if (auto lit = root.find("line1"); lit != root.end() && lit->second.is_object()) {
        const auto& lobj = *lit->second.as_object();
        if (auto fit = lobj.find("fresnel_curve"); fit != lobj.end() && !fit->second.is_null()) {
            if (auto* arr = fit->second.as_array(); arr && arr->size() >= 2) {
                fresnel_curve.stops.clear();
                fresnel_curve.stops.reserve(arr->size());
                for (const auto& item : *arr) {
                    if (item.is_number()) fresnel_curve.stops.push_back(static_cast<float>(item.as_double()));
                }
                if (fresnel_curve.stops.size() >= 2) fresnel_curve.enabled = true;
            } else if (auto* obj = fit->second.as_object()) {
                read_f(*obj, "power", fresnel_curve.power);
                read_f(*obj, "bias", fresnel_curve.bias);
                read_f(*obj, "scale", fresnel_curve.scale);
                if (auto iit = obj->find("invert"); iit != obj->end() && iit->second.is_bool()) {
                    fresnel_curve.invert = (iit->second.as_int(0) != 0);
                }
                fresnel_curve.enabled = true;
            }
        }
    }

    // Optional iridescent tint or multi-stop 2D shimmer gradient under particles
    if (auto pit = root.find("particles"); pit != root.end() && pit->second.is_object()) {
        const auto& pobj = *pit->second.as_object();
        if (auto git = pobj.find("iridescent_gradient"); git != pobj.end() && !git->second.is_null()) {
            ColorStops stops;
            if (parse_color_stops(git->second, stops) && stops.size() >= 2) {
                iridescent_gradient = std::move(stops);
            }
        }
        if (auto tit = pobj.find("iridescent_tint"); tit != pobj.end() && !tit->second.is_null()) {
            if (parse_color3(tit->second, iridescent_tint)) {
                has_iridescent_tint = true;
            }
        }
    }

    // Optional sky gradient palette (single day/night/dawn/dusk or 12-month array, 2+ stops each)
    if (auto sit = root.find("sky"); sit != root.end() && sit->second.is_object()) {
        const auto& sobj = *sit->second.as_object();
        if (auto mit = sobj.find("months"); mit != sobj.end() && mit->second.is_array()) {
            const auto& marr = *mit->second.as_array();
            if (!marr.empty()) {
                for (size_t i = 0; i < 12; ++i) {
                    const auto& entry = marr[i % marr.size()];
                    SkyMonth sm{};
                    if (auto* eo = entry.as_object(); eo && parse_sky_month(*eo, sm)) {
                        sky_months[i] = std::move(sm);
                    } else {
                        sm.day = {{0.1f, 0.2f, 0.45f}, {0.25f, 0.45f, 0.78f}};
                        sm.night = {{0.02f, 0.04f, 0.10f}, {0.06f, 0.12f, 0.24f}};
                        sky_months[i] = std::move(sm);
                    }
                }
                has_custom_sky = true;
            }
        } else {
            SkyMonth sm{};
            if (parse_sky_month(sobj, sm)) {
                for (size_t i = 0; i < 12; ++i) sky_months[i] = sm;
                has_custom_sky = true;
            }
        }
    }

    // Named overrides (night / yoake / day / higure / custom)
    if (auto oit = root.find("overrides"); oit != root.end() && oit->second.is_object()) {
        for (const auto& [ov_name, ov_val] : *oit->second.as_object()) {
            if (!ov_val.is_object()) continue;
            XmbThemeConfig ov_cfg = base_cfg;
            apply_theme_patch(*ov_val.as_object(), ov_cfg);
            ov_cfg.active_override = ov_name;
            named_overrides[ov_name] = std::move(ov_cfg);
        }
    }

    // Populate TimeOfDayRig
    tod_rig.base = base_cfg;
    auto pick = [&](std::string_view key) -> XmbThemeConfig {
        if (auto it = named_overrides.find(key); it != named_overrides.end()) return it->second;
        return base_cfg;
    };
    tod_rig.night  = pick("night");
    tod_rig.yoake  = pick("yoake");
    tod_rig.day    = pick("day");
    tod_rig.higure = pick("higure");
    tod_rig.loaded = true;
    return true;
}

std::vector<std::string> JsonThemeBundle::list_overrides() const {
    std::vector<std::string> out;
    out.reserve(named_overrides.size());
    for (const auto& [k, _] : named_overrides) out.push_back(k);
    return out;
}

bool JsonThemeBundle::get_override(std::string_view ov_name, XmbThemeConfig& out_cfg) const {
    if (ov_name.empty()) {
        out_cfg = tod_rig.base;
        return true;
    }
    if (auto it = named_overrides.find(ov_name); it != named_overrides.end()) {
        out_cfg = it->second;
        return true;
    }
    out_cfg = tod_rig.base;
    return false;
}

gfx::Rgba8Image JsonThemeBundle::build_sky_lut(int month, bool is_night) const {
    if (!has_custom_sky) {
        return gfx::generate_procedural_sky_lut(month, is_night);
    }
    const int m = std::clamp(month, 1, 12) - 1;
    const auto& sm = sky_months[m];
    const bool has_diurnal_hues = !is_night && (!sm.dawn.empty() || !sm.dusk.empty());
    const ColorStops& dawn_stops = sm.dawn.empty() ? sm.day : sm.dawn;
    const ColorStops& dusk_stops = sm.dusk.empty() ? sm.day : sm.dusk;

    gfx::Rgba8Image img;
    img.width = 64;
    img.height = 32;
    img.pixels.resize(64u * 32u * 4u);
    for (uint32_t y = 0; y < img.height; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(img.height - 1);
        const auto c_day   = sample_stops(sm.day, v);
        const auto c_night = sample_stops(sm.night, v);
        const auto c_dawn  = has_diurnal_hues ? sample_stops(dawn_stops, v) : c_day;
        const auto c_dusk  = has_diurnal_hues ? sample_stops(dusk_stops, v) : c_day;

        for (uint32_t x = 0; x < img.width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(img.width - 1);
            std::array<float, 3> rgb{};
            if (has_diurnal_hues) {
                // Full 24h hue-shifting ramp along X: 0=night, 0.25=dawn, 0.5=day, 0.75=dusk, 1=night
                auto mix3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
                    const float s = t * t * (3.0f - 2.0f * t);
                    return std::array<float, 3>{
                        a[0] + (b[0] - a[0]) * s,
                        a[1] + (b[1] - a[1]) * s,
                        a[2] + (b[2] - a[2]) * s,
                    };
                };
                if (u < 0.25f) {
                    rgb = mix3(c_night, c_dawn, u / 0.25f);
                } else if (u < 0.50f) {
                    rgb = mix3(c_dawn, c_day, (u - 0.25f) / 0.25f);
                } else if (u < 0.75f) {
                    rgb = mix3(c_day, c_dusk, (u - 0.50f) / 0.25f);
                } else {
                    rgb = mix3(c_dusk, c_night, (u - 0.75f) / 0.25f);
                }
            } else {
                const auto& base = is_night ? c_night : c_day;
                const float diurnal = is_night
                    ? (0.80f + 0.20f * std::sin(u * 3.14159265f))
                    : (0.72f + 0.28f * std::sin(u * 3.14159265f));
                rgb = {base[0] * diurnal, base[1] * diurnal, base[2] * diurnal};
            }
            const size_t idx = (static_cast<size_t>(y) * img.width + x) * 4u;
            img.pixels[idx + 0] = to_u8(rgb[0]);
            img.pixels[idx + 1] = to_u8(rgb[1]);
            img.pixels[idx + 2] = to_u8(rgb[2]);
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

gfx::Rgba8Image JsonThemeBundle::build_fresnel_lut() const {
    if (!fresnel_curve.enabled) {
        return gfx::generate_fresnel_lut();
    }
    gfx::Rgba8Image img;
    img.width = 512;
    img.height = 1;
    img.pixels.resize(512u * 4u);
    for (uint32_t x = 0; x < img.width; ++x) {
        const float u = static_cast<float>(x) / static_cast<float>(img.width - 1);
        float r = 0.0f;
        if (fresnel_curve.stops.size() >= 2) {
            r = sample_scalar_stops(fresnel_curve.stops, u);
        } else {
            const float base = fresnel_curve.invert ? u : (1.0f - u);
            r = std::pow(std::clamp(base, 0.0f, 1.0f), std::max(0.05f, fresnel_curve.power))
                * fresnel_curve.scale + fresnel_curve.bias;
        }
        const float g = 0.25f + 0.75f * std::clamp(r, 0.0f, 1.0f);
        const size_t idx = static_cast<size_t>(x) * 4u;
        img.pixels[idx + 0] = to_u8(r);
        img.pixels[idx + 1] = to_u8(g);
        img.pixels[idx + 2] = 0;
        img.pixels[idx + 3] = 255;
    }
    return img;
}

gfx::Rgba8Image JsonThemeBundle::build_iridescent_lut() const {
    if (iridescent_gradient.size() >= 2) {
        gfx::Rgba8Image img;
        img.width = 128;
        img.height = 128;
        img.pixels.resize(128u * 128u * 4u);
        for (uint32_t y = 0; y < img.height; ++y) {
            const float ny = (static_cast<float>(y) / static_cast<float>(img.height - 1)) * 2.0f - 1.0f;
            for (uint32_t x = 0; x < img.width; ++x) {
                const float nx = (static_cast<float>(x) / static_cast<float>(img.width - 1)) * 2.0f - 1.0f;
                const float rad = std::min(1.0f, std::sqrt(nx * nx + ny * ny));
                const float ang = (std::atan2(ny, nx) + 3.14159265f) / 6.2831853f; // [0..1]
                const float t = std::fmod(rad * 0.80f + ang * 0.20f, 1.0f);
                const auto rgb = sample_stops(iridescent_gradient, t);
                const size_t idx = (static_cast<size_t>(y) * img.width + x) * 4u;
                img.pixels[idx + 0] = to_u8(rgb[0]);
                img.pixels[idx + 1] = to_u8(rgb[1]);
                img.pixels[idx + 2] = to_u8(rgb[2]);
                img.pixels[idx + 3] = 255;
            }
        }
        return img;
    }
    if (!has_iridescent_tint) {
        return gfx::generate_iridescent_lut();
    }
    gfx::Rgba8Image img;
    img.width = 16;
    img.height = 16;
    img.pixels.resize(16u * 16u * 4u);
    const uint8_t r = to_u8(iridescent_tint[0]);
    const uint8_t g = to_u8(iridescent_tint[1]);
    const uint8_t b = to_u8(iridescent_tint[2]);
    for (size_t i = 0; i < 16u * 16u; ++i) {
        img.pixels[i * 4 + 0] = r;
        img.pixels[i * 4 + 1] = g;
        img.pixels[i * 4 + 2] = b;
        img.pixels[i * 4 + 3] = 255;
    }
    return img;
}

} // namespace xmb::assets
