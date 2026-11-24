#include "assets/dds_loader.hpp"
#include "assets/mnu_parser.hpp"
#include "assets/qrc_loader.hpp"
#include "gfx/proc_luts.hpp"
#include "sim/particle_sim.hpp"
#include "sim/ribbon_sim.hpp"

#ifdef XMB_HAS_VULKAN
#include "gfx/vk_renderer.hpp"
#include "wayland_layer.hpp"
#include <SDL3/SDL.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <vector>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct ClockDefaults {
    int month = 1;
    float month_day = 15.0f;
    float day_time = 1200.0f; // 0..2400 (minutes-ish PS3 sky scale: 600=dawn, 1200=noon, 1800=dusk)
};

ClockDefaults local_clock_defaults() {
    ClockDefaults out{};
    const std::time_t now = std::time(nullptr);
    std::tm tm_local{};
    if (localtime_r(&now, &tm_local)) {
        out.month = std::clamp(tm_local.tm_mon + 1, 1, 12);
        out.month_day = std::clamp(static_cast<float>(tm_local.tm_mday), 1.0f, 30.0f);
        out.day_time = static_cast<float>(tm_local.tm_hour) * 100.0f +
                       (static_cast<float>(tm_local.tm_min) + static_cast<float>(tm_local.tm_sec) / 60.0f) * (100.0f / 60.0f);
    }
    return out;
}

int run_inspect(
    const std::string& qrc_path,
    const std::string& override_name,
    int month,
    bool run_benchmark) {
    xmb::assets::QrcArchive archive;
    std::string err;
    if (!archive.load_from_file(qrc_path, &err)) {
        std::cerr << "Error loading QRC: " << err << "\n";
        return 1;
    }

    std::cout << "=== QRC Archive ===\n";
    std::cout << "  Path:       " << qrc_path << "\n";
    std::cout << "  Format:     " << (archive.was_compressed() ? "QRCC (zlib -> QRCF)" : "QRCF (raw)") << "\n";
    std::cout << "  Version:    0x" << std::hex << archive.version() << std::dec << "\n";
    std::cout << "  Files:      " << archive.file_count() << "\n";

    auto overrides = archive.list_overrides();
    std::cout << "  Overrides (" << overrides.size() << "): ";
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << overrides[i];
    }
    std::cout << "\n\n";

    xmb::assets::XmbThemeConfig theme;
    if (!xmb::assets::XmbThemeConfig::load_from_qrc(archive, theme, override_name, &err)) {
        std::cerr << "Error parsing MNU configs: " << err << "\n";
        return 1;
    }

    std::cout << "=== Parsed Theme Config"
              << (override_name.empty() ? " (base)" : (" (override/" + override_name + ")"))
              << " ===\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  BACKGROUND: fovy=" << theme.background.fovy
              << " colour_shader=" << theme.background.colour_shader
              << " night_blend=" << theme.background.night_blend
              << " c1=(" << theme.background.color1[0] << "," << theme.background.color1[1] << "," << theme.background.color1[2] << ")\n";
    std::cout << "  LINE1:      state=" << theme.line1.state
              << " shader=" << theme.line1.shader
              << " ffd_shader=" << theme.line1.ffd_shader
              << " brightness=" << theme.line1.brightness
              << " fresnel=" << theme.line1.fresnel
              << " spacing=" << theme.line1.spacing
              << " timestep=" << theme.line1.timestep << "\n";
    std::cout << "  HDR:        enabled=" << theme.hdr.enabled
              << " exposure=" << theme.hdr.exposure
              << " white_level=" << theme.hdr.white_level
              << " glare_level=" << theme.hdr.glare_level
              << " glare_thresh=" << theme.hdr.glare_thresh
              << " gauss_rgb=(" << theme.hdr.gaussian_rad[0] << "," << theme.hdr.gaussian_rad[1] << "," << theme.hdr.gaussian_rad[2] << ")\n";
    std::cout << "  PARTICLES:  emit_per_frame=" << theme.particles.emit_per_frame
              << " aging_speed=" << theme.particles.aging_speed
              << " gravity=" << theme.particles.gravity
              << " brownian=" << theme.particles.brownian_scale
              << " glare=" << theme.particles.glare << "\n\n";

    const int m1 = std::clamp(month, 1, 12);
    const int m2 = (m1 % 12) + 1;
    char m1_buf[8], m2_buf[8];
    std::snprintf(m1_buf, sizeof(m1_buf), "%02d", m1);
    std::snprintf(m2_buf, sizeof(m2_buf), "%02d", m2);

    std::cout << "=== Month Background DDS Textures (months " << m1_buf << " -> " << m2_buf << ") ===\n";
    for (const std::string& tex_path : {
             std::string("textures/month_bg/rgb/") + m1_buf + ".dds",
             std::string("textures/month_bg/rgb/") + m2_buf + ".dds",
             std::string("textures/month_bg/night/") + m1_buf + ".dds",
             std::string("textures/month_bg/night/") + m2_buf + ".dds",
         }) {
        if (!archive.has(tex_path)) {
            std::cout << "  " << tex_path << ": [not present in archive]\n";
            continue;
        }
        xmb::assets::DdsImage img;
        if (!xmb::assets::load_dds_from_memory(archive.read(tex_path), img, &err)) {
            std::cerr << "  Error loading " << tex_path << ": " << err << "\n";
            return 1;
        }
        const auto& p = img.mips[0].pixels;
        std::cout << "  " << tex_path << ": " << img.width << "x" << img.height
                  << " mips=" << img.mip_levels
                  << " fmt=" << xmb::assets::dds_format_name(img.format)
                  << " px[0]=(" << int(p[0]) << "," << int(p[1]) << "," << int(p[2]) << "," << int(p[3]) << ")\n";
    }

    // Exercise AVX-512 SPU kernel ports headlessly
    xmb::sim::RibbonSim ribbon;
    ribbon.configure(theme.line1);
    ribbon.step(1.0f / 60.0f, 16.0f / 9.0f, theme.background.fovy);

    xmb::sim::ParticleSim particles(xmb::sim::ParticleSim::kDefaultCapacity);
    particles.configure(theme.particles);
    particles.step(1.0f / 60.0f, &ribbon);
    particles.step(1.0f / 60.0f, &ribbon);

    std::cout << "\n=== SPU Kernel Sanity Check (AVX-512) ===\n";
    std::cout << "  RibbonSim (spline.elf):      " << ribbon.vertices().size() << " verts ("
              << ribbon.cols() << "x" << ribbon.rows() << "), "
              << ribbon.indices().size() << " indices\n";
    std::cout << "  ParticleSim (particles.elf): " << particles.active_count() << " active, "
              << particles.vertices().size() << " verts, "
              << particles.indices().size() << " indices\n";

    if (run_benchmark) {
        constexpr int kFrames = 1000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) {
            ribbon.step(1.0f / 60.0f, 16.0f / 9.0f, theme.background.fovy);
        }
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) {
            particles.step(1.0f / 60.0f);
        }
        const auto t2 = std::chrono::steady_clock::now();

        const double ribbon_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kFrames;
        const double particle_us = std::chrono::duration<double, std::micro>(t2 - t1).count() / kFrames;
        std::cout << "\n=== AVX-512 Benchmark (" << kFrames << " frames) ===\n";
        std::cout << std::setprecision(2);
        std::cout << "  RibbonSim (16,384 verts/frame):   " << ribbon_us << " us/frame\n";
        std::cout << "  ParticleSim (1024 max particles): " << particle_us << " us/frame\n";
        std::cout << "  Combined CPU sim time:            " << (ribbon_us + particle_us) << " us/frame\n";
    }
    return 0;
}

void print_usage(const char* argv0) {
    std::cout << "Usage:\n"
              << "  " << argv0 << " --inspect <qrc_path> [--override <name>] [--month <1-12>] [--benchmark]\n"
              << "  " << argv0 << " [--layer-shell] [--qrc] <qrc_path> [--override auto|<name>] [--month auto|<1-12>] [--month-day <1-30>] [--time auto|dawn|noon|dusk|night|<0-24>] [--day-time <0-2400>] [--opacity <0.05-1.0>]\n";
}

bool parse_time_value(std::string_view sv, float& out_day_time, bool& out_is_auto) {
    out_is_auto = false;
    if (sv == "auto") { out_is_auto = true; return true; }
    if (sv == "dawn")  { out_day_time = 550.0f;  return true; }
    if (sv == "noon")  { out_day_time = 1200.0f; return true; }
    if (sv == "dusk")  { out_day_time = 1850.0f; return true; }
    if (sv == "night") { out_day_time = 100.0f;  return true; }
    char* end = nullptr;
    std::string tmp(sv);
    const float v = std::strtof(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    out_day_time = (v <= 24.0f) ? (std::fmod(std::max(0.0f, v), 24.0f) * 100.0f)
                                : std::fmod(std::max(0.0f, v), 2400.0f);
    return true;
}

#ifdef XMB_HAS_VULKAN
volatile std::sig_atomic_t g_layer_stop = 0;
void handle_layer_signal(int) {
    g_layer_stop = 1;
}

struct LiveControlCallbacks {
    std::function<void(int month, bool is_auto)> on_month;
    std::function<void(float day_time, bool is_auto)> on_time;
    std::function<void(float opacity)> on_opacity;
    std::function<void(std::string_view override_name)> on_override; // "auto", "base", or named override
    std::function<void(std::string_view qrc_path)> on_qrc;
};

// Non-blocking UNIX datagram socket for live XMB menu -> particle wind & live preview controls
// Commands:
//   left | right | up | down | shake | wind <nx> <ny> <vx> <vy>
//   month auto|<1..12>
//   time auto|dawn|noon|dusk|night|<0..24>
//   opacity <0.05..1.0>
//   override auto|base|<name>
//   qrc <path>
class WindSocket {
public:
    WindSocket() = default;
    ~WindSocket() { close(); }

    void open() {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        path_ = (xdg && xdg[0] != '\0')
            ? (std::string(xdg) + "/modern-moyou-wind.sock")
            : "/tmp/modern-moyou-wind.sock";
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return;
        ::unlink(path_.c_str());
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void poll(xmb::sim::ParticleSim& particles, const LiveControlCallbacks& cb = {}) {
        if (fd_ < 0) return;
        char buf[512];
        for (int n = 0; n < 32; ++n) {
            ssize_t len = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (len <= 0) break;
            buf[len] = '\0';
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
                buf[--len] = '\0';
            }
            const std::string_view msg(buf, static_cast<size_t>(len));
            if (msg == "left") {
                particles.add_nav_wind(-1.0f, 0.0f);
            } else if (msg == "right") {
                particles.add_nav_wind(1.0f, 0.0f);
            } else if (msg == "up") {
                particles.add_nav_wind(0.0f, 1.0f);
            } else if (msg == "down") {
                particles.add_nav_wind(0.0f, -1.0f);
            } else if (msg == "shake") {
                particles.add_shake_swirl();
            } else if (msg.rfind("wind ", 0) == 0) {
                float nx = 0.5f, ny = 0.5f, vx = 0.0f, vy = 0.0f;
                if (std::sscanf(buf + 5, "%f %f %f %f", &nx, &ny, &vx, &vy) == 4) {
                    particles.add_flow_impulse(nx, ny, vx, vy);
                }
            } else if (msg.rfind("month ", 0) == 0 && cb.on_month) {
                const std::string_view val = msg.substr(6);
                if (val == "auto") {
                    cb.on_month(0, true);
                } else {
                    const int m = std::atoi(buf + 6);
                    if (m >= 1 && m <= 12) cb.on_month(m, false);
                }
            } else if (msg.rfind("time ", 0) == 0 && cb.on_time) {
                float dt_val = 1200.0f;
                bool is_auto = false;
                if (parse_time_value(msg.substr(5), dt_val, is_auto)) {
                    cb.on_time(dt_val, is_auto);
                }
            } else if (msg.rfind("opacity ", 0) == 0 && cb.on_opacity) {
                char* end = nullptr;
                const float op = std::strtof(buf + 8, &end);
                if (end != buf + 8) {
                    cb.on_opacity(std::clamp(op, 0.05f, 1.0f));
                }
            } else if (msg.rfind("override ", 0) == 0 && cb.on_override) {
                cb.on_override(msg.substr(9));
            } else if (msg.rfind("qrc ", 0) == 0 && cb.on_qrc) {
                const std::string_view p = msg.substr(4);
                if (!p.empty()) cb.on_qrc(p);
            }
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            if (!path_.empty()) ::unlink(path_.c_str());
        }
    }

private:
    int fd_ = -1;
    std::string path_;
};
#endif

} // namespace

int main(int argc, char** argv) {
    const ClockDefaults clk = local_clock_defaults();

    bool inspect_mode = false;
    bool benchmark_mode = false;
    bool layer_shell_mode = false;
    std::string qrc_path;
    std::string override_name;
    int month = clk.month;
    float month_day = clk.month_day;
    float day_time = clk.day_time;
    float surface_alpha = 1.0f;
    bool custom_month = false;
    bool custom_day_time = false;
    bool custom_override = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--inspect" && i + 1 < argc) {
            inspect_mode = true;
            qrc_path = argv[++i];
        } else if (arg == "--benchmark") {
            benchmark_mode = true;
        } else if (arg == "--layer-shell") {
            layer_shell_mode = true;
        } else if (arg == "--qrc" && i + 1 < argc) {
            qrc_path = argv[++i];
        } else if (arg == "--opacity" && i + 1 < argc) {
            surface_alpha = std::clamp(std::stof(argv[++i]), 0.05f, 1.0f);
        } else if (arg == "--override" && i + 1 < argc) {
            const std::string_view ov = argv[++i];
            if (ov == "auto") {
                override_name.clear();
                custom_override = false;
            } else {
                override_name = std::string(ov);
                custom_override = true;
            }
        } else if (arg == "--month" && i + 1 < argc) {
            const std::string_view mv = argv[++i];
            if (mv == "auto") {
                custom_month = false;
            } else {
                month = std::clamp(std::stoi(std::string(mv)), 1, 12);
                custom_month = true;
            }
        } else if (arg == "--month-day" && i + 1 < argc) {
            month_day = std::stof(argv[++i]);
            custom_month = true;
        } else if (arg == "--time" && i + 1 < argc) {
            float parsed = day_time;
            bool is_auto = false;
            if (parse_time_value(argv[++i], parsed, is_auto)) {
                custom_day_time = !is_auto;
                if (!is_auto) day_time = parsed;
            }
        } else if (arg == "--day-time" && i + 1 < argc) {
            day_time = std::stof(argv[++i]);
            custom_day_time = true;
        } else if (arg == "--procedural") {
            qrc_path = "procedural";
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (qrc_path.empty()) {
            qrc_path = std::string(arg);
        }
    }

    if (inspect_mode) {
        if (qrc_path.empty() || qrc_path == "procedural") {
            print_usage(argv[0]);
            return 1;
        }
        return run_inspect(qrc_path, override_name, month, benchmark_mode);
    }

#ifndef XMB_HAS_VULKAN
    (void)month_day;
    (void)day_time;
    std::cerr << "Windowed Vulkan renderer was disabled at configure time (missing vulkan-headers).\n"
              << "Install 'vulkan-headers' (`sudo pacman -S vulkan-headers`) and rebuild, or use --inspect <qrc_path>.\n";
    return 1;
#else
    namespace fs = std::filesystem;
    std::string err;

    // Auto-discover ~/.local/share/modern-moyou/ (or legacy modern-xmb/) if no .qrc path was specified;
    // fall back to built-in procedural theme.
    std::vector<std::string> playlist;
    if (qrc_path.empty()) {
        std::string data_root;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] != '\0') {
            data_root = std::string(xdg);
        } else if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
            data_root = std::string(home) + "/.local/share";
        }
        if (!data_root.empty()) {
            for (const char* sub : {"/modern-moyou", "/modern-xmb"}) {
                const std::string data_dir = data_root + sub;
                std::error_code ec;
                if (fs::is_directory(data_dir, ec)) {
                    for (const auto& e : fs::recursive_directory_iterator(data_dir, ec)) {
                        if (e.is_regular_file() && e.path().extension() == ".qrc") {
                            playlist.push_back(e.path().string());
                        }
                    }
                }
                if (!playlist.empty()) break;
            }
        }
        std::sort(playlist.begin(), playlist.end());
        if (playlist.empty()) {
            playlist.emplace_back("procedural");
        }
    } else if (qrc_path == "procedural") {
        playlist.emplace_back("procedural");
    } else {
        const fs::path in(qrc_path);
        std::error_code ec;
        const fs::path root = fs::is_directory(in, ec) ? in : in.parent_path();
        if (fs::is_directory(in, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
                if (e.is_regular_file() && e.path().extension() == ".qrc") playlist.push_back(e.path().string());
            }
        } else {
            for (const auto& e : fs::directory_iterator(root, ec)) {
                if (e.is_regular_file() && e.path().extension() == ".qrc") playlist.push_back(e.path().string());
            }
            if (playlist.empty()) playlist.push_back(qrc_path);
        }
        std::sort(playlist.begin(), playlist.end());
        if (playlist.empty()) {
            std::cerr << "No .qrc files found at " << qrc_path << ", falling back to built-in procedural theme.\n";
            playlist.emplace_back("procedural");
        }
    }

    size_t cur = 0;
    if (!qrc_path.empty() && qrc_path != "procedural" && !fs::is_directory(fs::path(qrc_path))) {
        const auto it = std::find(playlist.begin(), playlist.end(), qrc_path);
        if (it != playlist.end()) cur = static_cast<size_t>(it - playlist.begin());
    }

    auto make_procedural_tod_rig = []() -> xmb::assets::TimeOfDayRig {
        xmb::assets::TimeOfDayRig rig;
        rig.base = xmb::assets::XmbThemeConfig{};
        rig.day = rig.base;
        rig.yoake = rig.base;
        rig.higure = rig.base;
        rig.night = rig.base;
        rig.night.line1.brightness = 0.90f;
        rig.night.line1.fresnel = 0.52f;
        rig.night.line1.ang_rot = 10.0f;
        rig.night.line1.ffd_param1 = -1.83f;
        rig.night.hdr.exposure = 1.38f;
        rig.night.hdr.white_level = 1.00f;
        rig.night.hdr.glare_level = 1.30f;
        rig.loaded = true;
        return rig;
    };

    if (layer_shell_mode) {
        std::signal(SIGINT, handle_layer_signal);
        std::signal(SIGTERM, handle_layer_signal);

        xmb::platform::WaylandLayerSurface layer;
        if (!layer.init("omarchy-xmb-bg", &err)) {
            std::cerr << "WaylandLayerSurface::init failed: " << err << "\n";
            return 1;
        }

        xmb::gfx::VkRenderer renderer;
        if (!renderer.init_wayland(layer.display(), layer.surface(), layer.width(), layer.height(), &err)) {
            std::cerr << "VkRenderer::init_wayland failed: " << err << "\n";
            layer.shutdown();
            return 1;
        }

        bool is_procedural = (playlist[cur] == "procedural");
        xmb::assets::QrcArchive archive;
        xmb::assets::TimeOfDayRig tod_rig;
        xmb::assets::XmbThemeConfig theme;
        if (is_procedural) {
            tod_rig = make_procedural_tod_rig();
            theme = tod_rig.evaluate(day_time);
        } else {
            if (!archive.load_from_file(playlist[cur], &err)) {
                std::cerr << "Failed to load " << playlist[cur] << ": " << err << "\n";
                renderer.shutdown();
                layer.shutdown();
                return 1;
            }
            if (custom_override) {
                if (!xmb::assets::XmbThemeConfig::load_from_qrc(archive, theme, override_name, &err)) {
                    std::cerr << "Failed to parse theme: " << err << "\n";
                    renderer.shutdown();
                    layer.shutdown();
                    return 1;
                }
            } else {
                if (!tod_rig.load_from_qrc(archive, &err)) {
                    std::cerr << "Failed to parse time-of-day overrides: " << err << "\n";
                    renderer.shutdown();
                    layer.shutdown();
                    return 1;
                }
                theme = tod_rig.evaluate(day_time);
            }
        }

        auto reload_layer_luts = [&](int m) {
            const int m1 = std::clamp(m, 1, 12);
            const int m2 = (m1 % 12) + 1;
            if (is_procedural) {
                renderer.upload_background_luts_rgba8(
                    xmb::gfx::generate_procedural_sky_lut(m1, false),
                    xmb::gfx::generate_procedural_sky_lut(m2, false),
                    xmb::gfx::generate_procedural_sky_lut(m1, true),
                    xmb::gfx::generate_procedural_sky_lut(m2, true),
                    &err);
                return;
            }
            char b1[8], b2[8];
            std::snprintf(b1, sizeof(b1), "%02d", m1);
            std::snprintf(b2, sizeof(b2), "%02d", m2);
            auto load_dds = [&](const std::string& p, xmb::assets::DdsImage& out) {
                return archive.has(p) && xmb::assets::load_dds_from_memory(archive.read(p), out, &err);
            };
            xmb::assets::DdsImage d1, d2, n1, n2;
            if (load_dds(std::string("textures/month_bg/rgb/") + b1 + ".dds", d1) &&
                load_dds(std::string("textures/month_bg/rgb/") + b2 + ".dds", d2) &&
                load_dds(std::string("textures/month_bg/night/") + b1 + ".dds", n1) &&
                load_dds(std::string("textures/month_bg/night/") + b2 + ".dds", n2)) {
                renderer.upload_background_luts(d1, d2, n1, n2, &err);
            }
        };
        reload_layer_luts(month);
        auto reload_layer_tgas = [&]() {
            if (!is_procedural && archive.has("textures/TGA/freslut1.tga")) {
                xmb::gfx::Rgba8Image img;
                if (xmb::gfx::load_tga_rgba8(archive.read("textures/TGA/freslut1.tga"), img, &err)) {
                    renderer.upload_fresnel_lut(img, &err);
                }
            } else {
                renderer.upload_fresnel_lut(xmb::gfx::generate_fresnel_lut(), &err);
            }
            if (!is_procedural && archive.has("particles/proc_iridescent.tga")) {
                xmb::gfx::Rgba8Image img;
                if (xmb::gfx::load_tga_rgba8(archive.read("particles/proc_iridescent.tga"), img, &err)) {
                    renderer.upload_iridescent_lut(img, &err);
                }
            } else {
                renderer.upload_iridescent_lut(xmb::gfx::generate_iridescent_lut(), &err);
            }
        };
        reload_layer_tgas();

        xmb::sim::RibbonSim ribbon;
        ribbon.configure(theme.line1);
        xmb::sim::ParticleSim particles(xmb::sim::ParticleSim::kDefaultCapacity);
        particles.configure(theme.particles);
        WindSocket wind_sock;
        wind_sock.open();

        const float init_aspect = (layer.height() > 0)
            ? (static_cast<float>(layer.width()) / static_cast<float>(layer.height()))
            : (16.0f / 9.0f);
        // Pre-warm ribbon & particles so frame 1 opens with a lush, populated wave
        for (int warm = 0; warm < 45; ++warm) {
            ribbon.step(1.0f / 60.0f, init_aspect, theme.background.fovy);
            particles.step(1.0f / 60.0f, &ribbon);
        }

        auto last_time = std::chrono::steady_clock::now();
        float elapsed = 0.0f;
        float clock_sync_accum = 0.0f;
        bool auto_month = !custom_month;
        bool auto_time = !custom_day_time;
        bool auto_override = !custom_override;

        auto apply_layer_theme = [&]() {
            if (auto_override) {
                theme = tod_rig.evaluate(day_time);
            } else {
                xmb::assets::XmbThemeConfig t;
                if (xmb::assets::XmbThemeConfig::load_from_qrc(archive, t, override_name, &err)) {
                    theme = t;
                }
            }
            ribbon.update_params(theme.line1);
            particles.update_params(theme.particles);
        };

        LiveControlCallbacks layer_cb{};
        layer_cb.on_month = [&](int m, bool is_auto) {
            auto_month = is_auto;
            const int next_m = is_auto ? local_clock_defaults().month : m;
            if (is_auto) month_day = local_clock_defaults().month_day;
            if (next_m != month) {
                month = next_m;
                reload_layer_luts(month);
            }
        };
        layer_cb.on_time = [&](float dt_val, bool is_auto) {
            auto_time = is_auto;
            day_time = is_auto ? local_clock_defaults().day_time : dt_val;
            if (auto_override) apply_layer_theme();
        };
        layer_cb.on_opacity = [&](float op) {
            surface_alpha = op;
        };
        layer_cb.on_override = [&](std::string_view ov) {
            if (ov == "auto") {
                auto_override = true;
                override_name.clear();
            } else if (ov == "base") {
                auto_override = false;
                override_name.clear();
            } else {
                auto_override = false;
                override_name = std::string(ov);
            }
            apply_layer_theme();
        };
        layer_cb.on_qrc = [&](std::string_view p) {
            if (p == "procedural") {
                is_procedural = true;
                tod_rig = make_procedural_tod_rig();
                reload_layer_luts(month);
                reload_layer_tgas();
                apply_layer_theme();
                return;
            }
            xmb::assets::QrcArchive next_arch;
            if (!next_arch.load_from_file(std::string(p), &err)) return;
            xmb::assets::TimeOfDayRig next_rig;
            if (!next_rig.load_from_qrc(next_arch, &err)) return;
            is_procedural = false;
            archive = std::move(next_arch);
            tod_rig = std::move(next_rig);
            reload_layer_luts(month);
            reload_layer_tgas();
            apply_layer_theme();
        };

        float fade_out_elapsed = -1.0f;
        constexpr float kFadeInDuration = 0.20f;
        constexpr float kFadeOutDuration = 0.18f;
        while (layer.poll_events()) {
            if (g_layer_stop && fade_out_elapsed < 0.0f) {
                fade_out_elapsed = 0.0f;
            }
            if (layer.consume_resized()) {
                renderer.notify_external_resize(layer.width(), layer.height());
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.05f, std::chrono::duration<float>(now - last_time).count());
            last_time = now;
            elapsed += dt;
            if (auto_month || auto_time) {
                clock_sync_accum += dt;
                if (clock_sync_accum >= 1.0f) {
                    clock_sync_accum = 0.0f;
                    const ClockDefaults cur_clk = local_clock_defaults();
                    if (auto_time) {
                        day_time = cur_clk.day_time;
                        if (auto_override) apply_layer_theme();
                    }
                    if (auto_month) {
                        month_day = cur_clk.month_day;
                        if (cur_clk.month != month) {
                            month = cur_clk.month;
                            reload_layer_luts(month);
                        }
                    }
                }
            }
            if (fade_out_elapsed >= 0.0f) {
                fade_out_elapsed += dt;
                if (fade_out_elapsed >= kFadeOutDuration) {
                    break;
                }
            }

            const float t_in = std::clamp(elapsed / kFadeInDuration, 0.0f, 1.0f);
            const float ease_in = 1.0f - (1.0f - t_in) * (1.0f - t_in) * (1.0f - t_in);
            float ease_out = 1.0f;
            if (fade_out_elapsed >= 0.0f) {
                const float t_out = std::clamp(fade_out_elapsed / kFadeOutDuration, 0.0f, 1.0f);
                ease_out = (1.0f - t_out) * (1.0f - t_out);
            }

            const float aspect = (layer.height() > 0)
                ? (static_cast<float>(layer.width()) / static_cast<float>(layer.height()))
                : (16.0f / 9.0f);
            wind_sock.poll(particles, layer_cb);
            ribbon.step(dt, aspect, theme.background.fovy);
            particles.step(dt, &ribbon);

            xmb::gfx::FrameSceneData scene{};
            scene.theme = &theme;
            scene.ribbon_vertices = ribbon.vertices();
            scene.ribbon_uvs = ribbon.uvs();
            scene.ribbon_indices = ribbon.indices();
            scene.particle_vertices = particles.vertices();
            scene.particle_indices = particles.indices();
            scene.time_seconds = elapsed;
            scene.day_time = day_time;
            scene.month_time = month_day;
            scene.surface_alpha = surface_alpha * ease_in * ease_out;
            if (!renderer.render_frame(scene, &err)) {
                std::cerr << "render_frame error: " << err << "\n";
                break;
            }
        }

        renderer.shutdown();
        layer.shutdown();
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("modern-moyou", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    xmb::gfx::VkRenderer renderer;
    if (!renderer.init(window, &err)) {
        std::cerr << "VkRenderer::init failed: " << err << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    xmb::assets::QrcArchive archive;
    xmb::assets::TimeOfDayRig tod_rig;
    xmb::assets::XmbThemeConfig theme;
    xmb::sim::RibbonSim ribbon;
    xmb::sim::ParticleSim particles(xmb::sim::ParticleSim::kDefaultCapacity);
    std::vector<std::string> overrides;
    int override_idx = custom_override ? -1 : -2; // -2 = auto time-of-day, -1 = base, >=0 = specific override
    bool initial_override_pending = custom_override && !override_name.empty();
    bool live_clock = !(custom_month || custom_day_time);

    auto reload_luts = [&]() {
        const int m1 = std::clamp(month, 1, 12);
        const int m2 = (m1 % 12) + 1;
        if (playlist[cur] == "procedural") {
            renderer.upload_background_luts_rgba8(
                xmb::gfx::generate_procedural_sky_lut(m1, false),
                xmb::gfx::generate_procedural_sky_lut(m2, false),
                xmb::gfx::generate_procedural_sky_lut(m1, true),
                xmb::gfx::generate_procedural_sky_lut(m2, true),
                &err);
            return;
        }
        char b1[8], b2[8];
        std::snprintf(b1, sizeof(b1), "%02d", m1);
        std::snprintf(b2, sizeof(b2), "%02d", m2);
        auto load = [&](const std::string& p, xmb::assets::DdsImage& out) {
            return archive.has(p) && xmb::assets::load_dds_from_memory(archive.read(p), out, &err);
        };
        xmb::assets::DdsImage d1, d2, n1, n2;
        if (load(std::string("textures/month_bg/rgb/") + b1 + ".dds", d1) &&
            load(std::string("textures/month_bg/rgb/") + b2 + ".dds", d2) &&
            load(std::string("textures/month_bg/night/") + b1 + ".dds", n1) &&
            load(std::string("textures/month_bg/night/") + b2 + ".dds", n2)) {
            renderer.upload_background_luts(d1, d2, n1, n2, &err);
        } else {
            std::cerr << "Warning: failed to load month_bg DDS LUTs (" << err << ")\n";
        }
    };

    auto reload_all = [&]() -> bool {
        if (playlist[cur] == "procedural") {
            overrides.clear();
            override_idx = -2;
            tod_rig = make_procedural_tod_rig();
            theme = tod_rig.evaluate(day_time);
            reload_luts();
            renderer.upload_fresnel_lut(xmb::gfx::generate_fresnel_lut(), &err);
            renderer.upload_iridescent_lut(xmb::gfx::generate_iridescent_lut(), &err);
            ribbon.configure(theme.line1);
            particles.configure(theme.particles);
            return true;
        }
        xmb::assets::QrcArchive a;
        if (!a.load_from_file(playlist[cur], &err)) {
            std::cerr << "Failed to load " << playlist[cur] << ": " << err << "\n";
            return false;
        }
        archive = std::move(a);
        overrides = archive.list_overrides();
        if (initial_override_pending) {
            initial_override_pending = false;
            const auto it = std::find(overrides.begin(), overrides.end(), override_name);
            if (it != overrides.end()) override_idx = static_cast<int>(it - overrides.begin());
        }
        if (override_idx >= static_cast<int>(overrides.size())) override_idx = -2;
        if (!tod_rig.load_from_qrc(archive, &err)) {
            std::cerr << "Failed to parse time-of-day overrides: " << err << "\n";
            return false;
        }
        if (override_idx == -2) {
            theme = tod_rig.evaluate(day_time);
        } else {
            const std::string ov = (override_idx >= 0) ? overrides[override_idx] : std::string();
            xmb::assets::XmbThemeConfig t;
            if (!xmb::assets::XmbThemeConfig::load_from_qrc(archive, t, ov, &err)) {
                std::cerr << "Failed to parse theme: " << err << "\n";
                return false;
            }
            theme = t;
        }
        reload_luts();
        if (archive.has("textures/TGA/freslut1.tga")) {
            xmb::gfx::Rgba8Image img;
            if (xmb::gfx::load_tga_rgba8(archive.read("textures/TGA/freslut1.tga"), img, &err)) {
                renderer.upload_fresnel_lut(img, &err);
            }
        } else {
            renderer.upload_fresnel_lut(xmb::gfx::generate_fresnel_lut(), &err);
        }
        if (archive.has("particles/proc_iridescent.tga")) {
            xmb::gfx::Rgba8Image img;
            if (xmb::gfx::load_tga_rgba8(archive.read("particles/proc_iridescent.tga"), img, &err)) {
                renderer.upload_iridescent_lut(img, &err);
            }
        } else {
            renderer.upload_iridescent_lut(xmb::gfx::generate_iridescent_lut(), &err);
        }
        ribbon.configure(theme.line1);
        particles.configure(theme.particles);
        return true;
    };

    if (!reload_all()) {
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::cout << "Controls:\n"
                 "  Left/Right  prev/next ribbon (.qrc)     PgUp/PgDn  jump 10 ribbons\n"
                 "  Up/Down     month +/-                   , / .      time of day -/+ 50\n"
                 "  T           toggle day/night time-lapse  O          cycle override (auto-ToD/base/day/night...)\n"
                 "  C           re-sync to live system clock D          toggle on-screen debug HUD\n"
                 "  I           print full info to terminal  Space      pause\n"
                 "  R           reset simulation             F          fullscreen    Esc  quit\n"
              << playlist.size() << " ribbons loaded.\n";

    auto current_override_label = [&]() -> const char* {
        if (override_idx == -2) return theme.active_override.empty() ? "tod" : theme.active_override.c_str();
        if (override_idx >= 0) return overrides[override_idx].c_str();
        return "base";
    };

    auto print_current_info = [&](bool verbose) {
        const char* ov_str = current_override_label();
        std::cout << "[" << (cur + 1) << "/" << playlist.size() << "] "
                  << fs::path(playlist[cur]).filename().string()
                  << " | ov=" << ov_str << " | m=" << month << " t=" << static_cast<int>(day_time)
                  << " | particles=" << particles.active_count() << "\n";
        if (verbose) {
            std::cout << "  path: " << playlist[cur] << "\n"
                      << "  BG:   fovy=" << theme.background.fovy
                      << " c1=(" << theme.background.color1[0] << "," << theme.background.color1[1] << "," << theme.background.color1[2] << ")"
                      << " c3=(" << theme.background.color3[0] << "," << theme.background.color3[1] << "," << theme.background.color3[2] << ")"
                      << " c4=(" << theme.background.color4[0] << "," << theme.background.color4[1] << "," << theme.background.color4[2] << ")"
                      << " night_blend=" << theme.background.night_blend << "\n"
                      << "  LINE: spacing=" << theme.line1.spacing << " thin=" << theme.line1.thinness
                      << " bright=" << theme.line1.brightness << " fres=" << theme.line1.fresnel
                      << " ffd_shader=" << theme.line1.ffd_shader << "\n"
                      << "  HDR:  exp=" << theme.hdr.exposure << " white=" << theme.hdr.white_level
                      << " glare=" << theme.hdr.glare_level << " thresh=" << theme.hdr.glare_thresh << "\n"
                      << "  PART: emit=" << theme.particles.emit_per_frame << " prob=" << theme.particles.emit_prob
                      << " size=(" << theme.particles.size_middle << "," << theme.particles.size_near << "," << theme.particles.size_far << ")\n";
        }
        std::cout.flush();
    };
    print_current_info(false);

    bool running = true, paused = false, timelapse = false, show_hud = false;
    WindSocket wind_sock;
    wind_sock.open();
    auto last_time = std::chrono::steady_clock::now();
    float elapsed = 0.0f;
    float clock_sync_accum = 0.0f;
    bool title_dirty = true;

    auto step_playlist = [&](int delta) {
        const int n = static_cast<int>(playlist.size());
        const size_t old = cur;
        cur = static_cast<size_t>(((static_cast<int>(cur) + delta) % n + n) % n);
        if (!reload_all()) cur = old, reload_all();
        title_dirty = true;
        print_current_info(false);
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_WINDOW_RESIZED ||
                       ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                       ev.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                renderer.notify_resize();
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_RIGHT: step_playlist(1); particles.add_nav_wind(1.0f, 0.0f); break;
                    case SDLK_LEFT: step_playlist(-1); particles.add_nav_wind(-1.0f, 0.0f); break;
                    case SDLK_PAGEDOWN: step_playlist(10); particles.add_nav_wind(1.0f, 0.0f); break;
                    case SDLK_PAGEUP: step_playlist(-10); particles.add_nav_wind(-1.0f, 0.0f); break;
                    case SDLK_UP: live_clock = false; month = (month % 12) + 1; reload_luts(); particles.add_nav_wind(0.0f, 1.0f); title_dirty = true; break;
                    case SDLK_DOWN: live_clock = false; month = ((month + 10) % 12) + 1; reload_luts(); particles.add_nav_wind(0.0f, -1.0f); title_dirty = true; break;
                    case SDLK_PERIOD: live_clock = false; day_time = std::fmod(day_time + 50.0f, 2400.0f); title_dirty = true; break;
                    case SDLK_COMMA: live_clock = false; day_time = std::fmod(day_time + 2400.0f - 50.0f, 2400.0f); title_dirty = true; break;
                    case SDLK_T: timelapse = !timelapse; if (timelapse) live_clock = false; title_dirty = true; break;
                    case SDLK_C: {
                        live_clock = true;
                        timelapse = false;
                        const ClockDefaults cur_clk = local_clock_defaults();
                        day_time = cur_clk.day_time;
                        month_day = cur_clk.month_day;
                        if (cur_clk.month != month) { month = cur_clk.month; reload_luts(); }
                        override_idx = -2;
                        title_dirty = true;
                        break;
                    }
                    case SDLK_SPACE: paused = !paused; title_dirty = true; break;
                    case SDLK_R: ribbon.configure(theme.line1); particles.configure(theme.particles); break;
                    case SDLK_D: show_hud = !show_hud; break;
                    case SDLK_I: print_current_info(true); break;
                    case SDLK_O:
                        override_idx = (override_idx + 3) % (static_cast<int>(overrides.size()) + 2) - 2;
                        reload_all();
                        title_dirty = true;
                        print_current_info(false);
                        break;
                    case SDLK_F: {
                        const bool fsn = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0;
                        SDL_SetWindowFullscreen(window, fsn);
                        break;
                    }
                    default: break;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        elapsed += dt;

        if (timelapse) {
            day_time = std::fmod(day_time + dt * 60.0f, 2400.0f);
            title_dirty = true;
        } else if (live_clock) {
            clock_sync_accum += dt;
            if (clock_sync_accum >= 1.0f) {
                clock_sync_accum = 0.0f;
                const ClockDefaults cur_clk = local_clock_defaults();
                if (static_cast<int>(cur_clk.day_time) != static_cast<int>(day_time)) {
                    title_dirty = true;
                }
                day_time = cur_clk.day_time;
                month_day = cur_clk.month_day;
                if (cur_clk.month != month) {
                    month = cur_clk.month;
                    reload_luts();
                    title_dirty = true;
                }
            }
        }
        if (override_idx == -2) {
            theme = tod_rig.evaluate(day_time);
            ribbon.update_params(theme.line1);
            particles.update_params(theme.particles);
        }
        if (title_dirty) {
            char t[512];
            std::snprintf(t, sizeof(t), "modern-moyou [%zu/%zu] %s | %s | month %d | time %04d%s%s%s",
                          cur + 1, playlist.size(), fs::path(playlist[cur]).filename().string().c_str(),
                          current_override_label(),
                          month, static_cast<int>(day_time),
                          live_clock ? " (live)" : "", timelapse ? " (lapse)" : "", paused ? " (paused)" : "");
            SDL_SetWindowTitle(window, t);
            title_dirty = false;
        }

        int win_w = 1280, win_h = 720;
        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
        const float aspect = (win_h > 0) ? (static_cast<float>(win_w) / static_cast<float>(win_h)) : (16.0f / 9.0f);

        if (!paused) {
            LiveControlCallbacks win_cb{};
            win_cb.on_month = [&](int m, bool is_auto) {
                live_clock = is_auto;
                const int next_m = is_auto ? local_clock_defaults().month : m;
                if (is_auto) month_day = local_clock_defaults().month_day;
                if (next_m != month) { month = next_m; reload_luts(); }
                title_dirty = true;
            };
            win_cb.on_time = [&](float dt_val, bool is_auto) {
                live_clock = is_auto;
                timelapse = false;
                day_time = is_auto ? local_clock_defaults().day_time : dt_val;
                title_dirty = true;
            };
            win_cb.on_opacity = [&](float op) { surface_alpha = op; };
            win_cb.on_override = [&](std::string_view ov) {
                if (ov == "auto") {
                    override_idx = -2;
                } else if (ov == "base") {
                    override_idx = -1;
                } else {
                    const auto it = std::find(overrides.begin(), overrides.end(), std::string(ov));
                    if (it != overrides.end()) override_idx = static_cast<int>(it - overrides.begin());
                }
                reload_all();
                title_dirty = true;
            };
            win_cb.on_qrc = [&](std::string_view p) {
                const std::string sp(p);
                const auto it = std::find(playlist.begin(), playlist.end(), sp);
                if (it != playlist.end()) {
                    cur = static_cast<size_t>(it - playlist.begin());
                } else {
                    playlist.push_back(sp);
                    cur = playlist.size() - 1;
                }
                reload_all();
                title_dirty = true;
            };
            wind_sock.poll(particles, win_cb);
            ribbon.step(dt, aspect, theme.background.fovy);
            particles.step(dt, &ribbon);
        }

        xmb::gfx::FrameSceneData scene{};
        scene.theme = &theme;
        scene.ribbon_vertices = ribbon.vertices();
        scene.ribbon_uvs = ribbon.uvs();
        scene.ribbon_indices = ribbon.indices();
        scene.particle_vertices = particles.vertices();
        scene.particle_indices = particles.indices();
        scene.time_seconds = elapsed;
        scene.day_time = day_time;
        scene.month_time = month_day;
        if (show_hud) {
            char line[256];
            std::snprintf(line, sizeof(line), "[%zu/%zu] %s  (%s)",
                          cur + 1, playlist.size(),
                          fs::path(playlist[cur]).filename().string().c_str(), current_override_label());
            scene.hud_lines.emplace_back(line);
            std::snprintf(line, sizeof(line), "Month %d | Time %04d | Particles %u/2048 | FOVY %.1f%s%s",
                          month, static_cast<int>(day_time), particles.active_count(),
                          theme.background.fovy, timelapse ? " [LAPSE]" : "", paused ? " [PAUSED]" : "");
            scene.hud_lines.emplace_back(line);
            std::snprintf(line, sizeof(line), "LINE1: bright=%.2f fres=%.2f space=%.1f thin=%.2f ffd=%d",
                          theme.line1.brightness, theme.line1.fresnel, theme.line1.spacing,
                          theme.line1.thinness, theme.line1.ffd_shader);
            scene.hud_lines.emplace_back(line);
            std::snprintf(line, sizeof(line), "HDR: exp=%.2f white=%.2f glare=%.2f thresh=%.2f tonebefore=%d",
                          theme.hdr.exposure, theme.hdr.white_level, theme.hdr.glare_level,
                          theme.hdr.glare_thresh, theme.hdr.tonebefore);
            scene.hud_lines.emplace_back(line);
        }

        if (!renderer.render_frame(scene, &err)) {
            std::cerr << "render_frame error: " << err << "\n";
            break;
        }
    }

    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
#endif
}
