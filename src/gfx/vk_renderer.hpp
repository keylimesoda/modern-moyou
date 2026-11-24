#pragma once

#include "assets/dds_loader.hpp"
#include "assets/mnu_parser.hpp"
#include "gfx/proc_luts.hpp"
#include "sim/particle_sim.hpp"
#include "sim/ribbon_sim.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xmb::gfx {

struct FrameSceneData {
    const assets::XmbThemeConfig* theme = nullptr;
    std::span<const sim::RibbonVertex> ribbon_vertices;
    std::span<const sim::RibbonUV> ribbon_uvs;
    std::span<const uint32_t> ribbon_indices;
    std::span<const sim::ParticleVertex> particle_vertices;
    std::span<const uint32_t> particle_indices;
    float time_seconds = 0.0f;
    float day_time = 1200.0f;   // 0..2400 (600=dawn, 1200=noon, 1800=dusk)
    float month_time = 15.0f;   // 1..30 day of month
    float surface_alpha = 1.0f; // premultiplied output alpha (<1.0 for translucent Wayland layer-shell)
    std::vector<std::string> hud_lines; // non-empty -> draws on-screen debug overlay
};

class VkRenderer {
public:
    VkRenderer() = default;
    ~VkRenderer();

    VkRenderer(const VkRenderer&) = delete;
    VkRenderer& operator=(const VkRenderer&) = delete;

    bool init(SDL_Window* window, std::string* out_error = nullptr);
    bool init_wayland(void* wl_display, void* wl_surface, uint32_t width, uint32_t height, std::string* out_error = nullptr);
    void shutdown();

    bool upload_background_luts(
        const assets::DdsImage& day1,
        const assets::DdsImage& day2,
        const assets::DdsImage& night1,
        const assets::DdsImage& night2,
        std::string* out_error = nullptr);

    bool upload_background_luts_rgba8(
        const Rgba8Image& day1,
        const Rgba8Image& day2,
        const Rgba8Image& night1,
        const Rgba8Image& night2,
        std::string* out_error = nullptr);

    bool upload_fresnel_lut(const Rgba8Image& img, std::string* out_error = nullptr);
    bool upload_iridescent_lut(const Rgba8Image& img, std::string* out_error = nullptr);

    bool render_frame(const FrameSceneData& scene, std::string* out_error = nullptr);
    void notify_resize() { framebuffer_resized_ = true; }
    void notify_external_resize(uint32_t width, uint32_t height) {
        external_width_ = width;
        external_height_ = height;
        framebuffer_resized_ = true;
    }

private:
    struct GpuImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct GpuBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    struct HudVertex {
        float pos[2];
        float uv[2];
        float color[4];
    };

    static constexpr uint32_t kMaxFramesInFlight = 2;
    static constexpr uint32_t kGlareMips = 5; // exact Sony 256x128..16x8 5-mip pyramid

    struct FrameSync {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;

        // Per-frame host-visible UBOs (zero CPU/GPU race across frames in flight)
        GpuBuffer ubo_back_colours{};
        GpuBuffer ubo_lines1_vert{};
        GpuBuffer ubo_lines1_frag{};
        GpuBuffer ubo_lines_composite{};
        GpuBuffer ubo_particles_vert{};
        GpuBuffer ubo_particles_frag{};
        GpuBuffer ubo_glare_src[kGlareMips]{};
        GpuBuffer ubo_gauss_vert_h[kGlareMips]{};
        GpuBuffer ubo_gauss_vert_v[kGlareMips]{};
        GpuBuffer ubo_gauss_frag{};
        GpuBuffer ubo_glare_sum{};
        GpuBuffer ubo_tonemap{};

        // Per-frame descriptor sets bound to this frame's UBOs
        VkDescriptorSet ds_back_colours = VK_NULL_HANDLE;
        VkDescriptorSet ds_lines1 = VK_NULL_HANDLE;
        VkDescriptorSet ds_lines_composite = VK_NULL_HANDLE;
        VkDescriptorSet ds_particles = VK_NULL_HANDLE;
        VkDescriptorSet ds_glare_src[kGlareMips]{};
        VkDescriptorSet ds_gaussian_h[kGlareMips]{};
        VkDescriptorSet ds_gaussian_v[kGlareMips]{};
        VkDescriptorSet ds_glare_sum = VK_NULL_HANDLE;
        VkDescriptorSet ds_tonemap = VK_NULL_HANDLE;

        // Per-frame dynamic geometry buffers
        GpuBuffer ribbon_vb{};
        GpuBuffer particle_vb{};
        GpuBuffer particle_ib{};
        GpuBuffer hud_vb{};
    };

    bool create_instance(std::string* out_error);
    bool create_instance_wayland(std::string* out_error);
    bool finish_init(std::string* out_error);
    bool select_physical_device(std::string* out_error);
    bool create_device(std::string* out_error);
    bool create_swapchain(std::string* out_error);
    bool create_render_passes(std::string* out_error);
    bool create_offscreen_targets(std::string* out_error);
    bool create_default_luts(std::string* out_error);
    bool create_uniform_buffers(std::string* out_error);
    bool create_descriptors(std::string* out_error);
    bool create_pipelines(std::string* out_error);
    bool create_framebuffers(std::string* out_error);
    bool create_command_and_sync(std::string* out_error);
    bool recreate_swapchain(std::string* out_error);
    void destroy_swapchain_resources();

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const;
    bool create_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, GpuImage& out_img, std::string* out_error,
                      uint32_t mip_levels = 1);
    void destroy_image(GpuImage& img);
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       bool persistently_map, GpuBuffer& out_buf, std::string* out_error);
    void destroy_buffer(GpuBuffer& buf);
    bool ensure_dynamic_buffer(GpuBuffer& buf, VkDeviceSize required, VkBufferUsageFlags usage, std::string* out_error);
    bool upload_rgba8_to_image(uint32_t w, uint32_t h, const uint8_t* pixels, GpuImage& dst,
                               std::string* out_error, bool generate_mips = false);
    bool upload_dds_to_image(const assets::DdsImage& img, GpuImage& dst, std::string* out_error);
    VkShaderModule load_shader_module(const char* filename, std::string* out_error);
    void update_descriptor_sets();

    SDL_Window* window_ = nullptr;
    uint32_t external_width_ = 1920;
    uint32_t external_height_ = 1080;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_{0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkFramebuffer> swapchain_framebuffers_;
    std::vector<VkSemaphore> render_finished_semaphores_;

    // Offscreen FP16 HDR + ribbon accum + 64x32 sky + 5-mip glare pyramid
    GpuImage hdr_color_{};
    GpuImage ribbon_accum_{};
    GpuImage sky_64x32_{};
    GpuImage glare_mip_a_[kGlareMips]{};
    GpuImage glare_mip_b_[kGlareMips]{};
    GpuImage glare_combined_{};

    // Monthly sky LUTs (current + next month, day + night)
    GpuImage day1_lut_{};
    GpuImage day2_lut_{};
    GpuImage night1_lut_{};
    GpuImage night2_lut_{};

    // Procedural & stock TGA LUTs (_Stripes, _FresLUT, _IridescentTex, _Encode, debug font)
    GpuImage stripes_lut_{};
    GpuImage fresnel_lut_{};
    GpuImage iridescent_lut_{};
    GpuImage encode_lut_{};
    GpuImage debug_font_lut_{};

    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler nearest_sampler_ = VK_NULL_HANDLE;

    // Render passes & framebuffers
    VkRenderPass hdr_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass ribbon_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass bloom_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass present_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer hdr_fb_ = VK_NULL_HANDLE;
    VkFramebuffer ribbon_fb_ = VK_NULL_HANDLE;
    VkFramebuffer sky_fb_ = VK_NULL_HANDLE;
    VkFramebuffer glare_mip_a_fb_[kGlareMips]{};
    VkFramebuffer glare_mip_b_fb_[kGlareMips]{};
    VkFramebuffer glare_combined_fb_ = VK_NULL_HANDLE;

    // Descriptor layouts, pool, shared HUD set
    VkDescriptorSetLayout dsl_back_colours_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_lines1_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_lines_composite_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_particles_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_glare_src_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_gaussian_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_glare_sum_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_tonemap_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_hud_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet ds_hud_ = VK_NULL_HANDLE;

    // Pipeline layouts & pipelines
    VkPipelineLayout pl_back_colours_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_lines1_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_lines_composite_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_particles_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_glare_src_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_gaussian_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_glare_sum_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_tonemap_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_hud_ = VK_NULL_HANDLE;

    VkPipeline pipe_back_colours_ = VK_NULL_HANDLE;
    VkPipeline pipe_lines1_ = VK_NULL_HANDLE;
    VkPipeline pipe_lines_composite_ = VK_NULL_HANDLE;
    VkPipeline pipe_particles_ = VK_NULL_HANDLE;
    VkPipeline pipe_particles_second_ = VK_NULL_HANDLE;
    VkPipeline pipe_glare_src_ = VK_NULL_HANDLE;
    VkPipeline pipe_gaussian_ = VK_NULL_HANDLE;
    VkPipeline pipe_glare_sum_ = VK_NULL_HANDLE;
    VkPipeline pipe_tonemap_ = VK_NULL_HANDLE;
    VkPipeline pipe_hud_ = VK_NULL_HANDLE;

    // Static geometry buffers (uploaded once)
    GpuBuffer ribbon_uv_vb_{};
    GpuBuffer ribbon_ib_{};
    GpuBuffer fullscreen_quad_vb_{};
    VkDeviceSize ribbon_uv_uploaded_bytes_ = 0;
    VkDeviceSize ribbon_ib_uploaded_bytes_ = 0;
    std::vector<HudVertex> hud_verts_scratch_;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    FrameSync frames_[kMaxFramesInFlight]{};
    uint32_t current_frame_ = 0;
    uint32_t frame_index_ = 0;
    bool framebuffer_resized_ = false;
    bool glare_initialized_ = false;
    bool initialized_ = false;
};

} // namespace xmb::gfx
