#include "gfx/vk_renderer.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

namespace xmb::gfx {

namespace {

constexpr float kDegToRad = 0.01745329251994329577f;

// Exact std140 UBO structs matching shaders/decoded/*.{vert,frag}
struct alignas(16) BackColoursUBO {
    float uDayTime;
    float uMonthTime;
    float uNightTime;
    float uNightBrightness;
    float uAlpha;
    float uNightDayBlend;
    float _pad[2];
};

struct alignas(16) Lines1VertexUBO {
    float uFresnel;
    float uBrightness;
    float uMipmapBias;
    float _pad0;
};

struct alignas(16) Lines1FragUBO {
    float uSpacing;
    float uThinness;
    float uUseEncodeLUT;
    float uMipmapBias;
};

struct alignas(16) LinesCompositeUBO {
    float uColour1[4];
    float uColour2[4];
    float uColour3[4];
    float uColour4[4];
};

struct alignas(16) ParticlesVertexUBO {
    float uModelviewProjection[16];
    float uModelview[16];
    float uLightPack[16];
    float uParticleSize[4];
    float uTransparency[4];
    float uFocus[4];
    float uFocusCurves[4];
    float uFrontFacingQuaternion[4];
    float uNearControl[4];
    float uLifeBoundsMin[4];
    float uLifeBoundsMax[4];
    float uDarkness[4];
    float uGlare[4];
};

struct alignas(16) ParticlesFragUBO {
    float uLightPack[16];
    float uColor[4];
    float uNearControl[4];
    float uIridescentExp;
    float uGamma;
    float _pad0[2];
    float uGlare[4];
};

struct alignas(16) GlareSourceUBO {
    float uTexelOffset[2];
    float uThreshold;
    float uExposure;
};

struct alignas(16) GaussianVertUBO {
    float uModelviewProjection[16];
    float uOffset[2];
    float _pad0[2];
};

struct alignas(16) GaussianFragUBO {
    float uWeights[8][4];
};

struct alignas(16) GlareSumUBO {
    float uMipWeights0[4];
    float uMipWeights1[4];
};

struct alignas(16) ToneMapUBO {
    float uExposure;
    float uWhiteSqrRcp;
    float uGlareWeight;
    float uDitherScale;
    float uToneBefore;
    float uFrameSeed;
    float uSurfaceAlpha;
    float uHdrMode;
};

struct QuadVertex {
    float pos[4];
    float uv[2];
};

void set_err(std::string* out_error, std::string msg) {
    if (out_error) *out_error = std::move(msg);
}

void make_identity_mat4(float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

void mat4_mul_colmajor(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                             a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] +
                             a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
}

// Exact Sony Gaussian kernel formula (custom_render_plugin.prx 0x070cec, 0x071260)
void compute_sony_gaussian_weights(float sigma, float out_w[8]) {
    const float s = std::clamp(sigma, 0.46f, 2.9f);
    const int radius = std::clamp(static_cast<int>(std::ceil(s * 3.0f)) - 1, 1, 7);
    const float inv_2s2 = 0.5f / (s * s);
    float sum = 1.0f;
    std::memset(out_w, 0, sizeof(float) * 8);
    out_w[0] = 1.0f;
    for (int i = 1; i <= radius; ++i) {
        out_w[i] = std::exp(-static_cast<float>(i * i) * inv_2s2);
        sum += 2.0f * out_w[i];
    }
    const float inv_sum = 1.0f / sum;
    for (int i = 0; i <= radius; ++i) out_w[i] *= inv_sum;
}

} // namespace

VkRenderer::~VkRenderer() {
    shutdown();
}

bool VkRenderer::init(SDL_Window* window, std::string* out_error) {
    window_ = window;
    if (!create_instance(out_error)) return false;
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        set_err(out_error, std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
        return false;
    }
    return finish_init(out_error);
}

bool VkRenderer::init_wayland(void* wl_display, void* wl_surface, uint32_t width, uint32_t height, std::string* out_error) {
    window_ = nullptr;
    external_width_ = width;
    external_height_ = height;
    if (!create_instance_wayland(out_error)) return false;

    VkWaylandSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    sci.display = static_cast<struct wl_display*>(wl_display);
    sci.surface = static_cast<struct wl_surface*>(wl_surface);
    if (vkCreateWaylandSurfaceKHR(instance_, &sci, nullptr, &surface_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateWaylandSurfaceKHR failed");
        return false;
    }
    return finish_init(out_error);
}

bool VkRenderer::finish_init(std::string* out_error) {
    if (!select_physical_device(out_error)) return false;
    if (!create_device(out_error)) return false;
    if (!create_command_and_sync(out_error)) return false;
    if (!create_swapchain(out_error)) return false;
    if (!create_render_passes(out_error)) return false;
    if (!create_offscreen_targets(out_error)) return false;
    if (!create_default_luts(out_error)) return false;
    if (!create_uniform_buffers(out_error)) return false;
    if (!create_descriptors(out_error)) return false;
    if (!create_pipelines(out_error)) return false;
    if (!create_framebuffers(out_error)) return false;

    initialized_ = true;
    return true;
}

void VkRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    destroy_swapchain_resources();

    destroy_buffer(ribbon_uv_vb_);
    destroy_buffer(ribbon_ib_);
    destroy_buffer(fullscreen_quad_vb_);
    ribbon_uv_uploaded_bytes_ = 0;
    ribbon_ib_uploaded_bytes_ = 0;

    for (auto& f : frames_) {
        destroy_buffer(f.ribbon_vb);
        destroy_buffer(f.particle_vb);
        destroy_buffer(f.particle_ib);
        destroy_buffer(f.hud_vb);

        destroy_buffer(f.ubo_back_colours);
        destroy_buffer(f.ubo_lines1_vert);
        destroy_buffer(f.ubo_lines1_frag);
        destroy_buffer(f.ubo_lines_composite);
        destroy_buffer(f.ubo_particles_vert);
        destroy_buffer(f.ubo_particles_frag);
        for (uint32_t k = 0; k < kGlareMips; ++k) {
            destroy_buffer(f.ubo_glare_src[k]);
            destroy_buffer(f.ubo_gauss_vert_h[k]);
            destroy_buffer(f.ubo_gauss_vert_v[k]);
        }
        destroy_buffer(f.ubo_gauss_frag);
        destroy_buffer(f.ubo_glare_sum);
        destroy_buffer(f.ubo_tonemap);
    }

    destroy_image(day1_lut_);
    destroy_image(day2_lut_);
    destroy_image(night1_lut_);
    destroy_image(night2_lut_);
    destroy_image(stripes_lut_);
    destroy_image(fresnel_lut_);
    destroy_image(iridescent_lut_);
    destroy_image(encode_lut_);
    destroy_image(debug_font_lut_);

    if (device_ != VK_NULL_HANDLE) {
        auto destroy_pipe = [&](VkPipeline& p) {
            if (p != VK_NULL_HANDLE) { vkDestroyPipeline(device_, p, nullptr); p = VK_NULL_HANDLE; }
        };
        auto destroy_pl = [&](VkPipelineLayout& pl) {
            if (pl != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, pl, nullptr); pl = VK_NULL_HANDLE; }
        };
        auto destroy_dsl = [&](VkDescriptorSetLayout& d) {
            if (d != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, d, nullptr); d = VK_NULL_HANDLE; }
        };

        destroy_pipe(pipe_back_colours_);
        destroy_pipe(pipe_lines1_);
        destroy_pipe(pipe_lines_composite_);
        destroy_pipe(pipe_particles_);
        destroy_pipe(pipe_particles_second_);
        destroy_pipe(pipe_glare_src_);
        destroy_pipe(pipe_gaussian_);
        destroy_pipe(pipe_glare_sum_);
        destroy_pipe(pipe_tonemap_);
        destroy_pipe(pipe_hud_);

        destroy_pl(pl_back_colours_);
        destroy_pl(pl_lines1_);
        destroy_pl(pl_lines_composite_);
        destroy_pl(pl_particles_);
        destroy_pl(pl_glare_src_);
        destroy_pl(pl_gaussian_);
        destroy_pl(pl_glare_sum_);
        destroy_pl(pl_tonemap_);
        destroy_pl(pl_hud_);

        if (desc_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, desc_pool_, nullptr); desc_pool_ = VK_NULL_HANDLE; }
        destroy_dsl(dsl_back_colours_);
        destroy_dsl(dsl_lines1_);
        destroy_dsl(dsl_lines_composite_);
        destroy_dsl(dsl_particles_);
        destroy_dsl(dsl_glare_src_);
        destroy_dsl(dsl_gaussian_);
        destroy_dsl(dsl_glare_sum_);
        destroy_dsl(dsl_tonemap_);
        destroy_dsl(dsl_hud_);

        if (linear_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, linear_sampler_, nullptr); linear_sampler_ = VK_NULL_HANDLE; }
        if (nearest_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, nearest_sampler_, nullptr); nearest_sampler_ = VK_NULL_HANDLE; }
        if (hdr_render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, hdr_render_pass_, nullptr); hdr_render_pass_ = VK_NULL_HANDLE; }
        if (ribbon_render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, ribbon_render_pass_, nullptr); ribbon_render_pass_ = VK_NULL_HANDLE; }
        if (bloom_render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, bloom_render_pass_, nullptr); bloom_render_pass_ = VK_NULL_HANDLE; }
        if (present_render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, present_render_pass_, nullptr); present_render_pass_ = VK_NULL_HANDLE; }

        for (auto& f : frames_) {
            if (f.image_available != VK_NULL_HANDLE) { vkDestroySemaphore(device_, f.image_available, nullptr); f.image_available = VK_NULL_HANDLE; }
            if (f.in_flight != VK_NULL_HANDLE) { vkDestroyFence(device_, f.in_flight, nullptr); f.in_flight = VK_NULL_HANDLE; }
        }
        if (cmd_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; }

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

bool VkRenderer::create_instance(std::string* out_error) {
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
        set_err(out_error, std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
        return false;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Modern-XMB";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.pEngineName = "Modern-XMB";
    app_info.engineVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = sdl_ext_count;
    ci.ppEnabledExtensionNames = sdl_exts;

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateInstance failed");
        return false;
    }
    return true;
}

bool VkRenderer::create_instance_wayland(std::string* out_error) {
    const char* exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Modern-XMB";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.pEngineName = "Modern-XMB";
    app_info.engineVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateInstance (Wayland) failed");
        return false;
    }
    return true;
}

bool VkRenderer::select_physical_device(std::string* out_error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        set_err(out_error, "No Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkPhysicalDevice best_dev = VK_NULL_HANDLE;
    uint32_t best_family = 0;
    int best_score = -1;

    for (VkPhysicalDevice dev : devices) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops.data());

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 100;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 50;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 20;

        for (uint32_t i = 0; i < qcount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                if (score > best_score) {
                    best_score = score;
                    best_dev = dev;
                    best_family = i;
                }
                break;
            }
        }
    }

    if (best_dev != VK_NULL_HANDLE) {
        physical_device_ = best_dev;
        graphics_family_ = best_family;
        return true;
    }

    set_err(out_error, "No Vulkan physical device supports graphics + presentation");
    return false;
}

bool VkRenderer::create_device(std::string* out_error) {
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, graphics_family_, 1, &prio};
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;

    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &sci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateSampler failed");
        return false;
    }
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(device_, &sci, nullptr, &nearest_sampler_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateSampler (nearest) failed");
        return false;
    }
    return true;
}

bool VkRenderer::create_command_and_sync(std::string* out_error) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, graphics_family_};
    if (vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        set_err(out_error, "vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmd_pool_,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, kMaxFramesInFlight};
    VkCommandBuffer cmds[kMaxFramesInFlight]{};
    if (vkAllocateCommandBuffers(device_, &ai, cmds) != VK_SUCCESS) {
        set_err(out_error, "vkAllocateCommandBuffers failed");
        return false;
    }

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        frames_[i].cmd = cmds[i];
        if (vkCreateSemaphore(device_, &sci, nullptr, &frames_[i].image_available) != VK_SUCCESS ||
            vkCreateFence(device_, &fci, nullptr, &frames_[i].in_flight) != VK_SUCCESS) {
            set_err(out_error, "Failed to create Vulkan sync objects");
            return false;
        }
    }
    return true;
}

bool VkRenderer::create_swapchain(std::string* out_error) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    int w = 0, h = 0;
    if (window_) {
        SDL_GetWindowSizeInPixels(window_, &w, &h);
    } else {
        w = static_cast<int>(external_width_);
        h = static_cast<int>(external_height_);
    }
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    VkExtent2D extent{};
    if (caps.currentExtent.width != 0xFFFFFFFFu && caps.currentExtent.width > 0 && caps.currentExtent.height > 0) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(static_cast<uint32_t>(w), std::max(1u, caps.minImageExtent.width), std::max(1u, caps.maxImageExtent.width));
        extent.height = std::clamp(static_cast<uint32_t>(h), std::max(1u, caps.minImageExtent.height), std::max(1u, caps.maxImageExtent.height));
    }

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR comp_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!window_) {
        // Layer-shell underlay prefers premultiplied alpha so --opacity blends over the desktop
        for (VkCompositeAlphaFlagBitsKHR cand : {
                 VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                 VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
             }) {
            if (caps.supportedCompositeAlpha & cand) { comp_alpha = cand; break; }
        }
    } else {
        for (VkCompositeAlphaFlagBitsKHR cand : {
                 VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                 VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
             }) {
            if (caps.supportedCompositeAlpha & cand) { comp_alpha = cand; break; }
        }
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = img_count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = comp_alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;

    const VkResult sc_res = vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_);
    if (sc_res != VK_SUCCESS) {
        set_err(out_error, "vkCreateSwapchainKHR failed (VkResult=" + std::to_string(sc_res) + ")");
        return false;
    }

    swapchain_format_ = chosen.format;
    swapchain_extent_ = extent;

    uint32_t actual = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, nullptr);
    swapchain_images_.resize(actual);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, swapchain_images_.data());

    swapchain_views_.resize(actual);
    render_finished_semaphores_.resize(actual, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    for (uint32_t i = 0; i < actual; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
                                  swapchain_images_[i], VK_IMAGE_VIEW_TYPE_2D, swapchain_format_, {},
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(device_, &vci, nullptr, &swapchain_views_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sem_ci, nullptr, &render_finished_semaphores_[i]) != VK_SUCCESS) {
            set_err(out_error, "Failed to create swapchain image view or present semaphore");
            return false;
        }
    }
    return true;
}

bool VkRenderer::create_render_passes(std::string* out_error) {
    // 1. HDR offscreen render pass (FP16 color only, LOAD_OP_DONT_CARE since lines_composite overwrites full screen)
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &color_ref;

        VkSubpassDependency dep{0, VK_SUBPASS_EXTERNAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sp, 1, &dep};
        if (vkCreateRenderPass(device_, &rpci, nullptr, &hdr_render_pass_) != VK_SUCCESS) {
            set_err(out_error, "Failed to create HDR render pass");
            return false;
        }
    }

    // 2. Clear-to-zero FP16 render pass (ribbon_accum_ & sky_64x32_)
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkSubpassDependency dep{0, VK_SUBPASS_EXTERNAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sp, 1, &dep};
        if (vkCreateRenderPass(device_, &rpci, nullptr, &ribbon_render_pass_) != VK_SUCCESS) {
            set_err(out_error, "Failed to create ribbon/sky render pass");
            return false;
        }
    }

    // 3. Single-target FP16 glare pyramid / blur / sum pass
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkSubpassDependency dep{0, VK_SUBPASS_EXTERNAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sp, 1, &dep};
        if (vkCreateRenderPass(device_, &rpci, nullptr, &bloom_render_pass_) != VK_SUCCESS) {
            set_err(out_error, "Failed to create bloom render pass");
            return false;
        }
    }

    // 4. Tonemap -> swapchain present render pass
    {
        VkAttachmentDescription att{};
        att.format = swapchain_format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sp, 0, nullptr};
        if (vkCreateRenderPass(device_, &rpci, nullptr, &present_render_pass_) != VK_SUCCESS) {
            set_err(out_error, "Failed to create present render pass");
            return false;
        }
    }
    return true;
}

uint32_t VkRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return 0;
}

bool VkRenderer::create_image(
    uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
    VkImageAspectFlags aspect, GpuImage& out_img, std::string* out_error,
    uint32_t mip_levels) {

    const uint32_t mips = std::max(1u, mip_levels);
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ici, nullptr, &out_img.image) != VK_SUCCESS) {
        set_err(out_error, "vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, out_img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(device_, &mai, nullptr, &out_img.memory) != VK_SUCCESS) {
        set_err(out_error, "vkAllocateMemory failed for image");
        return false;
    }
    vkBindImageMemory(device_, out_img.image, out_img.memory, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
                              out_img.image, VK_IMAGE_VIEW_TYPE_2D, fmt, {}, {aspect, 0, mips, 0, 1}};
    if (vkCreateImageView(device_, &vci, nullptr, &out_img.view) != VK_SUCCESS) {
        set_err(out_error, "vkCreateImageView failed");
        return false;
    }

    out_img.format = fmt;
    out_img.width = w;
    out_img.height = h;
    return true;
}

void VkRenderer::destroy_image(GpuImage& img) {
    if (device_ == VK_NULL_HANDLE) return;
    if (img.view != VK_NULL_HANDLE) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image != VK_NULL_HANDLE) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory != VK_NULL_HANDLE) vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

bool VkRenderer::create_buffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
    bool persistently_map, GpuBuffer& out_buf, std::string* out_error) {

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    if (vkCreateBuffer(device_, &bci, nullptr, &out_buf.buffer) != VK_SUCCESS) {
        set_err(out_error, "vkCreateBuffer failed");
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out_buf.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             find_memory_type(req.memoryTypeBits, props)};
    if (vkAllocateMemory(device_, &mai, nullptr, &out_buf.memory) != VK_SUCCESS) {
        set_err(out_error, "vkAllocateMemory failed for buffer");
        return false;
    }
    vkBindBufferMemory(device_, out_buf.buffer, out_buf.memory, 0);
    out_buf.size = size;

    if (persistently_map) vkMapMemory(device_, out_buf.memory, 0, size, 0, &out_buf.mapped);
    return true;
}

void VkRenderer::destroy_buffer(GpuBuffer& buf) {
    if (device_ == VK_NULL_HANDLE) return;
    if (buf.mapped) vkUnmapMemory(device_, buf.memory);
    if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buf.buffer, nullptr);
    if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buf.memory, nullptr);
    buf = {};
}

bool VkRenderer::ensure_dynamic_buffer(
    GpuBuffer& buf, VkDeviceSize required, VkBufferUsageFlags usage, std::string* out_error) {
    if (required == 0 || buf.size >= required) return true;
    // Wait for GPU idle before destroying a buffer that the other in-flight frame may still read!
    if (buf.buffer != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    destroy_buffer(buf);
    return create_buffer(
        std::max<VkDeviceSize>(required * 2, 65536), usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, buf, out_error);
}

bool VkRenderer::create_offscreen_targets(std::string* out_error) {
    const uint32_t w = std::max(1u, swapchain_extent_.width);
    const uint32_t h = std::max(1u, swapchain_extent_.height);
    const float aspect = static_cast<float>(w) / static_cast<float>(h);

    // Exact Sony 5-mip glare pyramid base size (0x07f180): 256x128 in 16:9, 128x128 in 4:3
    const uint32_t glare_h = 128u;
    const uint32_t glare_w = (aspect > 1.5f) ? 256u : 128u;

    if (!create_image(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, hdr_color_, out_error) ||
        !create_image(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, ribbon_accum_, out_error) ||
        !create_image(64, 32, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, sky_64x32_, out_error) ||
        !create_image(glare_w, glare_h, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, glare_combined_, out_error)) {
        return false;
    }

    for (uint32_t k = 0; k < kGlareMips; ++k) {
        const uint32_t mw = std::max(1u, glare_w >> k);
        const uint32_t mh = std::max(1u, glare_h >> k);
        if (!create_image(mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, glare_mip_a_[k], out_error) ||
            !create_image(mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, glare_mip_b_[k], out_error)) {
            return false;
        }
    }
    return true;
}

bool VkRenderer::upload_rgba8_to_image(
    uint32_t w, uint32_t h, const uint8_t* pixels, GpuImage& dst,
    std::string* out_error, bool generate_mips) {
    uint32_t mip_levels = 1;
    if (generate_mips) {
        uint32_t d = std::max(w, h);
        while (d > 1u) { d >>= 1u; ++mip_levels; }
    }

    // Build CPU mip chain when requested (box-filtered 2x2 downsample, zero format-blit assumptions)
    struct MipData { uint32_t w, h; VkDeviceSize offset; std::vector<uint8_t> data; };
    std::vector<MipData> mips;
    mips.reserve(mip_levels);
    VkDeviceSize total_bytes = 0;
    {
        MipData m0{w, h, 0, std::vector<uint8_t>(pixels, pixels + static_cast<size_t>(w) * h * 4u)};
        total_bytes += m0.data.size();
        mips.push_back(std::move(m0));
        for (uint32_t lvl = 1; lvl < mip_levels; ++lvl) {
            const auto& prev = mips[lvl - 1];
            const uint32_t nw = std::max(1u, prev.w >> 1u);
            const uint32_t nh = std::max(1u, prev.h >> 1u);
            MipData next{nw, nh, total_bytes, std::vector<uint8_t>(static_cast<size_t>(nw) * nh * 4u)};
            for (uint32_t y = 0; y < nh; ++y) {
                const uint32_t y0 = std::min(prev.h - 1u, y * 2u);
                const uint32_t y1 = std::min(prev.h - 1u, y0 + 1u);
                for (uint32_t x = 0; x < nw; ++x) {
                    const uint32_t x0 = std::min(prev.w - 1u, x * 2u);
                    const uint32_t x1 = std::min(prev.w - 1u, x0 + 1u);
                    const size_t i00 = (static_cast<size_t>(y0) * prev.w + x0) * 4u;
                    const size_t i10 = (static_cast<size_t>(y0) * prev.w + x1) * 4u;
                    const size_t i01 = (static_cast<size_t>(y1) * prev.w + x0) * 4u;
                    const size_t i11 = (static_cast<size_t>(y1) * prev.w + x1) * 4u;
                    const size_t dst_i = (static_cast<size_t>(y) * nw + x) * 4u;
                    for (int ch = 0; ch < 4; ++ch) {
                        const uint32_t sum = prev.data[i00 + ch] + prev.data[i10 + ch] +
                                             prev.data[i01 + ch] + prev.data[i11 + ch];
                        next.data[dst_i + ch] = static_cast<uint8_t>((sum + 2u) >> 2u);
                    }
                }
            }
            total_bytes += next.data.size();
            mips.push_back(std::move(next));
        }
    }

    vkDeviceWaitIdle(device_);
    destroy_image(dst);
    if (!create_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, dst, out_error, mip_levels)) {
        return false;
    }

    GpuBuffer staging{};
    if (!create_buffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       true, staging, out_error)) {
        return false;
    }
    auto* mapped_bytes = static_cast<uint8_t*>(staging.mapped);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mip_levels);
    for (uint32_t lvl = 0; lvl < mip_levels; ++lvl) {
        std::memcpy(mapped_bytes + mips[lvl].offset, mips[lvl].data.data(), mips[lvl].data.size());
        VkBufferImageCopy r{};
        r.bufferOffset = mips[lvl].offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1};
        r.imageExtent = {mips[lvl].w, mips[lvl].h, 1};
        regions.push_back(r);
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmd_pool_,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = dst.image;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    vkCmdCopyBufferToImage(cmd, staging.buffer, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    VkImageMemoryBarrier b2 = b1;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    destroy_buffer(staging);
    return true;
}

bool VkRenderer::upload_dds_to_image(const assets::DdsImage& img, GpuImage& dst, std::string* out_error) {
    if (img.mips.empty()) {
        set_err(out_error, "DDS image has no mip levels");
        return false;
    }
    return upload_rgba8_to_image(img.mips[0].width, img.mips[0].height, img.mips[0].pixels.data(), dst, out_error);
}

bool VkRenderer::create_default_luts(std::string* out_error) {
    const uint8_t fallback[16] = {
        16, 48, 128, 255,  16, 48, 128, 255,
        8,  20, 64,  255,  8,  20, 64,  255,
    };
    const auto stripes = generate_stripes_lut();
    const auto fres = generate_fresnel_lut();
    const auto irid = generate_iridescent_lut();
    const auto enc = generate_encode_lut();
    const auto font = generate_debug_font_atlas();

    return upload_rgba8_to_image(2, 2, fallback, day1_lut_, out_error) &&
           upload_rgba8_to_image(2, 2, fallback, day2_lut_, out_error) &&
           upload_rgba8_to_image(2, 2, fallback, night1_lut_, out_error) &&
           upload_rgba8_to_image(2, 2, fallback, night2_lut_, out_error) &&
           upload_rgba8_to_image(stripes.width, stripes.height, stripes.pixels.data(), stripes_lut_, out_error, true) &&
           upload_rgba8_to_image(fres.width, fres.height, fres.pixels.data(), fresnel_lut_, out_error) &&
           upload_rgba8_to_image(irid.width, irid.height, irid.pixels.data(), iridescent_lut_, out_error) &&
           upload_rgba8_to_image(enc.width, enc.height, enc.pixels.data(), encode_lut_, out_error) &&
           upload_rgba8_to_image(font.width, font.height, font.pixels.data(), debug_font_lut_, out_error);
}

bool VkRenderer::upload_background_luts(
    const assets::DdsImage& day1, const assets::DdsImage& day2,
    const assets::DdsImage& night1, const assets::DdsImage& night2,
    std::string* out_error) {
    if (!upload_dds_to_image(day1, day1_lut_, out_error)) return false;
    if (!upload_dds_to_image(day2, day2_lut_, out_error)) return false;
    if (!upload_dds_to_image(night1, night1_lut_, out_error)) return false;
    if (!upload_dds_to_image(night2, night2_lut_, out_error)) return false;
    update_descriptor_sets();
    return true;
}

bool VkRenderer::upload_background_luts_rgba8(
    const Rgba8Image& day1, const Rgba8Image& day2,
    const Rgba8Image& night1, const Rgba8Image& night2,
    std::string* out_error) {
    if (!upload_rgba8_to_image(day1.width, day1.height, day1.pixels.data(), day1_lut_, out_error)) return false;
    if (!upload_rgba8_to_image(day2.width, day2.height, day2.pixels.data(), day2_lut_, out_error)) return false;
    if (!upload_rgba8_to_image(night1.width, night1.height, night1.pixels.data(), night1_lut_, out_error)) return false;
    if (!upload_rgba8_to_image(night2.width, night2.height, night2.pixels.data(), night2_lut_, out_error)) return false;
    update_descriptor_sets();
    return true;
}

bool VkRenderer::upload_fresnel_lut(const Rgba8Image& img, std::string* out_error) {
    if (!upload_rgba8_to_image(img.width, img.height, img.pixels.data(), fresnel_lut_, out_error)) return false;
    update_descriptor_sets();
    return true;
}

bool VkRenderer::upload_iridescent_lut(const Rgba8Image& img, std::string* out_error) {
    if (!upload_rgba8_to_image(img.width, img.height, img.pixels.data(), iridescent_lut_, out_error)) return false;
    update_descriptor_sets();
    return true;
}

bool VkRenderer::create_uniform_buffers(std::string* out_error) {
    auto mk_ubo = [&](VkDeviceSize sz, GpuBuffer& b) {
        return create_buffer(
            sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, b, out_error);
    };

    const QuadVertex quad[6] = {
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{ 1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{ 1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f,  1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    if (!create_buffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       true, fullscreen_quad_vb_, out_error)) {
        return false;
    }
    std::memcpy(fullscreen_quad_vb_.mapped, quad, sizeof(quad));

    for (auto& f : frames_) {
        if (!mk_ubo(sizeof(BackColoursUBO), f.ubo_back_colours) ||
            !mk_ubo(sizeof(Lines1VertexUBO), f.ubo_lines1_vert) ||
            !mk_ubo(sizeof(Lines1FragUBO), f.ubo_lines1_frag) ||
            !mk_ubo(sizeof(LinesCompositeUBO), f.ubo_lines_composite) ||
            !mk_ubo(sizeof(ParticlesVertexUBO), f.ubo_particles_vert) ||
            !mk_ubo(sizeof(ParticlesFragUBO), f.ubo_particles_frag) ||
            !mk_ubo(sizeof(GaussianFragUBO), f.ubo_gauss_frag) ||
            !mk_ubo(sizeof(GlareSumUBO), f.ubo_glare_sum) ||
            !mk_ubo(sizeof(ToneMapUBO), f.ubo_tonemap)) {
            return false;
        }
        for (uint32_t k = 0; k < kGlareMips; ++k) {
            if (!mk_ubo(sizeof(GlareSourceUBO), f.ubo_glare_src[k]) ||
                !mk_ubo(sizeof(GaussianVertUBO), f.ubo_gauss_vert_h[k]) ||
                !mk_ubo(sizeof(GaussianVertUBO), f.ubo_gauss_vert_v[k])) {
                return false;
            }
        }
    }
    return true;
}

bool VkRenderer::create_descriptors(std::string* out_error) {
    auto mk_dsl = [&](std::span<const VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout& out) {
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            static_cast<uint32_t>(bindings.size()), bindings.data(),
        };
        return vkCreateDescriptorSetLayout(device_, &ci, nullptr, &out) == VK_SUCCESS;
    };

    const VkDescriptorSetLayoutBinding b_bg[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_lines1[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_comp[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_particles[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_gsrc[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_gauss[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_gsum[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_tonemap[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding b_hud[1] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };

    if (!mk_dsl(b_bg, dsl_back_colours_) ||
        !mk_dsl(b_lines1, dsl_lines1_) ||
        !mk_dsl(b_comp, dsl_lines_composite_) ||
        !mk_dsl(b_particles, dsl_particles_) ||
        !mk_dsl(b_gsrc, dsl_glare_src_) ||
        !mk_dsl(b_gauss, dsl_gaussian_) ||
        !mk_dsl(b_gsum, dsl_glare_sum_) ||
        !mk_dsl(b_tonemap, dsl_tonemap_) ||
        !mk_dsl(b_hud, dsl_hud_)) {
        set_err(out_error, "Failed to create descriptor set layouts");
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 48 * kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 52 * kMaxFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 32 * kMaxFramesInFlight, 2, sizes};
    if (vkCreateDescriptorPool(device_, &pci, nullptr, &desc_pool_) != VK_SUCCESS) {
        set_err(out_error, "Failed to create descriptor pool");
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(1 + kMaxFramesInFlight * (6 + 3 * kGlareMips));
    layouts.push_back(dsl_hud_);
    for (uint32_t fi = 0; fi < kMaxFramesInFlight; ++fi) {
        layouts.push_back(dsl_back_colours_);
        layouts.push_back(dsl_lines1_);
        layouts.push_back(dsl_lines_composite_);
        layouts.push_back(dsl_particles_);
        layouts.push_back(dsl_glare_sum_);
        layouts.push_back(dsl_tonemap_);
        for (uint32_t k = 0; k < kGlareMips; ++k) layouts.push_back(dsl_glare_src_);
        for (uint32_t k = 0; k < kGlareMips; ++k) layouts.push_back(dsl_gaussian_);
        for (uint32_t k = 0; k < kGlareMips; ++k) layouts.push_back(dsl_gaussian_);
    }

    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, desc_pool_,
                                   static_cast<uint32_t>(layouts.size()), layouts.data()};
    if (vkAllocateDescriptorSets(device_, &ai, sets.data()) != VK_SUCCESS) {
        set_err(out_error, "Failed to allocate descriptor sets");
        return false;
    }

    size_t idx = 0;
    ds_hud_ = sets[idx++];
    for (auto& f : frames_) {
        f.ds_back_colours = sets[idx++];
        f.ds_lines1 = sets[idx++];
        f.ds_lines_composite = sets[idx++];
        f.ds_particles = sets[idx++];
        f.ds_glare_sum = sets[idx++];
        f.ds_tonemap = sets[idx++];
        for (uint32_t k = 0; k < kGlareMips; ++k) f.ds_glare_src[k] = sets[idx++];
        for (uint32_t k = 0; k < kGlareMips; ++k) f.ds_gaussian_h[k] = sets[idx++];
        for (uint32_t k = 0; k < kGlareMips; ++k) f.ds_gaussian_v[k] = sets[idx++];
    }

    update_descriptor_sets();
    return true;
}

void VkRenderer::update_descriptor_sets() {
    auto img_info = [&](VkImageView v) {
        return VkDescriptorImageInfo{linear_sampler_, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    auto buf_info = [&](const GpuBuffer& b) {
        return VkDescriptorBufferInfo{b.buffer, 0, b.size};
    };

    const VkDescriptorImageInfo ii_bg[4] = {
        img_info(day1_lut_.view), img_info(day2_lut_.view),
        img_info(night1_lut_.view), img_info(night2_lut_.view),
    };
    const VkDescriptorImageInfo ii_l1[3] = {
        img_info(stripes_lut_.view), img_info(fresnel_lut_.view), img_info(encode_lut_.view),
    };
    const VkDescriptorImageInfo ii_rib = img_info(ribbon_accum_.view);
    const VkDescriptorImageInfo ii_sky = img_info(sky_64x32_.view);
    const VkDescriptorImageInfo ii_p = img_info(iridescent_lut_.view);
    const VkDescriptorImageInfo ii_hdr = img_info(hdr_color_.view);
    const VkDescriptorImageInfo ii_gcomb = img_info(glare_combined_.view);
    const VkDescriptorImageInfo ii_font{nearest_sampler_, debug_font_lut_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkDescriptorImageInfo ii_ma[kGlareMips];
    VkDescriptorImageInfo ii_mb[kGlareMips];
    for (uint32_t k = 0; k < kGlareMips; ++k) {
        ii_ma[k] = img_info(glare_mip_a_[k].view);
        ii_mb[k] = img_info(glare_mip_b_[k].view);
    }

    struct PerFrameBufInfos {
        VkDescriptorBufferInfo bg, l1v, l1f, comp, pv, pf, gf, gsum, tm;
        VkDescriptorBufferInfo gsrc[kGlareMips], gvh[kGlareMips], gvv[kGlareMips];
    };
    PerFrameBufInfos fb[kMaxFramesInFlight]{};

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(1 + kMaxFramesInFlight * 50);
    auto push_ubo = [&](VkDescriptorSet ds, uint32_t binding, const VkDescriptorBufferInfo* info) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, binding, 0, 1,
                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, info, nullptr});
    };
    auto push_img = [&](VkDescriptorSet ds, uint32_t binding, const VkDescriptorImageInfo* info) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, binding, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info, nullptr, nullptr});
    };

    push_img(ds_hud_, 0, &ii_font);

    for (uint32_t fi = 0; fi < kMaxFramesInFlight; ++fi) {
        auto& f = frames_[fi];
        auto& b = fb[fi];
        b.bg = buf_info(f.ubo_back_colours);
        b.l1v = buf_info(f.ubo_lines1_vert);
        b.l1f = buf_info(f.ubo_lines1_frag);
        b.comp = buf_info(f.ubo_lines_composite);
        b.pv = buf_info(f.ubo_particles_vert);
        b.pf = buf_info(f.ubo_particles_frag);
        b.gf = buf_info(f.ubo_gauss_frag);
        b.gsum = buf_info(f.ubo_glare_sum);
        b.tm = buf_info(f.ubo_tonemap);
        for (uint32_t k = 0; k < kGlareMips; ++k) {
            b.gsrc[k] = buf_info(f.ubo_glare_src[k]);
            b.gvh[k] = buf_info(f.ubo_gauss_vert_h[k]);
            b.gvv[k] = buf_info(f.ubo_gauss_vert_v[k]);
        }

        push_ubo(f.ds_back_colours, 0, &b.bg);
        for (uint32_t i = 0; i < 4; ++i) push_img(f.ds_back_colours, 1 + i, &ii_bg[i]);

        push_ubo(f.ds_lines1, 0, &b.l1v);
        push_ubo(f.ds_lines1, 1, &b.l1f);
        for (uint32_t i = 0; i < 3; ++i) push_img(f.ds_lines1, 2 + i, &ii_l1[i]);

        push_ubo(f.ds_lines_composite, 0, &b.comp);
        push_img(f.ds_lines_composite, 1, &ii_rib);
        push_img(f.ds_lines_composite, 2, &ii_sky);

        push_ubo(f.ds_particles, 0, &b.pv);
        push_ubo(f.ds_particles, 1, &b.pf);
        push_img(f.ds_particles, 2, &ii_p);

        for (uint32_t k = 0; k < kGlareMips; ++k) {
            push_ubo(f.ds_glare_src[k], 0, &b.gsrc[k]);
            push_img(f.ds_glare_src[k], 1, (k == 0) ? &ii_hdr : &ii_ma[k - 1]);

            push_ubo(f.ds_gaussian_h[k], 0, &b.gvh[k]);
            push_ubo(f.ds_gaussian_h[k], 1, &b.gf);
            push_img(f.ds_gaussian_h[k], 2, &ii_ma[k]);

            push_ubo(f.ds_gaussian_v[k], 0, &b.gvv[k]);
            push_ubo(f.ds_gaussian_v[k], 1, &b.gf);
            push_img(f.ds_gaussian_v[k], 2, &ii_mb[k]);
        }

        push_ubo(f.ds_glare_sum, 0, &b.gsum);
        for (uint32_t k = 0; k < kGlareMips; ++k) push_img(f.ds_glare_sum, 1 + k, &ii_ma[k]);

        push_ubo(f.ds_tonemap, 0, &b.tm);
        push_img(f.ds_tonemap, 1, &ii_hdr);
        push_img(f.ds_tonemap, 2, &ii_gcomb);
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

VkShaderModule VkRenderer::load_shader_module(const char* filename, std::string* out_error) {
    std::vector<std::string> search_dirs;
    if (const char* env_dir = std::getenv("MODERN_XMB_SHADER_DIR"); env_dir && env_dir[0] != '\0') {
        search_dirs.emplace_back(env_dir);
    }
#ifdef XMB_INSTALL_SHADER_DIR
    search_dirs.emplace_back(XMB_INSTALL_SHADER_DIR);
#endif
    search_dirs.emplace_back(XMB_SHADER_DIR);

    std::ifstream ifs;
    std::string full;
    for (const auto& dir : search_dirs) {
        full = dir + "/" + filename;
        ifs.open(full, std::ios::binary | std::ios::ate);
        if (ifs) break;
    }
    if (!ifs) {
        set_err(out_error, std::string("Failed to open SPIR-V shader: ") + filename);
        return VK_NULL_HANDLE;
    }
    const auto sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<char> code(static_cast<size_t>(sz));
    if (!ifs.read(code.data(), sz)) {
        set_err(out_error, "Failed to read SPIR-V shader: " + full);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        code.size(), reinterpret_cast<const uint32_t*>(code.data()),
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS) {
        set_err(out_error, "vkCreateShaderModule failed for: " + full);
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool VkRenderer::create_pipelines(std::string* out_error) {
    auto mk_layout = [&](VkDescriptorSetLayout dsl, VkPipelineLayout& out_pl) {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 0, nullptr};
        return vkCreatePipelineLayout(device_, &ci, nullptr, &out_pl) == VK_SUCCESS;
    };

    if (!mk_layout(dsl_back_colours_, pl_back_colours_) ||
        !mk_layout(dsl_lines1_, pl_lines1_) ||
        !mk_layout(dsl_lines_composite_, pl_lines_composite_) ||
        !mk_layout(dsl_particles_, pl_particles_) ||
        !mk_layout(dsl_glare_src_, pl_glare_src_) ||
        !mk_layout(dsl_gaussian_, pl_gaussian_) ||
        !mk_layout(dsl_glare_sum_, pl_glare_sum_) ||
        !mk_layout(dsl_tonemap_, pl_tonemap_)) {
        set_err(out_error, "Failed to create pipeline layouts");
        return false;
    }

    const VkPushConstantRange hud_pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
    VkPipelineLayoutCreateInfo hud_pl_ci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl_hud_, 1, &hud_pc,
    };
    if (vkCreatePipelineLayout(device_, &hud_pl_ci, nullptr, &pl_hud_) != VK_SUCCESS) {
        set_err(out_error, "Failed to create HUD pipeline layout");
        return false;
    }

    // blend_mode: 0 = opaque, 1 = additive (ONE/ONE), 2 = alpha (SRC_ALPHA/ONE_MINUS_SRC_ALPHA)
    auto build_pipe = [&](
        const char* vs_file, const char* fs_file, VkRenderPass rp, VkPipelineLayout pl,
        std::span<const VkVertexInputBindingDescription> vbindings,
        std::span<const VkVertexInputAttributeDescription> vattrs,
        int blend_mode, VkPipeline& out_pipe) -> bool {

        VkShaderModule vs = load_shader_module(vs_file, out_error);
        VkShaderModule fs = load_shader_module(fs_file, out_error);
        if (!vs || !fs) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            if (fs) vkDestroyShaderModule(device_, fs, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
        };

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>(vbindings.size());
        vi.pVertexBindingDescriptions = vbindings.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(vattrs.size());
        vi.pVertexAttributeDescriptions = vattrs.data();

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend_mode == 1) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (blend_mode == 2) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyn_states};

        VkGraphicsPipelineCreateInfo gci{};
        gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gci.stageCount = 2;
        gci.pStages = stages;
        gci.pVertexInputState = &vi;
        gci.pInputAssemblyState = &ia;
        gci.pViewportState = &vp;
        gci.pRasterizationState = &rs;
        gci.pMultisampleState = &ms;
        gci.pDepthStencilState = &ds;
        gci.pColorBlendState = &cb;
        gci.pDynamicState = &dyn;
        gci.layout = pl;
        gci.renderPass = rp;

        const VkResult res = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gci, nullptr, &out_pipe);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (res != VK_SUCCESS) {
            set_err(out_error, std::string("vkCreateGraphicsPipelines failed for ") + fs_file);
            return false;
        }
        return true;
    };

    const VkVertexInputBindingDescription ribbon_b[2] = {
        {0, sizeof(sim::RibbonVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(sim::RibbonUV), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    const VkVertexInputAttributeDescription ribbon_a[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::RibbonVertex, pos)},
        {1, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(sim::RibbonUV, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::RibbonVertex, normal)},
    };
    const VkVertexInputBindingDescription part_b[1] = {
        {0, sizeof(sim::ParticleVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    const VkVertexInputAttributeDescription part_a[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::ParticleVertex, pos_alpha)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(sim::ParticleVertex, uv0)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::ParticleVertex, rot)},
    };
    const VkVertexInputBindingDescription quad_b[1] = {
        {0, sizeof(QuadVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    const VkVertexInputAttributeDescription quad_a[2] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadVertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)},
    };
    const VkVertexInputBindingDescription hud_b[1] = {
        {0, sizeof(HudVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    const VkVertexInputAttributeDescription hud_a[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HudVertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HudVertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HudVertex, color)},
    };

    return build_pipe("tonemap.vert.spv", "back_colours0.frag.spv", bloom_render_pass_, pl_back_colours_,
                      {}, {}, 0, pipe_back_colours_) &&
           build_pipe("lines1.vert.spv", "lines1.frag.spv", ribbon_render_pass_, pl_lines1_,
                      ribbon_b, ribbon_a, 1, pipe_lines1_) &&
           build_pipe("tonemap.vert.spv", "lines_composite.frag.spv", hdr_render_pass_, pl_lines_composite_,
                      {}, {}, 0, pipe_lines_composite_) &&
           build_pipe("particles_quads.vert.spv", "particles_quads.frag.spv", hdr_render_pass_, pl_particles_,
                      part_b, part_a, 1, pipe_particles_) &&
           build_pipe("particles_second.vert.spv", "particles_second.frag.spv", hdr_render_pass_, pl_particles_,
                      part_b, part_a, 1, pipe_particles_second_) &&
           build_pipe("tonemap.vert.spv", "glare_source_pre.frag.spv", bloom_render_pass_, pl_glare_src_,
                      {}, {}, 0, pipe_glare_src_) &&
           build_pipe("gaussian.vert.spv", "gaussian.frag.spv", bloom_render_pass_, pl_gaussian_,
                      quad_b, quad_a, 0, pipe_gaussian_) &&
           build_pipe("tonemap.vert.spv", "glare_sum.frag.spv", bloom_render_pass_, pl_glare_sum_,
                      {}, {}, 0, pipe_glare_sum_) &&
           build_pipe("tonemap.vert.spv", "tonemap.frag.spv", present_render_pass_, pl_tonemap_,
                      {}, {}, 0, pipe_tonemap_) &&
           build_pipe("hud.vert.spv", "hud.frag.spv", present_render_pass_, pl_hud_,
                      hud_b, hud_a, 2, pipe_hud_);
}

bool VkRenderer::create_framebuffers(std::string* out_error) {
    VkFramebufferCreateInfo hci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, hdr_render_pass_,
                                1, &hdr_color_.view, hdr_color_.width, hdr_color_.height, 1};
    VkFramebufferCreateInfo rci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, ribbon_render_pass_,
                                1, &ribbon_accum_.view, ribbon_accum_.width, ribbon_accum_.height, 1};
    VkFramebufferCreateInfo skyci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, bloom_render_pass_,
                                  1, &sky_64x32_.view, sky_64x32_.width, sky_64x32_.height, 1};
    VkFramebufferCreateInfo gcci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, bloom_render_pass_,
                                 1, &glare_combined_.view, glare_combined_.width, glare_combined_.height, 1};
    if (vkCreateFramebuffer(device_, &hci, nullptr, &hdr_fb_) != VK_SUCCESS ||
        vkCreateFramebuffer(device_, &rci, nullptr, &ribbon_fb_) != VK_SUCCESS ||
        vkCreateFramebuffer(device_, &skyci, nullptr, &sky_fb_) != VK_SUCCESS ||
        vkCreateFramebuffer(device_, &gcci, nullptr, &glare_combined_fb_) != VK_SUCCESS) {
        set_err(out_error, "Failed to create HDR/ribbon/sky/glare framebuffers");
        return false;
    }

    for (uint32_t k = 0; k < kGlareMips; ++k) {
        VkFramebufferCreateInfo aci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, bloom_render_pass_,
                                    1, &glare_mip_a_[k].view, glare_mip_a_[k].width, glare_mip_a_[k].height, 1};
        VkFramebufferCreateInfo bci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, bloom_render_pass_,
                                    1, &glare_mip_b_[k].view, glare_mip_b_[k].width, glare_mip_b_[k].height, 1};
        if (vkCreateFramebuffer(device_, &aci, nullptr, &glare_mip_a_fb_[k]) != VK_SUCCESS ||
            vkCreateFramebuffer(device_, &bci, nullptr, &glare_mip_b_fb_[k]) != VK_SUCCESS) {
            set_err(out_error, "Failed to create 5-mip glare framebuffers");
            return false;
        }
    }

    swapchain_framebuffers_.resize(swapchain_views_.size());
    for (size_t i = 0; i < swapchain_views_.size(); ++i) {
        VkFramebufferCreateInfo sci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, present_render_pass_,
                                    1, &swapchain_views_[i], swapchain_extent_.width, swapchain_extent_.height, 1};
        if (vkCreateFramebuffer(device_, &sci, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) {
            set_err(out_error, "Failed to create swapchain framebuffer");
            return false;
        }
    }
    return true;
}

void VkRenderer::destroy_swapchain_resources() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto fb : swapchain_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    swapchain_framebuffers_.clear();
    auto dfb = [&](VkFramebuffer& f) {
        if (f != VK_NULL_HANDLE) { vkDestroyFramebuffer(device_, f, nullptr); f = VK_NULL_HANDLE; }
    };
    dfb(hdr_fb_);
    dfb(ribbon_fb_);
    dfb(sky_fb_);
    dfb(glare_combined_fb_);
    for (uint32_t k = 0; k < kGlareMips; ++k) {
        dfb(glare_mip_a_fb_[k]);
        dfb(glare_mip_b_fb_[k]);
        destroy_image(glare_mip_a_[k]);
        destroy_image(glare_mip_b_[k]);
    }
    destroy_image(hdr_color_);
    destroy_image(ribbon_accum_);
    glare_initialized_ = false;
    destroy_image(sky_64x32_);
    destroy_image(glare_combined_);

    for (auto s : render_finished_semaphores_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(device_, s, nullptr);
    }
    render_finished_semaphores_.clear();
    for (auto v : swapchain_views_) vkDestroyImageView(device_, v, nullptr);
    swapchain_views_.clear();
    swapchain_images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

bool VkRenderer::recreate_swapchain(std::string* out_error) {
    int win_w = 0, win_h = 0;
    if (window_) {
        SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    } else {
        win_w = static_cast<int>(external_width_);
        win_h = static_cast<int>(external_height_);
    }
    if (win_w <= 0 || win_h <= 0) {
        return true; // Window minimized or mid-transition on Wayland; retry next frame
    }

    vkDeviceWaitIdle(device_);
    destroy_swapchain_resources();

    // Recreate per-frame image_available semaphores so a failed/aborted acquire during Wayland
    // fullscreen transitions never leaves a semaphore in a signaled state.
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (frames_[i].image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frames_[i].image_available, nullptr);
            frames_[i].image_available = VK_NULL_HANDLE;
        }
        if (vkCreateSemaphore(device_, &sci, nullptr, &frames_[i].image_available) != VK_SUCCESS) {
            set_err(out_error, "Failed to recreate frame acquire semaphore");
            return false;
        }
    }
    current_frame_ = 0;

    if (!create_swapchain(out_error)) return false;
    if (!create_offscreen_targets(out_error)) return false;
    if (!create_framebuffers(out_error)) return false;
    update_descriptor_sets();
    return true;
}

bool VkRenderer::render_frame(const FrameSceneData& scene, std::string* out_error) {
    if (!initialized_ || !scene.theme) return false;

    int win_w = 0, win_h = 0;
    if (window_) {
        SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    } else {
        win_w = static_cast<int>(external_width_);
        win_h = static_cast<int>(external_height_);
    }
    if (win_w <= 0 || win_h <= 0) {
        return true;
    }

    if (framebuffer_resized_ ||
        static_cast<uint32_t>(win_w) != swapchain_extent_.width ||
        static_cast<uint32_t>(win_h) != swapchain_extent_.height) {
        framebuffer_resized_ = false;
        if (!recreate_swapchain(out_error)) return false;
        return true;
    }

    FrameSync& frame = frames_[current_frame_];
    vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.image_available, VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        framebuffer_resized_ = false;
        return recreate_swapchain(out_error);
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        set_err(out_error, "vkAcquireNextImageKHR failed");
        return false;
    }

    const auto& theme = *scene.theme;
    const float aspect = static_cast<float>(swapchain_extent_.width) / std::max(1.0f, static_cast<float>(swapchain_extent_.height));
    ++frame_index_;

    // Upload static ribbon UVs & indices once (never change frame-to-frame)
    if (!scene.ribbon_uvs.empty() && ribbon_uv_uploaded_bytes_ != scene.ribbon_uvs.size_bytes()) {
        if (!ensure_dynamic_buffer(ribbon_uv_vb_, scene.ribbon_uvs.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out_error)) {
            return false;
        }
        std::memcpy(ribbon_uv_vb_.mapped, scene.ribbon_uvs.data(), scene.ribbon_uvs.size_bytes());
        ribbon_uv_uploaded_bytes_ = scene.ribbon_uvs.size_bytes();
    }
    if (!scene.ribbon_indices.empty() && ribbon_ib_uploaded_bytes_ != scene.ribbon_indices.size_bytes()) {
        if (!ensure_dynamic_buffer(ribbon_ib_, scene.ribbon_indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, out_error)) {
            return false;
        }
        std::memcpy(ribbon_ib_.mapped, scene.ribbon_indices.data(), scene.ribbon_indices.size_bytes());
        ribbon_ib_uploaded_bytes_ = scene.ribbon_indices.size_bytes();
    }

    // Upload per-frame dynamic geometry buffers (zero CPU/GPU race across frames in flight)
    if (!scene.ribbon_vertices.empty()) {
        if (!ensure_dynamic_buffer(frame.ribbon_vb, scene.ribbon_vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out_error)) {
            return false;
        }
        std::memcpy(frame.ribbon_vb.mapped, scene.ribbon_vertices.data(), scene.ribbon_vertices.size_bytes());
    }
    if (!scene.particle_vertices.empty()) {
        if (!ensure_dynamic_buffer(frame.particle_vb, scene.particle_vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out_error) ||
            !ensure_dynamic_buffer(frame.particle_ib, scene.particle_indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, out_error)) {
            return false;
        }
        std::memcpy(frame.particle_vb.mapped, scene.particle_vertices.data(), scene.particle_vertices.size_bytes());
        std::memcpy(frame.particle_ib.mapped, scene.particle_indices.data(), scene.particle_indices.size_bytes());
    }

    uint32_t hud_vert_count = 0;
    if (!scene.hud_lines.empty()) {
        const float scale = (swapchain_extent_.height >= 1440u) ? 3.0f : 2.0f;
        const float adv_x = 6.0f * scale;
        const float line_h = 10.0f * scale;
        const float glyph_w = 8.0f * scale;
        const float glyph_h = 8.0f * scale;
        const float pad_x = 10.0f * (scale * 0.5f);
        const float pad_y = 8.0f * (scale * 0.5f);
        const float origin_x = 16.0f;
        const float origin_y = 16.0f;

        size_t max_chars = 0;
        for (const auto& ln : scene.hud_lines) max_chars = std::max(max_chars, ln.size());

        hud_verts_scratch_.clear();
        hud_verts_scratch_.reserve(6 + max_chars * scene.hud_lines.size() * 6);

        auto push_quad = [&](float x0, float y0, float x1, float y1,
                             float u0, float v0, float u1, float v1,
                             float r, float g, float b, float a) {
            const HudVertex v00{{x0, y0}, {u0, v0}, {r, g, b, a}};
            const HudVertex v10{{x1, y0}, {u1, v0}, {r, g, b, a}};
            const HudVertex v11{{x1, y1}, {u1, v1}, {r, g, b, a}};
            const HudVertex v01{{x0, y1}, {u0, v1}, {r, g, b, a}};
            hud_verts_scratch_.push_back(v00); hud_verts_scratch_.push_back(v10); hud_verts_scratch_.push_back(v11);
            hud_verts_scratch_.push_back(v00); hud_verts_scratch_.push_back(v11); hud_verts_scratch_.push_back(v01);
        };

        // Translucent dark backdrop box (uv.x < 0 -> solid fill in hud.frag)
        const float box_w = static_cast<float>(max_chars) * adv_x + pad_x * 2.0f + 2.0f * scale;
        const float box_h = static_cast<float>(scene.hud_lines.size()) * line_h + pad_y * 2.0f;
        push_quad(origin_x, origin_y, origin_x + box_w, origin_y + box_h,
                  -1.0f, -1.0f, -1.0f, -1.0f, 0.02f, 0.03f, 0.05f, 0.70f);

        // Bitmap font glyphs (16x6 grid of 8x8 cells in 128x48 atlas, ASCII 32..127)
        constexpr float kInvCols = 1.0f / 16.0f;
        constexpr float kInvRows = 1.0f / 6.0f;
        for (size_t row = 0; row < scene.hud_lines.size(); ++row) {
            const auto& ln = scene.hud_lines[row];
            const float gy = origin_y + pad_y + static_cast<float>(row) * line_h;
            const float cr = (row == 0) ? 1.0f : 0.90f;
            const float cg = (row == 0) ? 1.0f : 0.94f;
            const float cb = (row == 0) ? 1.0f : 0.98f;
            for (size_t col = 0; col < ln.size(); ++col) {
                const unsigned char ch = static_cast<unsigned char>(ln[col]);
                if (ch <= 32 || ch > 127) continue;
                const uint32_t gidx = static_cast<uint32_t>(ch - 32);
                const float u0 = static_cast<float>(gidx % 16u) * kInvCols;
                const float v0 = static_cast<float>(gidx / 16u) * kInvRows;
                const float u1 = u0 + kInvCols;
                const float v1 = v0 + kInvRows;
                const float gx = origin_x + pad_x + static_cast<float>(col) * adv_x;
                push_quad(gx, gy, gx + glyph_w, gy + glyph_h, u0, v0, u1, v1, cr, cg, cb, 1.0f);
            }
        }

        if (!hud_verts_scratch_.empty()) {
            const VkDeviceSize bytes = hud_verts_scratch_.size() * sizeof(HudVertex);
            if (!ensure_dynamic_buffer(frame.hud_vb, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out_error)) {
                return false;
            }
            std::memcpy(frame.hud_vb.mapped, hud_verts_scratch_.data(), static_cast<size_t>(bytes));
            hud_vert_count = static_cast<uint32_t>(hud_verts_scratch_.size());
        }
    }

    // Update UBOs using exact Sony custom_render_plugin.prx formulas
    {
        auto smoothstep = [](float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-4f), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };

        // 1. Exact back_colours0 UBO (0x052ad8)
        const float t = std::clamp(scene.day_time / 2400.0f, 0.0f, 1.0f);
        const float dayspread = theme.background.dayspread;
        const float t_curved = (theme.background.colour_shader == 0)
            ? ((-2.0f * t * t * t + 3.0f * t * t) + (2.0f * t * t * t - 3.0f * t * t + t) * dayspread)
            : t;
        const float s_dawn = smoothstep(theme.background.night2day_begin / 24.0f, theme.background.night2day_end / 24.0f, t);
        const float s_dusk = smoothstep(theme.background.day2night_begin / 24.0f, theme.background.day2night_end / 24.0f, t);
        const float day_mask = s_dawn * (1.0f - s_dusk);

        BackColoursUBO bg{};
        bg.uDayTime = t_curved * 2400.0f;
        bg.uMonthTime = scene.month_time;
        bg.uNightTime = ((t - 0.5f < 0.0f) ? (t + 0.5f) : (t - 0.5f)) * 2400.0f;
        bg.uNightBrightness = theme.background.night_white_bias;
        bg.uAlpha = 1.0f;
        bg.uNightDayBlend = std::clamp(day_mask * (1.0f - theme.background.night_blend), 0.0f, 1.0f);
        std::memcpy(frame.ubo_back_colours.mapped, &bg, sizeof(bg));

        // 2. Exact lines1 uniforms (0x04c94c)
        Lines1VertexUBO l1v{theme.line1.fresnel, theme.line1.brightness, theme.line1.mipmap_bias, 0.0f};
        std::memcpy(frame.ubo_lines1_vert.mapped, &l1v, sizeof(l1v));
        Lines1FragUBO l1f{theme.line1.spacing, theme.line1.thinness, 0.0f, theme.line1.mipmap_bias};
        std::memcpy(frame.ubo_lines1_frag.mapped, &l1f, sizeof(l1f));

        // 3. Exact LinesController gradient colors (0x05829c)
        LinesCompositeUBO comp{};
        comp.uColour1[0] = theme.background.color2[0]; comp.uColour1[1] = theme.background.color2[1]; comp.uColour1[2] = theme.background.color2[2]; comp.uColour1[3] = 1.0f;
        comp.uColour2[0] = theme.background.color1[0]; comp.uColour2[1] = theme.background.color1[1]; comp.uColour2[2] = theme.background.color1[2]; comp.uColour2[3] = 1.0f;
        comp.uColour3[0] = theme.background.color3[0]; comp.uColour3[1] = theme.background.color3[1]; comp.uColour3[2] = theme.background.color3[2]; comp.uColour3[3] = 1.0f;
        comp.uColour4[0] = theme.background.color4[0]; comp.uColour4[1] = theme.background.color4[1]; comp.uColour4[2] = theme.background.color4[2]; comp.uColour4[3] = 1.0f;
        std::memcpy(frame.ubo_lines_composite.mapped, &comp, sizeof(comp));

        // 4. Exact particles_quads & particles_second uniforms (0x05c254, 0x5de2c)
        float cam_v[16], cam_p[16];
        sim::RibbonSim::build_camera_view(cam_v);
        sim::RibbonSim::build_camera_proj(aspect, theme.background.fovy, cam_p);
        const float eff_fovy = (theme.background.fovy > 1.0f) ? theme.background.fovy : 53.0f;
        const float fovy_rad = std::clamp(eff_fovy, 20.0f, 120.0f) * kDegToRad;

        ParticlesVertexUBO pv{};
        mat4_mul_colmajor(cam_p, cam_v, pv.uModelviewProjection);
        std::memcpy(pv.uModelview, cam_v, sizeof(cam_v));
        pv.uLightPack[0] = 0.0f; pv.uLightPack[4] = 0.0f; pv.uLightPack[8] = 2.0f;
        pv.uLightPack[1] = theme.particles.spot_pos[0]; pv.uLightPack[5] = theme.particles.spot_pos[1]; pv.uLightPack[9] = theme.particles.spot_pos[2];
        pv.uLightPack[2] = theme.particles.spot_attn[0]; pv.uLightPack[6] = theme.particles.spot_attn[1]; pv.uLightPack[10] = theme.particles.spot_attn[2];
        pv.uLightPack[3] = theme.particles.lambert_coeff;
        pv.uLightPack[7] = theme.particles.specular_coeff;
        pv.uLightPack[11] = theme.particles.exposure;
        pv.uLightPack[13] = theme.particles.specular_power;
        pv.uLightPack[15] = theme.particles.color_control;
        pv.uParticleSize[0] = theme.particles.size_middle;
        pv.uParticleSize[1] = theme.particles.size_near;
        pv.uParticleSize[2] = theme.particles.size_far;
        pv.uTransparency[0] = theme.particles.fresnel;
        pv.uTransparency[1] = theme.particles.global_alpha;
        pv.uFocus[0] = theme.particles.near_focus;
        pv.uFocus[1] = theme.particles.near_focus + theme.particles.near_focus_dist;
        pv.uFocus[2] = theme.particles.far_focus;
        pv.uFocus[3] = theme.particles.far_focus + theme.particles.far_focus_dist;
        pv.uFocusCurves[0] = theme.particles.near_focus_pow;
        pv.uFocusCurves[1] = theme.particles.far_focus_pow;
        pv.uFocusCurves[2] = fovy_rad; // exact PRX 0x5de2c
        pv.uFrontFacingQuaternion[3] = 1.0f;
        pv.uNearControl[0] = theme.particles.size_align;
        pv.uNearControl[1] = theme.particles.near_fuzziness;
        pv.uNearControl[2] = theme.particles.near_align;
        pv.uLifeBoundsMin[0] = -10.0f; pv.uLifeBoundsMin[1] = -10.0f; pv.uLifeBoundsMin[2] = -12.0f;
        pv.uLifeBoundsMax[0] =  10.0f; pv.uLifeBoundsMax[1] =  10.0f; pv.uLifeBoundsMax[2] =   7.0f;
        pv.uDarkness[0] = theme.particles.near_darkness;
        pv.uDarkness[1] = theme.particles.far_darkness;
        pv.uGlare[0] = theme.particles.glare;
        pv.uGlare[1] = theme.particles.glare_scale;
        pv.uGlare[2] = theme.particles.glare_p1;
        pv.uGlare[3] = theme.particles.glare_p2;
        std::memcpy(frame.ubo_particles_vert.mapped, &pv, sizeof(pv));

        ParticlesFragUBO pf{};
        std::memcpy(pf.uLightPack, pv.uLightPack, sizeof(pf.uLightPack));
        pf.uColor[0] = pf.uColor[1] = pf.uColor[2] = pf.uColor[3] = 1.0f;
        pf.uNearControl[1] = theme.particles.near_fuzziness;
        pf.uIridescentExp = theme.particles.iridescent_exp;
        pf.uGamma = 1.0f;
        std::memcpy(pf.uGlare, pv.uGlare, sizeof(pf.uGlare));
        std::memcpy(frame.ubo_particles_frag.mapped, &pf, sizeof(pf));

        // 5. Exact Sony 5-mip RGB Gaussian Glare Pyramid (0x079818..0x07c680)
        float wr[8], wg[8], wb[8];
        compute_sony_gaussian_weights(theme.hdr.gaussian_rad[0], wr);
        compute_sony_gaussian_weights(theme.hdr.gaussian_rad[1], wg);
        compute_sony_gaussian_weights(theme.hdr.gaussian_rad[2], wb);

        GaussianFragUBO gf{};
        for (int i = 0; i < 8; ++i) {
            gf.uWeights[i][0] = wr[i];
            gf.uWeights[i][1] = wg[i];
            gf.uWeights[i][2] = wb[i];
            gf.uWeights[i][3] = 1.0f;
        }
        std::memcpy(frame.ubo_gauss_frag.mapped, &gf, sizeof(gf));

        for (uint32_t k = 0; k < kGlareMips; ++k) {
            GlareSourceUBO gs{};
            const uint32_t src_w = (k == 0) ? hdr_color_.width : glare_mip_a_[k - 1].width;
            const uint32_t src_h = (k == 0) ? hdr_color_.height : glare_mip_a_[k - 1].height;
            gs.uTexelOffset[0] = 0.5f / static_cast<float>(src_w);
            gs.uTexelOffset[1] = 0.5f / static_cast<float>(src_h);
            gs.uThreshold = (k == 0) ? theme.hdr.glare_thresh : 0.0f;
            gs.uExposure = (k == 0) ? theme.hdr.exposure : 1.0f;
            std::memcpy(frame.ubo_glare_src[k].mapped, &gs, sizeof(gs));

            GaussianVertUBO gvh{}, gvv{};
            make_identity_mat4(gvh.uModelviewProjection);
            make_identity_mat4(gvv.uModelviewProjection);
            gvh.uOffset[0] = 1.0f / static_cast<float>(glare_mip_a_[k].width);
            gvv.uOffset[1] = 1.0f / static_cast<float>(glare_mip_a_[k].height);
            std::memcpy(frame.ubo_gauss_vert_h[k].mapped, &gvh, sizeof(gvh));
            std::memcpy(frame.ubo_gauss_vert_v[k].mapped, &gvv, sizeof(gvv));
        }

        // Exact Sony mip accumulation weights: w[k] = (pow(GLARE_SUM_POW, k) / sum) * GLARE_LEVEL
        float mw[kGlareMips]{};
        float wsum = 0.0f;
        const float sum_pow = std::clamp(theme.hdr.glare_sum_pow, 0.05f, 2.0f);
        for (uint32_t k = 0; k < kGlareMips; ++k) {
            mw[k] = std::pow(sum_pow, static_cast<float>(k));
            wsum += mw[k];
        }
        const float inv_wsum = (wsum > 1e-6f) ? (theme.hdr.glare_level / wsum) : 0.0f;
        GlareSumUBO gsum{};
        gsum.uMipWeights0[0] = mw[0] * inv_wsum;
        gsum.uMipWeights0[1] = mw[1] * inv_wsum;
        gsum.uMipWeights0[2] = mw[2] * inv_wsum;
        gsum.uMipWeights0[3] = mw[3] * inv_wsum;
        gsum.uMipWeights1[0] = mw[4] * inv_wsum;
        std::memcpy(frame.ubo_glare_sum.mapped, &gsum, sizeof(gsum));

        // 6. Exact Extended Reinhard Tonemap + TONEBEFORE + animated dither (0x077398)
        ToneMapUBO tm{};
        tm.uExposure = theme.hdr.exposure;
        const float wl = std::max(theme.hdr.white_level, 0.25f);
        tm.uWhiteSqrRcp = 1.0f / (wl * wl);
        tm.uGlareWeight = theme.hdr.glare ? 0.5f : 0.0f;
        tm.uDitherScale = 0.125f / 255.0f;
        tm.uToneBefore = (theme.hdr.tonebefore != 0) ? 1.0f : 0.0f;
        tm.uFrameSeed = static_cast<float>(frame_index_ & 1023u);
        tm.uSurfaceAlpha = std::clamp(scene.surface_alpha, 0.0f, 1.0f);
        tm.uHdrMode = (theme.hdr.glare_only != 0) ? 2.0f : ((theme.hdr.enabled == 0) ? 1.0f : 0.0f);
        std::memcpy(frame.ubo_tonemap.mapped, &tm, sizeof(tm));
    }

    vkResetFences(device_, 1, &frame.in_flight);
    vkResetCommandBuffer(frame.cmd, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr};
    vkBeginCommandBuffer(frame.cmd, &bi);

    auto set_vp_sc = [&](uint32_t w, uint32_t h) {
        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {w, h}};
        vkCmdSetViewport(frame.cmd, 0, 1, &vp);
        vkCmdSetScissor(frame.cmd, 0, 1, &sc);
    };

    const VkClearValue clear_zero = {{{0.0f, 0.0f, 0.0f, 0.0f}}};

    // Pass 1a: Procedural 64x32 sky (back_colours0) -> sky_64x32_ (LOAD_OP_DONT_CARE)
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, bloom_render_pass_, sky_fb_,
                                   {{0, 0}, {sky_64x32_.width, sky_64x32_.height}}, 0, nullptr};
        vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp_sc(sky_64x32_.width, sky_64x32_.height);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_back_colours_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_back_colours_, 0, 1, &frame.ds_back_colours, 0, nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.cmd);
    }

    // Pass 1b: Additive ribbon accumulation (lines1) -> ribbon_accum_
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, ribbon_render_pass_, ribbon_fb_,
                                   {{0, 0}, {ribbon_accum_.width, ribbon_accum_.height}}, 1, &clear_zero};
        vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp_sc(ribbon_accum_.width, ribbon_accum_.height);
        if (!scene.ribbon_indices.empty()) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_lines1_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_lines1_, 0, 1, &frame.ds_lines1, 0, nullptr);
            VkBuffer vbs[2] = {frame.ribbon_vb.buffer, ribbon_uv_vb_.buffer};
            VkDeviceSize offs[2] = {0, 0};
            vkCmdBindVertexBuffers(frame.cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(frame.cmd, ribbon_ib_.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.cmd, static_cast<uint32_t>(scene.ribbon_indices.size()), 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(frame.cmd);
    }

    // Pass 2: LinesController composite + additive particles (particles_quads + optional particles_second) -> hdr_color_
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, hdr_render_pass_, hdr_fb_,
                                   {{0, 0}, {hdr_color_.width, hdr_color_.height}}, 0, nullptr};
        vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp_sc(hdr_color_.width, hdr_color_.height);

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_lines_composite_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_lines_composite_, 0, 1, &frame.ds_lines_composite, 0, nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);

        if (!scene.particle_indices.empty()) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(frame.cmd, 0, 1, &frame.particle_vb.buffer, &off);
            vkCmdBindIndexBuffer(frame.cmd, frame.particle_ib.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_particles_, 0, 1, &frame.ds_particles, 0, nullptr);

            // Pass 2a: Bokeh discs (particles_quads)
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_particles_);
            vkCmdDrawIndexed(frame.cmd, static_cast<uint32_t>(scene.particle_indices.size()), 1, 0, 0, 0);

            // Pass 2b: Specular starburst glints (particles_second) — skip when glare == 0
            if (theme.particles.glare > 1e-5f) {
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_particles_second_);
                vkCmdDrawIndexed(frame.cmd, static_cast<uint32_t>(scene.particle_indices.size()), 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(frame.cmd);
    }

    // Pass 3: Exact Sony 5-Mip RGB Gaussian Glare Pyramid (256x128..16x8) — skip when glare disabled
    const bool run_glare = !glare_initialized_ || (theme.hdr.glare != 0 && theme.hdr.glare_level > 1e-5f);
    if (run_glare) {
        glare_initialized_ = true;
        for (uint32_t k = 0; k < kGlareMips; ++k) {
            const uint32_t mw = glare_mip_a_[k].width;
            const uint32_t mh = glare_mip_a_[k].height;
            // Downsample (+ threshold on mip 0) -> glare_mip_a_[k]
            {
                VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, bloom_render_pass_,
                                           glare_mip_a_fb_[k], {{0, 0}, {mw, mh}}, 0, nullptr};
                vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                set_vp_sc(mw, mh);
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_glare_src_);
                vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_glare_src_, 0, 1, &frame.ds_glare_src[k], 0, nullptr);
                vkCmdDraw(frame.cmd, 3, 1, 0, 0);
                vkCmdEndRenderPass(frame.cmd);
            }
            // Separable RGB Gaussian H: glare_mip_a_[k] -> glare_mip_b_[k]
            {
                VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, bloom_render_pass_,
                                           glare_mip_b_fb_[k], {{0, 0}, {mw, mh}}, 0, nullptr};
                vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                set_vp_sc(mw, mh);
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_gaussian_);
                vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gaussian_, 0, 1, &frame.ds_gaussian_h[k], 0, nullptr);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(frame.cmd, 0, 1, &fullscreen_quad_vb_.buffer, &off);
                vkCmdDraw(frame.cmd, 6, 1, 0, 0);
                vkCmdEndRenderPass(frame.cmd);
            }
            // Separable RGB Gaussian V: glare_mip_b_[k] -> glare_mip_a_[k]
            {
                VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, bloom_render_pass_,
                                           glare_mip_a_fb_[k], {{0, 0}, {mw, mh}}, 0, nullptr};
                vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                set_vp_sc(mw, mh);
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_gaussian_);
                vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gaussian_, 0, 1, &frame.ds_gaussian_v[k], 0, nullptr);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(frame.cmd, 0, 1, &fullscreen_quad_vb_.buffer, &off);
                vkCmdDraw(frame.cmd, 6, 1, 0, 0);
                vkCmdEndRenderPass(frame.cmd);
            }
        }

        // Pass 3b: Weighted sum of all 5 glare mips -> glare_combined_
        {
            VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, bloom_render_pass_,
                                       glare_combined_fb_, {{0, 0}, {glare_combined_.width, glare_combined_.height}}, 0, nullptr};
            vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            set_vp_sc(glare_combined_.width, glare_combined_.height);
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_glare_sum_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_glare_sum_, 0, 1, &frame.ds_glare_sum, 0, nullptr);
            vkCmdDraw(frame.cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(frame.cmd);
        }
    }

    // Pass 4: Extended Reinhard Tonemap + 5-Mip Glare + Animated Dither + Optional Debug HUD -> Swapchain
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, present_render_pass_,
                                   swapchain_framebuffers_[img_idx], {{0, 0}, swapchain_extent_}, 0, nullptr};
        vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp_sc(swapchain_extent_.width, swapchain_extent_.height);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_tonemap_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_tonemap_, 0, 1, &frame.ds_tonemap, 0, nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);

        if (hud_vert_count > 0) {
            const float inv_screen[2] = {
                1.0f / static_cast<float>(std::max(1u, swapchain_extent_.width)),
                1.0f / static_cast<float>(std::max(1u, swapchain_extent_.height)),
            };
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_hud_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_hud_, 0, 1, &ds_hud_, 0, nullptr);
            vkCmdPushConstants(frame.cmd, pl_hud_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(inv_screen), inv_screen);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(frame.cmd, 0, 1, &frame.hud_vb.buffer, &off);
            vkCmdDraw(frame.cmd, hud_vert_count, 1, 0, 0);
        }

        vkCmdEndRenderPass(frame.cmd);
    }

    vkEndCommandBuffer(frame.cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_done = render_finished_semaphores_[img_idx];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &frame.image_available, &wait_stage,
                    1, &frame.cmd, 1, &render_done};
    if (vkQueueSubmit(graphics_queue_, 1, &si, frame.in_flight) != VK_SUCCESS) {
        set_err(out_error, "vkQueueSubmit failed");
        return false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &render_done,
                        1, &swapchain_, &img_idx, nullptr};
    VkResult pres = vkQueuePresentKHR(graphics_queue_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || framebuffer_resized_) {
        framebuffer_resized_ = false;
        return recreate_swapchain(out_error);
    }

    current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
    return true;
}

} // namespace xmb::gfx
