#include "sim/particle_sim.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#if defined(__AVX512F__)
#include <immintrin.h>
#endif

namespace xmb::sim {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kDegToRad = 0.01745329251994329577f;
constexpr float kMaxAge = 0.99999f;

} // namespace

float ParticleSim::rng_neg1_to_1(uint32_t& seed) noexcept {
    seed = seed * 16807u;
    const uint32_t bits = 0x40000000u | (seed >> 9);
    return std::bit_cast<float>(bits) - 3.0f;
}

float ParticleSim::eval_fade_alpha(float age) noexcept {
    const float fade_in = std::min(age, 0.02f) * 50.0f;
    const float fade_out = 1.0f + std::max(age - 0.94f, 0.0f) * (-16.666664f);
    return std::clamp(fade_in * fade_out, 0.0f, 1.0f);
}

ParticleSim::ParticleSim(uint32_t max_particles)
    : max_particles_(std::max(16u, max_particles)),
      capacity_((std::max(16u, max_particles) + 15u) & ~15u) {
    px_.resize(capacity_, 0.0f);
    py_.resize(capacity_, 0.0f);
    pz_.resize(capacity_, 0.0f);
    age_.resize(capacity_, kDeadSentinel);
    vx_.resize(capacity_, 0.0f);
    vy_.resize(capacity_, 0.0f);
    vz_.resize(capacity_, 0.0f);
    aging_.resize(capacity_, 0.0f);
    rx_.resize(capacity_, 0.0f);
    ry_.resize(capacity_, 0.0f);
    rz_.resize(capacity_, 0.0f);
    rw_.resize(capacity_, 1.0f);

    dead_freelist_.reserve(capacity_);
    pending_emit_.reserve(64);
    vertices_.reserve(static_cast<size_t>(capacity_) * 4u);
    indices_.reserve(static_cast<size_t>(capacity_) * 6u);
    reset();
}

void ParticleSim::configure(const assets::ParticlesConfig& cfg) {
    cfg_ = cfg;
    reset();
}

void ParticleSim::reset() {
    rng_seeds_[0] = kInitialSeeds[0];
    rng_seeds_[1] = kInitialSeeds[1];
    rng_seeds_[2] = kInitialSeeds[2];
    active_count_ = 0;
    frame_counter_ = 0;
    burst_u_ = 0;
    burst_v_ = 0;
    burst_remaining_ = 0;
    pending_emit_.clear();
    flow_texels_.fill(0);
    flow_active_ = false;

    std::fill(age_.begin(), age_.end(), kDeadSentinel);
    dead_freelist_.clear();
    for (uint32_t i = 0; i < max_particles_; ++i) {
        dead_freelist_.push_back(max_particles_ - 1u - i);
    }
}

void ParticleSim::spawn_particle_with_vel(uint32_t idx, const float pos[3], const float vel[3], float aging) {
    px_[idx] = pos[0];
    py_[idx] = pos[1];
    pz_[idx] = pos[2];
    age_[idx] = 0.0f;

    vx_[idx] = vel[0];
    vy_[idx] = vel[1];
    vz_[idx] = vel[2];
    aging_[idx] = aging;

    float qx = rng_neg1_to_1(rng_seeds_[0]);
    float qy = rng_neg1_to_1(rng_seeds_[1]);
    float qz = rng_neg1_to_1(rng_seeds_[2]);
    float qw = rng_neg1_to_1(rng_seeds_[0]);
    const float qlen = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    const float inv_q = (qlen > 1e-6f) ? (1.0f / qlen) : 1.0f;
    rx_[idx] = qx * inv_q;
    ry_[idx] = qy * inv_q;
    rz_[idx] = qz * inv_q;
    rw_[idx] = qw * inv_q;
}

void ParticleSim::spawn_fallback_particle(uint32_t idx) {
    const float pos[3] = {
        rng_neg1_to_1(rng_seeds_[0]) * 4.0f,
        rng_neg1_to_1(rng_seeds_[1]) * 1.8f,
        -4.5f + rng_neg1_to_1(rng_seeds_[2]) * 2.5f,
    };
    const float speed = std::max(0.01f, cfg_.emit_vel_min);
    const float vel[3] = {
        rng_neg1_to_1(rng_seeds_[0]) * speed,
        rng_neg1_to_1(rng_seeds_[1]) * speed,
        rng_neg1_to_1(rng_seeds_[2]) * speed * 0.1f,
    };
    const float aging = std::max(0.001f, cfg_.aging_speed * (1.0f + rng_neg1_to_1(rng_seeds_[1]) * cfg_.aging_variance));
    spawn_particle_with_vel(idx, pos, vel, aging);
}

void ParticleSim::add_flow_impulse(float norm_x, float norm_y, float vx, float vy) {
    const int cx = std::clamp(static_cast<int>(norm_x * static_cast<float>(kFlowW - 1)), 0, static_cast<int>(kFlowW - 1));
    const int cy = std::clamp(static_cast<int>(norm_y * static_cast<float>(kFlowH - 1)), 0, static_cast<int>(kFlowH - 1));
    for (int dy = -2; dy <= 2; ++dy) {
        const int y = std::clamp(cy + dy, 0, static_cast<int>(kFlowH - 1));
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = std::clamp(cx + dx, 0, static_cast<int>(kFlowW - 1));
            const float w = std::exp(-0.5f * static_cast<float>(dx * dx + dy * dy));
            const size_t off = (static_cast<size_t>(y) * kFlowW + static_cast<size_t>(x)) * 3u;
            const float nx = std::clamp(static_cast<float>(flow_texels_[off + 0]) / 127.0f + vx * w, -1.0f, 1.0f);
            const float ny = std::clamp(static_cast<float>(flow_texels_[off + 1]) / 127.0f + vy * w, -1.0f, 1.0f);
            flow_texels_[off + 0] = static_cast<int8_t>(nx * 127.0f);
            flow_texels_[off + 1] = static_cast<int8_t>(ny * 127.0f);
        }
    }
    flow_active_ = true;
}

void ParticleSim::add_nav_wind(float dir_x, float dir_y) {
    // Broad directional gust across the 32x16 flow field (mirrors Sony icon_wind / dpad impulses)
    const float gx = std::clamp(dir_x * 0.45f, -0.85f, 0.85f);
    const float gy = std::clamp(dir_y * 0.45f, -0.85f, 0.85f);
    for (uint32_t y = 0; y < kFlowH; ++y) {
        const float fy = (static_cast<float>(y) / static_cast<float>(kFlowH - 1)) * 2.0f - 1.0f;
        const float wy = std::exp(-0.8f * fy * fy); // strongest across the central XMB band
        for (uint32_t x = 0; x < kFlowW; ++x) {
            const size_t off = (static_cast<size_t>(y) * kFlowW + static_cast<size_t>(x)) * 3u;
            const float nx = std::clamp(static_cast<float>(flow_texels_[off + 0]) / 127.0f + gx * wy, -1.0f, 1.0f);
            const float ny = std::clamp(static_cast<float>(flow_texels_[off + 1]) / 127.0f + gy * wy, -1.0f, 1.0f);
            flow_texels_[off + 0] = static_cast<int8_t>(nx * 127.0f);
            flow_texels_[off + 1] = static_cast<int8_t>(ny * 127.0f);
        }
    }
    flow_active_ = true;
}

void ParticleSim::add_shake_swirl() {
    // Sixaxis-style rotational vortex around screen center in the 32x16 flow field
    for (uint32_t y = 0; y < kFlowH; ++y) {
        const float dy = (static_cast<float>(y) / static_cast<float>(kFlowH - 1)) * 2.0f - 1.0f;
        for (uint32_t x = 0; x < kFlowW; ++x) {
            const float dx = (static_cast<float>(x) / static_cast<float>(kFlowW - 1)) * 2.0f - 1.0f;
            const float r2 = dx * dx + dy * dy;
            const float falloff = std::exp(-1.2f * r2) * 0.75f;
            const size_t off = (static_cast<size_t>(y) * kFlowW + static_cast<size_t>(x)) * 3u;
            const float nx = std::clamp(static_cast<float>(flow_texels_[off + 0]) / 127.0f - dy * falloff, -1.0f, 1.0f);
            const float ny = std::clamp(static_cast<float>(flow_texels_[off + 1]) / 127.0f + dx * falloff, -1.0f, 1.0f);
            flow_texels_[off + 0] = static_cast<int8_t>(nx * 127.0f);
            flow_texels_[off + 1] = static_cast<int8_t>(ny * 127.0f);
        }
    }
    flow_active_ = true;
}

void ParticleSim::decay_flow_field() {
    if (!flow_active_) return;
    bool any = false;
    for (int8_t& cell : flow_texels_) {
        if (cell == 0) continue;
        const float v = std::clamp((static_cast<float>(cell) / 127.0f) * 0.98f, -1.0f, 1.0f);
        cell = static_cast<int8_t>(v * 127.0f);
        if (cell != 0) any = true;
    }
    flow_active_ = any;
}

void ParticleSim::sample_flow_field(float world_x, float world_y, float out_xyz[3]) const noexcept {
    if (!flow_active_) {
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.0f;
        return;
    }
    const float u = std::clamp((world_x + 10.0f) * (1.0f / 20.0f), 0.0f, 1.0f) * static_cast<float>(kFlowW - 1);
    const float v = std::clamp((world_y + 6.0f) * (1.0f / 12.0f), 0.0f, 1.0f) * static_cast<float>(kFlowH - 1);
    const uint32_t x0 = static_cast<uint32_t>(u);
    const uint32_t y0 = static_cast<uint32_t>(v);
    const uint32_t x1 = std::min(x0 + 1u, kFlowW - 1u);
    const uint32_t y1 = std::min(y0 + 1u, kFlowH - 1u);
    const float fx = u - static_cast<float>(x0);
    const float fy = v - static_cast<float>(y0);

    auto fetch = [&](uint32_t x, uint32_t y, int ch) {
        return static_cast<float>(flow_texels_[(static_cast<size_t>(y) * kFlowW + x) * 3u + ch]) * (1.0f / 127.0f);
    };
    for (int ch = 0; ch < 3; ++ch) {
        const float c00 = fetch(x0, y0, ch), c10 = fetch(x1, y0, ch);
        const float c01 = fetch(x0, y1, ch), c11 = fetch(x1, y1, ch);
        const float cx0 = c00 + (c10 - c00) * fx;
        const float cx1 = c01 + (c11 - c01) * fx;
        out_xyz[ch] = cx0 + (cx1 - cx0) * fy;
    }
}

void ParticleSim::update_batch_avx512(float sim_dt, float age_dt) {
    // Exact Sony ParticleSimParams (0x031650, 0x0316d0):
    //   delta_time = (PART.delta_time, PART.delta_time, PART.delta_time, 1.0f)
    //   -> pos/vel integrate with sim_dt, while age integrates with age_dt (1.0 per 60Hz frame)!
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    const float wlen = std::sqrt(cfg_.wind_dir[0] * cfg_.wind_dir[0] +
                                 cfg_.wind_dir[1] * cfg_.wind_dir[1] +
                                 cfg_.wind_dir[2] * cfg_.wind_dir[2]);
    if (wlen > 1e-4f) {
        wx = (cfg_.wind_dir[0] / wlen) * cfg_.wind_scale;
        wy = (cfg_.wind_dir[1] / wlen) * cfg_.wind_scale;
        wz = (cfg_.wind_dir[2] / wlen) * cfg_.wind_scale;
    }
    const float grav_y = cfg_.gravity;
    const float fric = cfg_.friction;
    const float brown_scale = cfg_.brownian_scale;
    const float spin_scale = 0.5f * (cfg_.spin_time_scale * (1.0f / 60.0f));

#if defined(__AVX512F__)
    const __m512 v_dead = _mm512_set1_ps(kDeadSentinel);
    const __m512 v_dt = _mm512_set1_ps(sim_dt);
    const __m512 v_age_dt = _mm512_set1_ps(age_dt);
    const __m512 v_fric = _mm512_set1_ps(fric);
    const __m512 v_wind_x = _mm512_set1_ps(wx);
    const __m512 v_wind_y = _mm512_set1_ps(wy + grav_y);
    const __m512 v_wind_z = _mm512_set1_ps(wz);
    const __m512 v_brown = _mm512_set1_ps(brown_scale);
    const __m512 v_max_age = _mm512_set1_ps(kMaxAge);
    const __m512 v_min_x = _mm512_set1_ps(aabb_min_[0]);
    const __m512 v_min_y = _mm512_set1_ps(aabb_min_[1]);
    const __m512 v_min_z = _mm512_set1_ps(aabb_min_[2]);
    const __m512 v_max_x = _mm512_set1_ps(aabb_max_[0]);
    const __m512 v_max_y = _mm512_set1_ps(aabb_max_[1]);
    const __m512 v_max_z = _mm512_set1_ps(aabb_max_[2]);
#endif

    for (uint32_t base = 0; base < max_particles_; base += 16u) {
        const uint32_t count = std::min(16u, max_particles_ - base);

        alignas(64) float bx[16], by[16], bz[16];
        for (uint32_t k = 0; k < count; ++k) {
            float ff[3] = {0.0f, 0.0f, 0.0f};
            if (flow_active_) sample_flow_field(px_[base + k], py_[base + k], ff);
            bx[k] = rng_neg1_to_1(rng_seeds_[0]) * brown_scale + ff[0];
            by[k] = rng_neg1_to_1(rng_seeds_[1]) * brown_scale + ff[1];
            bz[k] = rng_neg1_to_1(rng_seeds_[2]) * brown_scale + ff[2];
        }
        for (uint32_t k = count; k < 16u; ++k) {
            bx[k] = by[k] = bz[k] = 0.0f;
        }

#if defined(__AVX512F__)
        const __m512 age = _mm512_loadu_ps(&age_[base]);
        const __mmask16 alive_mask = _mm512_cmp_ps_mask(age, v_dead, _CMP_NEQ_OQ);
        if (alive_mask == 0) continue;

        __m512 px = _mm512_loadu_ps(&px_[base]);
        __m512 py = _mm512_loadu_ps(&py_[base]);
        __m512 pz = _mm512_loadu_ps(&pz_[base]);
        __m512 vx = _mm512_loadu_ps(&vx_[base]);
        __m512 vy = _mm512_loadu_ps(&vy_[base]);
        __m512 vz = _mm512_loadu_ps(&vz_[base]);
        __m512 ag = _mm512_loadu_ps(&aging_[base]);

        (void)v_brown;
        const __m512 ax = _mm512_fnmadd_ps(v_fric, vx, _mm512_add_ps(_mm512_load_ps(bx), v_wind_x));
        const __m512 ay = _mm512_fnmadd_ps(v_fric, vy, _mm512_add_ps(_mm512_load_ps(by), v_wind_y));
        const __m512 az = _mm512_fnmadd_ps(v_fric, vz, _mm512_add_ps(_mm512_load_ps(bz), v_wind_z));
        // Note: friction.w = 0.0f in Sony ParticleSimParams (0x031494) -> aging rate is constant!

        // NOTE: _mm512_mask3_fmadd_ps(a, b, c, k) computes (a * b + c) on masked lanes, preserving c on unmasked lanes!
        vx = _mm512_mask3_fmadd_ps(ax, v_dt, vx, alive_mask);
        vy = _mm512_mask3_fmadd_ps(ay, v_dt, vy, alive_mask);
        vz = _mm512_mask3_fmadd_ps(az, v_dt, vz, alive_mask);

        px = _mm512_mask3_fmadd_ps(vx, v_dt, px, alive_mask);
        py = _mm512_mask3_fmadd_ps(vy, v_dt, py, alive_mask);
        pz = _mm512_mask3_fmadd_ps(vz, v_dt, pz, alive_mask);
        __m512 new_age = _mm512_mask3_fmadd_ps(ag, v_age_dt, age, alive_mask);

        const __mmask16 age_ok = _mm512_cmp_ps_mask(new_age, v_max_age, _CMP_LT_OQ);
        const __mmask16 box_ok =
            _mm512_cmp_ps_mask(px, v_min_x, _CMP_GE_OQ) & _mm512_cmp_ps_mask(px, v_max_x, _CMP_LE_OQ) &
            _mm512_cmp_ps_mask(py, v_min_y, _CMP_GE_OQ) & _mm512_cmp_ps_mask(py, v_max_y, _CMP_LE_OQ) &
            _mm512_cmp_ps_mask(pz, v_min_z, _CMP_GE_OQ) & _mm512_cmp_ps_mask(pz, v_max_z, _CMP_LE_OQ);
        const __mmask16 still_alive = alive_mask & age_ok & box_ok;
        const __mmask16 newly_dead = alive_mask & ~still_alive;

        new_age = _mm512_mask_blend_ps(newly_dead, new_age, v_dead);

        _mm512_storeu_ps(&px_[base], px);
        _mm512_storeu_ps(&py_[base], py);
        _mm512_storeu_ps(&pz_[base], pz);
        _mm512_storeu_ps(&vx_[base], vx);
        _mm512_storeu_ps(&vy_[base], vy);
        _mm512_storeu_ps(&vz_[base], vz);
        _mm512_storeu_ps(&age_[base], new_age);

        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t idx = base + k;
            if ((newly_dead >> k) & 1u) {
                dead_freelist_.push_back(idx);
                continue;
            }
            if (!((still_alive >> k) & 1u)) continue;

            const float theta = kTwoPi * age_[idx];
            const float avx = std::cos(0.37f * theta);
            const float avy = std::sin(0.17f * theta);
            const float avz = std::sin(0.31f * theta);

            const float qx = rx_[idx], qy = ry_[idx], qz = rz_[idx], qw = rw_[idx];
            const float nx = qx + (avx * qw + avy * qz - avz * qy) * spin_scale;
            const float ny = qy + (avy * qw + avz * qx - avx * qz) * spin_scale;
            const float nz = qz + (avz * qw + avx * qy - avy * qx) * spin_scale;
            const float nw = qw + (-avx * qx - avy * qy - avz * qz) * spin_scale;
            const float inv_len = 1.0f / std::max(std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw), 1e-6f);
            rx_[idx] = nx * inv_len;
            ry_[idx] = ny * inv_len;
            rz_[idx] = nz * inv_len;
            rw_[idx] = nw * inv_len;
        }
#else
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t idx = base + k;
            if (age_[idx] == kDeadSentinel) continue;

            const float ax = bx[k] + wx - fric * vx_[idx];
            const float ay = by[k] + wy + grav_y - fric * vy_[idx];
            const float az = bz[k] + wz - fric * vz_[idx];

            vx_[idx] += ax * sim_dt;
            vy_[idx] += ay * sim_dt;
            vz_[idx] += az * sim_dt;

            px_[idx] += vx_[idx] * sim_dt;
            py_[idx] += vy_[idx] * sim_dt;
            pz_[idx] += vz_[idx] * sim_dt;
            age_[idx] += aging_[idx] * age_dt;

            const bool in_box =
                px_[idx] >= aabb_min_[0] && px_[idx] <= aabb_max_[0] &&
                py_[idx] >= aabb_min_[1] && py_[idx] <= aabb_max_[1] &&
                pz_[idx] >= aabb_min_[2] && pz_[idx] <= aabb_max_[2];
            if (age_[idx] >= kMaxAge || !in_box) {
                age_[idx] = kDeadSentinel;
                dead_freelist_.push_back(idx);
                continue;
            }

            const float theta = kTwoPi * age_[idx];
            const float avx = std::cos(0.37f * theta);
            const float avy = std::sin(0.17f * theta);
            const float avz = std::sin(0.31f * theta);

            const float qx = rx_[idx], qy = ry_[idx], qz = rz_[idx], qw = rw_[idx];
            const float nx = qx + (avx * qw + avy * qz - avz * qy) * spin_scale;
            const float ny = qy + (avy * qw + avz * qx - avx * qz) * spin_scale;
            const float nz = qz + (avz * qw + avx * qy - avy * qx) * spin_scale;
            const float nw = qw + (-avx * qx - avy * qy - avz * qz) * spin_scale;
            const float inv_len = 1.0f / std::max(std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw), 1e-6f);
            rx_[idx] = nx * inv_len;
            ry_[idx] = ny * inv_len;
            rz_[idx] = nz * inv_len;
            rw_[idx] = nw * inv_len;
        }
#endif
    }
}

void ParticleSim::step(float dt_seconds, const RibbonSim* ribbon) {
    ++frame_counter_;
    const float frame_scale = std::clamp(dt_seconds, 0.0f, 0.05f) * 60.0f;
    const float sim_dt = (cfg_.delta_time > 0.0f) ? (cfg_.delta_time * frame_scale) : dt_seconds;

    auto rng01 = [&](int ch) -> float {
        return 0.5f * rng_neg1_to_1(rng_seeds_[ch]) + 0.5f;
    };

    if (ribbon) {
        // 1. Emit particles for Frame N-1 queued ribbon-vertex candidates (0x030b80..0x031488)
        const float denom_dt = std::max(sim_dt, 1e-4f);
        for (const EmitCandidate& cand : pending_emit_) {
            if (dead_freelist_.empty()) break;

            float cur_pos[3];
            ribbon->sample_world_pos(cand.u, cand.v, cur_pos);
            const float rvx = (cur_pos[0] - cand.prev_pos[0]) / denom_dt;
            const float rvy = (cur_pos[1] - cand.prev_pos[1]) / denom_dt;
            const float rvz = (cur_pos[2] - cand.prev_pos[2]) / denom_dt;
            const float v_len = std::sqrt(rvx * rvx + rvy * rvy + rvz * rvz);
            if (v_len < 1e-4f) continue;

            float ax = rvx / v_len, ay = rvy / v_len, az = rvz / v_len;
            if (rng01(0) < cfg_.emit_neg_prob) {
                ax = -ax; ay = -ay; az = -az;
            }

            // Orthonormal basis (T, B, axis)
            float tx = 0.0f, ty = az, tz = -ay;
            const float l0 = ty * ty + tz * tz;
            const float l1 = az * az + ax * ax;
            const float l2 = ay * ay + ax * ax;
            if (l1 >= l0 && l1 >= l2) { tx = -az; ty = 0.0f; tz = ax; }
            else if (l2 >= l0 && l2 >= l1) { tx = ay; ty = -ax; tz = 0.0f; }
            const float inv_t = 1.0f / std::max(std::sqrt(tx * tx + ty * ty + tz * tz), 1e-6f);
            tx *= inv_t; ty *= inv_t; tz *= inv_t;

            const float bx = ay * tz - az * ty;
            const float by = az * tx - ax * tz;
            const float bz = ax * ty - ay * tx;

            const float theta = rng01(1) * (cfg_.emit_cone_angle * kDegToRad);
            const float phi = rng01(2) * kTwoPi;
            const float st = std::sin(theta), ct = std::cos(theta);
            const float cp = std::cos(phi), sp = std::sin(phi);

            float dx = tx * (st * cp) + bx * (st * sp) + ax * ct;
            float dy = ty * (st * cp) + by * (st * sp) + ay * ct;
            float dz = tz * (st * cp) + bz * (st * sp) + az * ct;

            // Camera-Z velocity flattening (cam_forward = (0, 0, -1))
            dz *= cfg_.emit_vel_zscale;

            const float speed_sq = v_len * cfg_.emit_vel_mul * (1.0f + rng_neg1_to_1(rng_seeds_[0]) * cfg_.emit_vel_var);
            const float speed = std::max(std::sqrt(std::max(speed_sq, 0.0f)), cfg_.emit_vel_min);
            const float vel[3] = {dx * speed, dy * speed, dz * speed};
            const float aging = std::max(0.001f, cfg_.aging_speed * (1.0f + rng_neg1_to_1(rng_seeds_[1]) * cfg_.aging_variance));

            const uint32_t slot = dead_freelist_.back();
            dead_freelist_.pop_back();
            spawn_particle_with_vel(slot, cur_pos, vel, aging);
        }
        pending_emit_.clear();

        // 2. Queue Frame N candidates: Main random ribbon emitter (0x02dde4)
        if (rng01(0) < cfg_.emit_prob) {
            const int count = std::max(1, static_cast<int>(cfg_.emit_per_frame));
            for (int i = 0; i < count; ++i) {
                const uint32_t u = static_cast<uint32_t>(rng01(1) * static_cast<float>(RibbonSim::kGridDim - 1));
                const uint32_t v = static_cast<uint32_t>(rng01(2) * static_cast<float>(RibbonSim::kGridDim - 1));
                EmitCandidate cand{};
                ribbon->sample_world_pos(u, v, cand.prev_pos);
                cand.u = u;
                cand.v = v;
                pending_emit_.push_back(cand);
            }
        }

        // 3. Ribbon-row burst emitter (0x02dbf4)
        if (burst_remaining_ == 0) {
            if (rng01(0) < 0.06f) {
                burst_u_ = static_cast<uint32_t>(rng01(1) * static_cast<float>(RibbonSim::kGridDim - 1));
                burst_v_ = static_cast<uint32_t>(rng01(2) * static_cast<float>(RibbonSim::kGridDim - 1));
                burst_remaining_ = static_cast<uint32_t>(std::max(20, 100 + static_cast<int>(rng_neg1_to_1(rng_seeds_[0]) * 80.0f)));
            }
        } else if ((frame_counter_ & 1u) != 0u) {
            if (burst_u_ >= RibbonSim::kGridDim) {
                burst_remaining_ = 0;
            } else {
                EmitCandidate cand{};
                ribbon->sample_world_pos(burst_u_, burst_v_, cand.prev_pos);
                cand.u = burst_u_;
                cand.v = burst_v_;
                pending_emit_.push_back(cand);
                ++burst_u_;
                --burst_remaining_;
            }
        }
    } else {
        // Standalone fallback emitter for unit tests without a RibbonSim
        const uint32_t to_spawn = std::min<uint32_t>(4u, static_cast<uint32_t>(dead_freelist_.size()));
        for (uint32_t i = 0; i < to_spawn; ++i) {
            const uint32_t slot = dead_freelist_.back();
            dead_freelist_.pop_back();
            spawn_fallback_particle(slot);
        }
    }

    // 4. SPU particles.elf AVX-512 batch integration (delta_time.w = 1.0f -> age_dt = frame_scale)
    update_batch_avx512(sim_dt, frame_scale);
    decay_flow_field();

    // 5. Stream surviving particles as 4-vertex quads matching particles_quads.vert
    vertices_.clear();
    indices_.clear();
    active_count_ = 0;

    static constexpr float kQuadUVs[4][2] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    };

    for (uint32_t i = 0; i < max_particles_; ++i) {
        if (age_[i] == kDeadSentinel) continue;

        const float alpha = eval_fade_alpha(age_[i]);
        const uint32_t base_v = static_cast<uint32_t>(vertices_.size());

        for (int c = 0; c < 4; ++c) {
            ParticleVertex v{};
            v.pos_alpha[0] = px_[i];
            v.pos_alpha[1] = py_[i];
            v.pos_alpha[2] = pz_[i];
            v.pos_alpha[3] = alpha;
            v.uv0[0] = kQuadUVs[c][0];
            v.uv0[1] = kQuadUVs[c][1];
            v.rot[0] = rx_[i];
            v.rot[1] = ry_[i];
            v.rot[2] = rz_[i];
            v.rot[3] = rw_[i];
            vertices_.push_back(v);
        }

        indices_.push_back(base_v + 0);
        indices_.push_back(base_v + 1);
        indices_.push_back(base_v + 2);
        indices_.push_back(base_v + 0);
        indices_.push_back(base_v + 2);
        indices_.push_back(base_v + 3);
        ++active_count_;
    }
}

} // namespace xmb::sim
