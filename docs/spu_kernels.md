# PlayStation 3 XMB (`lines.qrc`) Cell SPU Kernel Reverse Engineering

This document records the complete reverse engineering of the two stock Cell Broadband Engine SPU SPURS task binaries unpacked from `lines.qrc`:

- `extracted/dblue_day/spurs/moyou/spline/spline.elf` (`.text`: `0x56c8` bytes, `.rodata`: `0x1390` bytes, `.bss`: `0x4c70` bytes)
- `extracted/dblue_day/spurs/particles/particles/particles.elf` (`.text`: `0x7930` bytes, `.rodata`: `0x4b0` bytes, `.bss`: `0x2520` bytes)

Both binaries were disassembled with [`tools/disasm_spu.py`](../tools/disasm_spu.py) (0 unknown instructions across 13,384 total instructions) into:

- [`re/spu/spline.asm`](../re/spu/spline.asm) (5,586 instructions + `.rodata`/`.data` dumps)
- [`re/spu/particles.asm`](../re/spu/particles.asm) (7,798 instructions + `.rodata`/`.data` dumps)

---

## 1. Common SPURS Task Architecture

Both binaries are Sony `libspurs` SPURS Tasks compiled with Sony's SPU C++ toolchain (notice standard `CELL_SPURS_TASK_ERROR_*` strings in `.rodata` and C++ vtables for double-buffered DMA ring streams).

- **Entry point (`0x3070`)**: Standard SPURS crt0. Sets up the stack pointer from Local Store top (`0x3ffd0`), calls `.init` (`sub_03010`, which walks `.ctors`), invokes `main` (`sub_05ec8` in `spline.elf`, `sub_080e8` in `particles.elf`), and exits via `cellSpursTaskExit`.
- **Work loop**: Both `main` functions initialize a SPURS event-flag / task-queue receiver, block waiting for a PPU kick (`bit 0x10` = work ready, `bit 0x20` = shutdown), DMA a batch descriptor block from main memory, iterate over the job entries in the batch, signal completion back to the PPU, and loop until shutdown.

### Key Architectural Discovery (PPU vs. SPU Split)

Neither SPU kernel performs the full simulation described in `LINE1.mnu` / `PARTICLES.mnu` on its own — Sony split the lightweight control/spawn logic on the PPU from the heavy SIMD math on the SPU:

| Subsystem | Runs on PPU | Runs on SPU (`*.elf`) |
|---|---|---|
| **Ribbon (`LINE1.mnu`)** | Spring/cloth grid integration (`DAMPING`, `LENGTH`, `TENSION`, `TIMESTEP`, `PERTURBATION`) on the `19×19` control grid, FFD lattice animation (`FFD PARAM 1`), world transform matrix (`POS`/`ANG`). | `spline.elf`: Tricubic B-spline FFD (`11×7×7` lattice) on all 361 control points → 4×4 matrix transform → Bicubic B-spline surface tessellation (`16×16` patches × `8×8` = `128×128` vertices) + analytical surface normals (`∂P/∂v × ∂P/∂u`). |
| **Particles (`PARTICLES.mnu`)** | Cone emission (`emit vel *`, `emit cone angle`, `emit neg prob`, `emit per frame`, `emit prob`, `aging *`) into dead slots returned by SPU free-list; updating wind flow-field & swirl axis. | `particles.elf`: Per-particle flow-field sampling, Brownian motion, swirl rotation, gravity/wind/friction Euler integration, quaternion spin, age/AABB culling, dead-index free-list collection, fade-in/out alpha, fp16 quaternion packing, and RSX draw-command generation (`alive_count * 4` verts). |

---

## 2. `spline.elf` — Ribbon FFD & Bicubic B-Spline Tessellator

### 2.1 Memory & Function Map

| Address | Symbol / Purpose |
|---|---|
| `0x03070` | `_start` (SPURS crt0 entry) |
| `0x03440` | `vec4_div(const vec4* a, vec4 b)` via `frest` + `fi` + `fm` |
| `0x03580` | `dma_get_8704(ls, ea)` — DMAs FFD lattice (`0x2200` bytes) to `0xb380` |
| `0x035c8` | `dma_get_64(ls, ea)` — DMAs `mat4` (`0x40` bytes) to `0xb300` |
| `0x03610` | `dma_get_5776(ls, ea)` — DMAs `19×19` control grid (`0x1690` bytes) to `0x9c00` |
| `0x03a10` | `vec4_clamp(const vec4* v, float lo, float hi)` |
| `0x03a60` | `vec4_floor(vec4 v)` |
| `0x03bf0` | `mat4_mul_vec4(const mat4* m, vec4 v)` (column-major: `col0*v.x + col1*v.y + col2*v.z + col3*v.w`) |
| `0x03c68` | **`ffd_eval(vec4 p)`** — Tricubic B-spline FFD deformation |
| `0x044d0` | `mat4_transpose(mat4* dst, const mat4* src)` |
| `0x045c0` | **`patch_tessellate(Vertex* out_tile)`** — Evaluates one `8×8` bicubic B-spline patch + normals |
| `0x053e0` | **`process_spline_job(ctx, job)`** — Full pipeline for one ribbon job |
| `0x05ec8` | `main` — SPURS event loop; dispatches `job_type ∈ {0, 1, 2}` to `sub_053e0` |
| `0x05fd8` | Static constructor (`.ctors` `0x6958`): builds `(1/6)*M_bspline` at `0xd5c0`, copies 129-entry `B(t)` to `0xd600..0xde00` and `B'(t)` to `0xde00..0xe600`, sets `0xd580 = 0xb380`. |

**Local Store Buffers (`.bss`):**
- `0x9c00` (`5776` bytes = `361 × 16`): `vec4 ctrl_grid[19][19]`
- `0xb300` (`64` bytes): `mat4 world_mtx`
- `0xb380` (`8704` bytes padded; `539 × 16 = 8624` used): `vec4 ffd_lattice[7][7][11]` (`[iz][iy][ix]`)
- `0xd580`: pointer to `0xb380`
- `0xd590`: `vec4 ffd_scale1` (from `LINE1.mnu` `FFD SCALE1 X/Y/Z`)
- `0xd5a0`: `vec4 ffd_scale2` (from `LINE1.mnu` `FFD SCALE2 X/Y/Z`)
- `0xd5b0`: `vec4 ffd_offset` (from `LINE1.mnu` `FFD OFFSET X/Y/Z`)
- `0xd5c0` (`64` bytes): Uniform cubic B-spline basis matrix `M_bspline`
- `0xd600` (`2064` bytes): `vec4 B_lut[129]` (precomputed `B(k/128)` for `k = 0..128`)
- `0xde00` (`2064` bytes): `vec4 dB_lut[129]` (precomputed `B'(k/128)` for `k = 0..128`)
- `0xe640`: `uint32_t patch_idx` (`0..255`)

### 2.2 Recovered SPURS Job Struct (`80` bytes, stride `0x50`)

```cpp
struct SplineJobDesc {
    uint32_t out_vb_ea;       // +0x00: Main-memory EA of 128x128 RibbonVertex output buffer (524,288 bytes)
    uint32_t ctrl_grid_ea;    // +0x04: Main-memory EA of 19x19 vec4 control points (5,776 bytes)
    uint32_t job_type;        // +0x08: 0, 1, or 2 (all dispatch to sub_053e0)
    uint32_t world_mtx_ea;    // +0x0C: Main-memory EA of 4x4 column-major transform matrix (64 bytes)
    uint32_t ffd_lattice_ea;  // +0x10: Main-memory EA of 11x7x7 vec4 FFD lattice (8,704 bytes padded)
    uint32_t pad[3];          // +0x14..0x1F
    float    ffd_scale1[4];   // +0x20: -> LS 0xd590 (LINE1.mnu: FFD SCALE1 X/Y/Z, 1.0)
    float    ffd_scale2[4];   // +0x30: -> LS 0xd5a0 (LINE1.mnu: FFD SCALE2 X/Y/Z, 1.0)
    float    ffd_offset[4];   // +0x40: -> LS 0xd5b0 (LINE1.mnu: FFD OFFSET X/Y/Z, 0.0)
};
```

### 2.3 Uniform Cubic B-Spline Basis Constants (`.rodata` `0x89c0..0x99f0`)

The basis matrix stored at `0xd5c0` is:
$$
M_{\text{bspline}} = \frac{1}{6}
\begin{bmatrix}
-1 &  3 & -3 & 1 \\
 3 & -6 &  3 & 0 \\
-3 &  0 &  3 & 0 \\
 1 &  4 &  1 & 0
\end{bmatrix}
$$
Applied to $[t^3, t^2, t, 1]^T$:
- $B_0(t) = \frac{1}{6}(1 - t)^3$
- $B_1(t) = \frac{1}{6}(3t^3 - 6t^2 + 4)$
- $B_2(t) = \frac{1}{6}(-3t^3 + 3t^2 + 3t + 1)$
- $B_3(t) = \frac{1}{6}t^3$

And its derivative $B'(t)$ (`0xde00` LUT):
- $B'_0(t) = -\frac{1}{2}(1 - t)^2$
- $B'_1(t) = \frac{1}{2}(3t^2 - 4t)$
- $B'_2(t) = \frac{1}{2}(-3t^2 + 2t + 1)$
- $B'_3(t) = \frac{1}{2}t^2$

*(Note: in `.rodata`, `0x8a10` has `[0, 1/6, 4/6, 1/6]` = reversed lane order `[B3, B2, B1, B0]` at $t=0$, and `0x9200` has `[1/6, 4/6, 1/6, 0]` at $t=1$; `0x05fd8` copies `0x8a10..0x9200` in reverse into `0xd600..0xde00`, restoring forward order `B(t)` at index `t*128`!)*

### 2.4 Exact Algorithms (`spline.elf`)

#### Pass 1: Tricubic B-Spline FFD (`sub_03c68`)
Executed on all `361` (`19×19`) control points in-place:
```cpp
// FFD lattice dimensions: 11 (X) x 7 (Y) x 7 (Z) = 539 vec4s
// Spans: (8, 4, 4) -> rodata 0x8980 = (8.0f, 4.0f, 4.0f, 1.0f)
vec4 ffd_eval(vec4 p, const vec4 lattice[7][7][11],
              vec4 ffd_offset, vec4 ffd_scale1, vec4 ffd_scale2) {
    vec4 u = (p - ffd_offset) / ffd_scale1;                  // 03c88, 03cbc (sub_03440)
    vec4 u_clamped = clamp(u, 0.0f, 0.999f);                 // 03cdc (0x3f7fbe76 = 0.999f)

    vec4 s = u_clamped * vec4(8.0f, 4.0f, 4.0f, 1.0f);       // 03cec
    int ix = (int)floorf(s.x), iy = (int)floorf(s.y), iz = (int)floorf(s.z);
    float fx = s.x - ix, fy = s.y - iy, fz = s.z - iz;

    vec4 wx = M_bspline * vec4(fx*fx*fx, fx*fx, fx, 1.0f);   // 03d54
    vec4 wy = M_bspline * vec4(fy*fy*fy, fy*fy, fy, 1.0f);   // 03d7c
    vec4 wz = M_bspline * vec4(fz*fz*fz, fz*fz, fz, 1.0f);   // 03da4

    vec4 disp = vec4(0.0f);
    for (int dz = 0; dz < 4; ++dz)
      for (int dy = 0; dy < 4; ++dy)
        for (int dx = 0; dx < 4; ++dx)
          disp += (wx[dx] * wy[dy] * wz[dz]) * lattice[iz + dz][iy + dy][ix + dx];

    // Displacement is added to UNCLAMPED u, then scaled by ffd_scale2
    return (u + disp) * ffd_scale2;                          // 04460..0446c
}
```

#### Pass 2: World Matrix Transform (`sub_03bf0`)
```cpp
for (int i = 0; i < 361; ++i)
    ctrl_grid[i] = world_mtx * ctrl_grid[i];
```

#### Pass 3: Bicubic B-Spline Surface & Normal Tessellation (`sub_045c0`)
Iterates `patch_idx = 0..255` (`16 × 16` patches):
- `row = patch_idx >> 4` (`0..15`), `col = patch_idx & 15` (`0..15`)
- Control point sub-grid: `P[r][c] = ctrl_grid[(row + r) * 19 + (col + c)]` for `r, c ∈ 0..3`.
- Evaluates an `8 × 8` tile of vertices at `u = i / 8.0f` (`i = 0..7`) and `v = j / 8.0f` (`j = 0..7`):
```cpp
struct RibbonVertex {
    float pos[4];     // +0x00: P(u, v) (x, y, z, w)
    float normal[4];  // +0x10: (dP/dv) x (dP/du), w = 0.0f (unnormalized)
}; // 32 bytes

void eval_patch(int patch_idx, const vec4 ctrl_grid[19][19], RibbonVertex out_tile[8][8]) {
    int row = patch_idx >> 4;
    int col = patch_idx & 15;
    for (int i = 0; i < 8; ++i) {
        vec4 Bu  = B_lut[i * 16];    // t = i / 8.0f
        vec4 dBu = dB_lut[i * 16];
        // Contract along columns (c = 0..3) for each of the 4 rows (r = 0..3)
        vec4 curve[4], dcurve_du[4];
        for (int r = 0; r < 4; ++r) {
            curve[r]     = Bu[0]*P[r][0]  + Bu[1]*P[r][1]  + Bu[2]*P[r][2]  + Bu[3]*P[r][3];
            dcurve_du[r] = dBu[0]*P[r][0] + dBu[1]*P[r][1] + dBu[2]*P[r][2] + dBu[3]*P[r][3];
        }
        // Evaluate 8 samples along v (j = 0..7, 4 at a time on SPU)
        for (int j = 0; j < 8; ++j) {
            vec4 Bv  = B_lut[j * 16];
            vec4 dBv = dB_lut[j * 16];
            vec4 pos = Bv[0]*curve[0]     + Bv[1]*curve[1]     + Bv[2]*curve[2]     + Bv[3]*curve[3];
            vec3 du  = Bv[0]*dcurve_du[0] + Bv[1]*dcurve_du[1] + Bv[2]*dcurve_du[2] + Bv[3]*dcurve_du[3];
            vec3 dv  = dBv[0]*curve[0]    + dBv[1]*curve[1]    + dBv[2]*curve[2]    + dBv[3]*curve[3];
            vec3 n   = cross(dv, du); // 04b30..04b68: (dv.y*du.z - dv.z*du.y, dv.z*du.x - dv.x*du.z, dv.x*du.y - dv.y*du.x)
            out_tile[j][i] = { pos, vec4(n, 0.0f) };
        }
    }
}
```

#### Output Vertex Format vs. `lines1.vpo`
- Each `8 × 8` tile is `2048` bytes (`8` rows × `256` bytes/row) DMA'd in 2D-tiled fashion into a **`128 × 128` vertex grid** (`16,384` vertices = `524,288` bytes total).
- Each vertex is **32 bytes**: `POSITION0` (`vec4 pos` at `+0x00`) + `TEXCOORD0` (`vec4 normal` at `+0x10`, unnormalized — normalized in `lines1.vpo`). `TEXCOORD1` (ribbon UV `(u, v)`) is supplied from a static vertex stream since the grid topology is fixed at `128 × 128`.

---

## 3. `particles.elf` — Particle Integrator, Culler & Streamer

### 3.1 Memory & Function Map

| Address | Symbol / Purpose |
|---|---|
| `0x03070` | `_start` (SPURS crt0 entry) |
| `0x030e8` | **`rng_next_neg1_to_1(uint32_t* seed)`** — Park-Miller LCG (`* 16807`) → `[-1.0, +1.0)` |
| `0x033d8` | `rng_vec3(s0, s1, s2)` — Calls `sub_030e8` 3 times → `vec4(rx, ry, rz, 0)` |
| `0x03610` | `freelist_push(FreeList* fl, uint32_t particle_idx)` |
| `0x03648` | `out_stream_init(0xbb80, out_ea, count, tag_base)` — Double-buffered 32B particle output |
| `0x04060` | `unpack_rgb8_s8(uint32_t packed)` — Sign-extends 3 bytes (`s8`) × `1.0f / 127.0f` (`0x3c010204`) |
| `0x04100` | `fetch_flow_texel(FlowField* ff, ivec2 xy)` — Loads 3-byte RGB8 texel at `(y*width + x)*3` |
| `0x043e8` | `mark_particle_dead(Particle* p)` — Sets `p->pos_age.w = -666.0f` (`0xc4268000`) |
| `0x044d0` / `0x045b0` | Fast polynomial `sinf` / `cosf` |
| `0x04688` | `mat4_from_axis_angle(float angle, vec3 axis)` |
| `0x048b8` | `update_swirl_matrix(SwirlState* s)` — Builds rotation matrix `R` from axis-angle vector (identity if `|ω| < 1e-5`) |
| `0x04978` | `init_frame_state(FrameState* st, SimParams* p)` — Computes `1.0f / dt` and seeds RNG (`0x98756161`, `0x21324889`, `0x82181158`) |
| `0x05dd8` | `stream_process_512(out_ea, in_ea, cb=0x6ed0, chunks=8)` — Double-buffered DMA of 512 particles (`8 × 64`, `24,576` bytes) |
| `0x06040` / `0x061c0` | Double-buffered output flush (`64` particles × `32` bytes = `2048` bytes per DMA) |
| `0x06650` | `is_particle_dead(const Particle* p)` — Returns `p->pos_age.w == -666.0f` |
| `0x06688` | `kill_particle(uint32_t idx, Particle* p)` — If alive, pushes `idx` to free-list (`0xbb40`) and marks dead |
| `0x06770` | `is_age_expired(const Particle* p)` — Returns `p->pos_age.w >= 0.99999f` (`0x3f7fff58`) |
| `0x068e0` / `0x06b48` | **`sample_flow_field(FlowField* ff, vec4 uv)`** — Bilinear RGB8 (`s8/127`) 2D vector field lookup |
| `0x06b88` | `quat_mul(const quat* a, quat b)` |
| `0x06d60` | `quat_normalize(const quat* q)` |
| `0x06da0` | `aabb_contains(const AABB* box, vec4 pos)` — Returns `pos.xyz == clamp(pos.xyz, box.min, box.max)` |
| `0x06e08` / `0x06e98` | **`pack_fp16x8(dst, vec4 a, vec4 b)`** — IEEE-754 fp32 → fp16 packer |
| `0x06ed0` | **`update_particle_batch_64(Particle* batch)`** — Core 64-particle physics & output kernel |
| `0x080e8` | `main` — SPURS loop; processes batches in 512-particle chunks (`0x6000` bytes each), writes RSX draw command (`alive_count * 4` verts) |

### 3.2 Recovered Structs

#### Particle State (`48` bytes, `3 × vec4`, chunk stride `512 × 48 = 24,576 = 0x6000`)
```cpp
struct Particle {
    float pos_age[4];     // +0x00: xyz = world position, w = age in [0, 1) (or -666.0f if DEAD)
    float vel_aging[4];   // +0x10: xyz = velocity,       w = aging speed (integrated via pos_age += vel_aging * dt!)
    float rot_quat[4];    // +0x20: xyzw = unit orientation quaternion
}; // 48 bytes
```

#### Simulation Parameters (`2304` bytes = `0x900`, DMA'd to LS `0xb200`)
```cpp
struct ParticleSimParams {
    float    gravity[4];           // +0x000: xyz = gravity vector (w masked to 0) [PARTICLES.mnu: gravity]
    float    friction[4];          // +0x010: broadcast friction coeff             [PARTICLES.mnu: friction]
    uint8_t  pad0[0x60];           // +0x020..0x07F
    uint8_t  flow_texels[1664];    // +0x080..0x6FF: 2D RGB8 (signed int8) flow-field texture
    float    M_from_field[16];     // +0x700..0x73F: mat4 transforming sampled flow vector -> world space
    float    M_to_field[16];       // +0x740..0x77F: mat4 transforming world pos -> flow-field UV space
    struct {                       // +0x780..0x7FF: FlowField descriptor (offset +1792 from +0x080)
        uint32_t data_ptr;         //   +0x00: patched in-place on SPU to LS 0xb280
        uint32_t width;            //   +0x04: texture width in texels
        uint32_t pad[2];
        float    size_f[4];        //   +0x10: (float)width, (float)height, ...
        int32_t  min_coord[4];     //   +0x20: clamp min (0, 0) & step (1, 0)
        int32_t  step_coord[4];    //   +0x30: step (0, 1) & (1, 1)
        int32_t  max_coord[4];     //   +0x40: clamp max (width-1, height-1)
    } flow_meta;
    float    aabb_min[4];          // +0x800: culling AABB min (x, y, z, _)
    float    aabb_max[4];          // +0x810: culling AABB max (x, y, z, _)
    uint8_t  pad1[0x10];           // +0x820..0x82F
    float    swirl_center[4];      // +0x830: center of vortex/swirl rotation (xyz, 1.0)
    float    swirl_rot_mtx[16];    // +0x840: 4x4 rotation matrix computed on SPU by sub_048b8
    float    swirl_axis_angle[4];  // +0x880: angular step vector (axis * angle)
    float    wind_vec[4];          // +0x890: constant wind vector                 [PARTICLES.mnu: wind dir * wind scale]
    float    wind_field_scale;     // +0x8A0: multiplier on flow-field force       [PARTICLES.mnu: wind scale 10]
    float    brownian_scale;       // +0x8A4: multiplier on Brownian noise         [PARTICLES.mnu: brownian scale]
    float    pad2;                 // +0x8A8
    float    spin_time_scale;      // +0x8AC: multiplier on quaternion spin        [PARTICLES.mnu: spin time scale]
    float    delta_time[4];        // +0x8B0: broadcast dt                         [PARTICLES.mnu: delta time]
    uint8_t  pad3[0x40];           // +0x8C0..0x8FF
}; // 2304 bytes (0x900)
```

#### Output Compressed Particle Record (`32` bytes, streamed via `0xbb80`)
```cpp
struct ParticleGpuRecord {
    float    pos_alpha[4];   // +0x00: xyz = world pos, w = fade alpha in [0, 1]
    uint16_t rot_fp16[8];    // +0x10: fp16(rot.x, rot.y, rot.z, rot.w, 0, 0, 0, 0)
}; // 32 bytes
```

### 3.3 Exact Algorithms (`particles.elf`)

#### RNG (`sub_030e8` & `sub_033d8`)
Initialized per batch (and note `sub_04978` is also invoked inside the per-particle output path at `071d4`, though the main RNG state lives in `0xbb20..0xbb28` seeded from `0x98756161`, `0x21324889`, `0x82181158`):
```cpp
float rng_neg1_to_1(uint32_t& seed) {
    seed = seed * 16807u; // mod 2^32 (Park-Miller multiplier)
    uint32_t bits = 0x40000000u | (seed >> 9); // float in [2.0, 4.0)
    return std::bit_cast<float>(bits) - 3.0f;  // uniform in [-1.0, +1.0)
}
```

#### Per-Particle Update (`0x06ed0`, 64 particles per batch)
```cpp
constexpr float DEAD_SENTINEL = -666.0f;   // 0xc4268000
constexpr float TWO_PI        = 6.2831853f; // 0x40c90fda

void update_particle(uint32_t global_idx, Particle& p, const ParticleSimParams& prm,
                     float inv_dt, uint32_t rng[3], FreeList& dead_list,
                     OutputStream& gpu_out) {
    if (p.pos_age[3] == DEAD_SENTINEL) return;

    // 1. Sample 2D RGB8 (signed int8 / 127) flow field
    vec4 uv_pos  = prm.M_to_field * vec4(p.pos_age.xyz, 1.0f);
    vec4 f_local = sample_flow_field_bilinear(prm.flow_texels, prm.flow_meta, uv_pos.xy);
    vec4 f_world = prm.M_from_field * vec4(f_local.xyz, 0.0f);

    // 2. Total external acceleration: Brownian + constant wind + flow field
    vec3 brown   = vec3(rng_neg1_to_1(rng[0]), rng_neg1_to_1(rng[1]), rng_neg1_to_1(rng[2]));
    vec4 a_ext   = vec4(prm.brownian_scale * brown + prm.wind_vec.xyz + prm.wind_field_scale * f_world.xyz, 0.0f);

    // 3. Swirl / vortex velocity kick around swirl_center
    vec4 center  = vec4(prm.swirl_center.xyz, 1.0f);
    vec4 pos1    = vec4(p.pos_age.xyz, 1.0f);
    vec4 p_rot   = center + prm.swirl_rot_mtx * (pos1 - center);
    p.vel_aging += (p_rot - pos1) * inv_dt;

    // 4. Euler velocity & position+age integration
    //    acc.xyz = gravity + a_ext - friction * vel;  acc.w = 0 - friction * aging_rate
    vec4 acc     = (vec4(prm.gravity.xyz, 0.0f) + a_ext) - prm.friction[0] * p.vel_aging;
    p.vel_aging += acc * prm.delta_time[0];
    p.pos_age   += p.vel_aging * prm.delta_time[0]; // advances BOTH pos.xyz AND age (pos_age.w)!

    // 5. Quaternion spin integration (quasi-periodic frequencies 0.37, 0.17, 0.31)
    float age   = p.pos_age[3];
    float theta = TWO_PI * age;
    vec4  ang_v = vec4(cosf(0.37f * theta), sinf(0.17f * theta), sinf(0.31f * theta), 0.0f);
    float scale = 0.5f * (prm.spin_time_scale * (1.0f / 60.0f)); // 0x3c888888 = 1/60
    vec4  dq    = quat_mul(ang_v, p.rot_quat) * scale;
    p.rot_quat  = normalize(p.rot_quat + dq);

    // 6. Death & AABB culling
    if (age >= 0.99999f || !aabb_contains(prm.aabb_min, prm.aabb_max, p.pos_age.xyz)) {
        dead_list.push(global_idx);
        p.pos_age[3] = DEAD_SENTINEL;
        return;
    }

    // 7. Smooth linear fade-in [0, 0.02] and fade-out [0.94, 1.0]
    float fade_in  = std::min(age, 0.02f) * 50.0f;
    float fade_out = 1.0f + std::max(age - 0.94f, 0.0f) * (-16.666664f); // 1 - max(age-0.94, 0)/0.06
    float alpha    = fade_in * fade_out;

    // 8. Emit 32-byte compressed GPU record
    ParticleGpuRecord rec;
    rec.pos_alpha = vec4(p.pos_age.xyz, alpha);
    pack_fp16x8(rec.rot_fp16, p.rot_quat, vec4(0.0f));
    gpu_out.push(rec);
}
```

#### Output Format vs. `particles_quads.vpo`
- `particles.elf` writes `alive_count` compact **32-byte** records (`pos_alpha` + `rot_fp16`) to the current frame's GPU buffer, writes `alive_count` to main memory (`+0x24` EA), and generates a 1024-byte RSX draw command buffer (`+0x2C` EA) drawing `alive_count * 4` vertices (`08464: shli $6, $4, 2`).
- In `particles_quads.vpo` (`pos`, `uv0`, `old_pos`, `rot`):
  - `pos` (`vec4`: xyz + fade alpha) and `rot` (`fp16×4` quaternion) are read from the current frame's 32-byte `ParticleGpuRecord` stream (frequency-divided / instanced 1 per 4 quad vertices).
  - `old_pos` comes from the previous frame's double-buffered `ParticleGpuRecord` stream (used for motion-blur stretching / velocity alignment).
  - `uv0` comes from a static repeating 4-corner quad UV stream (`(0,0), (1,0), (1,1), (0,1)`).

---

## 4. C++20 / AVX-512 Porting Notes

1. **`spline.elf` → AVX-512**:
   - The `19×19` FFD + matrix pass is tiny (361 points) and runs in < 5 µs even scalar or AVX2.
   - The `16×16` bicubic B-spline tessellation (`128×128 = 16,384` vertices) maps directly onto **AVX-512** (16 `float`s = 2 full rows of an `8×8` patch, or 8 `j` samples × 2 patches simultaneously in SoA). Precomputing the 8-entry `B(k/8)` and `B'(k/8)` tables eliminates the 129-entry LUT overhead.
   - Note that the PPU-side **19×19 spring/cloth simulation** (`DAMPING`, `LENGTH`, `TENSION`, `TIMESTEP`, `PERTURBATION`) and **FFD lattice animation** (`FFD PARAM 1`) live outside `spline.elf` and should be implemented in C++20 feeding `ctrl_grid[19][19]` into the exact FFD + B-spline pipeline above.
2. **`particles.elf` → AVX-512**:
   - Converting `Particle` to **SoA** (`pos_x/y/z`, `age`, `vel_x/y/z`, `aging`, `rot_x/y/z/w`) lets AVX-512 integrate **16 particles per iteration** with branchless mask-blending for dead-particle skipping (`_mm512_cmp_ps_mask` on `age == -666.0f`), AABB culling, and fade alpha.
   - Use `_mm512_cvtps_ph` (`F16C` / `AVX512F`) for the exact fp32→fp16 quaternion packing, or pass fp32 quaternions directly to modern Vulkan/GL shaders.
   - Because `particles.elf` expects the PPU to spawn new particles into the returned dead-index free-list, our C++20 particle manager can fuse cone emission (`PARTICLES.mnu` `emit *` parameters) directly into the same pass whenever `age == -666.0f`.
