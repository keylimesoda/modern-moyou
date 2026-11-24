# Modern-XMB

A native **C++20 + Vulkan + SIMD (AVX2 / AVX-512) + SDL3 / Wayland `wlr-layer-shell`** recreation of the PlayStation 3 XMB (*XrossMediaBar*) dynamic silk ribbon, bokeh particle field, procedural seasonal sky, and 5-mip RGB Gaussian HDR glare pipeline.

Works **out of the box** with a built-in 12-month procedural theme (zero proprietary files required), and can also load your own legally dumped PS3 `.qrc` wave archives from `~/.local/share/modern-xmb/`.

## Features
- **True SPU-Faithful Simulation (`~0.09 ms/frame` CPU):**
  - `19×19` damped cloth/spring physics, `8×4×4 → 11×7×7` FFD lattice with `ffd_shader0..3` 200-frame crossfade, and vectorized `128×128` (`16,384`-vertex) bicubic B-spline tessellation.
  - `2,048`-particle bokeh & specular starburst pool with two-frame ribbon-surface cone & row-burst emitters, quaternion spin, and `32×16` signed-byte 2D flow-field wind impulses.
- **Full Vulkan HDR Pipeline:**
  - `64×32` diurnal/seasonal sky → additive mipmapped silk ribbon accumulation (`MIPMAP BIAS`) → composite + 2-pass additive bokeh/glint particles → 5-mip RGB Gaussian glare pyramid (`256×128..16×8`) → Extended Reinhard tonemap (`TONEBEFORE`) + animated dither.
- **Wayland `wlr-layer-shell` Underlay Mode (`--layer-shell`):**
  - Renders directly to a click-through compositor layer (ideal for Hyprland / Omarchy overlays and live wallpapers) with smooth fade-in/fade-out and live IPC preview.
- **Live System Clock & Smooth Time-of-Day Morphing:**
  - Tracks local month and time of day automatically, smoothly crossfading `night → yoake (dawn) → day → higure (dusk) → night` overrides.
- **Zero-Restart Live IPC Socket (`$XDG_RUNTIME_DIR/modern-xmb-wind.sock`):**
  - Non-blocking UNIX datagram socket for menu-driven particle wind gusts and live hover-previews of month, time of day, opacity, overrides, and `.qrc` presets.

## Install on Arch / Omarchy

### From Source
```bash
sudo pacman -S --needed cmake ninja vulkan-headers vulkan-icd-loader shaderc sdl3 wayland wayland-protocols pkgconf zlib
git clone https://github.com/keylimesoda/Modern-XMB.git
cd Modern-XMB
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```
*(Pass `-DXMB_PORTABLE=ON` to target `x86-64-v3` AVX2+FMA instead of `-march=native` AVX-512.)*

### Using `PKGBUILD`
```bash
makepkg -si
```

## Usage

### Run Out of the Box (Built-in Procedural Theme)
```bash
modern_xmb
```
Or as a translucent Wayland `wlr-layer-shell` background:
```bash
modern_xmb --layer-shell --opacity 0.96
```

### Using Your Own Legally Dumped PS3 `.qrc` Assets
This repository contains **no proprietary Sony files** (no `.qrc`, `.rco`, `.prx`, or `SCE-PS3-*.TTF` fonts).

If you own a PS3 and have legally dumped `lines.qrc` (or community `.qrc` waves), drop them into:
```bash
mkdir -p ~/.local/share/modern-xmb/qrc
cp /path/to/your/lines.qrc ~/.local/share/modern-xmb/qrc/
```
`modern_xmb` automatically discovers `.qrc` files in `~/.local/share/modern-xmb/` on startup, or you can pass an explicit file/directory:
```bash
modern_xmb --qrc /path/to/lines.qrc
modern_xmb --procedural          # force built-in procedural theme
```

### CLI Options
```text
modern_xmb [--layer-shell] [--qrc <path> | --procedural]
           [--month auto|<1-12>] [--month-day <1-30>]
           [--time auto|dawn|noon|dusk|night|<0-24>] [--day-time <0-2400>]
           [--override auto|base|<name>]
           [--opacity <0.05-1.0>]
```

### Windowed Controls
| Key | Action |
|---|---|
| `Left` / `Right` | Previous / next `.qrc` ribbon |
| `PgUp` / `PgDn` | Jump 10 ribbons |
| `Up` / `Down` | Month +/- |
| `,` / `.` | Time of day -/+ |
| `T` | Toggle 24-hour time-lapse |
| `C` | Re-sync to live system clock |
| `O` | Cycle override (`auto-ToD` → `base` → named overrides) |
| `D` | Toggle on-screen debug HUD |
| `I` | Print full parameters to terminal |
| `Space` / `R` / `F` / `Esc` | Pause / Reset / Fullscreen / Quit |

### Live IPC Socket (`$XDG_RUNTIME_DIR/modern-xmb-wind.sock`)
Send single-line datagrams while `modern_xmb` is running (works in both `--layer-shell` and windowed modes):
- **Particle wind:** `left`, `right`, `up`, `down`, `shake`, `wind <nx> <ny> <vx> <vy>`
- **Live preview:** `month auto|<1..12>`, `time auto|dawn|noon|dusk|night|<0..24>`, `opacity <0.05..1.0>`, `override auto|base|<name>`, `qrc <path>|procedural`

```bash
printf 'right'     | socat - UNIX-SENDTO:"$XDG_RUNTIME_DIR/modern-xmb-wind.sock"
printf 'time dusk' | socat - UNIX-SENDTO:"$XDG_RUNTIME_DIR/modern-xmb-wind.sock"
printf 'month 7'   | socat - UNIX-SENDTO:"$XDG_RUNTIME_DIR/modern-xmb-wind.sock"
```

## License
MIT — see [LICENSE](LICENSE). PlayStation and XMB are registered trademarks of Sony Interactive Entertainment Inc. This is an independent clean-room open-source graphics engine.
