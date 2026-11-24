#pragma once

#include <cstdint>
#include <string>

struct wl_display;
struct wl_surface;

namespace xmb::platform {

class WaylandLayerSurface {
public:
    WaylandLayerSurface() = default;
    ~WaylandLayerSurface();

    WaylandLayerSurface(const WaylandLayerSurface&) = delete;
    WaylandLayerSurface& operator=(const WaylandLayerSurface&) = delete;

    // Creates a fullscreen, click-through wlr-layer-shell surface on LAYER_TOP
    // (strictly below WlrLayer.Overlay menus and above all normal windows).
    bool init(const char* namespace_name, std::string* out_error = nullptr);
    void shutdown();

    // Non-blocking Wayland event dispatch. Returns false if compositor closed the surface.
    bool poll_events();

    wl_display* display() const { return display_; }
    wl_surface* surface() const { return surface_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool consume_resized() {
        const bool r = resized_;
        resized_ = false;
        return r;
    }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    wl_display* display_ = nullptr;
    wl_surface* surface_ = nullptr;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    bool configured_ = false;
    bool closed_ = false;
    bool resized_ = false;
};

} // namespace xmb::platform
