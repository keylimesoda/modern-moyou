#include "assets/dds_loader.hpp"
#include "assets/mnu_parser.hpp"
#include "assets/qrc_loader.hpp"
#include "gfx/proc_luts.hpp"
#include "sim/particle_sim.hpp"
#include "sim/ribbon_sim.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[FAIL] line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

} // namespace

int main() {
    const std::filesystem::path qrc_dir = std::filesystem::path(XMB_ASSETS_DIR) / "qrc";
    const std::filesystem::path dblue = qrc_dir / "dblue_day.qrc";

    // 1-4. QRC archive, MNU override, and DDS checks (run when local .qrc assets exist)
    xmb::assets::XmbThemeConfig base_cfg;
    std::string err;
    if (std::filesystem::exists(dblue)) {
        xmb::assets::QrcArchive archive;
        CHECK(archive.load_from_file(dblue, &err));
        CHECK(archive.was_compressed());
        CHECK(archive.file_count() > 100);
        CHECK(archive.has("BACKGROUND.mnu"));
        CHECK(archive.has("LINE1.mnu"));
        CHECK(archive.has("HDR.mnu"));
        CHECK(archive.has("PARTICLES.mnu"));
        CHECK(archive.has("override/night/BACKGROUND.mnu"));
        CHECK(archive.has("textures/month_bg/rgb/01.dds"));
        CHECK(archive.has("textures/month_bg/night/01.dds"));

        CHECK(xmb::assets::XmbThemeConfig::load_from_qrc(archive, base_cfg, "", &err));
        CHECK(std::abs(base_cfg.background.fovy - 71.846f) < 1e-3f);
        CHECK(std::abs(base_cfg.hdr.exposure - 1.05f) < 1e-3f);
        CHECK(std::abs(base_cfg.line1.brightness - 0.701917f) < 1e-4f);
        CHECK(std::abs(base_cfg.particles.emit_per_frame - 16.6539f) < 1e-3f);

        xmb::assets::XmbThemeConfig night_cfg;
        CHECK(xmb::assets::XmbThemeConfig::load_from_qrc(archive, night_cfg, "night", &err));
        CHECK(std::abs(night_cfg.hdr.exposure - 1.41f) < 1e-3f);
        CHECK(std::abs(night_cfg.line1.brightness - 0.9f) < 1e-3f);
        CHECK(std::abs(night_cfg.background.night_blend - 0.499947f) < 1e-4f);

        xmb::assets::DdsImage day_dds, night_dds;
        CHECK(xmb::assets::load_dds_from_memory(archive.read("textures/month_bg/rgb/01.dds"), day_dds, &err));
        CHECK(xmb::assets::load_dds_from_memory(archive.read("textures/month_bg/night/01.dds"), night_dds, &err));
        CHECK(day_dds.width == 64 && day_dds.height == 32);
        CHECK(day_dds.mip_levels == 5);
        CHECK(day_dds.format == xmb::assets::DdsPixelFormat::RGBA8);
        CHECK(day_dds.mips[0].pixels.size() == 64u * 32u * 4u);
        CHECK(night_dds.width == 64 && night_dds.height == 32);

        for (const char* name : {"black.qrc", "ps3_4k_pro_lines.qrc"}) {
            const auto p = qrc_dir / name;
            if (std::filesystem::exists(p)) {
                xmb::assets::QrcArchive other;
                CHECK(other.load_from_file(p, &err));
                CHECK(other.file_count() > 0);
            }
        }
    } else {
        std::cout << "  [info] No local .qrc files in " << qrc_dir << " (testing built-in procedural theme)\n";
    }

    // 5. Exact spline.elf B-spline basis partition of unity & zero derivative sum
    for (int k = 0; k <= 8; ++k) {
        float b[4], db[4];
        xmb::sim::RibbonSim::eval_bspline_basis(static_cast<float>(k) / 8.0f, b, db);
        const float sum_b = b[0] + b[1] + b[2] + b[3];
        const float sum_db = db[0] + db[1] + db[2] + db[3];
        CHECK(std::abs(sum_b - 1.0f) < 1e-5f);
        CHECK(std::abs(sum_db) < 1e-5f);
    }

    // 6. Exact spline.elf FFD identity under zero displacement lattice: (p - offset) / scale1 * scale2
    {
        std::vector<float> zero_lattice(11u * 7u * 7u * 4u, 0.0f);
        const float p[4] = {2.5f, 0.3f, 0.4f, 1.0f};
        const float offset[3] = {0.5f, -0.2f, 0.1f};
        const float scale1[3] = {4.0f, 1.0f, 2.0f};
        const float scale2[3] = {2.0f, 3.0f, 5.0f};
        float out_p[4]{};
        xmb::sim::RibbonSim::eval_ffd_point(p, zero_lattice.data(), offset, scale1, scale2, out_p);
        CHECK(std::abs(out_p[0] - ((2.5f - 0.5f) / 4.0f) * 2.0f) < 1e-5f);
        CHECK(std::abs(out_p[1] - ((0.3f - (-0.2f)) / 1.0f) * 3.0f) < 1e-5f);
        CHECK(std::abs(out_p[2] - ((0.4f - 0.1f) / 2.0f) * 5.0f) < 1e-5f);
    }

    // 7. RibbonSim (exact Sony Preset 1 + 19x19 cloth + FFD + spline.elf 128x128 AVX-512)
    xmb::sim::RibbonSim ribbon;
    ribbon.configure(base_cfg.line1);
    for (int i = 0; i < 10; ++i) {
        ribbon.step(1.0f / 60.0f, 16.0f / 9.0f, base_cfg.background.fovy);
    }
    CHECK(ribbon.vertices().size() == 128u * 128u);
    CHECK(ribbon.uvs().size() == 128u * 128u);
    CHECK(ribbon.indices().size() == 127u * 127u * 6u);
    int non_zero_normals = 0;
    int in_viewport = 0;
    float min_nx = 1e9f, max_nx = -1e9f, min_ny = 1e9f, max_ny = -1e9f, max_nlen = 0.0f;
    for (const auto& v : ribbon.vertices()) {
        CHECK(std::isfinite(v.pos[0]) && std::isfinite(v.pos[1]) && std::isfinite(v.pos[2]) && std::isfinite(v.pos[3]));
        CHECK(std::isfinite(v.normal[0]) && std::isfinite(v.normal[1]) && std::isfinite(v.normal[2]));
        const float nlen = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
        if (nlen > 1e-8f) ++non_zero_normals;
        if (nlen > max_nlen) max_nlen = nlen;
        if (v.pos[3] > 0.0f) {
            const float nx = v.pos[0] / v.pos[3];
            const float ny = v.pos[1] / v.pos[3];
            const float nz = v.pos[2] / v.pos[3];
            min_nx = std::min(min_nx, nx); max_nx = std::max(max_nx, nx);
            min_ny = std::min(min_ny, ny); max_ny = std::max(max_ny, ny);
            if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f && nz >= -1.0f && nz <= 1.0f) {
                ++in_viewport;
            }
        }
    }
    std::cout << "  [ribbon NDC] x=[" << min_nx << "," << max_nx << "] y=[" << min_ny << "," << max_ny
              << "] in_viewport=" << in_viewport << "/16384 max_|N|=" << max_nlen << "\n";
    CHECK(non_zero_normals > 16000);
    CHECK(in_viewport > 1000);

    // 8. Exact particles.elf RNG & fade curve checks
    {
        uint32_t seed = xmb::sim::ParticleSim::kInitialSeeds[0];
        for (int i = 0; i < 64; ++i) {
            const float r = xmb::sim::ParticleSim::rng_neg1_to_1(seed);
            CHECK(r >= -1.0f && r < 1.0f);
        }
        CHECK(std::abs(xmb::sim::ParticleSim::eval_fade_alpha(0.01f) - 0.5f) < 1e-4f);
        CHECK(std::abs(xmb::sim::ParticleSim::eval_fade_alpha(0.50f) - 1.0f) < 1e-4f);
        CHECK(std::abs(xmb::sim::ParticleSim::eval_fade_alpha(0.97f) - 0.5f) < 1e-3f);
    }

    // 9. ParticleSim coupled with RibbonSim (two-frame ribbon-surface cone emission)
    xmb::sim::ParticleSim particles(2048);
    particles.configure(base_cfg.particles);
    for (int i = 0; i < 60; ++i) {
        ribbon.step(1.0f / 60.0f, 16.0f / 9.0f, base_cfg.background.fovy);
        particles.step(1.0f / 60.0f, &ribbon);
    }
    std::cout << "  [particles] active after 60 coupled frames: " << particles.active_count() << "\n";
    for (size_t i = 0; i < std::min<size_t>(5, particles.active_count()); ++i) {
        const auto& v = particles.vertices()[i * 4];
        std::cout << "    p[" << i << "] pos=(" << v.pos_alpha[0] << "," << v.pos_alpha[1] << "," << v.pos_alpha[2]
                  << ") alpha=" << v.pos_alpha[3] << "\n";
    }
    CHECK(particles.active_count() > 20);
    CHECK(particles.vertices().size() == particles.active_count() * 4u);
    CHECK(particles.indices().size() == particles.active_count() * 6u);
    for (const auto& v : particles.vertices()) {
        CHECK(std::isfinite(v.pos_alpha[0]) && std::isfinite(v.pos_alpha[1]) && std::isfinite(v.pos_alpha[2]));
        CHECK(v.pos_alpha[3] >= 0.0f && v.pos_alpha[3] <= 1.0f);
        const float qlen = std::sqrt(v.rot[0] * v.rot[0] + v.rot[1] * v.rot[1] + v.rot[2] * v.rot[2] + v.rot[3] * v.rot[3]);
        CHECK(std::abs(qlen - 1.0f) < 1e-3f);
    }

    // 10. Procedural LUTs (_Stripes, _FresLUT, _IridescentTex, _Encode) + stock TGA loading
    {
        const auto stripes = xmb::gfx::generate_stripes_lut();
        const auto fres = xmb::gfx::generate_fresnel_lut();
        const auto irid = xmb::gfx::generate_iridescent_lut();
        const auto enc = xmb::gfx::generate_encode_lut();
        CHECK(stripes.width == 16 && stripes.height == 4 && stripes.pixels.size() == 16u * 4u * 4u);
        CHECK(fres.width == 512 && fres.height == 1 && fres.pixels.size() == 512u * 4u);
        CHECK(irid.width == 128 && irid.height == 128 && irid.pixels.size() == 128u * 128u * 4u);
        CHECK(enc.width == 4096 && enc.height == 1 && enc.pixels.size() == 4096u * 4u);

        const auto stock_qrc_path = qrc_dir / "stock_ofw_lines.qrc";
        if (std::filesystem::exists(stock_qrc_path)) {
            xmb::assets::QrcArchive stock_qrc;
            CHECK(stock_qrc.load_from_file(stock_qrc_path, &err));
            xmb::gfx::Rgba8Image fres_tga, irid_tga;
            CHECK(xmb::gfx::load_tga_rgba8(stock_qrc.read("textures/TGA/freslut1.tga"), fres_tga, &err));
            CHECK(fres_tga.width == 512 && fres_tga.height == 1);
            CHECK(xmb::gfx::load_tga_rgba8(stock_qrc.read("particles/proc_iridescent.tga"), irid_tga, &err));
            CHECK(irid_tga.width == 128 && irid_tga.height == 128);

            // 11. TimeOfDayRig smooth morphing (night -> yoake -> day -> higure -> night)
            xmb::assets::TimeOfDayRig tod;
            CHECK(tod.load_from_qrc(stock_qrc, &err));
            const auto t_night = tod.evaluate(200.0f);   // 02:00
            const auto t_dawn  = tod.evaluate(550.0f);   // 05:30 (night -> yoake)
            const auto t_noon  = tod.evaluate(1200.0f);  // 12:00 (day)
            const auto t_dusk  = tod.evaluate(2000.0f);  // 20:00 (higure -> night)
            CHECK(t_night.active_override == "tod:night");
            CHECK(t_dawn.active_override == "tod:night->yoake");
            CHECK(t_noon.active_override == "tod:day");
            CHECK(t_dusk.active_override == "tod:higure->night");
            // Stock OFW night has higher exposure (1.41) & ribbon brightness (0.90) than day (1.05 / 0.70)
            CHECK(t_night.hdr.exposure > t_noon.hdr.exposure + 0.2f);
            CHECK(t_night.line1.brightness > t_noon.line1.brightness + 0.1f);
            CHECK(t_dusk.hdr.exposure > t_noon.hdr.exposure && t_dusk.hdr.exposure < t_night.hdr.exposure);
        }
    }

    if (g_failures == 0) {
        std::cout << "All xmb_tests passed!\n";
        return 0;
    }
    std::cerr << g_failures << " test check(s) failed.\n";
    return 1;
}
