# `modern-moyou-theme/v1` — Open JSON Theme Specification

An open, human-readable, MIT-licensed JSON theme format for **`modern-moyou`**, providing 1:1 expressive parity with all simulation parameters, overrides, and lookup tables used by the engine — with zero proprietary binary assets required.

## Quick Example

```json
{
  "schema": "modern-moyou-theme/v1",
  "name": "Aurora Sunset",
  "sky": {
    "night": ["#050918", "#0d1a36", "#162a4a"],
    "dawn":  ["#1d1538", "#9c3b5b", "#f29b50"],
    "day":   ["#0c2d6b", "#2a6cb8", "#7ec4e8"],
    "dusk":  ["#191130", "#b83c4c", "#f07c3a"]
  },
  "line1": {
    "brightness": 0.78,
    "fresnel": 0.62,
    "fresnel_curve": { "power": 2.8, "bias": 0.02, "scale": 0.98 }
  },
  "hdr": {
    "exposure": 1.08,
    "glare_level": 1.15
  },
  "particles": {
    "emit_per_frame": 18.0,
    "iridescent_gradient": ["#ff6ad5", "#6ad8ff", "#ffe57f"]
  },
  "overrides": {
    "night": {
      "line1": { "brightness": 0.92, "fresnel": 0.50 },
      "hdr":   { "exposure": 1.38, "glare_level": 1.30 }
    }
  }
}
```

Every section and field except the outer `{}` object is **optional**; omitted fields inherit engine defaults (or the built-in 12-month seasonal sky). Single-line `//` comments are permitted.

---

## Color & Vector Values

- **RGB colors** (`color1..4`, `sky` stops, `iridescent_tint`, `iridescent_gradient` stops) accept either:
  - Hex strings: `"#rrggbb"` (case-insensitive)
  - Normalized float triples: `[r, g, b]` in `[0.0, 1.0]`
- **3D vectors** (`pos`, `ang`, `end`, `ffd_scale1`, `ffd_scale2`, `ffd_offset`, `gaussian_rad`, `wind_dir`, `spot_pos`, `spot_attn`) are `[x, y, z]` float arrays.

---

## Top-Level Fields

| Field | Type | Description |
|---|---|---|
| `schema` | `string` | Format identifier: `"modern-moyou-theme/v1"`. |
| `name` | `string` | Human-readable display name (defaults to filename stem). |
| `sky` | `object` | Optional 2D diurnal/seasonal sky LUT definition. If omitted, uses the built-in 12-month procedural sky. |
| `background` | `object` | Camera FOV, background color multipliers, and diurnal transition timing. |
| `line1` | `object` | Silk ribbon cloth physics, FFD lattice, shading, and optional `fresnel_curve` LUT. |
| `hdr` | `object` | Tone mapping, exposure, white level, and 5-mip RGB Gaussian glare. |
| `particles` | `object` | Bokeh/glint particle emitter, physics, DOF focus, and optional iridescent LUT. |
| `overrides` | `object` | Map of named override patches merged on top of the base theme. |

---

## `sky` — 2D Diurnal & Seasonal Sky LUT (`64×32`)

Synthesizes the `64×32` day and night sky textures sampled by the background shader (`X` = time of day `00:00..24:00`, `Y` = vertical zenith-to-horizon gradient).

### Single Palette (Applied to All 12 Months)
```json
"sky": {
  "night": ["#top", "#mid", "#horizon"],
  "dawn":  ["#top", "#mid", "#horizon"],
  "day":   ["#top", "#mid", "#horizon"],
  "dusk":  ["#top", "#mid", "#horizon"]
}
```
- Each stop list accepts **2 or more** colors (interpolated smoothly from zenith `Y=0` to horizon `Y=1`).
- If `dawn` and/or `dusk` are present, the day LUT smoothsteps across `X`: `0.0 = night → 0.25 = dawn → 0.50 = day → 0.75 = dusk → 1.0 = night` (enabling full hue-shifting sunrises and sunsets).
- If `dawn` and `dusk` are omitted, a subtle diurnal brightness curve is applied across `X`.
- If `night` is omitted, it defaults to `day` scaled by `0.22`.

### 12-Month Seasonal Palette
```json
"sky": {
  "months": [
    { "day": ["#2e476b", "#7a9ec7"], "night": ["#080e1a", "#182840"] },
    ...
  ]
}
```
Up to 12 entries (January–December; shorter arrays wrap modulo length). Each month object supports `day`, `night`, `dawn`, and `dusk`.

---

## `background`

| Key | Type | Default | Notes |
|---|---|---|---|
| `color1` .. `color4` | `rgb` | `[1,1,1]`, `[1,1,1]`, `[0.85,0.85,0.85]`, `[0.92,0.92,0.92]` | Sky shader color multipliers |
| `fovy` | `float` | `71.846` | Vertical camera FOV (degrees) |
| `colour_shader` | `int` | `0` | Background shader mode |
| `night_blend` | `float` | `0.0` | Extra night LUT blend factor |
| `night2day_begin`, `night2day_end` | `float` | `0.0`, `5.166` | Dawn transition window (hours) |
| `day2night_begin`, `day2night_end` | `float` | `18.498`, `20.331` | Dusk transition window (hours) |
| `dayspread` | `float` | `2.68` | Day/night curve spread |
| `night_white_bias` | `float` | `0.486` | Night luminance bias |
| `neutrinos_x` | `float` | `0.0` | Horizontal sky offset |

---

## `line1` — Silk Ribbon

| Key | Type | Default | Notes |
|---|---|---|---|
| `state` | `int` | `1` | `1` = enabled, `0` = hidden |
| `shader` | `int` | `1` | Ribbon fragment shader variant |
| `damping`, `length`, `tension` | `float` | `0.0001`, `0.306`, `0.25` | `19×19` cloth spring simulation |
| `spacing`, `thinness` | `float` | `407.658`, `1.0` | Stripe density and silhouette width |
| `brightness`, `mipmap_bias` | `float` | `0.702`, `1.867` | Ribbon luminance & `_Stripes` mip bias |
| `fresnel`, `falloff` | `float` | `0.639`, `1.003` | Fresnel grazing glow & edge falloff |
| `timestep`, `perturbation` | `float` | `4.0`, `0.1` | Cloth speed and wave agitation |
| `pos`, `ang`, `end` | `vec3` | `[-7.679,-1.088,-6.403]`, `[-0.994,0.087,0.065]`, `[3.0,0.4,0.2]` | Ribbon world placement & orientation |
| `ang_rot` | `float` | `18.121` | Rotation angle (degrees) |
| `ffd_scale1`, `ffd_scale2`, `ffd_offset` | `vec3` | `[5.677,1.001,1.0]`, `[2.828,1.276,2.888]`, `[0,-0.47,0]` | `11×7×7` FFD lattice transform |
| `ffd_param1`, `show_ffd`, `ffd_shader` | `float`/`int` | `-1.335`, `0`, `1` | FFD wave deformation shader & phase |
| `fresnel_curve` | `object` or `float[]` | *(procedural)* | Custom `512×1` `_FresLUT`: either `{ "power", "bias", "scale", "invert" }` or an array of `2+` scalar stops `[0..1]`. |

---

## `hdr` — Tone Mapping & 5-Mip RGB Glare

| Key | Type | Default | Notes |
|---|---|---|---|
| `enabled` | `int` | `1` | `1` = HDR pipeline active |
| `exposure`, `white_level` | `float` | `1.05`, `0.899` | Extended Reinhard exposure & white point |
| `glare`, `glare_only` | `int` | `1`, `0` | Enable glare pass / debug glare-only view |
| `glare_level`, `glare_thresh`, `glare_sum_pow` | `float` | `1.102`, `0.739`, `0.557` | Bloom intensity, threshold, and mip exponent |
| `gaussian_rad` | `vec3` | `[1.239, 1.432, 1.558]` | Per-channel RGB Gaussian blur radii |
| `tonebefore`, `blur` | `int` | `1`, `0` | Tonemap before glare / background blur |
| `tex_size`, `tex_max_mip`, `gamma`, `dither` | `int`/`float` | `8`, `8`, `2.2`, `0.0` | Auxiliary HDR metadata |

---

## `particles` — Bokeh & Glint Field

| Group | Keys |
|---|---|
| **Emission** | `emit_per_frame`, `emit_prob`, `emit_vel_min`, `emit_vel_mul`, `emit_vel_var`, `emit_cone_angle`, `emit_neg_prob`, `emit_vel_zscale` |
| **Physics** | `aging_speed`, `aging_variance`, `friction`, `spin_time_scale`, `delta_time`, `gravity`, `wind_dir`, `wind_scale`, `wind_scale_10`, `brownian_scale` |
| **Lighting** | `spot_pos`, `spot_attn`, `specular_power`, `specular_coeff`, `lambert_coeff`, `exposure`, `fresnel`, `color_control`, `iridescent_exp`, `global_alpha` |
| **Bokeh DOF** | `size_middle`, `size_near`, `size_far`, `near_focus`, `near_focus_dist`, `near_focus_pow`, `near_darkness`, `near_fuzziness`, `far_focus`, `far_focus_dist`, `far_focus_pow`, `far_darkness`, `near_align`, `size_align` |
| **Glint** | `glare`, `glare_scale`, `glare_p1`, `glare_p2` |
| **Iridescent LUT** | `iridescent_gradient` (`rgb[]`, `2+` stops → `128×128` 2D shimmer LUT) **or** `iridescent_tint` (`rgb` → solid tint). Omit both for the procedural pearl LUT. |

---

## `overrides` — Time-of-Day & Named Overrides

Each entry under `"overrides"` is a partial patch (`background`, `line1`, `hdr`, `particles`) merged onto the base theme.

Four reserved names automatically drive the smooth real-time **Time-of-Day rig** (`--override auto`):
- `night` (`21:00–04:30`)
- `yoake` (dawn, `04:30–08:30`)
- `day` (`08:30–17:00`)
- `higure` (dusk, `17:00–21:00`)

Any other override name (e.g. `"calm"`, `"bright"`) can be selected via `--override <name>`, cycling `O` in windowed mode, or sending `override <name>` over `$XDG_RUNTIME_DIR/modern-moyou-wind.sock`.

---

## Testing & Validating a Theme

```bash
# Headless validation & parameter dump
modern-moyou --inspect themes/aurora-sunset.json
modern-moyou --inspect themes/aurora-sunset.json --override night

# Run directly
modern-moyou --theme themes/aurora-sunset.json

# Hot-swap live while running in --layer-shell mode
printf 'theme %s' "$PWD/themes/aurora-sunset.json" | socat - UNIX-SENDTO:"$XDG_RUNTIME_DIR/modern-moyou-wind.sock"
```
