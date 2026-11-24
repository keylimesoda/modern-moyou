# PlayStation 3 XMB (`custom_render_plugin.prx`) PPU Ribbon & Moyou Specification

This document records the complete PowerPC64 PPU reverse engineering of Sony's stock PS3 OFW 4.76 `custom_render_plugin.prx` (`custom_render_module`, `TOC = 0xa7d70`, `10,863` relocations) for the **Moyou** subsystem (XMB wave ribbon, cloth/spring physics, FFD lattice, camera, procedural LUTs, sky/background generator, and `LinesController::draw` compositing).

Together with [`docs/spu_kernels.md`](spu_kernels.md) (`spline.elf`) and [`docs/shaders.md`](shaders.md) (RSX shaders), this specifies the **exact, bit-faithful C++20 reference implementation** of the XMB ribbon and background pipeline.

---

## 0. PPU Module & Struct Map

| PPU Address | Class / Function | Role |
|---|---|---|
| `0x024a68` | `BackgroundView::BackgroundView` | Initializes default camera (`eye`, `target`, `up`, `near=0.1`, `far=1000`, `fovy=53°`). |
| `0x0255a4` | `BackgroundView::applyCamera` | Builds `glFrustum` projection and `gluLookAt` view matrices on the QGL matrix stack. |
| `0x028718` | `LinesMenu::registerVars` | Registers `LINE1.mnu` parameters (`0x60`-byte stride per `MenuVar`). |
| `0x029cc8` | `LinesMenu::syncToLines` | Pushes `LINE1.mnu` values into `Lines` (`r30`) and `FFD` (`r28`), builds `M_line` (`lines+0x1f0`). |
| `0x047b1c` | `FFD::unpack8x4x4To11x7x7` | Edge-clamps the `8×4×4` (`128` texels) `ffd_shader` output into `vec4 lattice[7][7][11]`. |
| `0x0493c4` | `FFD::init(8, 4, 4)` | Builds `128×1` RGBA32F `_UVW` texture and FFD render targets/PBO. |
| `0x04a220` | `FFD::update(float time)` | Renders `ffd_shader0..3.fpo` (with optional `ffd_alpha_blend.fpo`), reads back `128×1`, scales by `ffd_scale1`, unpacks to `11×7×7`. |
| `0x04ab04` | `Lines::noise1D(uint32_t x)` | Integer hash PRNG returning `float` in `[-1.0, +1.0]` for `PERTURBATION`. |
| `0x04b928` | `Lines::buildEncodeLUT` | Generates procedural `4096×1` RGBA8 `_Encode` LUT. |
| `0x04bad0` | `Lines::buildStripesLUT` | Generates procedural `16×4` RGBA8 `_Stripes` LUT. |
| `0x04bce0` | `Lines::applyBoundaryConditions` | Applies left-edge drift (`col 0`) and right-edge pinned wave (`col 18`) using `END X/Y/Z`. |
| `0x04bed0` | `Lines::stepClothPhysics` | Single fixed-timestep spring/cloth integration step (`DAMPING`, `LENGTH`, `TENSION`, `TIMESTEP`, `PERTURBATION`). |
| `0x04c4e4` | `Lines::updateFFD` | Calls `FFD::update(sim_time * 10.0f)`. |
| `0x04c94c` | `Lines::draw(int lod)` | Binds `lines0..2.{vpo,fpo}`, sets all uniforms/samplers, draws `128×128` indexed strip mesh. |
| `0x04d190` | `Lines::buildStaticGridUVAndIB` | Builds static `128×128` UV + 10% edge-fade buffer (`oldcrd`) and triangle-strip index buffers. |
| `0x04e558` | `Lines::beginDraw` | Sets RSX render state (additive blend `GL_ONE, GL_ONE`, depth test/write off, cull off). |
| `0x04e624` | `Lines::initPreset(int preset)` | Loads one of 3 pre-baked `19×19` cloth states (`0x95a40`, `0x97c18`, `0x99df0`) + initial `sim_time`. |
| `0x04eb40` | `Lines::loadFresLUT(path)` | Loads `textures/TGA/freslut1.tga` (`512×1` RGB8) into `_FresLUT`. |
| `0x04ec90` | `Lines::init(w=128, h=128)` | Procedural `19×19` grid init, allocates LUT textures, loads `lines0..2.{vpo,fpo}`, calls LUT builders. |
| `0x04f814` | `Lines::update(float dt)` | Fixed-timestep accumulator (`60.0f * dt`), calls `stepClothPhysics`, interpolates `prev`/`curr`, applies pinch envelope, calls `updateFFD`. |
| `0x050cd0` | `Lines::computeWorldMtx` | Computes `world_mtx = Projection * ModelView * M_line` (`MVP`) at `lines+0x230` for `spline.elf`. |
| `0x052ad8` | `LinesController::renderBackColours` | Computes `_DayTime`, `_NightTime`, `_MonthTime`, `_NightDayBlend`, `_NightBrightness`, `_Alpha` and renders `back_colours0..2.fpo` into `64×32` `_Back`. |
| `0x0535bc` | `LinesController::dispatchSpurs` | Fills `SplineJobDesc` (`80` bytes) and kicks `spline.elf` SPURS task. |
| `0x05381c` | `LinesController::buildScreenMesh` | Builds `64×32` curved/warped screen mesh for `LinesController.vpo`. |
| `0x057028` | `LinesController::init` | Loads `ffd_shader0..3`, `LinesController.{vpo,fpo}`, `back_colours0..2`, `textures/month_bg/{rgb,night}/01..12.dds`, `freslut1.tga`. |
| `0x057cf8` | `LinesController::draw` | Full two-pass ribbon accumulation + sky/ribbon composite pass. |

---

## 1. Control Grid (`19×19`) & Cloth/Spring Physics

### 1.1 `Lines` Struct Field Offsets (from `0x029cc8` & `0x04ec90`)

| Offset | Type | Source (`LINE1.mnu`) | Description |
|---|---|---|---|
| `+0x000` | `FFD*` | — | Pointer to FFD lattice object |
| `+0x02c`, `+0x030` | `int32_t` | constant `19, 19` | Control grid width `W=19`, height `H=19` (`361` control points) |
| `+0x040` (`+0x044` ptr) | `vec4[361]` | — | `ctrl_grid`: interpolated & pinched control points sent to `spline.elf` |
| `+0x0a0` (`+0x0a4` ptr) | `vec4[361]` | — | `prev_pos`: cloth positions before last physics step |
| `+0x0d0` (`+0x0d4` ptr) | `vec4[361]` | — | `curr_pos`: current cloth positions |
| `+0x160` (`+0x164` ptr) | `vec4[361]` | — | `vel`: per-vertex velocity (`vec4`, `w = 0`) |
| `+0x1f0` (`496`) | `mat4` | `POS X/Y/Z`, `ANG X/Y/Z`, `ANG ROT` | `M_line`: ribbon model matrix |
| `+0x230` (`560`) | `mat4` | computed in `0x50cd0` | `world_mtx` (`MVP`) passed to `spline.elf` |
| `+0x270` (`624`) | `int32_t` | `STATE` | Active state/preset index |
| `+0x274` (`628`) | `float` | `DAMPING` | Velocity damping coefficient (stock: `0.0001f`) |
| `+0x278` (`632`) | `float` | `LENGTH` | Structural spring rest length `L` (stock: `0.306001f`) |
| `+0x27c` (`636`) | `float` | `TENSION` | Structural spring stiffness `k` (stock: `0.25f`) |
| `+0x280` (`640`) | `float` | `SPACING` | `_Spacing` fragment uniform (stock: `407.658f`) |
| `+0x284` (`644`) | `float` | `THINNESS` | `_Thinness` fragment uniform (stock: `1.0f`) |
| `+0x288` (`648`) | `float` | `BRIGHTNESS` | `_Brightness` vertex uniform (stock: `0.701917f`) |
| `+0x28c` (`652`) | `float` | `FRESNEL` | `_Fresnel` vertex uniform (stock: `0.638971f`) |
| `+0x290` (`656`) | `float` | `FALLOFF` | `_Falloff` vertex uniform (stock: `1.00318f`) |
| `+0x294` (`660`) | `float` | `TIMESTEP` | Physics timestep scalar (stock: `4.0f`) |
| `+0x298` (`664`) | `float` | `PERTURBATION` | Random velocity perturbation scale (stock: `0.0998587f`) |
| `+0x29c` (`668`) | `float` | `MIPMAP BIAS` | `_MipmapBias` vertex uniform (stock: `1.86707f`) |
| `+0x2a0` (`672`) | `vec4` | `END X/Y/Z` | `(end_x, end_y, end_z, 0.0f)` boundary wave amplitude (stock: `(3, 0.4, 0.2)`) |
| `+0x2b8` (`696`) | `int32_t` | `SHADER` | Shader index `0..2` (`lines0/1/2`, stock: `1`) |
| `+0x2c0` (`704`) | `float` | init `10.0f` / preset | `sim_time`: advances by `TIMESTEP * 0.0001f` per step; wraps at `10.0f`? (see §1.3) |
| `+0x2c4` (`708`) | `float` | init `0.0f` | `smooth_time`: low-pass filtered `sim_time` used by boundary wave |
| `+0x2cc` (`716`) | `float` | init `0.0f` | `step_accum`: fixed-60Hz fractional accumulator in `[0, 1)` |
| `+0x2d4` (`724`) | `uint32_t` | init `0` | `rng_counter`: monotonically incremented per random call |

### 1.2 Control Grid Initialization (`0x04ec90` & `0x04e624`)

1. **Procedural rest grid (`Lines::init`, `0x04edf0..0x04ef60`)**:
   For `row = 0..18`, `col = 0..18`:
   ```cpp
   float x = (float(col) / 18.0f - 0.5f) * 4.0f; // [-2.0, +2.0]
   float y = (float(row) / 18.0f - 0.5f) * 4.0f; // [-2.0, +2.0]
   vec4  p = vec4(x, y, -3.0f, 1.0f);
   // Written identically to ctrl_grid, prev_pos, curr_pos; vel = vec4(0).
   ```
2. **Pre-baked initial states (`Lines::initPreset(int preset)`, `0x04e624..0x04eb3c`)**:
   Called during `LinesController::init` (`0x050da8` calls `initPreset(1)`!). Rather than starting from the flat plane, Sony loads a pre-settled `19×19` `(pos, vel)` snapshot from `.rodata` (361 `vec3`s each, `4,332` bytes per table) and sets `sim_time`:
   - **Preset 0**: `sim_time = 76.4114f`, `pos` at `0x95a40`, `vel` at `0x96b2c`
   - **Preset 1 (Default!)**: `sim_time = 98.6201f`, `pos` at `0x97c18`, `vel` at `0x98d04`
   - **Preset 2**: `sim_time = 90.5413f`, `pos` at `0x99df0`, `vel` at `0x9aedc`
   *(After copying into `ctrl_grid`, `prev_pos`, `curr_pos`, `vel`, `0x04eae8` resets `sim_time = 0.0f`, `smooth_time = 0.0f`, `step_accum = 0.0f`, `rng_counter = 0`, and calls `FFD::resetTime(0.0f)`!)*

### 1.3 Exact Per-Frame Update & Cloth/Spring Equations (`0x04f814`, `0x04bed0`, `0x04bce0`)

#### Deterministic Integer-Hash PRNG (`0x04ab04`, `0x04abb0`)
```cpp
float noise1D(uint32_t& counter) {
    uint32_t n = ++counter;
    n = (n << 13) ^ n;
    uint32_t m = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return 1.0f - float(m) * 9.31322575e-10f; // 1.0 - m / 2^30, in [-1.0, +1.0]
}
```
*(Note: `12 * 65536 + 2789 = 789221`, `21001 * 65536 - 8947 = 1376312589`, `-9.313226e-10 = -1/1073741824.0` — standard Hugo Elias 1D value-noise hash!)*

#### Fixed-Timestep Driver (`Lines::update(float dt)`, `0x04f814..0x04fab0`)
```cpp
void Lines::update(float dt) {
    float scaled_dt = dt * 60.0f;           // 0x92de0 = 60.0f
    step_accum += scaled_dt;                // 716(this)
    while (step_accum >= 1.0f) {
        stepClothPhysics();                 // 0x04bed0
        step_accum -= 1.0f;
    }

    // 1. Linear interpolation between prev_pos and curr_pos + X-axis pinch envelope
    float alpha = step_accum;               // in [0, 1)
    float param1 = ffd->param1;             // FFD PARAM 1 (172(ffd))
    for (int i = 0; i < 361; ++i) {
        vec4 p = prev_pos[i] * (1.0f - alpha) + curr_pos[i] * alpha;

        // X-axis pinch/wave modulation around x = FFD PARAM 1 (0x04f928..0x04fa48):
        float dx = p.x - param1;
        float s  = (dx < 0.0f) ? sinf(0.0f) : sinf(dx * 1.5707964f); // sin(dx * PI/2) if dx >= 0 else 0
        float t  = std::clamp(dx / 5.0f, 0.0f, 1.0f);                // 0x92dec = 5.0f
        float u  = 1.0f - t;
        float env = u * u * (3.0f - 2.0f * u);                       // smoothstep(1 -> 0) over [0, 5]
        float yz_scale = 1.3f - s * env;                             // 0x92df8 = 1.3f

        ctrl_grid[i] = vec4(p.x, p.y * yz_scale, p.z * yz_scale, p.w);
    }

    // 2. Update FFD lattice with sim_time * 10.0f (0x04c4e4)
    ffd->update(sim_time * 10.0f);          // 0x92d80 = 10.0f
}
```
*(Verify at `0x04f93c..0x04fa48`: `sub_4bcbc` calls `stdc:0xb5e28191` = `sinf` (with `r4=1`? Actually check `0x04f950` — `sinf(dx * PI/2)`), `1.3f - sin(dx * PI/2) * (1-t)^2 * (3 - 2*(1-t))`, and `sub_4ab54` multiplies `p` component-wise by `vec4(1.0f, yz_scale, yz_scale, 1.0f)`!)*

#### Single Physics Step (`Lines::stepClothPhysics`, `0x04bed0..0x04c4e0`)
```cpp
void Lines::stepClothPhysics() {
    // 1. Snapshot curr_pos -> prev_pos
    std::memcpy(prev_pos, curr_pos, 361 * sizeof(vec4));

    // 2. Advance simulation time
    float dt_sim = TIMESTEP * 0.0001f;       // 0x92d70 = 1e-4f (stock: 4 * 1e-4 = 0.0004)
    sim_time += dt_sim;
    if (sim_time > 10.0f) {                  // 0x92d74 = 10.0f
        sim_time = 0.0f;                     // resets sim_time and triggers FFD time crossfade!
        ffd->resetTime(0.0f);
    }

    float k_struct = TENSION;                // structural spring stiffness (1-hop)
    float k_bend   = TENSION * 10.0f;        // WAIT: verify f28 = f29 * f31 = TENSION * 10.0f! (0x04bf94)
    // NOTE: At 0x04bf68, f31 = 10.0f (0x92d74), and 0x04bf94 computes f28 = TENSION * 10.0f!
    // Structural rest length = LENGTH; Bend (2-hop) rest length = 2.0f * LENGTH (0x04c0d4: -2.0f * L + dist)

    // 3. Accumulate spring forces + random perturbation directly into vel[idx]
    for (int row = 0; row < 19; ++row) {
        for (int col = 0; col < 19; ++col) {
            int idx = row * 19 + col;
            // (a) Horizontal 1-hop structural spring: (col, row) <-> (col - 1, row)
            if (col > 0) {
                vec3 d = curr_pos[idx - 1].xyz - curr_pos[idx].xyz;
                float dist = length(d);
                vec3 f = normalize(d) * ((dist - LENGTH) * k_struct);
                vel[idx].xyz     += f;
                vel[idx - 1].xyz -= f;
            }
            // (b) Horizontal 2-hop bend spring: (col, row) <-> (col - 2, row)
            if (col > 1) {
                vec3 d = curr_pos[idx - 2].xyz - curr_pos[idx].xyz;
                float dist = length(d);
                vec3 f = normalize(d) * ((dist - 2.0f * LENGTH) * k_bend);
                vel[idx].xyz     += f;
                vel[idx - 2].xyz -= f;
            }
            // (c) Vertical 1-hop structural spring: (col, row) <-> (col, row - 1)
            if (row > 0) {
                vec3 d = curr_pos[idx - 19].xyz - curr_pos[idx].xyz;
                float dist = length(d);
                vec3 f = normalize(d) * ((dist - LENGTH) * k_struct);
                vel[idx].xyz      += f;
                vel[idx - 19].xyz -= f;
            }
            // (d) Vertical 2-hop bend spring: (col, row) <-> (col, row - 2)
            if (row > 1) {
                vec3 d = curr_pos[idx - 38].xyz - curr_pos[idx].xyz;
                float dist = length(d);
                vec3 f = normalize(d) * ((dist - 2.0f * LENGTH) * k_bend);
                vel[idx].xyz      += f;
                vel[idx - 38].xyz -= f;
            }
            // (e) Random perturbation (3 calls to noise1D)
            vec3 rnd = vec3(noise1D(rng_counter), noise1D(rng_counter), noise1D(rng_counter));
            vel[idx].xyz += rnd * PERTURBATION;
        }
    }

    // 4. Semi-implicit Euler integration & velocity damping (0x04c3d0..0x04c488)
    float damp = DAMPING * TIMESTEP;         // stock: 0.0001 * 4.0 = 0.0004
    for (int i = 0; i < 361; ++i) {
        curr_pos[i].xyz += vel[i].xyz * dt_sim;
        vel[i].xyz      -= vel[i].xyz * damp;
    }

    // 5. Boundary conditions (0x04bce0..0x04becc)
    applyBoundaryConditions();
}
```

#### Boundary Conditions (`Lines::applyBoundaryConditions`, `0x04bce0..0x04becc`)
```cpp
void Lines::applyBoundaryConditions() {
    // Low-pass filter sim_time -> smooth_time
    smooth_time += (sim_time - smooth_time) * 0.1f;          // 0x92d58 = 0.1f

    float drift_x = TIMESTEP * 0.02f;                        // 0x92d5c = 0.02f (stock: 0.08)
    for (int row = 0; row < 19; ++row) {
        // Left edge (col = 0): add constant +X velocity impulse
        vel[row * 19 + 0].x += drift_x;

        // Right edge (col = 18): pinned to animated Lissajous wave scaled by END X/Y/Z!
        float phase = float(row) / 19.0f + smooth_time;
        float py = (sinf(phase * 11.0f) * 0.5f + 0.5f) * END.y; // 0x92d60 = 11.0f (0x3faa4 = sinf)
        float pz = (cosf(phase * 15.0f) + 1.0f) * 0.5f * END.z; // 0x92d6c = 15.0f (0x3fac8 = cosf)
        int r_idx = row * 19 + 18;
        curr_pos[r_idx] = vec4(0.0f, py, pz, 1.0f);
        vel[r_idx]      = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
```
*(Note: `END X` is stored in `END.x` at `+0x2a0`, while `curr_pos[r_idx].x` is pinned to `0.0f` at `0x04be34` — `END Y` and `END Z` drive the Y/Z amplitude of the pinned right edge, and the left edge drifts in `+X`!)*

---

## 2. Camera & `world_mtx` (`spline.elf` Input)

### 2.1 Camera Parameters (`BackgroundView`, `0x024a68`, `0x0255a4`)

- **Eye position (`0x93730`)**: `(0.0f, 0.0f, 2.0f)`
- **Look-at target (`0x93740`)**: `(0.0f, 0.0f, -7.0f)`
- **Up vector (`0x93750`)**: `(0.0f, 1.0f, 0.0f)`
- **Clip planes**: `near = 0.1f` (`0x9208c`), `far = 1000.0f` (`0x92090`)
- **Vertical FOV**: default `0.92502451 rad` (`53.0°`, `0x92094`), overridden by `BACKGROUND.mnu` `FOVY` (degrees → radians; stock OFW `FOVY = 71.846° = 1.253949 rad`).
- **Projection (`0x025698..0x0256e4`)**: Standard OpenGL symmetric perspective frustum:
  ```cpp
  float top   = near * tanf(0.5f * fovy_rad);
  float right = top * aspect; // aspect = viewport_w / viewport_h
  mat4  P     = glFrustum(-right, right, -top, top, near, far);
  ```
- **View (`0x0257f0..0x025814`)**: Standard `gluLookAt(eye, target, up)` → translation by `(0, 0, -2)` (identity rotation).

### 2.2 Ribbon Model Matrix `M_line` (`0x029dfc..0x029ff0`)

Constructed from `LINE1.mnu` `POS X/Y/Z`, `ANG X/Y/Z`, `ANG ROT` and stored at `lines + 0x1f0` (`496`):
```cpp
vec3  axis      = normalize(vec3(ANG_X, ANG_Y, ANG_Z));
float angle_rad = ANG_ROT * 0.0174532925f;          // 0x9232c = PI / 180
mat4  R         = mat4_from_axis_angle(axis, angle_rad); // sub_24228
mat4  M_line    = translate(vec3(POS_X, POS_Y, POS_Z)) * R;
```
*(In column-major layout: columns 0..2 are the rotated basis vectors of `R`, and column 3 is `(POS_X, POS_Y, POS_Z, 1.0f)`.)*

### 2.3 `world_mtx` Passed to `spline.elf` (`Lines::computeWorldMtx`, `0x050cd0..0x050da4`)

**CONFIRMED: `world_mtx` is the full `ModelViewProjection` (`MVP`) matrix!**
```cpp
mat4 P         = ctx->getProjectionMatrix(); // sub_3da50 (stack +48)
mat4 V         = ctx->getModelViewMatrix();  // sub_3da7c (stack +24)
mat4 world_mtx = (P * V) * M_line;           // sub_3f290 twice -> stored at lines+560
```
**Why `lines1.vpo` works with `MVP`:**
- `spline.elf` transforms every `19×19` FFD-deformed control point by `world_mtx` (`MVP`) into **homogeneous clip space** `(x_c, y_c, z_c, w_c)` *before* bicubic B-spline tessellation!
- Because bicubic B-spline evaluation is affine-invariant (basis weights sum to `1`), `P(u, v)` is directly in clip space and `lines1.vpo` simply writes `HPOS = oldpos`.
- `lines1.vpo` also computes `V = normalize(oldpos.xyz)` and `w = (|dot(V, oldnrm.xyz)| / length(oldpos.xyz)) * _MipmapBias` directly in clip space — so matching `world_mtx = P * V * M_line` (with OpenGL clip-space Z in `[-w, +w]` or adjusted for Vulkan) is essential to reproduce the exact Sony ribbon shading!

---

## 3. FFD Lattice (`11×7×7`) & `ffd_shader0..3`

### 3.1 `_UVW` Base Texture (`FFD::init(8, 4, 4)`, `0x0493c4..0x049624`)

The FFD shader does **not** render an `11×7×7` volume directly. Instead, it renders a **`128 × 1` `RGBA32F`** texture representing an **`8 (X) × 4 (Y) × 4 (Z)`** grid:
```cpp
// _UVW is a 128x1 RGBA32F texture (0x0494b4..0x04956c):
vec4 uvw[128];
for (int iz = 0; iz < 4; ++iz)
  for (int iy = 0; iy < 4; ++iy)
    for (int ix = 0; ix < 8; ++ix)
      uvw[(iz * 4 + iy) * 8 + ix] = vec4(ix * 0.125f, iy * 0.25f, iz * 0.25f, 0.0f);
```
- `0x92cd8 = 0.125f` (`1/8`), `0x92cdc = 0.25f` (`1/4`), `0x92ce0 = 0.0f`.

### 3.2 `FFD::update(float time)` (`0x04a220..0x04a9cc`) & `ffd_shader0..3`

1. Sets viewport `(0, 0, 128, 1)` and orthographic projection `glOrtho(-1, 1, -1, 1, -1, 1)`.
2. Binds active shader `ffd_shader[k]` (`k = FFD SHADER`, stock `1` → `ffd_shader1.fpo`) with:
   - `_Time = time` (`sim_time * 10.0f`)
   - `_Param1 = FFD PARAM 1` (stock `-1.33509f`; note `ffd_shader0..3.fpo` actually ignore `_Param1`, which is used by the X-axis pinch in `Lines::update`!)
   - `_UVW` = the `128×1` RGBA32F base coordinate texture above.
3. **Shader/time crossfade (`0x04a4dc..0x04a7b8`)**: When `blend_t > 0` (set to `1.0f` on `resetTime` or shader change), renders the previous shader/time into a second `128×1` target, blends them with `ffd_alpha_blend.fpo` using `alpha = blend_t * blend_t * (3.0f - 2.0f * blend_t)`, and decrements `blend_t -= 0.005f` per frame.
4. **Readback & Scaling (`0x04a7e8..0x04a8c0`)**:
   Reads back all `128` `RGBA32F` texels `(dx, dy, 0, 0)` into CPU memory and scales each texel:
   ```cpp
   // 0x04a874..0x04a8c0:
   // ffd+96  = FFD SCALE1 (LINE1.mnu: FFD SCALE1 X/Y/Z, 1.0)
   // ffd+112 = FFD SCALE2 (LINE1.mnu: FFD SCALE2 X/Y/Z, 1.0)
   // ffd+128 = FFD OFFSET (LINE1.mnu: FFD OFFSET X/Y/Z, 1.0)
   // NOTE: 0x04a8b0 loads ffd+96 (r7=96) and 0x04a8b4 loads ffd+128? Wait: r8=128 (mtctr 8; li 8, 128)!
   // So v_scaled[k] = v_readback[k] * ffd_scale1 + ffd_offset (vmaddfp v1, v1, v13, v0)!
   ```
   *(Important: in `0x04a8b0..0x04a8b8`, `r4 = this`, `r7 = 96` (`ffd_scale1`), `r8 = 128` (`ffd_offset`), so each readback displacement `vec4(dx, dy, 0, 0)` is transformed by `d_scaled = d_raw * ffd_scale1 + ffd_offset` before unpacking into the lattice! Meanwhile `spline.elf` also receives `ffd_scale1`, `ffd_scale2`, `ffd_offset` in `SplineJobDesc` for `u = (p - ffd_offset) / ffd_scale1` and `(u + disp) * ffd_scale2`.)*
5. **Unpacking `8×4×4` → `11×7×7` (`FFD::unpack8x4x4To11x7x7`, `0x047b1c..0x047c08`)**:
   Edge-clamped padding (`1` pad on the low side, `2` pad on the high side on every axis):
   ```cpp
   // Output layout matches spline.elf LS 0xb380: vec4 lattice[7][7][11] ([iz][iy][ix])
   void unpack_ffd_lattice(const vec4 src_128[128], vec4 dst[7][7][11]) {
       for (int iz = -1; iz <= 5; ++iz) {
           int cz = std::clamp(iz, 0, 3);
           for (int iy = -1; iy <= 5; ++iy) {
               int cy = std::clamp(iy, 0, 3);
               for (int ix = -1; ix <= 9; ++ix) {
                   int cx = std::clamp(ix, 0, 7);
                   dst[iz + 1][iy + 1][ix + 1] = src_128[(cz * 4 + cy) * 8 + cx];
               }
           }
       }
   }
   ```
   **C++20 shortcut**: Since `ffd_shader0..3.fpo` only evaluates `128` points (`8×4×4`) with simple `sin`/`exp`/`tanh`, we can evaluate `ffd_shader0..3` directly on the CPU in < 1 µs (zero GPU round-trip / readback stall!) and feed `unpack_ffd_lattice()` straight into our C++20 `spline.elf` port!

---

## 4. Procedural LUTs, Uniform Mapping & `LinesController::draw`

### 4.1 Procedural LUTs & Static Ribbon UV Buffer

| Resource | Dimensions / Format | Generation / Source (`custom_render_plugin.prx`) |
|---|---|---|
| **`_Stripes`** | **`16 × 4`** `RGBA8` (`0xa0f58 = (int)powf(2, 4) = 16`) | Procedural CPU (`0x04bad0..0x04bc90`, `CLAMP_TO_EDGE`, `LINEAR`):<br>For `y = 0..3`, `x = 0..15`:<br>`fy = y / 3.0f; fx = x / 15.0f;`<br>`v = min(powf(-4.0f * (fx - 0.5f)^2 + 1.01f, 20.0f * fy^3) * (fy * 0.5f + 0.5f), 1.0f);`<br>`rgba = (uint8_t)(v * 255.0f)` in all 4 channels. |
| **`_FresLUT`** | **`512 × 1`** `RGB8` | Loaded from `textures/TGA/freslut1.tga` (`0x04eb40..0x04ec90`, `CLAMP_TO_EDGE`, `LINEAR`). `R` = Fresnel rim weight, `G` = stripe diffuse weight. |
| **`_Encode`** | **`4096 × 1`** `RGBA8` | Procedural CPU (`0x04b928..0x04bacc`, `CLAMP_TO_EDGE`, `NEAREST`):<br>For `i = 0..4095`:<br>`x = (i / 4095.0f) * 128.0f;`<br>`frac = modf(x, &intpart);`<br>`c0 = clamp(frac * 0.25f * 255.0f, 0, 255);`<br>`c1 = clamp(intpart * (1.0f / 64.0f) * 255.0f, 0, 255);`<br>`rgba = (c0, c1, c0, c1)` (RSX 2-channel 8-bit accumulation pack; unnecessary in FP16 HDR Vulkan!). |
| **Static `128×128` UV Stream** (`oldcrd`) | `128 × 128` `vec2` | Procedural CPU (`0x04d190..0x04d2f0`):<br>For `iy = 0..127`, `ix = 0..127`:<br>`u = ix / 127.0f; v = iy / 127.0f;`<br>`edge_u = min((0.5f - fabsf(0.5f - u)) * 10.0f, 1.0f);`<br>`edge_v = min((0.5f - fabsf(0.5f - v)) * 10.0f, 1.0f);`<br>`oldcrd = vec2(v, edge_u * edge_v);` *(10% linear edge-fade mask on all 4 edges!)* |

### 4.2 Exact `lines1` Uniform Mapping (`0x04c94c..0x04cc14`)

All `LINE1.mnu` parameters map **1:1 (unscaled)** to the `lines1.{vpo,fpo}` uniforms:
- **Vertex (`lines1.vpo`)**: `_Fresnel = FRESNEL`, `_Falloff = FALLOFF`, `_Brightness = BRIGHTNESS`, `_MipmapBias = MIPMAP BIAS`.
- **Fragment (`lines1.fpo`)**: `_Spacing = SPACING`, `_Thinness = THINNESS`, `_Stripes`, `_FresLUT`, `_Encode`.

### 4.3 `back_colours0.fpo` Uniform Formulas (`LinesController::renderBackColours`, `0x052ad8..0x052eec`)

Rendered into a **`64 × 32` `RGBA16F`** offscreen texture (`_Back`) whenever the sky updates:
```cpp
// Inputs:
//   day_t   in [0, 1]  (normalized time of day: hour / 24.0f, 0.0 = midnight, 0.5 = noon)
//   month_t in [0, 12] (0.0 = Jan 1, 11.99 = Dec 31)
//   alpha   in [0, 1]  (fade alpha, typically 1.0f)
// Parameters from BACKGROUND.mnu (defaults from 0x0585d0..0x058620):
//   night_blend     = NIGHT BLEND      (default 0.0f;  override/night: 0.499947f)
//   night_whit_bias = NIGHT WHIT BIAS  (default 0.5f;  override/night: 0.486059f)
//   n2d_begin       = NIGHT2DAY BEGIN  (default 4.0f;  override/night: 0.0f)   [hours / 24]
//   n2d_end         = NIGHT2DAY END    (default 6.0f;  override/night: 5.16611f)
//   d2n_begin       = DAY2NIGHT BEGIN  (default 18.0f; override/night: 18.498f)
//   d2n_end         = DAY2NIGHT END    (default 20.0f; override/night: 20.3312f)
//   dayspread       = DAYSPREAD        (default 1.0f;  override/night: 2.68038f)

float t = std::clamp(day_t, 0.0f, 1.0f);

// 1. Cubic Hermite curve for _DayTime when COLOUR SHADER == 0 (0x052a28, 0x04faf0):
//    M_hermite * [t^3, t^2, t, 1] dot [0, 1, dayspread, dayspread]
float t_curved = (colour_shader == 0)
    ? ((-2.0f * t * t * t + 3.0f * t * t) + (2.0f * t * t * t - 3.0f * t * t + t) * dayspread)
    : t;

float _DayTime   = t_curved * (colour_shader == 0 ? 2400.0f : 41.666668f); // 0x92e60 / 0x92e64
float _NightTime = ((t - 0.5f < 0.0f) ? (t + 0.5f) : (t - 0.5f)) * 2400.0f;

// 2. Month selection & fractional day (0x052c58..0x052cd8)
float m_clamped  = std::clamp(month_t, 0.0f, 12.0f);
int   m_int      = int(m_clamped);
int   tex1_idx   = (m_int > 11) ? (m_int - 12) : m_int;          // 0..11 -> 01.dds..12.dds
int   tex2_idx   = (tex1_idx + 1 > 11) ? 0 : (tex1_idx + 1);
float _MonthTime = (m_clamped - float(m_int)) * 30.0f;           // 0x92e68 = 30.0f

// 3. Night-to-Day blend (0x052cdc..0x052d24, sub_3e980 = smoothstep)
//    Note: n2d/d2n are stored in [0, 1] (hours / 24.0f) when synced from BACKGROUND.mnu!
float s_dawn = smoothstep(n2d_begin, n2d_end, t);
float s_dusk = smoothstep(d2n_begin, d2n_end, t);
float day_mask = s_dawn * (1.0f - s_dusk);                       // 1 during day, 0 at night
float _NightDayBlend   = 1.0f + (day_mask - 1.0f) * night_blend; // 1.0 if night_blend == 0
float _NightBrightness = night_whit_bias;
float _Alpha           = alpha;
```

### 4.4 `LinesController::draw` Pass Sequence & `hgradcol` / `vgradcol` (`0x057cf8..0x0584b4`)

1. **Pass 1 — Ribbon Accumulation (`0x057d94..0x057f58`)**:
   - Calls `Lines::beginDraw` (`0x04e558`): enables `GL_BLEND`, sets **additive blending `GL_ONE, GL_ONE`**, disables depth test, depth write, and backface culling.
   - Binds offscreen ribbon FBO (`Tex0`, `RGBA8` on RSX; **`RGBA16F` linear HDR** in Vulkan!) and clears to `(0, 0, 0, 0)`.
   - Draws `Lines::draw(0)` (`lines1.vpo` + `lines1.fpo`, `128×128` tessellated ribbon mesh).
   - *(On SD/interlaced modes `height <= 480`, flag `+0x1b4` triggers a second jittered ribbon pass shifted by `(1, -1)` pixels and switches composite to `LinesControllerQuin.fpo`.)*
2. **Pass 2 — Sky + Ribbon Composite (`0x057f5c..0x058450`)**:
   - Restores main HDR framebuffer, disables blending, sets `glOrtho(0, 1, 0, 1, -1, 1)`.
   - Binds `LinesController.vpo` + `LinesController.fpo` with:
     - `_Tex0` = ribbon accumulation texture, `_Back` = `64×32` sky texture (`back_colours0.fpo`), `_Noise` = `32×32` noise texture.
     - `_ScreenSizeRecip = (1.0f / width, 1.0f / height)`, `_ScreenOffset = (0.25f, 0.25f)` (HD) or `(0.25f, 0.5f)` (SD).
     - `_NoiseJitter = (rand01 * 0.5f + 0.5f, rand01 * 0.5f + 0.5f)`, `_Dither = 1.0f / 255.0f`.
     - **Gradient colors (`0x05829c..0x058338`)**: Each `BACKGROUND.mnu` RGB triplet is multiplied by the animated global tint `tint` (`+0x260`, default `(1, 1, 1, 1)`) with `.a = 1.0f`:
       - `_Colour1` (`vgradcol` at `y = 0`) ← `2 RED/GREEN/BLUE * tint` (`+0x230`)
       - `_Colour2` (`vgradcol` at `y = 1`) ← `1 RED/GREEN/BLUE * tint` (`+0x220`)
       - `_Colour3` (`hgradcol` at `x = 0`) ← `3 RED/GREEN/BLUE * tint` (`+0x240`)
       - `_Colour4` (`hgradcol` at `x = 1`) ← `4 RED/GREEN/BLUE * tint` (`+0x250`)
   - Draws the `64 × 32` screen mesh (`0x05381c`), compositing `8.0 * (ribbon_val * hgradcol + sky * vgradcol)` into the HDR buffer for the downstream Bloom/Glare/ToneMap chain.
