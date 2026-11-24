#include "sim/ribbon_sim.hpp"
#include "sim/ribbon_preset1.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

namespace xmb::sim {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kDegToRad = 0.01745329251994329577f;

// Exact Sony Lines::noise1D (custom_render_plugin.prx 0x04ab04): Hugo Elias integer hash PRNG
float noise1d(uint32_t& counter) noexcept {
    uint32_t n = ++counter;
    n = (n << 13) ^ n;
    const uint32_t m = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return 1.0f - static_cast<float>(m) * 9.31322575e-10f;
}

float clamped_tanh(float x) noexcept {
    const float c = std::clamp(x, -10.0f, 10.0f);
    const float e2 = std::exp(2.0f * c);
    return (e2 - 1.0f) / (e2 + 1.0f);
}

void mat4_mul_vec4_colmajor(const float m[16], const float v[4], float out[4]) noexcept {
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
    out[3] = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
}

void mat4_mul_colmajor(const float a[16], const float b[16], float out[16]) noexcept {
    for (int c = 0; c < 4; ++c) {
        const float* bv = &b[c * 4];
        mat4_mul_vec4_colmajor(a, bv, &out[c * 4]);
    }
}

} // namespace

void RibbonSim::build_camera_view(float out_v[16]) noexcept {
    // Exact Sony BackgroundView (0x024a68, 0x0255a4): eye=(0,0,2), target=(0,0,-7), up=(0,1,0)
    // gluLookAt -> pure translation by (0, 0, -2)
    std::memset(out_v, 0, sizeof(float) * 16);
    out_v[0] = 1.0f;
    out_v[5] = 1.0f;
    out_v[10] = 1.0f;
    out_v[14] = -2.0f;
    out_v[15] = 1.0f;
}

void RibbonSim::build_camera_proj(float aspect, float fovy_deg, float out_p[16]) noexcept {
    // Exact Sony glFrustum (near=0.1, far=1000.0) adapted to Vulkan NDC (Y-down, Z in [0,1]).
    // BackgroundView default FOVY is 53.0 deg (0x024a68); modded .mnu files with FOVY:0 mean "use default".
    const float effective_fovy = (fovy_deg > 1.0f) ? fovy_deg : 53.0f;
    const float fovy_rad = std::clamp(effective_fovy, 20.0f, 120.0f) * kDegToRad;
    const float f = 1.0f / std::tan(0.5f * fovy_rad);
    constexpr float znear = 0.1f;
    constexpr float zfar = 1000.0f;

    std::memset(out_p, 0, sizeof(float) * 16);
    out_p[0] = f / std::max(aspect, 0.1f);
    out_p[5] = -f; // Vulkan Y-down clip space
    out_p[10] = zfar / (znear - zfar);
    out_p[11] = -1.0f;
    out_p[14] = (zfar * znear) / (znear - zfar);
}

void RibbonSim::build_camera_proj_gl(float aspect, float fovy_deg, float out_p[16]) noexcept {
    // Exact OpenGL glFrustum (Y-up, Z in [-w, +w]) matching PS3 spline.elf + lines1.vpo 1:1.
    const float effective_fovy = (fovy_deg > 1.0f) ? fovy_deg : 53.0f;
    const float fovy_rad = std::clamp(effective_fovy, 20.0f, 120.0f) * kDegToRad;
    const float f = 1.0f / std::tan(0.5f * fovy_rad);
    constexpr float znear = 0.1f;
    constexpr float zfar = 1000.0f;

    std::memset(out_p, 0, sizeof(float) * 16);
    out_p[0] = f / std::max(aspect, 0.1f);
    out_p[5] = f;
    out_p[10] = -(zfar + znear) / (zfar - znear);
    out_p[11] = -1.0f;
    out_p[14] = -(2.0f * zfar * znear) / (zfar - znear);
}

void RibbonSim::eval_bspline_basis(float t, float out_b[4], float out_db[4]) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float omt = 1.0f - t;
    constexpr float kInv6 = 1.0f / 6.0f;

    out_b[0] = (omt * omt * omt) * kInv6;
    out_b[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) * kInv6;
    out_b[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) * kInv6;
    out_b[3] = t3 * kInv6;

    out_db[0] = -0.5f * omt * omt;
    out_db[1] = 0.5f * (3.0f * t2 - 4.0f * t);
    out_db[2] = 0.5f * (-3.0f * t2 + 2.0f * t + 1.0f);
    out_db[3] = 0.5f * t2;
}

void RibbonSim::eval_ffd_point(
    const float p[4],
    const float* lattice_xyz4,
    const float offset[3],
    const float scale1[3],
    const float scale2[3],
    float out_p[4]) noexcept {
    // Exact spline.elf sub_03c68:
    const float sx1 = (std::abs(scale1[0]) > 1e-6f) ? scale1[0] : 1.0f;
    const float sy1 = (std::abs(scale1[1]) > 1e-6f) ? scale1[1] : 1.0f;
    const float sz1 = (std::abs(scale1[2]) > 1e-6f) ? scale1[2] : 1.0f;

    const float ux = (p[0] - offset[0]) / sx1;
    const float uy = (p[1] - offset[1]) / sy1;
    const float uz = (p[2] - offset[2]) / sz1;

    const float cx = std::clamp(ux, 0.0f, 0.999f) * 8.0f;
    const float cy = std::clamp(uy, 0.0f, 0.999f) * 4.0f;
    const float cz = std::clamp(uz, 0.0f, 0.999f) * 4.0f;

    const int ix = std::clamp(static_cast<int>(std::floor(cx)), 0, static_cast<int>(kFfdX) - 4);
    const int iy = std::clamp(static_cast<int>(std::floor(cy)), 0, static_cast<int>(kFfdY) - 4);
    const int iz = std::clamp(static_cast<int>(std::floor(cz)), 0, static_cast<int>(kFfdZ) - 4);

    float wx[4], wy[4], wz[4], dummy[4];
    eval_bspline_basis(cx - static_cast<float>(ix), wx, dummy);
    eval_bspline_basis(cy - static_cast<float>(iy), wy, dummy);
    eval_bspline_basis(cz - static_cast<float>(iz), wz, dummy);

    float dx_acc = 0.0f, dy_acc = 0.0f, dz_acc = 0.0f;
    for (int dz = 0; dz < 4; ++dz) {
        const float fz = wz[dz];
        const size_t z_off = static_cast<size_t>(iz + dz) * kFfdY * kFfdX;
        for (int dy = 0; dy < 4; ++dy) {
            const float fzy = fz * wy[dy];
            const size_t zy_off = z_off + static_cast<size_t>(iy + dy) * kFfdX;
            for (int dx = 0; dx < 4; ++dx) {
                const float w = fzy * wx[dx];
                const float* cell = &lattice_xyz4[(zy_off + static_cast<size_t>(ix + dx)) * 4u];
                dx_acc += w * cell[0];
                dy_acc += w * cell[1];
                dz_acc += w * cell[2];
            }
        }
    }

    out_p[0] = (ux + dx_acc) * scale2[0];
    out_p[1] = (uy + dy_acc) * scale2[1];
    out_p[2] = (uz + dz_acc) * scale2[2];
    out_p[3] = 1.0f;
}

RibbonSim::RibbonSim() {
    for (uint32_t k = 0; k < kSamplesPerPatch; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(kSamplesPerPatch);
        eval_bspline_basis(t, b_lut_[k], db_lut_[k]);
    }
    rebuild_static_mesh();
    reset();
}

void RibbonSim::update_params(const assets::LineConfig& cfg) {
    if (cfg.ffd_shader != cfg_.ffd_shader) {
        prev_ffd_shader_ = cfg_.ffd_shader;
        prev_ffd_time_ = sim_time_ * 10.0f;
        ffd_blend_t_ = 1.0f;
    }
    cfg_ = cfg;
}

void RibbonSim::configure(const assets::LineConfig& cfg) {
    if (cfg.ffd_shader != cfg_.ffd_shader) {
        prev_ffd_shader_ = cfg_.ffd_shader;
        prev_ffd_time_ = sim_time_ * 10.0f;
        ffd_blend_t_ = 1.0f;
    }
    cfg_ = cfg;
    reset();
}

void RibbonSim::rebuild_static_mesh() {
    vertices_.resize(static_cast<size_t>(kGridDim) * kGridDim);
    uvs_.resize(static_cast<size_t>(kGridDim) * kGridDim);

    // Exact Sony Lines::buildStaticGridUVAndIB (custom_render_plugin.prx 0x04d190):
    // oldcrd.x = iy / 127.0f, oldcrd.y = 10% linear edge-fade mask on all 4 edges!
    const float inv_dim = 1.0f / static_cast<float>(kGridDim - 1);
    for (uint32_t iy = 0; iy < kGridDim; ++iy) {
        const float v = static_cast<float>(iy) * inv_dim;
        const float edge_v = std::min((0.5f - std::abs(0.5f - v)) * 10.0f, 1.0f);
        for (uint32_t ix = 0; ix < kGridDim; ++ix) {
            const float u = static_cast<float>(ix) * inv_dim;
            const float edge_u = std::min((0.5f - std::abs(0.5f - u)) * 10.0f, 1.0f);
            uvs_[static_cast<size_t>(iy) * kGridDim + ix] = {v, edge_u * edge_v};
        }
    }

    indices_.clear();
    indices_.reserve(static_cast<size_t>(kGridDim - 1) * (kGridDim - 1) * 6u);
    for (uint32_t r = 0; r + 1 < kGridDim; ++r) {
        for (uint32_t c = 0; c + 1 < kGridDim; ++c) {
            const uint32_t i00 = r * kGridDim + c;
            const uint32_t i10 = i00 + 1;
            const uint32_t i01 = i00 + kGridDim;
            const uint32_t i11 = i01 + 1;
            indices_.push_back(i00);
            indices_.push_back(i01);
            indices_.push_back(i10);
            indices_.push_back(i10);
            indices_.push_back(i01);
            indices_.push_back(i11);
        }
    }
}

void RibbonSim::reset() {
    // Exact Sony Lines::initPreset(1) (custom_render_plugin.prx 0x04e624, 0x050da8)
    sim_time_ = 0.0f;
    smooth_time_ = 0.0f;
    step_accum_ = 0.0f;
    rng_counter_ = 0;
    ffd_lattice_.fill(0.0f);

    for (uint32_t i = 0; i < kCtrlGridDim * kCtrlGridDim; ++i) {
        curr_pos_[i * 4u + 0] = kRibbonPreset1Pos[i * 3u + 0];
        curr_pos_[i * 4u + 1] = kRibbonPreset1Pos[i * 3u + 1];
        curr_pos_[i * 4u + 2] = kRibbonPreset1Pos[i * 3u + 2];
        curr_pos_[i * 4u + 3] = 1.0f;

        vel_[i * 4u + 0] = kRibbonPreset1Vel[i * 3u + 0];
        vel_[i * 4u + 1] = kRibbonPreset1Vel[i * 3u + 1];
        vel_[i * 4u + 2] = kRibbonPreset1Vel[i * 3u + 2];
        vel_[i * 4u + 3] = 0.0f;
    }
    prev_pos_ = curr_pos_;
    ctrl_grid_ = curr_pos_;
    world_ctrl_ = curr_pos_;
}

void RibbonSim::apply_boundary_conditions() {
    // Exact Sony Lines::applyBoundaryConditions (custom_render_plugin.prx 0x04bce0)
    smooth_time_ += (sim_time_ - smooth_time_) * 0.1f;
    const float drift_x = cfg_.timestep * 0.02f;

    for (uint32_t row = 0; row < kCtrlGridDim; ++row) {
        // Left edge (col = 0): +X velocity impulse
        vel_[(row * kCtrlGridDim + 0u) * 4u + 0] += drift_x;

        // Right edge (col = 18): pinned Lissajous wave scaled by END.y / END.z
        const float phase = static_cast<float>(row) / 19.0f + smooth_time_;
        const float py = (std::sin(phase * 11.0f) * 0.5f + 0.5f) * cfg_.end[1];
        const float pz = (std::cos(phase * 15.0f) + 1.0f) * 0.5f * cfg_.end[2];
        const size_t r_idx = (row * kCtrlGridDim + 18u) * 4u;
        curr_pos_[r_idx + 0] = 0.0f;
        curr_pos_[r_idx + 1] = py;
        curr_pos_[r_idx + 2] = pz;
        curr_pos_[r_idx + 3] = 1.0f;
        vel_[r_idx + 0] = 0.0f;
        vel_[r_idx + 1] = 0.0f;
        vel_[r_idx + 2] = 0.0f;
        vel_[r_idx + 3] = 0.0f;
    }
}

void RibbonSim::step_cloth_physics() {
    // Exact Sony Lines::stepClothPhysics (custom_render_plugin.prx 0x04bed0)
    prev_pos_ = curr_pos_;

    const float dt_sim = cfg_.timestep * 0.0001f;
    sim_time_ += dt_sim;
    if (sim_time_ > 10.0f) {
        prev_ffd_shader_ = cfg_.ffd_shader;
        prev_ffd_time_ = sim_time_ * 10.0f;
        ffd_blend_t_ = 1.0f;
        sim_time_ = 0.0f;
    }

    const float rest_len = cfg_.length;
    const float k_struct = cfg_.tension;
    const float k_bend = cfg_.tension * 10.0f;
    const float perturb = cfg_.perturbation;

    auto apply_spring = [&](uint32_t a, uint32_t b, float rest_l, float stiffness) {
        const float dx = curr_pos_[b * 4u + 0] - curr_pos_[a * 4u + 0];
        const float dy = curr_pos_[b * 4u + 1] - curr_pos_[a * 4u + 1];
        const float dz = curr_pos_[b * 4u + 2] - curr_pos_[a * 4u + 2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist <= 1e-7f) return;
        const float mag = ((dist - rest_l) * stiffness) / dist;
        const float fx = dx * mag;
        const float fy = dy * mag;
        const float fz = dz * mag;
        vel_[a * 4u + 0] += fx;
        vel_[a * 4u + 1] += fy;
        vel_[a * 4u + 2] += fz;
        vel_[b * 4u + 0] -= fx;
        vel_[b * 4u + 1] -= fy;
        vel_[b * 4u + 2] -= fz;
    };

    for (uint32_t row = 0; row < kCtrlGridDim; ++row) {
        for (uint32_t col = 0; col < kCtrlGridDim; ++col) {
            const uint32_t idx = row * kCtrlGridDim + col;
            if (col > 0) apply_spring(idx, idx - 1u, rest_len, k_struct);
            if (col > 1) apply_spring(idx, idx - 2u, 2.0f * rest_len, k_bend);
            if (row > 0) apply_spring(idx, idx - 19u, rest_len, k_struct);
            if (row > 1) apply_spring(idx, idx - 38u, 2.0f * rest_len, k_bend);

            vel_[idx * 4u + 0] += noise1d(rng_counter_) * perturb;
            vel_[idx * 4u + 1] += noise1d(rng_counter_) * perturb;
            vel_[idx * 4u + 2] += noise1d(rng_counter_) * perturb;
        }
    }

    const float damp = cfg_.damping * cfg_.timestep;
    for (uint32_t i = 0; i < kCtrlGridDim * kCtrlGridDim; ++i) {
        curr_pos_[i * 4u + 0] += vel_[i * 4u + 0] * dt_sim;
        curr_pos_[i * 4u + 1] += vel_[i * 4u + 1] * dt_sim;
        curr_pos_[i * 4u + 2] += vel_[i * 4u + 2] * dt_sim;

        vel_[i * 4u + 0] -= vel_[i * 4u + 0] * damp;
        vel_[i * 4u + 1] -= vel_[i * 4u + 1] * damp;
        vel_[i * 4u + 2] -= vel_[i * 4u + 2] * damp;
    }

    apply_boundary_conditions();
}

void RibbonSim::eval_ffd_shader_128(int shader_id, float ffd_time, float out_128[128][4]) const noexcept {
    // Exact decoded ffd_shader0..3.fpo (docs/shaders.md) evaluated across 8x4x4 _UVW grid
    for (int iz = 0; iz < 4; ++iz) {
        for (int iy = 0; iy < 4; ++iy) {
            const float v = static_cast<float>(iy) * 0.25f;
            for (int ix = 0; ix < 8; ++ix) {
                const float u = static_cast<float>(ix) * 0.125f;
                float dx = 0.0f;
                float dy = 0.0f;

                if (shader_id == 0) {
                    const float t = ffd_time + 2400.0f;
                    const float c1 = (std::sin(0.1f * t) + 0.2f) * 0.83333f;
                    const float c2 = (std::sin(0.1f * t + 20.0f) + 0.2f) * 0.83333f;
                    const float g1 = std::exp(-50.0f * (u - c1) * (u - c1));
                    const float g2 = std::exp(-10.0f * (u - c2) * (u - c2));
                    const float main_w = (0.5f - v) * (1.0f - std::exp(-2.0f * u)) *
                                         std::sin(-6.28f * u + 0.5f * t * (1.0f - 0.8f * u));
                    const float rip = 0.25f * std::sin(15.7f * u - 0.13f * t - 2.0f);
                    dx = 1.31f * u * u + 0.2f * u;
                    dy = g1 - 0.15f * g2 + main_w + rip;
                } else if (shader_id == 2) {
                    const float t = ffd_time;
                    const float th = clamped_tanh(u - 0.5f);
                    const float decay = std::exp(-0.0001f * t);
                    const float breathe = (std::sin(2.0f * t) + 3.0f) * (0.24f * th + 0.24f) *
                                          std::sin(7.85f * u - 2.5f * t - 1.25f);
                    dx = 1.3f * u * u + 0.2f * u;
                    dy = decay * breathe;
                } else if (shader_id == 3) {
                    const float t = ffd_time;
                    const float c1 = (std::sin(0.1f * t) + 0.2f) * 0.83333f;
                    const float c2 = (std::sin(0.1f * t + 20.0f) + 0.2f) * 0.83333f;
                    const float g1 = std::exp(-50.0f * (u - c1) * (u - c1));
                    const float g2 = std::exp(-10.0f * (u - c2) * (u - c2));
                    const float main_w = 2.0f * (0.5f - v) * (1.0f - std::exp(-2.0f * u)) *
                                         std::sin(-6.28f * u + 0.5f * t * (1.0f - 0.8f * u));
                    dx = 1.2f * u * u;
                    dy = g1 - 0.15f * g2 + main_w;
                } else {
                    // ffd_shader1 (default)
                    const float t = ffd_time;
                    const float th = clamped_tanh(u - 0.5f);
                    const float decay = std::exp(-0.0001f * t);
                    const float breathe = (std::sin(2.0f * t) + 3.0f) * (0.24f * th + 0.24f) *
                                          std::sin(7.85f * u - 2.5f * t - 1.25f);
                    const float c1 = (std::sin(0.1f * t) + 0.2f) * 0.83333f;
                    const float gauss = std::exp(-50.0f * (u - c1) * (u - c1));
                    const float v_rip = 0.125f * std::sin(t - 6.28f * v);
                    const float sway = 0.5f * std::sin(0.25f * t);
                    dx = 1.3f * u * u + 0.2f * u - 0.15f;
                    dy = decay * (breathe + gauss + v_rip + sway);
                }

                const int idx = (iz * 4 + iy) * 8 + ix;
                out_128[idx][0] = dx * cfg_.ffd_scale1[0] + cfg_.ffd_offset[0];
                out_128[idx][1] = dy * cfg_.ffd_scale1[1] + cfg_.ffd_offset[1];
                out_128[idx][2] = 0.0f * cfg_.ffd_scale1[2] + cfg_.ffd_offset[2];
                out_128[idx][3] = 0.0f;
            }
        }
    }
}

void RibbonSim::update_ffd_lattice(float ffd_time) {
    alignas(64) float raw_128[128][4]{};
    eval_ffd_shader_128(cfg_.ffd_shader, ffd_time, raw_128);

    // Exact Sony ffd_alpha_blend.fpo 200-frame smoothstep crossfade (0x04a4dc..0x04a7b8)
    if (ffd_blend_t_ > 0.0f) {
        alignas(64) float prev_128[128][4]{};
        eval_ffd_shader_128(prev_ffd_shader_, prev_ffd_time_, prev_128);
        const float b = std::clamp(ffd_blend_t_, 0.0f, 1.0f);
        const float a = b * b * (3.0f - 2.0f * b);
        for (int i = 0; i < 128; ++i) {
            for (int c = 0; c < 4; ++c) {
                raw_128[i][c] = raw_128[i][c] * (1.0f - a) + prev_128[i][c] * a;
            }
        }
        ffd_blend_t_ = std::max(0.0f, ffd_blend_t_ - 0.005f);
    }

    // Edge-clamped unpack 8x4x4 -> 11x7x7 (0x047b1c)
    for (int iz = -1; iz <= 5; ++iz) {
        const int cz = std::clamp(iz, 0, 3);
        for (int iy = -1; iy <= 5; ++iy) {
            const int cy = std::clamp(iy, 0, 3);
            for (int ix = -1; ix <= 9; ++ix) {
                const int cx = std::clamp(ix, 0, 7);
                const int src = (cz * 4 + cy) * 8 + cx;
                const size_t dst = (static_cast<size_t>(iz + 1) * kFfdY * kFfdX +
                                    static_cast<size_t>(iy + 1) * kFfdX +
                                    static_cast<size_t>(ix + 1)) * 4u;
                ffd_lattice_[dst + 0] = raw_128[src][0];
                ffd_lattice_[dst + 1] = raw_128[src][1];
                ffd_lattice_[dst + 2] = raw_128[src][2];
                ffd_lattice_[dst + 3] = raw_128[src][3];
            }
        }
    }
}

void RibbonSim::build_model_matrix(float out_m[16]) const noexcept {
    // Exact Sony M_line (custom_render_plugin.prx 0x029dfc):
    // M_line = translate(POS_X, POS_Y, POS_Z) * mat4_from_axis_angle(normalize(ANG), radians(ANG_ROT))
    float ax = cfg_.ang[0], ay = cfg_.ang[1], az = cfg_.ang[2];
    const float alen = std::sqrt(ax * ax + ay * ay + az * az);
    if (alen > 1e-6f) {
        ax /= alen;
        ay /= alen;
        az /= alen;
    } else {
        ax = 0.0f;
        ay = 1.0f;
        az = 0.0f;
    }

    const float rad = cfg_.ang_rot * kDegToRad;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float omc = 1.0f - c;

    out_m[0] = c + ax * ax * omc;
    out_m[1] = ay * ax * omc + az * s;
    out_m[2] = az * ax * omc - ay * s;
    out_m[3] = 0.0f;

    out_m[4] = ax * ay * omc - az * s;
    out_m[5] = c + ay * ay * omc;
    out_m[6] = az * ay * omc + ax * s;
    out_m[7] = 0.0f;

    out_m[8] = ax * az * omc + ay * s;
    out_m[9] = ay * az * omc - ax * s;
    out_m[10] = c + az * az * omc;
    out_m[11] = 0.0f;

    out_m[12] = cfg_.pos[0];
    out_m[13] = cfg_.pos[1];
    out_m[14] = cfg_.pos[2];
    out_m[15] = 1.0f;
}

void RibbonSim::sample_world_pos(uint32_t u_idx, uint32_t v_idx, float out_xyz[3]) const noexcept {
    // Sample world-space position on the 128x128 ribbon surface (for PPU particle emission)
    const uint32_t ux = u_idx % kGridDim;
    const uint32_t vy = v_idx % kGridDim;
    const uint32_t pcol = ux / kSamplesPerPatch;
    const uint32_t prow = vy / kSamplesPerPatch;
    const uint32_t i = ux % kSamplesPerPatch;
    const uint32_t j = vy % kSamplesPerPatch;

    const float* bu = b_lut_[i];
    const float* bv = b_lut_[j];
    float acc[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t r = 0; r < 4; ++r) {
        const float wv = bv[r];
        for (uint32_t c = 0; c < 4; ++c) {
            const float w = wv * bu[c];
            const size_t idx = (static_cast<size_t>(prow + r) * kCtrlGridDim + (pcol + c)) * 4u;
            acc[0] += w * world_ctrl_[idx + 0];
            acc[1] += w * world_ctrl_[idx + 1];
            acc[2] += w * world_ctrl_[idx + 2];
        }
    }
    out_xyz[0] = acc[0];
    out_xyz[1] = acc[1];
    out_xyz[2] = acc[2];
}

void RibbonSim::tessellate_patches_avx512(
    const std::array<float, kCtrlGridDim * kCtrlGridDim * 4>& clip_ctrl) {
    alignas(64) float bv_soa[4][8];
    alignas(64) float dbv_soa[4][8];
    for (uint32_t j = 0; j < kSamplesPerPatch; ++j) {
        for (uint32_t k = 0; k < 4; ++k) {
            bv_soa[k][j] = b_lut_[j][k];
            dbv_soa[k][j] = db_lut_[j][k];
        }
    }

#if defined(__AVX2__) && defined(__FMA__)
    const __m256 v_bv0 = _mm256_load_ps(bv_soa[0]);
    const __m256 v_bv1 = _mm256_load_ps(bv_soa[1]);
    const __m256 v_bv2 = _mm256_load_ps(bv_soa[2]);
    const __m256 v_bv3 = _mm256_load_ps(bv_soa[3]);
    const __m256 v_dbv0 = _mm256_load_ps(dbv_soa[0]);
    const __m256 v_dbv1 = _mm256_load_ps(dbv_soa[1]);
    const __m256 v_dbv2 = _mm256_load_ps(dbv_soa[2]);
    const __m256 v_dbv3 = _mm256_load_ps(dbv_soa[3]);
#endif

    for (uint32_t patch_idx = 0; patch_idx < kPatches * kPatches; ++patch_idx) {
        const uint32_t prow = patch_idx >> 4;
        const uint32_t pcol = patch_idx & 15u;

        float p_sub[4][4][4];
        for (uint32_t r = 0; r < 4; ++r) {
            for (uint32_t c = 0; c < 4; ++c) {
                const size_t src = (static_cast<size_t>(prow + r) * kCtrlGridDim + (pcol + c)) * 4u;
                p_sub[r][c][0] = clip_ctrl[src + 0];
                p_sub[r][c][1] = clip_ctrl[src + 1];
                p_sub[r][c][2] = clip_ctrl[src + 2];
                p_sub[r][c][3] = clip_ctrl[src + 3];
            }
        }

        for (uint32_t i = 0; i < kSamplesPerPatch; ++i) {
            const float* bu = b_lut_[i];
            const float* dbu = db_lut_[i];

            float curve[4][4];
            float dcurve_du[4][4];
            for (uint32_t r = 0; r < 4; ++r) {
                for (uint32_t comp = 0; comp < 4; ++comp) {
                    curve[r][comp] = bu[0] * p_sub[r][0][comp] + bu[1] * p_sub[r][1][comp] +
                                     bu[2] * p_sub[r][2][comp] + bu[3] * p_sub[r][3][comp];
                    dcurve_du[r][comp] = dbu[0] * p_sub[r][0][comp] + dbu[1] * p_sub[r][1][comp] +
                                         dbu[2] * p_sub[r][2][comp] + dbu[3] * p_sub[r][3][comp];
                }
            }

            const uint32_t gx = pcol * kSamplesPerPatch + i;

#if defined(__AVX2__) && defined(__FMA__)
            auto eval_comp = [&](int comp, __m256 b0, __m256 b1, __m256 b2, __m256 b3, const float src[4][4]) -> __m256 {
                __m256 acc = _mm256_mul_ps(b0, _mm256_set1_ps(src[0][comp]));
                acc = _mm256_fmadd_ps(b1, _mm256_set1_ps(src[1][comp]), acc);
                acc = _mm256_fmadd_ps(b2, _mm256_set1_ps(src[2][comp]), acc);
                acc = _mm256_fmadd_ps(b3, _mm256_set1_ps(src[3][comp]), acc);
                return acc;
            };

            const __m256 px = eval_comp(0, v_bv0, v_bv1, v_bv2, v_bv3, curve);
            const __m256 py = eval_comp(1, v_bv0, v_bv1, v_bv2, v_bv3, curve);
            const __m256 pz = eval_comp(2, v_bv0, v_bv1, v_bv2, v_bv3, curve);
            const __m256 pw = eval_comp(3, v_bv0, v_bv1, v_bv2, v_bv3, curve);

            const __m256 dux = eval_comp(0, v_bv0, v_bv1, v_bv2, v_bv3, dcurve_du);
            const __m256 duy = eval_comp(1, v_bv0, v_bv1, v_bv2, v_bv3, dcurve_du);
            const __m256 duz = eval_comp(2, v_bv0, v_bv1, v_bv2, v_bv3, dcurve_du);

            const __m256 dvx = eval_comp(0, v_dbv0, v_dbv1, v_dbv2, v_dbv3, curve);
            const __m256 dvy = eval_comp(1, v_dbv0, v_dbv1, v_dbv2, v_dbv3, curve);
            const __m256 dvz = eval_comp(2, v_dbv0, v_dbv1, v_dbv2, v_dbv3, curve);

            // Exact spline.elf 04b30..04b68: unnormalized N = cross(dv, du)
            const __m256 nx = _mm256_fmsub_ps(dvy, duz, _mm256_mul_ps(dvz, duy));
            const __m256 ny = _mm256_fmsub_ps(dvz, dux, _mm256_mul_ps(dvx, duz));
            const __m256 nz = _mm256_fmsub_ps(dvx, duy, _mm256_mul_ps(dvy, dux));

            alignas(32) float sx[8], sy[8], sz[8], sw[8], snx[8], sny[8], snz[8];
            _mm256_store_ps(sx, px);
            _mm256_store_ps(sy, py);
            _mm256_store_ps(sz, pz);
            _mm256_store_ps(sw, pw);
            _mm256_store_ps(snx, nx);
            _mm256_store_ps(sny, ny);
            _mm256_store_ps(snz, nz);

            for (uint32_t j = 0; j < kSamplesPerPatch; ++j) {
                const uint32_t gy = prow * kSamplesPerPatch + j;
                RibbonVertex& v = vertices_[static_cast<size_t>(gy) * kGridDim + gx];
                v.pos[0] = sx[j];
                v.pos[1] = sy[j];
                v.pos[2] = sz[j];
                v.pos[3] = sw[j];
                v.normal[0] = snx[j];
                v.normal[1] = sny[j];
                v.normal[2] = snz[j];
                v.normal[3] = 0.0f;
            }
#else
            for (uint32_t j = 0; j < kSamplesPerPatch; ++j) {
                const float* bv = b_lut_[j];
                const float* dbv = db_lut_[j];
                float pos[4], du[3], dv[3];
                for (int comp = 0; comp < 4; ++comp) {
                    pos[comp] = bv[0] * curve[0][comp] + bv[1] * curve[1][comp] +
                                bv[2] * curve[2][comp] + bv[3] * curve[3][comp];
                }
                for (int comp = 0; comp < 3; ++comp) {
                    du[comp] = bv[0] * dcurve_du[0][comp] + bv[1] * dcurve_du[1][comp] +
                               bv[2] * dcurve_du[2][comp] + bv[3] * dcurve_du[3][comp];
                    dv[comp] = dbv[0] * curve[0][comp] + dbv[1] * curve[1][comp] +
                               dbv[2] * curve[2][comp] + dbv[3] * curve[3][comp];
                }
                const uint32_t gy = prow * kSamplesPerPatch + j;
                RibbonVertex& v = vertices_[static_cast<size_t>(gy) * kGridDim + gx];
                v.pos[0] = pos[0]; v.pos[1] = pos[1]; v.pos[2] = pos[2]; v.pos[3] = pos[3];
                v.normal[0] = dv[1] * du[2] - dv[2] * du[1];
                v.normal[1] = dv[2] * du[0] - dv[0] * du[2];
                v.normal[2] = dv[0] * du[1] - dv[1] * du[0];
                v.normal[3] = 0.0f;
            }
#endif
        }
    }
}

void RibbonSim::step(float dt_seconds, float aspect, float fovy_deg) {
    // Exact Sony Lines::update (0x04f814): fixed 60 Hz accumulator
    const float clamped_dt = std::clamp(dt_seconds, 0.0f, 0.1f);
    step_accum_ += clamped_dt * 60.0f;
    while (step_accum_ >= 1.0f) {
        step_cloth_physics();
        step_accum_ -= 1.0f;
    }

    // 1. Interpolate prev_pos -> curr_pos + X-axis pinch around FFD PARAM 1 (0x04f8a0)
    const float alpha = step_accum_;
    const float param1 = cfg_.ffd_param1;
    for (uint32_t i = 0; i < kCtrlGridDim * kCtrlGridDim; ++i) {
        const float px = prev_pos_[i * 4u + 0] * (1.0f - alpha) + curr_pos_[i * 4u + 0] * alpha;
        const float py = prev_pos_[i * 4u + 1] * (1.0f - alpha) + curr_pos_[i * 4u + 1] * alpha;
        const float pz = prev_pos_[i * 4u + 2] * (1.0f - alpha) + curr_pos_[i * 4u + 2] * alpha;

        const float dx = px - param1;
        const float s = (dx < 0.0f) ? 0.0f : std::sin(dx * kHalfPi);
        const float t = std::clamp(dx / 5.0f, 0.0f, 1.0f);
        const float u = 1.0f - t;
        const float env = u * u * (3.0f - 2.0f * u);
        const float yz_scale = 1.3f - s * env;

        ctrl_grid_[i * 4u + 0] = px;
        ctrl_grid_[i * 4u + 1] = py * yz_scale;
        ctrl_grid_[i * 4u + 2] = pz * yz_scale;
        ctrl_grid_[i * 4u + 3] = 1.0f;
    }

    // 2. Update 11x7x7 FFD lattice with sim_time * 10.0f
    update_ffd_lattice(sim_time_ * 10.0f);

    // 3. Build exact OpenGL M_line, V, P_gl, and MVP = P_gl * V * M_line (0x050cd0)
    //    so spline.elf & lines1.vert compute V and rawNdotV in exact PS3 OpenGL [-w, +w] clip space!
    float m_line[16], view[16], proj_gl[16], pv_gl[16];
    build_model_matrix(m_line);
    build_camera_view(view);
    build_camera_proj_gl(aspect, fovy_deg, proj_gl);
    mat4_mul_colmajor(proj_gl, view, pv_gl);

    // 4. SPU spline.elf Pass 1 (FFD) + Pass 2 (M_line for world_ctrl_, PV_gl*M_line for clip_ctrl)
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> clip_ctrl{};
    for (uint32_t i = 0; i < kCtrlGridDim * kCtrlGridDim; ++i) {
        float ffd_pt[4];
        eval_ffd_point(
            &ctrl_grid_[i * 4u],
            ffd_lattice_.data(),
            cfg_.ffd_offset.data(),
            cfg_.ffd_scale1.data(),
            cfg_.ffd_scale2.data(),
            ffd_pt);
        mat4_mul_vec4_colmajor(m_line, ffd_pt, &world_ctrl_[i * 4u]);
        mat4_mul_vec4_colmajor(pv_gl, &world_ctrl_[i * 4u], &clip_ctrl[i * 4u]);
    }

    // 5. SPU spline.elf Pass 3: 16x16 bicubic B-spline patches -> 128x128 vertices + normals
    tessellate_patches_avx512(clip_ctrl);
}

} // namespace xmb::sim
