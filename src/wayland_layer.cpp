#include "wayland_layer.hpp"

#include <wayland-client.h>
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <cstring>
#include <poll.h>

namespace xmb::platform {

struct WaylandLayerSurface::Impl {
    WaylandLayerSurface* owner = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;

    static void on_registry_global(void* data, wl_registry* reg, uint32_t name,
                                   const char* iface, uint32_t version) {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
            self->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4));
        } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
            self->layer_shell = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4));
        }
    }

    static void on_registry_remove(void*, wl_registry*, uint32_t) {}

    static void on_layer_configure(void* data, zwlr_layer_surface_v1* lsurf,
                                   uint32_t serial, uint32_t w, uint32_t h) {
        auto* self = static_cast<Impl*>(data);
        zwlr_layer_surface_v1_ack_configure(lsurf, serial);
        if (w > 0 && h > 0) {
            if (self->owner->width_ != w || self->owner->height_ != h) {
                self->owner->width_ = w;
                self->owner->height_ = h;
                if (self->owner->configured_) {
                    self->owner->resized_ = true;
                }
            }
        }
        self->owner->configured_ = true;
    }

    static void on_layer_closed(void* data, zwlr_layer_surface_v1*) {
        auto* self = static_cast<Impl*>(data);
        self->owner->closed_ = true;
    }

    static constexpr wl_registry_listener kRegistryListener = {
        on_registry_global,
        on_registry_remove,
    };

    static constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
        on_layer_configure,
        on_layer_closed,
    };
};

WaylandLayerSurface::~WaylandLayerSurface() {
    shutdown();
}

bool WaylandLayerSurface::init(const char* namespace_name, std::string* out_error) {
    impl_ = new Impl();
    impl_->owner = this;

    display_ = wl_display_connect(nullptr);
    if (!display_) {
        if (out_error) *out_error = "wl_display_connect failed";
        shutdown();
        return false;
    }

    impl_->registry = wl_display_get_registry(display_);
    wl_registry_add_listener(impl_->registry, &Impl::kRegistryListener, impl_);
    wl_display_roundtrip(display_);

    if (!impl_->compositor || !impl_->layer_shell) {
        if (out_error) *out_error = "Wayland compositor missing wl_compositor or zwlr_layer_shell_v1";
        shutdown();
        return false;
    }

    surface_ = wl_compositor_create_surface(impl_->compositor);
    if (!surface_) {
        if (out_error) *out_error = "wl_compositor_create_surface failed";
        shutdown();
        return false;
    }

    // Empty input region -> 100% click-through so omarchy-menu above receives all input
    wl_region* empty_region = wl_compositor_create_region(impl_->compositor);
    wl_surface_set_input_region(surface_, empty_region);
    wl_region_destroy(empty_region);

    // LAYER_TOP sits strictly below omarchy-menu's WlrLayer.Overlay and above normal windows
    impl_->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        impl_->layer_shell,
        surface_,
        nullptr, // currently focused monitor
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        namespace_name ? namespace_name : "omarchy-xmb-bg");

    if (!impl_->layer_surface) {
        if (out_error) *out_error = "zwlr_layer_shell_v1_get_layer_surface failed";
        shutdown();
        return false;
    }

    zwlr_layer_surface_v1_set_anchor(
        impl_->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(impl_->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        impl_->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_set_size(impl_->layer_surface, 0, 0);
    zwlr_layer_surface_v1_add_listener(impl_->layer_surface, &Impl::kLayerSurfaceListener, impl_);

    wl_surface_commit(surface_);

    // Wait for initial configure with actual monitor dimensions
    while (!configured_ && !closed_) {
        if (wl_display_dispatch(display_) < 0) {
            if (out_error) *out_error = "wl_display_dispatch failed waiting for configure";
            shutdown();
            return false;
        }
    }

    if (closed_) {
        if (out_error) *out_error = "Layer surface closed before configure";
        shutdown();
        return false;
    }

    return true;
}

bool WaylandLayerSurface::poll_events() {
    if (!display_ || closed_) return false;
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);

    pollfd pfd{};
    pfd.fd = wl_display_get_fd(display_);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_dispatch(display_) < 0) {
            closed_ = true;
            return false;
        }
    }
    return !closed_;
}

void WaylandLayerSurface::shutdown() {
    if (impl_) {
        if (impl_->layer_surface) {
            zwlr_layer_surface_v1_destroy(impl_->layer_surface);
            impl_->layer_surface = nullptr;
        }
        if (surface_) {
            wl_surface_destroy(surface_);
            surface_ = nullptr;
        }
        if (impl_->layer_shell) {
            zwlr_layer_shell_v1_destroy(impl_->layer_shell);
            impl_->layer_shell = nullptr;
        }
        if (impl_->compositor) {
            wl_compositor_destroy(impl_->compositor);
            impl_->compositor = nullptr;
        }
        if (impl_->registry) {
            wl_registry_destroy(impl_->registry);
            impl_->registry = nullptr;
        }
        delete impl_;
        impl_ = nullptr;
    }
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
    configured_ = false;
    closed_ = false;
    resized_ = false;
}

} // namespace xmb::platform
