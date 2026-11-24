#pragma once

#include "assets/mnu_parser.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace xmb::sim {

// Exact 32-byte output record from Cell SPU spline.elf (128x128 = 16,384 vertices).
struct RibbonVertex {
    float pos[4];     // +0x00: clip-space P(u, v) (MVP * M_line * FFD(ctrl))
    float normal[4];  // +0x10: unnormalized (dP/dv) x (dP/du), w = 0.0f
};
static_assert(sizeof(RibbonVertex) == 32, "RibbonVertex must match spline.elf 32-byte layout");

// Static UV + 10% edge-fade stream (exact custom_render_plugin.prx 0x04d190).
struct RibbonUV {
    float uv[2]; // x = iy/127, y = edge_u * edge_v
};
static_assert(sizeof(RibbonUV) == 8, "RibbonUV must be 8 bytes");

class RibbonSim {
public:
    static constexpr uint32_t kCtrlGridDim = 19;  // 19x19 = 361 control points
    static constexpr uint32_t kFfdX = 11;         // 11x7x7 = 539 vec4 FFD lattice
    static constexpr uint32_t kFfdY = 7;
    static constexpr uint32_t kFfdZ = 7;
    static constexpr uint32_t kPatches = 16;      // 16x16 bicubic B-spline patches
    static constexpr uint32_t kSamplesPerPatch = 8;
    static constexpr uint32_t kGridDim = kPatches * kSamplesPerPatch; // 128x128

    RibbonSim();

    void configure(const assets::LineConfig& cfg);
    void update_params(const assets::LineConfig& cfg); // live parameter morph without resetting cloth state
    void reset();
    void step(float dt_seconds, float aspect = 16.0f / 9.0f, float fovy_deg = 71.846f);

    [[nodiscard]] std::span<const RibbonVertex> vertices() const noexcept { return vertices_; }
    [[nodiscard]] std::span<const RibbonUV> uvs() const noexcept { return uvs_; }
    [[nodiscard]] std::span<const uint32_t> indices() const noexcept { return indices_; }
    [[nodiscard]] uint32_t cols() const noexcept { return kGridDim; }
    [[nodiscard]] uint32_t rows() const noexcept { return kGridDim; }
    [[nodiscard]] float elapsed_time() const noexcept { return sim_time_; }

    // World-space position of any (u_idx, v_idx) vertex on the 128x128 ribbon (for particle emission).
    void sample_world_pos(uint32_t u_idx, uint32_t v_idx, float out_xyz[3]) const noexcept;

    // Exact Sony camera & model matrices (shared with ParticleSim & VkRenderer)
    static void build_camera_view(float out_v[16]) noexcept;
    static void build_camera_proj(float aspect, float fovy_deg, float out_p[16]) noexcept;
    static void build_camera_proj_gl(float aspect, float fovy_deg, float out_p[16]) noexcept;

    // Exposed for exact unit testing against spline.elf & custom_render_plugin.prx
    static void eval_bspline_basis(float t, float out_b[4], float out_db[4]) noexcept;
    static void eval_ffd_point(
        const float p[4],
        const float* lattice_xyz4,
        const float offset[3],
        const float scale1[3],
        const float scale2[3],
        float out_p[4]) noexcept;

private:
    void rebuild_static_mesh();
    void step_cloth_physics();
    void apply_boundary_conditions();
    void eval_ffd_shader_128(int shader_id, float ffd_time, float out_128[128][4]) const noexcept;
    void update_ffd_lattice(float ffd_time);
    void build_model_matrix(float out_m[16]) const noexcept;
    void tessellate_patches_avx512(const std::array<float, kCtrlGridDim * kCtrlGridDim * 4>& clip_ctrl);

    assets::LineConfig cfg_{};
    float sim_time_ = 0.0f;
    float smooth_time_ = 0.0f;
    float step_accum_ = 0.0f;
    uint32_t rng_counter_ = 0;

    // FFD shader/time crossfade state (exact custom_render_plugin.prx 0x04a4dc: 200-frame smoothstep)
    int prev_ffd_shader_ = 1;
    float prev_ffd_time_ = 0.0f;
    float ffd_blend_t_ = 0.0f;

    // Exact Sony Lines::curr_pos / prev_pos / vel / ctrl_grid (19x19 vec4)
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> curr_pos_{};
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> prev_pos_{};
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> vel_{};
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> ctrl_grid_{};
    alignas(64) std::array<float, kCtrlGridDim * kCtrlGridDim * 4> world_ctrl_{};

    // 11x7x7 FFD displacement lattice ([iz][iy][ix], 4 floats each)
    alignas(64) std::array<float, kFfdZ * kFfdY * kFfdX * 4> ffd_lattice_{};

    // Precomputed 8-entry B(k/8) and B'(k/8) basis tables
    alignas(64) float b_lut_[kSamplesPerPatch][4]{};
    alignas(64) float db_lut_[kSamplesPerPatch][4]{};

    std::vector<RibbonVertex> vertices_;
    std::vector<RibbonUV> uvs_;
    std::vector<uint32_t> indices_;
};

} // namespace xmb::sim
