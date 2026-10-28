/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef VK_NO_PROTOTYPES
#include <volk.h>  // (Android: entry points loaded at run time, device.c)
#else
#include <vulkan/vulkan.h>
#endif
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

#define VK_SWAPCHAIN_MAX_IMAGES 32u

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    uint32_t ts_bits;          // the queue's timestampValidBits (0: no timestamps)
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, dmabuf;
    uint32_t api;              // the Vulkan version the device is used at (1.3; Android: 1.1 / 1.2 + extensions)
    // VK_EXT_vertex_input_dynamic_state (exec.c): the vertex layout is set per draw (vkCmdSetVertexInputEXT), not
    // baked into each pipeline - fewer pipelines. Off: no extension / feature, or LARECOMP_VK_DYN_VERTEX=0.
    bool vids;
    PFN_vkCmdSetVertexInputEXT set_vertex_input;
    // Opt-in reusable fast-linked libraries; diagnostic probe can independently enable the extension.
    bool gpl, gpl_probe;
    bool map_placed, unmap_reserve;  // VK_EXT_map_memory_placed: driver memory mapped at a given address (exec.c guest_heap_dmabuf)
    bool ssbo_chunks;  // stores and atomics from fragment and vertex shaders (exec.c: the heap in chunks counts misses)
    bool lazy_heap, bda;  // the heap imported chunk by chunk (device.c lazy_heap_predicted), buffer device addresses enabled for it
    bool heap_probe;      // one import tried at the start, lazy chunks if it runs out of memory (exec.c whole_import_probe_oom)
    // on by default on integrated GPUs only (device.c; LARECOMP_VK_LEAN_USAGE / LARECOMP_VK_SMAA_STENCIL /
    // LARECOMP_NO_FRAME_CLEAR =0 / =1 override):
    bool lean_usage;      // colour images list their view formats instead of a bare MUTABLE_FORMAT (textures.c create_image)
    bool smaa_stencil;    // SMAA's weights pass only where the edge pass found edges (exec.c post_process)
    bool no_frame_clear;  // scene targets not cleared at their first use in each frame (textures.c renderer_texture_frame_fresh)
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;  // the window as the game and UI see it (landscape on phones)
    VkExtent2D sc_phys;    // the swapchain images (the panel's own orientation when pre-rotated)
    int sc_quarter;        // pre-rotation applied when drawing into them: 0, 1 (90 degrees) or 3 (270)
    uint32_t sc_count;
    VkImage sc_images[VK_SWAPCHAIN_MAX_IMAGES];
    VkImageView sc_views[VK_SWAPCHAIN_MAX_IMAGES];
} VkG;
void vk_log_memory(const char *why);  // (device.c: per-heap budget and use, at start and on allocation failures)
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
// GPU timer (exec.c gpu_timer_add): the window image's submission starts its counted work here (after its acquire wait)
void vk_gpu_timer_window_start(void);
bool vk_gpu_split_ns(uint64_t out[4]);  // cumulative: game GPU ns, window GPU ns, frame spans' ns, frames; false: no timer
uint64_t vk_cum_wait_ns(void);          // cumulative vk_wait_seq time
// (device.c) CPU time blocked presenting, cumulative ns: [0] frame slot's fence, [1] window image acquire, [2] present;
// [3]: frames presented
extern uint64_t g_vk_frame_waits[4];

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageUsageFlags usage;  // actual successful image creation usage, not the requested/fallback format
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint64_t cpu_write_generation;  // last explicit CPU generation consumed (render thread only)
    uint64_t source_address, source_generation; // resolved CPU backing; remapping invalidates upload memoization
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;        // the scale the contents were drawn at (scalable textures)
    float ax, ay;        // the allocation scale (the image's size; Dynamic: the contents use a part of it)
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    bool pending_nv12;        // pending_rgba holds NV12 (Y, then UV at pending_uv_off): converted on the GPU
    uint32_t pending_uv_off;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    uint8_t bcn;  // BC without GPU support (phones): decoded on upload (runtime/nvn/bcn.c), BCN_* kind
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    uint16_t vslots[8];  // its views' slots in textures.c's view table (views_forget visits these, not the whole table)
    uint8_t nvslots;
    bool vslot_overflow;  // (more than 8 since the last forget: the whole table is searched)
    VkFormat fmt_twin;    // format list images (textures.c): the one other view format allowed, else UNDEFINED
    bool fmt_listed;
    // (g_vk.lean_usage) the only formats views of this image may have: nview_fmt 0 = any compatible one
    VkFormat view_fmt[2];
    uint8_t nview_fmt;
    VkImageCreateFlags create_flags; // actual successful allocation flags, including fallback
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
VkFormat vk_image_format(uint32_t nvn_format, VkImageAspectFlags *aspect);  // (shader preparation)
void vk_pipeline_cache_save(void);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
// Pure effective sampling policy: explicit view mode overrides texture default; intrinsic S8 is UINT.
bool vk_sampled_stencil(const NvnTexture *t, const NvnTextureView *v, bool has_view);
bool vk_sampled_float_compatible(NvnTexture *t, const NvnTextureView *v, bool has_view);
// Exact storage views: identity swizzle, one mip, strict bounds/type/format. NULL
// means unsupported; never substitutes a dummy or silently changes the format.
VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_textype,
                            VkFormat *actual_format);
int vk_storage_format_kind(VkFormat format);  // 0 float/normalized, 1 sint, 2 uint, -1 unsupported
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkImageView vk_dummy_depth_view(int textype);  // shadow (depth compare) slots: 1x1 depth, cleared to 1.0
bool vk_format_depth_compare(VkFormat f);  // SAMPLED_IMAGE_DEPTH_COMPARISON
bool vk_tex_depth_compare(NvnTexture *t);  // t's image can be sampled with a depth compare
VkFormat vk_view_format(TexBackend *b, VkFormat want);  // want, or the image's own format where want isn't allowed
void vk_convert_nv12(TexBackend *b, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);  // exec.c
VkImageView vk_dummy_color_view(uint32_t w, uint32_t h, uint32_t samples);
VkSampler vk_sampler(NvnSamplerDesc *d);
// Integer-image specialization; comparison/custom-border-without-support fail closed.
VkSampler vk_sampler_uint(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_defer_buffer(VkBuffer b, VkDeviceMemory m);  // raw memory (an import); destroyed once the pending submission is done
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
