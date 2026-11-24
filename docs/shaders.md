# PS3 XMB (`lines.qrc`) RSX Shader Reference

Decoded from the stock Sony PS3 `lines.qrc` Cg binary shaders (`0x1b5b` `SCE_VP_RSX` / `0x1b5c` `SCE_FP_RSX`, revision 6) extracted to `extracted/dblue_day/lib/` using [decode_rsx_shaders.py](file:///home/ric/Modern-XMB/tools/decode_rsx_shaders.py).

---

## Key RSX-Specific Details & Pipeline Quirks

1. **No `_MVM` transform in `lines1.vpo`**: Unlike `lines0.vpo` and `lines2.vpo`, `lines1.vpo` passes `oldpos` straight to `HPOS` (`newpos = oldpos`) and uses `oldpos.xyz` directly as the eye-space position vector `V = normalize(oldpos.xyz)`. The SPU/CPU ribbon generator pre-transforms vertices, and `oldnrm.xyz` is *not* unit-length — its magnitude scales `w = (|dot(V, oldnrm)| / |oldpos|) * _MipmapBias`, which drives both `_Stripes` LOD (`newcrd.y`) and intensity (`params.xyz`).
2. **2-Channel `_Encode` LUT / 8-bit Accumulation Buffer Trick**:
   - On PS3 RSX, `lines1.fpo` does not output linear HDR directly. Instead, it evaluates a scalar ribbon intensity `val` and looks it up in a 1D texture `_Encode` at `(val, 0.5)`.
   - `lines2.fpo` performs the same trick arithmetically (`val * 128`, splitting integer and fractional parts into `.w * (1/64)` and `.z * 0.25`).
   - `LinesController.fpo` then decodes the accumulated 2-channel ribbon buffer `_Tex0` via `H2.w = R1.y * 0.5 + R1.x * 0.03125` (a 16:1 high/low channel split), multiplies by the horizontal gradient `hgradcol`, adds `_Back * vgradcol`, scales by `8x` (`MADR*8`), and passes through `preexpose_ExposeTex0/1` + `preexpose_Noise * 0.125` dithering.
   - In modern Vulkan (`R16G16B16A16_SFLOAT` HDR targets), `lines1.frag` can output linear HDR directly without `_Encode` quantization.
3. **Per-Channel RGB Gaussian Radii (`Gaussian.fpo`)**:
   - `_Weights0`..`_Weights7` are **`vec4` (RGB)**, not scalars. Each symmetric tap pair `(sample(+k) + sample(-k)).rgb` is multiplied by `_Weights[k].rgb`, allowing distinct blur radii for R, G, and B (chromatic bloom dispersion).
4. **FP16 Half-Precision & Inline Constants**:
   - `lines1.fpo`, `ffd_shader0..3.fpo`, and `background.fpo` output from `H0` (`outH0=1`).
   - `ToneMap.fpo` executes Extended Reinhard in `fp16` (`H0..H3`) before writing `R0.xyz`.
   - Exponential functions in RSX FP use `EX2(x * 1.442695)` (`log2(e)`), and `tanh(x)` is expanded as `(exp(2x) - 1) / (exp(2x) + 1)` with `x` clamped to `[-10, 10]`.

---

## 1. Background & Procedural Sky (`lib/moyou/`)

### `background.vpo` / `background.fpo`
- **VP Inputs**: `oldpos` (`ATTR0`, `vec4`), `oldcrd` (`ATTR8`, `vec2`)
- **VP Uniforms**: `_MVM` (`mat4`, `c[256..259]`), `_Colour1` (`vec4`, `c[467]`), `_Colour2` (`vec4`, `c[466]`)
- **VP Outputs**: `newpos` (`HPOS`), `newcrd` (`TEX0`, `vec2`), `gradcol` (`TEX1`, `vec4`)
- **FP Samplers**: `_Tex` (`TEXUNIT0`, `sampler2D`)
- **Decoded Math**:
  ```glsl
  // Vertex
  newpos  = _MVM * oldpos;
  newcrd  = oldcrd;
  gradcol = mix(_Colour1, _Colour2, oldcrd.y);

  // Fragment
  col.rgb = texture(_Tex, uv).rgb * gradcol.rgb;
  ```

### `back_colours0.fpo` (Primary Time-of-Day / Month-of-Year Sky Generator, 538 insts)
- **Inputs**: `uv` (`TEX0`, `vec2`)
- **Samplers**: `_MonthlyTex1Day` (`TEX0`), `_MonthlyTex2Day` (`TEX1`), `_MonthlyTex1Night` (`TEX2`), `_MonthlyTex2Night` (`TEX3`)
- **Uniforms**: `_DayTime`, `_MonthTime`, `_NightTime`, `_NightBrightness`, `_Alpha`, `_NightDayBlend`, `PI` (`3.14159`)
- **Decoded Math**:
  1. **Quintic smoothstep interpolation on 64×32 monthly LUTs**:
     ```glsl
     vec2 p = uv * vec2(64.0, 32.0) + 0.5;
     vec2 i = floor(p);
     vec2 f = p - i;
     vec2 u = (i + f * f * f * (f * (f * 6.0 - 15.0) + 10.0) - 0.5) * vec2(1.0 / 64.0, 1.0 / 32.0);
     ```
  2. Samples `_MonthlyTex1Day(u)` and `_MonthlyTex2Day(u)` and converts both **RGB → HSV** (branchless 6-sector).
  3. Computes solar/lunar glow discs from `sin`/`cos` of `(_DayTime - 600) * PI / 1200` and `(_NightTime - 600) * PI / 1200` (`exp(-8 * d^2)` and `exp(-4 * d^2)`), plus clamped `tanh` time-of-day transitions around `600`, `1200`, `1800` and monthly curve centered at `_MonthTime = 15`.
  4. Modulates HSV saturation and value with vertical sky falloff `exp(uv.y * -6.4 * (1 - ...))` and floors brightness at `_NightBrightness`.
  5. Converts both day colors **HSV → RGB**, blends them by a `tanh` monthly/horizontal mask, blends `_MonthlyTex1Night(u)` and `_MonthlyTex2Night(u)` by the same mask, and mixes Night → Day by `_NightDayBlend` with `col.a = _Alpha`.

### `back_colours1.fpo` (Analytical Sky Gradient, 79 insts)
- **Inputs**: `uv` (`TEX0`); **Uniforms**: `_DayTime`, `_Alpha` (no samplers).
- Computes 3 analytical Gaussian/exponential lobes in `uv` space modulated by `0.5 * cos(_DayTime)` and a `tanh` horizon transition; outputs `col.a = _Alpha`.

### `back_colours2.fpo` (2 insts)
- Outputs `col.a = _Alpha` only.

---

## 2. Ribbon / Wave & FFD (`lib/moyou/`)

### `lines1.vpo` / `lines1.fpo` (Primary XMB Wave Ribbon)
- **VP Inputs**: `oldpos` (`ATTR0`, `vec4`), `oldcrd` (`ATTR8`, `vec2`), `oldnrm` (`ATTR9`, `vec4`)
- **VP Uniforms**: `_Fresnel` (`c[467]`), `_Brightness` (`c[466]`), `_MipmapBias` (`c[465]`)
- **VP Outputs**: `newpos` (`HPOS`), `newcrd` (`TEX0`, `vec2`), `params` (`TEX1`, `vec4`), `eyevec` (`TEX2`, `vec3`), `newnrm` (`TEX3`, `vec3`)
- **FP Uniforms**: `_Spacing` (float), `_Thinness` (float)
- **FP Samplers**: `_Stripes` (`TEXUNIT0`), `_FresLUT` (`TEXUNIT1`), `_Encode` (`TEXUNIT2`)
- **Decoded Math**:
  ```glsl
  // Vertex
  newpos = oldpos;
  vec3 N = normalize(oldnrm.xyz);
  float invLenPos = inversesqrt(dot(oldpos.xyz, oldpos.xyz));
  vec3 V = oldpos.xyz * invLenPos;
  float NdotV = dot(V, N);
  float w = invLenPos * abs(dot(V, oldnrm.xyz)) * _MipmapBias;
  eyevec = V;
  newnrm = V - 2.0 * NdotV * N; // reflect(V, N)
  params = vec4(vec3(_Fresnel, _Brightness, abs(NdotV)) * w, oldcrd.y);
  newcrd = vec2(oldcrd.x, w);

  // Fragment
  float u = fract(oldcrd.x * _Spacing);
  float stripeU = clamp(u - _Thinness, 0.0, 1.0) / (1.0 - _Thinness);
  float stripe = texture(_Stripes, vec2(stripeU, oldcrd.y)).r;
  vec2 fres = texture(_FresLUT, params.zw).rg;
  float val = (fres.g * params.y * stripe + fres.r * params.x) * params.w;
  newcol = texture(_Encode, vec2(val, 0.5)); // RSX 2-channel pack; direct HDR in Vulkan
  ```

### `lines0.{vpo,fpo}` & `lines2.{vpo,fpo}`
- **`lines0`**: Passthrough (`newpos = _MVM * oldpos`, `newcol = oldcol`).
- **`lines2`**: Analytical Fresnel variant (no `_FresLUT`, no `_Encode`). Computes `pow(|NdotV|, _Falloff) * _Fresnel * (5*|NdotV| - 0.5)` and `|NdotV| * _Brightness` in VP, then arithmetically splits `(stripe * params.x + params.y) * 128` into `.w = floor/64` and `.z = frac/4`.

### `ffd_shader0..3.fpo` (Free-Form Deformation Wave Lattice)
- **Inputs**: `uv` (`TEX0`); **Uniforms**: `_Time`; **Sampler**: `_UVW` (`TEXUNIT0`, base `(u, v)` lattice).
- **Output**: `H0 = vec4(dx, dy, 0, 0)`.
- **`ffd_shader0`**: `t = _Time + 2400.0`; two traveling Gaussians `exp(-50*(u - c1)^2) - 0.15*exp(-10*(u - c2)^2)` where `c1 = (sin(0.1*t)+0.2)*0.83333`, `c2 = (sin(0.1*t+20)+0.2)*0.83333`; main wave `(0.5 - v)*(1 - exp(-2*u))*sin(-6.28*u + 0.5*t*(1 - 0.8*u))`; ripple `0.25*sin(15.7*u - 0.13*t - 2.0)`; `dx = 1.31*u*u + 0.2*u`.
- **`ffd_shader1`**: `t = _Time`; `tanh(u - 0.5)` envelope; slow decay `exp(-0.0001*t)`; breathing `(sin(2*t)+3) * (0.24*tanh(u-0.5) + 0.24) * sin(7.85*u - 2.5*t - 1.25)`; traveling Gaussian `exp(-50*(u - (sin(0.1*t)+0.2)*0.83333)^2)`; vertical ripple `0.125*sin(t - 6.28*v)`; sway `0.5*sin(0.25*t)`; `dx = 1.3*u*u + 0.2*u - 0.15`.
- **`ffd_shader2`**: Simplified `ffd_shader1` (just the `tanh(u-0.5)` + `exp(-0.0001*t)` + `(sin(2*t)+3)` + `sin(7.85*u - 2.5*t - 1.25)` wave, `dx = 1.3*u*u + 0.2*u`).
- **`ffd_shader3`**: Simplified `ffd_shader0` with `t = _Time` (no `+2400`, no `15.7*u` ripple, `2*(0.5 - v)` amplitude, `dx = 1.2*u*u`).

### `LinesController.{vpo,fpo}` & `LinesEdge.fpo`
- **`LinesController.vpo`**: `newpos = _MVM * oldpos`, `vgradcol = mix(_Colour1, _Colour2, oldcrd.y)`, `hgradcol = mix(_Colour3, _Colour4, oldcrd.x)`.
- **`LinesController.fpo`**: Decodes `_Tex0` at `(WPOS + _ScreenOffset) * _ScreenSizeRecip` via `val = tex0.g * 0.5 + tex0.r * 0.03125`, computes `rgb = 8.0 * (val * hgradcol.rgb + texture(_Back, uv).rgb * vgradcol.rgb)`, looks up `preexpose_ExposeTex0/1`, and adds `preexpose_Noise * 0.125`.
- **`LinesEdge.fpo`**: 3×3 binomial blur (`[1,2,1; 2,4,2; 1,2,1]/16` at `1/1920, 1/1080`) diffed against center to produce edge mask `clamp(|center - blur| * 7.0, 0.0, 1.0)^2`.

---

## 3. Bokeh / Sparkle Particles (`lib/particles/`)

### `particles_quads.vpo` / `particles_quads.fpo`
- **VP Inputs**: `IN.pos` (`ATTR0`, `.xyz` = world pos, `.w` = base alpha), `IN.uv0` (`ATTR8`, quad `[0,1]^2`), `IN.rot` (`ATTR11`, quaternion)
- **VP Uniforms**: `_ModelviewProjection` (`c[256..259]`), `_Modelview` (`c[260..263]`), `_LightPack` (`c[264..267]`), `_ParticleSize` (`c[467]`), `_Transparency` (`c[466]`), `_Focus` (`c[465]`), `_FocusCurves` (`c[464]`), `_FrontFacingQuaternion` (`c[463]`), `_NearControl` (`c[462]`), `_LifeBoundsMin` (`c[461]`), `_LifeBoundsMax` (`c[460]`), `_Darkness` (`c[459]`)
- **FP Samplers**: `_IridescentTex` (`TEXUNIT0`)
- **FP Uniforms**: `_LightPack[0..3]`, `_Color`, `_NearControl`, `_IridescentExp`, `_Gamma`
- **Decoded Math**:
  - **Life bounds fade**: `t = clamp(min(pos - _LifeBoundsMin, _LifeBoundsMax - pos) * 5.0, 0, 1)`, `lifeFade = t*t*(3 - 2*t)`.
  - **Depth-of-Field CoC**: `dist = length(eyePos - pos)`; `nearBlur = pow(clamp((dist - _Focus.y)/(_Focus.x - _Focus.y), 0, 1), _FocusCurves.x)`; `farBlur = pow(clamp((dist - _Focus.z)/(_Focus.w - _Focus.z), 0, 1), _FocusCurves.y)`; `size = mix(mix(_ParticleSize.x, _ParticleSize.y, nearBlur), _ParticleSize.z, farBlur)`.
  - **Angular size**: `angularSize = 2.0 * atan(size / dist) / _FocusCurves.z` (evaluated via 11th-order minimax polynomial in RSX VP).
  - **Billboard nlerp**: Blends `IN.rot` toward `_FrontFacingQuaternion` by `nearFade + nearBlur*_NearControl.z + angularSize*_NearControl.x`, converts quaternion to basis `(right, up, forward)`, expands quad by `(uv0 - 0.5) * size`.
  - **Fragment lighting**: Iridescent normal lookup `pow(1 + (tex(normal_v.xy*0.5+0.5) - 1) * _LightPack[3].w, _IridescentExp)`, Blinn-Phong specular + diffuse with quadratic attenuation `c0 + d*(c1 + d*c2)`, exponential tone compression `1 - exp(-lit * _LightPack[2].w)`.
  - **Bokeh disc**: `r = length(2*(uv0 - 0.5))`, `blur = clamp(nearBlur + farBlur, 0, 1)`, `softness = blur * 0.6345 + 0.05`, `disc = 1 - smoothstep(0.5 - softness, 0.5 + softness, r)`, `k = mix(0.85, _NearControl.y, nearBlur)`, `alpha = mix(disc, 1 - exp(-disc * k), blur) * uv3.w`.

### `particles_second.vpo` / `particles_second.fpo`
- Secondary sparkle/glare pass: scales quad by specular intensity `pow(|dot(N, H)|, _LightPack[3].y) * _Glare.y` and replaces the bokeh disc with a radial starburst `exp(-pow(r, _Glare.z) * _Glare.w) * _Glare.x`.

---

## 4. HDR / Bloom / Glare / Tonemap Chain (`lib/glutils/`)

| Shader | Inputs / Samplers | Uniforms | Decoded Math |
|---|---|---|---|
| `GlareSource.fpo` | `_TexSamp` (`TEX0`) | (all unused) | Direct passthrough `texture(_TexSamp, uv)` |
| `GlareSourcePre.fpo` | `_TexSamp` (`TEX0`) | `_Exposure`, `_WhiteSqrRcp` | Inverse Reinhard `(-1 + sqrt(1 + 4*c*_WhiteSqrRcp)) / (2*_WhiteSqrRcp*_Exposure)`, `.a = dot(rgb, (0.27, 0.67, 0.06))` |
| `Gaussian.vpo` | `pos_in`, `tex_in` | `_ModelviewProjection`, `_Offset` | Outputs `tex_out0 = uv.xyxy`, `tex_out[k] = uv.xyxy + k*vec4(_Offset, -_Offset)` (`k=1..7`) |
| `Gaussian.fpo` | `_TexSamp` (`TEX0`), `tex_out0..7` | `_Weights0..7` (`vec4` RGB!) | 15-tap separable **per-channel RGB** Gaussian: `sum(k=0..7) (tap(+k)+tap(-k)).rgb * _Weights[k].rgb` |
| `DoubleGaussian.fpo` | `_TexSamp0`, `_TexSamp1` | `_Weights`, `_Weights2`, `_Offset` | MRT 15-tap separable Gaussian blurring both textures simultaneously (`COL0`, `COL1`) |
| `GaussBloomAndBlur.fpo` | `_TexSamp` (`TEX0`) | `_Weights`, `_Weights2`, `_Offset`, `_Threshold`, `_GlareWeight` | MRT 15-tap pass: `COL0 = blur(clamp(max(c - _Threshold, 0)*_GlareWeight, 0, 1))`, `COL1 = blur(c)` |
| `ToneMap.fpo` | `_TexSamp` (`TEX0`) | `_Exposure`, `_WhiteSqrRcp` | Extended Reinhard: `L = rgb * _Exposure; out = L * (1 + L * _WhiteSqrRcp) / (1 + L)` |
| `ApplyGlare.fpo` | `_TexSamp0`, `_TexSamp1` | `_Weight1` (`vec4`) | `col.rgb = tex0 * _Weight1.x + tex1 * _Weight1.y; col.a = 1.0` |
| `ToneApplyDisplay.fpo` | `_Glare` (`TEX1`), `preexpose_Noise` (`TEX13`) | `_GlareWeight` | `col = texture(_Glare, uv) * _GlareWeight - texture(preexpose_Noise, winpos) * 0.125` |

---

## 5. Translated Vulkan GLSL 450 Shaders (`shaders/decoded/`)

All core passes have been translated to clean Vulkan GLSL 450 and verified with `glslc`:
- [background.vert](file:///home/ric/Modern-XMB/shaders/decoded/background.vert) / [background.frag](file:///home/ric/Modern-XMB/shaders/decoded/background.frag)
- [back_colours0.frag](file:///home/ric/Modern-XMB/shaders/decoded/back_colours0.frag)
- [lines1.vert](file:///home/ric/Modern-XMB/shaders/decoded/lines1.vert) / [lines1.frag](file:///home/ric/Modern-XMB/shaders/decoded/lines1.frag)
- [ffd_shader0.frag](file:///home/ric/Modern-XMB/shaders/decoded/ffd_shader0.frag) / [ffd_shader1.frag](file:///home/ric/Modern-XMB/shaders/decoded/ffd_shader1.frag)
- [particles_quads.vert](file:///home/ric/Modern-XMB/shaders/decoded/particles_quads.vert) / [particles_quads.frag](file:///home/ric/Modern-XMB/shaders/decoded/particles_quads.frag)
- [gaussian.vert](file:///home/ric/Modern-XMB/shaders/decoded/gaussian.vert) / [gaussian.frag](file:///home/ric/Modern-XMB/shaders/decoded/gaussian.frag)
- [gauss_bloom_and_blur.frag](file:///home/ric/Modern-XMB/shaders/decoded/gauss_bloom_and_blur.frag)
- [apply_glare.frag](file:///home/ric/Modern-XMB/shaders/decoded/apply_glare.frag)
- [tonemap.vert](file:///home/ric/Modern-XMB/shaders/decoded/tonemap.vert) / [tonemap.frag](file:///home/ric/Modern-XMB/shaders/decoded/tonemap.frag)
