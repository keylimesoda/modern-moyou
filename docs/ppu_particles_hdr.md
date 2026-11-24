# Sony PS3 OFW 4.76 PPU Reverse Engineering — Particles & HDR/Bloom Pipeline

> **Binary:** `custom_render_plugin.prx` (PS3 OFW 4.76, 915 KB big-endian PowerPC64 PRX, `PT_LOAD#0` `.text` `0x00000..0x8bc70`, `.rodata` `0x8bc70..0x9c010`, `PT_LOAD#1` `.data`/`.bss` `0x9c010..0xa2a50`, `PT_SCE_PPURELA` 10,863 relocations).
> **Companion Specs:** [`docs/spu_kernels.md`](spu_kernels.md) (`particles.elf` SPU kernel & `ParticleSimParams`), [`docs/shaders.md`](shaders.md) (RSX shaders for `particles_quads`, `particles_second`, and `lib/glutils/` HDR passes), [`extracted/stock_ofw/`](../extracted/stock_ofw/) (`PARTICLES.mnu`, `PARTICLES_SPE.mnu`, `PARTICLES_UI.mnu`, `HDR.mnu`, `particles/proc_iridescent.tga`).

All addresses below are virtual offsets in `custom_render_plugin.prx`.

---

## 1. Particle Pool, Ribbon Emission & `ParticleSimParams` (`particles.elf` Host)

### 1.1 Pool Capacity & Buffer Allocation (`0x02dfb4`, `0x02e0dc`, `0x060b14`, `0x05e5c8`)

- Internal system name: `"neutrinos"` (`0x02dfb4`).
- Requested capacity: **`2048` particles** (`0x02e0dc`: `li r5, 2048; bl 0x60b14`).
- `ParticleSystem::ParticleSystem(n)` (`0x060b14`) rounds capacity up to a multiple of 512:
  ```cpp
  constexpr uint32_t kRequestedParticles = 2048;
  uint32_t capacity = (kRequestedParticles + 511u) & ~511u; // 2048
  ```
- Allocations (`0x05e5c8`, `0x060b14`):
  - **Double-buffered GPU particle arrays:** `2 × (capacity × 32 bytes)` = `2 × 65,536 bytes` (matches `SPU_Particle` 32-byte layout in `spu_kernels.md`: `vec4 pos_age`, `vec4 vel_aging`).
  - **Double-buffered `ParticleSimParams`:** `2 × 0x900` (`2304`) bytes at `ParticleSystem + 0x100` and `ParticleSystem + 0xA00`.
  - **Double-buffered command queues:** `2 × 1024 bytes` (`cmd_buf[0..1]`) for PPU→SPU spawn/kill commands (`cmd 0` alloc free-list slot, `cmd 1` set `pos/age`, `cmd 2` set `vel`, `cmd 4` set `aging`).

### 1.2 Spawn Volume & Candidate Ribbon Vertex Selection (`0x02dbf4`, `0x02dde4`, `0x02d640`, `0x02d6e8`)

Particles are **not** spawned in an arbitrary box or plane — they are spawned **directly on vertices of the tessellated 128×128 ribbon mesh** (`RibbonVertex` stride = 32 bytes, `slwi r5, r5, 5` at `0x02d640`), transformed into world space by the ribbon's model matrix.

Because emission velocity is derived from the ribbon's **instantaneous surface velocity**, spawning is a **two-frame deferred process**:
1. **Frame $N$ (`0x02dde4` + `0x02dbf4`):** Select candidate `(u_idx, v_idx)` ribbon grid coordinates, sample their world-space position `prev_pos`, and push `(prev_pos, u_idx, v_idx)` into a pending candidate queue (`this + 144`).
2. **Frame $N+1$ (`0x030b80..0x031488`):** Re-sample `cur_pos = ribbon_world_pos(u_idx, v_idx)` for every queued candidate, compute `ribbon_vel = (cur_pos - prev_pos) / dt`, and emit a particle with cone-scattered velocity.

There are **two** candidate selectors run per frame:

#### A. Main Random Ribbon Emitter (`0x02dde4`)
Uses `PARTICLES.mnu` parameters `emit prob` (`+28`) and `emit per frame` (`+24`):
```cpp
// rng_neg1_to_1() = 0x889f8 -> uniform float in [-1, 1]
auto rng01 = []() -> float { return 0.5f * rng_neg1_to_1() + 0.5f; };

if (rng01() < params.emit_prob) {
    for (int i = 0; i < params.emit_per_frame; ++i) {
        int u = static_cast<int>(rng01() * float(ribbon_width  - 1));
        int v = static_cast<int>(rng01() * float(ribbon_height - 1));
        vec3 prev_pos = sample_ribbon_world_pos(u, v); // 0x02d6e8 wraps (u % W, v % H)
        pending_emit_queue.push({prev_pos, u, v});
    }
}
```

#### B. Ribbon-Row Burst / Stream Emitter (`0x02dbf4`)
Creates occasional coherent "trails" marching along a ribbon row (`+u` direction):
```cpp
// Constants: rodata@0x923e4 = 0.06f, rodata@0x923e8 = 80.0f
if (burst_remaining == 0) {
    if (rng01() < 0.06f) {
        burst_u         = static_cast<int>(rng01() * float(ribbon_width  - 1));
        burst_v         = static_cast<int>(rng01() * float(ribbon_height - 1));
        burst_remaining = 100 + static_cast<int>(rng_neg1_to_1() * 80.0f); // [20, 180]
    }
} else {
    // Only emits on odd frames (0x8a978() & 1 != 0)
    if ((frame_counter() & 1u) != 0) {
        if (burst_u >= ribbon_width) {
            burst_remaining = 0;
        } else {
            vec3 prev_pos = sample_ribbon_world_pos(burst_u, burst_v);
            pending_emit_queue.push({prev_pos, burst_u, burst_v});
            burst_u += 1;
            burst_remaining -= 1;
        }
    }
}
```

### 1.3 Exact Cone Emission Math (`0x030b80..0x031488`, `0x02b560`)

Constants in `.rodata`:
- `0x9242c`: `1.0e-4f` (minimum ribbon velocity magnitude)
- `0x92438`: `0.017453292f` ($\pi / 180$, `deg2rad`)
- `0x9243c`: `6.2831855f` ($2\pi$)
- `0x92440`: `0.001f` (minimum aging rate)

Camera-Z velocity flattening helper (`0x02b560`):
- Projects `dir` onto `cam_forward`, scales **only** the component parallel to `cam_forward` by `emit_vel_zscale`, and recombines with the perpendicular component:
  $$\mathbf{d}' = (\mathbf{d} - \mathbf{d}_\parallel) + s_z\,\mathbf{d}_\parallel, \qquad \mathbf{d}_\parallel = \frac{\mathbf{d}\cdot\mathbf{f}}{\mathbf{f}\cdot\mathbf{f}}\,\mathbf{f}$$
  With stock `PARTICLES.mnu` `emit vel zscale = 0.05`, particle velocities are flattened almost into the camera's XY plane.

```cpp
// Stock PARTICLES.mnu defaults:
// emit_vel_min = 0.015, emit_vel_mul = 0.005, emit_vel_var = 0.48
// emit_cone_angle = 13.7 (deg), emit_neg_prob = 0.5, emit_vel_zscale = 0.05
// emit_per_frame = 2, emit_prob = 0.37
// aging_speed = 0.006, aging_variance = 0.5

for (const EmitCandidate& c : pending_emit_queue) {
    vec3 cur_pos    = sample_ribbon_world_pos(c.u, c.v);
    vec3 ribbon_vel = (cur_pos - c.prev_pos) / sim_dt; // sim_dt = PARTICLES.mnu "delta time" (0.7)
    float v_len     = length(ribbon_vel);
    if (v_len < 1e-4f) continue;

    vec3 axis = ribbon_vel / v_len;
    if (rng01() < params.emit_neg_prob) {
        axis = -axis; // 0x03148c
    }

    // Orthonormal basis (T, B, axis): pick largest perpendicular among (0, az, -ay), (-az, 0, ax), (ay, -ax, 0)
    vec3 p0 = {0.0f, axis.z, -axis.y};
    vec3 p1 = {-axis.z, 0.0f, axis.x};
    vec3 p2 = {axis.y, -axis.x, 0.0f};
    vec3 T  = (dot(p0, p0) >= dot(p1, p1) && dot(p0, p0) >= dot(p2, p2)) ? p0
            : (dot(p1, p1) >= dot(p2, p2) ? p1 : p2);
    T = normalize(T);
    vec3 B = cross(axis, T);

    // Uniform cone angle & azimuth
    float theta = rng01() * (params.emit_cone_angle * 0.017453292f);
    float phi   = rng01() * 6.2831855f;
    float st = sinf(theta), ct = cosf(theta);
    vec3 dir = T * (st * cosf(phi)) + B * (st * sinf(phi)) + axis * ct;

    // Camera-Z velocity flattening (0x02b560)
    float proj_t = dot(dir, cam_forward) / dot(cam_forward, cam_forward);
    vec3 dir_par = cam_forward * proj_t;
    dir = (dir - dir_par) + dir_par * params.emit_vel_zscale;

    // Speed: sqrt(|ribbon_vel| * emit_vel_mul * (1 + rng[-1,1] * emit_vel_var)), floored at emit_vel_min
    float speed_sq = v_len * params.emit_vel_mul * (1.0f + rng_neg1_to_1() * params.emit_vel_var);
    float speed    = fmaxf(powf(fmaxf(speed_sq, 0.0f), 0.5f), params.emit_vel_min); // 0x87878(val, 0.5f)
    vec3  vel      = dir * speed;

    // Aging rate (age advances by aging * dt per frame in SPU kernel)
    float aging = fmaxf(0.001f, params.aging_speed * (1.0f + rng_neg1_to_1() * params.aging_variance));

    // Queue 4 SPU commands: alloc slot (0), pos+age=0 (1), vel (2), aging (4)
    particle_system.spawn(cur_pos, vel, aging);
}
```

### 1.4 `ParticleSimParams` Setup & 2D `int8` Flow-Field (`0x031494..0x031e44`, `0x02c588`, `0x02d1ec`, `0x05d47c`)

Every field matches the `0x900`-byte `ParticleSimParams` struct documented in `docs/spu_kernels.md`:

| Offset | Field | PPU Formula / Value | PRX Address |
|---|---|---|---|
| `+0x000` | `gravity` (`vec4`) | `(0.0f, PART.gravity, 0.0f, 0.0f)` (ctor default `(0, -5e-6f, 0, 1)` at `0x94af0`) | `0x031494` |
| `+0x010` | `friction` (`vec4`) | `(PART.friction, PART.friction, PART.friction, 0.0f)` (ctor default `0.01f`) | `0x031494` |
| `+0x080` | `flow_texels` (`int8[32*16*3]`) | **32 × 16** RGB8 signed flow-field (`512 × 3 = 1536` bytes, `+0x080..+0x67F`) | `0x05d47c` |
| `+0x700` | `M_from_field` (`mat4`) | World-from-field transform computed from camera frustum at depth **`z = 9.0f`** (`rodata@0x92458`) | `0x031a78..0x031d30` |
| `+0x740` | `M_to_field` (`mat4`) | `inverse(M_from_field)` | `0x031d30` |
| `+0x800` | `aabb_min` (`vec4`) | **`(-10.0f, -10.0f, -12.0f, 0.0f)`** (`rodata@0x940b0`; collapsed to `0` if kill-all flag set) | `0x031520` |
| `+0x810` | `aabb_max` (`vec4`) | **`(+10.0f, +10.0f,  +7.0f, 0.0f)`** (`rodata@0x940c0`; collapsed to `0` if kill-all flag set) | `0x031520` |
| `+0x830` | `swirl_center` (`vec4`) | `cam_pos - 2.0f * normalize(cam_pos - cam_target)` | `0x02d1ec` |
| `+0x840` | `swirl_rot_mtx` (`mat4`) | `Identity` if `|swirl_axis_angle| < 1e-5f` (`0x923d4`), else `mat4_from_axis_angle(swirl_axis_angle)` (`0x24228`) | `0x0315a0` |
| `+0x880` | `swirl_axis_angle` (`vec4`) | Zeroed per frame unless Sixaxis/D-pad/shake UI impulse active (`0x02c3e0`, `0x02d02c`) | `0x0315a0` |
| `+0x890` | `wind_vec` (`vec4`) | `(|wind_dir| > 1e-4f ? normalize(wind_dir) * wind_scale : vec3(0)) + icon_wind_vec` (`rodata@0x92450`) | `0x031650` |
| `+0x8A0` | `wind_field_scale` (`float`) | `1.0f` default (`this + 192` at `0x324b0`) | `0x031690` |
| `+0x8A4` | `brownian_scale` (`float`) | `PART.brownian_scale + shake_brownian * UI.brownian + rshake_brownian` | `0x0316a0` |
| `+0x8AC` | `spin_time_scale` (`float`) | `PART.spin_time_scale` | `0x0316c0` |
| `+0x8B0` | `delta_time` (`vec4`) | `(PART.delta_time, PART.delta_time, PART.delta_time, 1.0f)` | `0x0316d0` |

**Per-Frame 2D Flow-Field Decay (`0x02c588`):**
Every frame, each of the `32 × 16 = 512` signed-byte RGB texels is decayed by `0.98f` (`this + 188`, initialized at `0x3248c`):
```cpp
for (int i = 0; i < 32 * 16 * 3; ++i) {
    float v = float(flow_texels[i]) * (1.0f / 127.0f);
    v = std::clamp(v * 0.98f, -1.0f, 1.0f);
    flow_texels[i] = static_cast<int8_t>(v * 127.0f);
}
```
When idle (no XMB icon scroll / Sixaxis shake injecting impulses), `flow_texels` decays to zero and only `wind_vec`, `gravity`, `friction`, and `brownian_scale` drive particle motion.

---

## 2. Particle Rendering (`particles_quads` & `particles_second`)

### 2.1 Render State, Blend Mode & Draw Order (`0x060660..0x0607ec`, `0x05c254`)

1. **GL / RSX State Setup (`0x0606e0..0x060734`, `0x05c2c0..0x05c320`):**
   - **Blending:** **Enabled** (`0x8031` = `GL_BLEND`), **`glBlendFunc(GL_ONE, GL_ONE)`** (pure additive blending), **`glBlendEquation(GL_FUNC_ADD)`** (`0x8006`).
   - **Alpha Test:** **Enabled** (`0x8032` = `GL_ALPHA_TEST`), **`glAlphaFunc(GL_GREATER, 0.0f)`** (`516`).
   - **Depth Test / Depth Write / Culling:** **Depth test disabled**, **`glDepthMask(GL_FALSE)`** (`0x403f8(ctx, 0)`), **backface culling disabled** (`0x05c2c0..0x05c320` disables `0x8059`, `0x8034`, `0x8039`, `0x8036`).
2. **Two-Pass Draw Order (`0x060738..0x0607ec`):**
   - **Pass 1** (when `(draw_flags + 1) & 1`): renders all live particles with `particles_quads.vpo` + `particles_quads.fpo` (`bss@0xa0f64`).
   - **Pass 2** (when `(draw_flags + 1) & 2`): renders all live particles with `particles_second.vpo` + `particles_second.fpo` (`bss@0xa0f74`).
   - Both passes share the exact same uniform upload routine (`0x05c254`) and additive blend state, rendering **into the HDR scene FBO** after the ribbon (`moyou`).

### 2.2 Iridescent Texture (`0x05ea50..0x05eab4`)

- `_IridescentTex` is **not** procedurally generated at runtime on PPU — it is loaded directly from the static asset `/dataplat/particles/proc_iridescent.tga` (unpacked at `extracted/stock_ofw/particles/proc_iridescent.tga`).
- Verified file format: **128 × 128**, 32-bit BGRA uncompressed TGA (`img_type = 2`, 65,580 bytes).
- Sampler state (`0x05ea80..0x05eab4`): `WRAP_S = GL_CLAMP_TO_EDGE`, `WRAP_T = GL_CLAMP_TO_EDGE`, linear filtering.

### 2.3 Exact Uniform Packing (`0x05c254..0x05c650`, `0x035500..0x035c00`)

All `PARTICLES.mnu` shader parameters live in `ParticleSystem` starting at base `+4784` (`r31 + 5056..5192`):

| Shader Uniform | Type | Exact Packing (PRX `0x05c254`) | `PARTICLES.mnu` Default |
|---|---|---|---|
| `_ModelviewProjection` | `mat4` | `Projection * View` (model is identity; particles are in world space) | — |
| `_Modelview` | `mat4` | `View` matrix | — |
| `_Camera` | `mat4` | Camera world transform (`inverse(View)`) | — |
| `_LightPack[0]` (col 0) | `vec4` | `(eyePos.x, eyePos.y, eyePos.z, 1.0f)` (`0x5bd30`: `cam_world[3]`) | Camera pos |
| `_LightPack[1]` (col 1) | `vec4` | `(light_pos.x, light_pos.y, light_pos.z, specular_power)` (`5056..5064`, `5088`) | `(0, 1.15, 0, 10.7)` |
| `_LightPack[2]` (col 2) | `vec4` | `(spot_attn.x, spot_attn.y, spot_attn.z, 0.0f)` (`5072..5080`, `0.0f`) | `(1.0, 0.0, 0.0, 0.0)` |
| `_LightPack[3]` (col 3) | `vec4` | `(lambert_coeff, specular_coeff, exposure, color_control)` (`5096`, `5092`, `5100`, `5108`) | `(0.0, 100.0, 0.99, 0.0)` |
| `_ParticleSize` | `vec4` | `(size_middle, size_near, size_far, 0.0f)` (`5120`, `5124`, `5128`) | `(0.0085, 0.0884, 0.023, 0)` |
| `_Transparency` | `vec2` | `(fresnel, global_alpha)` (`5104`, `5116`) | `(1.08, 0.89)` |
| `_Focus` | `vec4` | `(near_focus, near_focus + near_focus_dist, far_focus, far_focus + far_focus_dist)` (`0x5c498..0x5c4d0`) | `(0.0, 0.72, 1.56, 3.06)` |
| `_FocusCurves` | `vec3` | `(near_focus_pow, far_focus_pow, focus_curve_z)` (`5140`, `5160`, `4952` set via `0x5de2c`) | `(0.34, 0.87, ...)` |
| `_FrontFacingQuaternion` | `vec4` | Camera rotation quaternion (`0x889c0(cam_world)`) | — |
| `_NearControl` | `vec4` | `(size_align, near_fuzziness, near_align, 0.0f)` (`5172`, `5148`, `5168`) | `(0.17, 1.0, 0.78, 0)` |
| `_LifeBoundsMin` | `vec3` | `ParticleSimParams.aabb_min.xyz` (`r31 + 2304`) | `(-10, -10, -12)` |
| `_LifeBoundsMax` | `vec3` | `ParticleSimParams.aabb_max.xyz` (`r31 + 2320`) | `(+10, +10, +7)` |
| `_Darkness` | `vec2` | `(near_darkness, far_darkness)` (`5144`, `5164`) | `(0.26, 0.37)` |
| `_Glare` | `vec4` | `(glare, glare_scale, glare_p1, glare_p2)` (`5176`, `5180`, `5184`, `5188`) | `(0.0, 0.48, 0.64, 0.77)` |
| `_Color` (FP) | `vec4` | Global tint (`r31 + 4928`, default `(1, 1, 1, 1)`) | `(1, 1, 1, 1)` |
| `_Gamma` (FP) | `float` | Particle gamma (`r31 + 4944`, default `1.0f`) | `1.0` |
| `_IridescentExp` (FP) | `float` | `iridescent_exp` (`5112`) | `1.13` |

---

## 3. HDR, Bloom (Glare) & Tonemap Pipeline (`HDR.mnu` + `lib/glutils/`)

### 3.1 Offscreen Render Targets & Resolutions (`0x0760c0`, `0x07a63c`, `0x07f180`, `0x071570`, `0x0821f4`)

Stock `HDR.mnu` values:
`EXPOSURE = 1.0`, `WHITE LEVEL = 1.0`, `GLARE LEVEL = 0.25`, `GLARE THRESH = 0.06`, `GAUSSIAN RAD R/G/B = 1.65`, `GLARE SUM POW = 0.38`, `TEX SIZE = 8`, `TEX MAX MIP = 5`, `GLARE = 1`, `GLARE_ONLY = 0`, `TONEBEFORE = 1`, `BLUR = 0`, `AUTO EXPOSE = 0`.

1. **Main HDR Scene Buffer:** Full screen (`W × H`, ctor default `1920 × 1080` at `0x7159c`/`0x715e8`), `RGBA16F` (`0x802d` = RSX FP16 format).
2. **Glare / Bloom Pyramid (`0x07f180..0x07f1ec`):**
   ```cpp
   // TEX SIZE = 8, TEX MAX MIP = 5 in stock HDR.mnu
   int glare_h  = 1 << (tex_size - 1);                     // 1 << 7 = 128
   int glare_w  = (aspect > 1.5f) ? (glare_h << 1) : glare_h; // 256 (16:9) or 128 (4:3)
   int num_mips = ((tex_size - 1) < tex_max_mip) ? tex_size : tex_max_mip; // 5 mips: 0..4
   ```
   - **Ping-pong mip chains:** Two `RGBA16F` mip pyramids of base size `glare_w × glare_h` (`256 × 128` in 16:9), with `num_mips = 5` levels: `256×128`, `128×64`, `64×32`, `32×16`, `16×8`.
   - **Intermediate half-height downsample target:** `(glare_w, H / 2)` used as pass 1 of `get_glare_source`.
3. **Luminance Readback Target (`0x07a63c..0x07a820`):**
   - `32 × 32` `RGBA16F` FBO + double-buffered PBOs (`32 × 32 × 8 = 8192` bytes each) for `HDR::sample_luminance` (`0x0722fc`).
4. **Pre-Exposure LUTs & Animated Dither Texture (`0x0760c0..0x07651c`, `0x071794..0x071c68`):**
   - **Two double-buffered `128 × 128` `RG16F` (`0x8029`) LUT textures** (bound to texture units `15` and `14` in `HDR::beginFrame` at `0x071d48..0x071db4`), rebuilt every frame in `0x071794` from `EXPOSURE`, `WHITE LEVEL`, `GLARE THRESH`, and `PreExpose` scale.
   - **One double-buffered `32 × 32` `RGBA8` (`0x8018`) animated dither/noise texture** (bound to unit `13` at `0x071db8..0x071de8`), filled per frame with an integer hash PRNG (`x ^= x << 13; (x*(x*x*15731 + 789221) + 1376312589) & 0x7fffffff`), scaled by `dither_amount * 8.0f`, clamped `[-1, 1]`, quantized to `uint8`.
   - **`PreExposeMem` (`0x0821f4..0x08251c`, `0x082690..0x082cd8`):** Allocates the scratch FBO/memory pool (`"PreExposeMem"`) used when `TONEBEFORE = 1` (pre-exposed / LUT-tonemapped rendering path) so `GlareSourcePre.fpo` extracts glare directly from pre-exposed/tonemapped scene luminance, and `HDR::endFrame` composites final output + glare + dither in `0x077398`.

### 3.2 Exact Per-Frame Pass Sequence (`HDR::beginFrame` `0x071cc4`, `HDR::endFrame` `0x07d02c..0x07d54c`)

1. **`HDR::beginFrame` (`0x071cc4`):**
   - If `AUTO EXPOSE` (`this + 412`, default `0`) is enabled, updates `EXPOSURE` & `WHITE LEVEL` (`0x070b54`) from previous frame's `sample_luminance` stats.
   - Rebuilds and uploads the `128×128` Reinhard tonemap LUTs and `32×32` dither texture (`0x071794`), binding them to units `15`, `14`, `13`.
   - Binds the full-screen `RGBA16F` HDR FBO. Scene (background gradient, ribbon `moyou`, particles `particles_quads` + `particles_second`) renders into it.
2. **`HDR::endFrame` (`0x07d02c`):**
   - Disables alpha test (`0x8032`), depth/cull (`0x8036`, `0x8034`), blend (`0x8031`).
   - If not `GLARE_ONLY` bypass (`this + 720 == 0`, at `0x07d380..0x07d54c`):
     - **Pass 1 — `HDR::get_glare_source` (`0x079818`):**
       - **Subpass 1a:** Downsample full-res scene to `(glare_w, H / 2)` using `GlareSourcePre.fpo` (when `TONEBEFORE = 1`) or `GlareSource.fpo`, passing:
         - `_Threshold = GLARE_THRESH` (`0.06f`)
         - `_Exposure = EXPOSURE` (`1.0f`)
         - `_WhiteSqrRcp = 1.0f / (WHITE_LEVEL * WHITE_LEVEL)` (`1.0f`)
       - **Subpass 1b:** Downsample `(glare_w, H / 2)` to mip 0 `(glare_w, glare_h)` (`256×128`) with 4 bilinear tap offsets `(±0.5/w, ±0.5/h)` (`0x079dc4..0x07a050`) + `_Threshold`, `_Exposure`, `_WhiteSqrRcp`.
     - **Pass 2 — `HDR::make_glare` (`0x07b0a4`):**
       - Computes per-mip bloom weights for `k = 0 .. num_mips - 1` (`0x07b0a4..0x07b200`):
         ```cpp
         float sum = 0.0f;
         for (int k = 0; k < num_mips; ++k) {
             mip_w[k] = powf(GLARE_SUM_POW, float(k)); // GLARE_SUM_POW = 0.38
             sum += mip_w[k];
         }
         for (int k = 0; k < num_mips; ++k) {
             mip_w[k] = (mip_w[k] / sum) * GLARE_LEVEL; // GLARE_LEVEL = 0.25
         }
         ```
         For stock `HDR.mnu` (`GLARE_SUM_POW = 0.38`, `GLARE_LEVEL = 0.25`, `num_mips = 5`):
         normalized weights before `GLARE_LEVEL` are `[0.6249, 0.2375, 0.0902, 0.0343, 0.0130]`; after `× 0.25`: `[0.1562, 0.0594, 0.0226, 0.0086, 0.0033]`.
       - For each mip `k = 0 .. num_mips - 1`:
         - **Horizontal 1D Gaussian blur** into ping-pong chain A at viewport `(glare_w >> k, glare_h >> k)`, texel step `((1.0f / glare_w) * float(1 << k), 0.0f)` using per-channel RGB weights `_Weights0..7`.
         - **Vertical 1D Gaussian blur** into ping-pong chain B at viewport `(glare_w >> k, glare_h >> k)`, texel step `(0.0f, (1.0f / glare_h) * float(1 << k))`.
       - **Mip Accumulation (`0x07c384..0x07c680`):** Sums mips from coarsest (`num_mips - 1`) to finest (`0`) with additive blending (`glBlendFunc(GL_ONE, GL_ONE)`), scaling each mip by `mip_w[k]`.
     - **Pass 3 — `HDR::sample_luminance` (`0x0722fc`):**
       - Renders scene luminance into `32×32` `RGBA16F`, reads back to CPU, computes `log_avg_lum = expf(mean(logf(L + 0.001f)))` (only affects exposure if `AUTO EXPOSE = 1`).
   - **Pass 4 — Final Tonemap & Glare Composite (`0x077398`):**
     - Applies Reinhard extended tonemapping + adds accumulated glare + `32×32` dither into the backbuffer:
       $$L_{\text{exposed}} = \text{EXPOSURE} \cdot C_{\text{hdr}}, \qquad C_{\text{tm}} = \frac{L_{\text{exposed}} \cdot \left(1 + L_{\text{exposed}} \cdot \frac{1}{\text{WHITE\_LEVEL}^2}\right)}{1 + L_{\text{exposed}}}$$
       (Verified both in `0x071948..0x07197c` LUT builder and `0x070c44` uniform packer.)

### 3.3 Exact Gaussian Kernel Weight Formula (`0x070cec`, `0x071260`, `0x071384`)

Built independently for **R, G, B** from `GAUSSIAN RAD R/G/B` (`this + 688, 692, 696`, range `[0.46, 2.9]`, stock `1.65`):

```cpp
// 0x070cec: rodata@0x931a8 = 2.5066283f = sqrt(2 * pi)
inline float gaussian_pdf(float x, float sigma) {
    return expf(-(x * x) / (2.0f * sigma * sigma)) / (sigma * 2.50662827f);
}

// 0x071260: rodata@0x931ac = 3.0f, 0x876b8 = ceilf
int build_gaussian_kernel(float sigma, float out_weights[/*2*radius+1*/]) {
    int radius = static_cast<int>(ceilf(sigma * 3.0f)) - 1;
    int count  = 2 * radius + 1;
    float sum  = 0.0f;
    for (int i = 0; i < count; ++i) {
        float x        = float(i) - float(radius);
        float w        = gaussian_pdf(x, sigma);
        out_weights[i] = w;
        sum           += w;
    }
    for (int i = 0; i < count; ++i) {
        out_weights[i] /= sum;
    }
    return radius; // stored in HDR+648 (R), HDR+652 (G), HDR+656 (B)
}
```

- **Uniform packing into `_Weights0..7` (`0x07b0a4`):**
  - `FilterSize = radius + 1` (half-kernel including center tap; `FilterSize0 = min(radius + 1, 4)`).
  - Tap `j = 0 .. radius` packs `vec3(w_R[radius + j], w_G[radius + j], w_B[radius + j])` (the shader applies `w[0]` at center and `w[j]` symmetrically to `±j` offsets).
  - For stock `GAUSSIAN RAD R/G/B = 1.65`: `radius = ceil(4.95) - 1 = 4` (9-tap kernel, `FilterSize = 5`), half-kernel weights:
    `w[0] = 0.2449`, `w[1] = 0.2039`, `w[2] = 0.1176`, `w[3] = 0.0470`, `w[4] = 0.0130` (`w[0] + 2*(w[1]+w[2]+w[3]+w[4]) = 1.0000`).
