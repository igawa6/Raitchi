/* Derived from LAS Recomp Android cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// Dear ImGui's Vulkan backend on our device (start screen and in-game menu; both draw inside the window image's
// dynamic rendering begun by vk_frame_begin).
#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "vk.h"
#include "record.h"
}

// Per ImGui context (the start screen and the in-game menu each have one, and the start screen comes back after an
// in-place reboot while the menu's context lives on): the backend keeps its data in the current context.
// The backend's vertex shader with the swapchain's pre-rotation (device.c sc_quarter) in its push constants: the same
// projection as the stock shader, then turned by (cos, sin)
static const char kImguiVert[] =
    "#version 450 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "layout(push_constant) uniform uPushConstant { vec2 uScale; vec2 uTranslate; vec2 uRot; } pc;\n"
    "out gl_PerVertex { vec4 gl_Position; };\n"
    "layout(location = 0) out struct { vec4 Color; vec2 UV; } Out;\n"
    "void main() {\n"
    "  Out.Color = aColor;\n"
    "  Out.UV = aUV;\n"
    "  vec2 p = aPos * pc.uScale + pc.uTranslate;\n"
    "  gl_Position = vec4(pc.uRot.x * p.x - pc.uRot.y * p.y, pc.uRot.y * p.x + pc.uRot.x * p.y, 0.0, 1.0);\n"
    "}\n";

extern "C" uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);  // (shaders.c)

extern "C" void vk_imgui_prerotation(int quarter) { ImGui_ImplVulkan_SetPreRotation(quarter); }

// The backend continues after an unsuccessful allocation/map unless supplied a
// callback. Stop at that first Vulkan error instead of memcpy to a NULL map.
static void imgui_vk_result(VkResult result) {
    if (result != VK_SUCCESS)
        FATAL("[vk] ImGui Vulkan operation failed: VkResult=%d (allocation/map/upload/init)\n", result);
}

extern "C" bool vk_imgui_backend_init(void) {
    if (ImGui::GetIO().BackendRendererUserData) return true;
    static uint32_t *spv;
    static size_t nwords;
    if (!spv) spv = vk_glsl_to_spirv(kImguiVert, STAGE_VERTEX, "imgui.vert", &nwords);
    if (spv) ImGui_ImplVulkan_SetVertexShader(spv, nwords * 4);
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.CheckVkResultFn = imgui_vk_result;
    ii.ApiVersion = g_vk.api;
    ii.Instance = g_vk.inst;
    ii.PhysicalDevice = g_vk.phys;
    ii.Device = g_vk.dev;
    ii.QueueFamily = g_vk.qfam;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = 2;
    ii.ImageCount = (g_vk.sc_count > 3 ? g_vk.sc_count : 3) + 1;  // vertex buffers rotate per image: cover frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = g_vk.pcache;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk.sc_format;
    bool ok = ImGui_ImplVulkan_Init(&ii);
    if (!ok) LOG("[vk] ImGui Vulkan backend init failed\n");
    // The font texture now (the fonts were added before this): ImGui uploads it with a submission and a queue wait of
    // its own, which its NewFrame would otherwise make at the first frame, while the recorder thread (record.c) may
    // be submitting. Here the recorder is drained first, so the queue is idle.
    if (ok) {
        rec_drain();
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    return ok;
}

extern "C" void vk_imgui_backend_shutdown(void) {
    if (!ImGui::GetIO().BackendRendererUserData) return;
    rec_drain();  // (the queue is the recorder's: idle for the wait below)
    vkDeviceWaitIdle(g_vk.dev);
    ImGui_ImplVulkan_Shutdown();
}

// A static RGBA8 image for ImGui (start screen background). Returns the ImTextureID.
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return 0;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSpan s = vk_transient_copy(rgba, (uint64_t)w * h * 4, 16);
    VkBufferImageCopy rg = {};
    rg.bufferOffset = s.off;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    rec_CmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &view) != VK_SUCCESS) return 0;
    return (uint64_t)ImGui_ImplVulkan_AddTexture(vk_default_sampler(true), view, VK_IMAGE_LAYOUT_GENERAL);
}
