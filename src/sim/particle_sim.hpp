#pragma once

#include "assets/mnu_parser.hpp"
#include "sim/ribbon_sim.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace xmb::sim {

// Matches decoded particles_quads.vert inputs:
//   location 0: vec4 inPos (xyz = world center, w = fade alpha)
//   location 1: vec2 inUV0 (quad corner [0,1]^2)
//   location 2: vec4 inRot (unit orientation quaternion xyzw)
struct ParticleVertex {
    float pos_alpha[4]; // +0x00
    float uv0[2];       // +0x10
    float pad[2];       // +0x18
    float rot[4];       // +0x20
};
static_assert(sizeof(ParticleVertex) == 48, "ParticleVertex must be 48 bytes");

class ParticleSim {
public:
    static constexpr uint32_t kDefaultCapacity = 2048; // exact custom_render_plugin.prx 0x02e0dc
    static constexpr float kDeadSentinel = -666.0f;    // exact particles.elf 0xc4268000
    static constexpr uint32_t kInitialSeeds[3] = {0x98756161u, 0x21324889u, 0x82181158u};

    explicit ParticleSim(uint32_t max_particles = kDefaultCapacity);

    void configure(const assets::ParticlesConfig& cfg);
    void update_params(const assets::ParticlesConfig& cfg) noexcept { cfg_ = cfg; }
    void reset();

    // Steps particle physics + two-frame ribbon-surface cone emission (ribbon may be nullptr for standalone tests)
    void step(float dt_seconds, const RibbonSim* ribbon = nullptr);

    // Sony 32x16 int8 2D flow-field UI impulse (e.g. menu scroll / pointer stir, decays 0.98/frame)
    void add_flow_impulse(float norm_x, float norm_y, float vx, float vy);
    // Broad XMB navigation wind gust (left/right/up/down menu scroll) or shake swirl
    void add_nav_wind(float dir_x, float dir_y);
    void add_shake_swirl();

    [[nodiscard]] std::span<const ParticleVertex> vertices() const noexcept { return vertices_; }
    [[nodiscard]] std::span<const uint32_t> indices() const noexcept { return indices_; }
    [[nodiscard]] uint32_t max_particles() const noexcept { return max_particles_; }
    [[nodiscard]] uint32_t active_count() const noexcept { return active_count_; }

    // Exposed for exact unit testing against particles.elf constants & math.
    static float rng_neg1_to_1(uint32_t& seed) noexcept;
    static float eval_fade_alpha(float age) noexcept;

private:
    struct EmitCandidate {
        float prev_pos[3];
        uint32_t u;
        uint32_t v;
    };

    void spawn_particle_with_vel(uint32_t idx, const float pos[3], const float vel[3], float aging);
    void spawn_fallback_particle(uint32_t idx);
    void update_batch_avx512(float sim_dt, float age_dt);

    uint32_t max_particles_ = kDefaultCapacity;
    uint32_t capacity_ = kDefaultCapacity;
    uint32_t active_count_ = 0;
    uint32_t frame_counter_ = 0;
    assets::ParticlesConfig cfg_{};
    uint32_t rng_seeds_[3]{kInitialSeeds[0], kInitialSeeds[1], kInitialSeeds[2]};

    // Ribbon-row burst emitter state (custom_render_plugin.prx 0x02dbf4)
    uint32_t burst_u_ = 0;
    uint32_t burst_v_ = 0;
    uint32_t burst_remaining_ = 0;
    std::vector<EmitCandidate> pending_emit_;

    // Exact Sony AABB culling bounds (rodata@0x940b0 / 0x940c0)
    std::array<float, 3> aabb_min_{-10.0f, -10.0f, -12.0f};
    std::array<float, 3> aabb_max_{10.0f, 10.0f, 7.0f};

    // Exact Sony 32x16 signed-int8 RGB flow field (0x05d47c, 0x02c588)
    static constexpr uint32_t kFlowW = 32;
    static constexpr uint32_t kFlowH = 16;
    std::array<int8_t, kFlowW * kFlowH * 3> flow_texels_{};
    bool flow_active_ = false;
    void decay_flow_field();
    void sample_flow_field(float world_x, float world_y, float out_xyz[3]) const noexcept;

    // 16-wide AVX-512 SoA particle state
    std::vector<float> px_, py_, pz_, age_;
    std::vector<float> vx_, vy_, vz_, aging_;
    std::vector<float> rx_, ry_, rz_, rw_;
    std::vector<uint32_t> dead_freelist_;

    std::vector<ParticleVertex> vertices_;
    std::vector<uint32_t> indices_;
};

} // namespace xmb::sim
