/* Adapted from LAS Recomp Android, pinned cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c)2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * Shared runtime: tracked dirty pages require a full content check. */
// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include "gamelib.h"
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../rqueue.h"
#include "vk.h"
#include "record.h"
#include "../astc.h"
#include "../bcn.h"
#include "../tex_hash.h"
#include "../renderer.h"
#include "../texpatch.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel): half the
// bytes of D32_SFLOAT_S8_UINT (4 instead of 8 per pixel) for every depth test, clear and copy - the game copies its
// full-size depth twice a frame. AMD has no D24S8 attachments: D32S8 there (and on Metal). LARECOMP_VK_D24=0: D32S8.
// Phones: D32S8 - as fast as D24S8 on Adreno or a little faster (separate depth / stencil planes), and the depth
// the game reads back in shaders (the sea's reflections) keeps full precision. LARECOMP_VK_D24=1: D24S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = getenv("LARECOMP_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
#ifdef __ANDROID__
        bool want_d24 = e && atoi(e);
#else
        bool want_d24 = !(e && !atoi(e));
#endif
        bool ok = (p.optimalTilingFeatures & want) == want && want_d24;
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_B5G6R5_UNORM_PACK16};  // RGB565: R in low5, G in middle6, B in high5
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: {
        static int unorm = -1;  // (per texture lookup: getenv scans the environment, notably on Windows)
        if (unorm < 0) unorm = getenv("LARECOMP_BC5_UNORM") != NULL;
        return (VkFmt){unorm ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    }
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_A1R5G5B5_UNORM_PACK16};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

int vk_storage_format_kind(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return 0;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return 1;
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return 2;
    default: return -1;
    }
}

static bool storage_nvn_format(uint32_t f) {
    // Excludes unknown fmt_of's RGBA8 fallback, RGB expansion/alpha-one aliases,
    // sRGB, compressed and depth formats: none preserves raw storage writes.
    return (f >= 0x01 && f <= 0x18) || (f >= 0x25 && f <= 0x30) ||
           (f >= 0x3d && f <= 0x3f) || f == 0x99;
}

static bool storage_format_writable(VkFormat f) {
    // Formatless OpImageWrite additionally requires this per-view format bit.
    // Core 1.3 exposes it; older APIs need an explicitly enabled extension path.
    if (g_vk.api < VK_API_VERSION_1_3) return false;
    VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
    vkGetPhysicalDeviceFormatProperties2(g_vk.phys, f, &p2);
    VkFormatFeatureFlags2 want = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    return (p3.optimalTilingFeatures & want) == want;
}

// (shader preparation) the format and aspects an image of this NVN format gets as a render target
// (create_backend_image: a format the device can't sample becomes RGBA8; compressed formats are never targets)
VkFormat vk_image_format(uint32_t nvn, VkImageAspectFlags *aspect) {
    VkFormat vf = vk_format(nvn);
    if (!format_supports(vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) vf = VK_FORMAT_R8G8B8A8_UNORM;
    if (aspect) *aspect = renderer_format_is_depth(nvn) ? aspect_of(nvn) : VK_IMAGE_ASPECT_COLOR_BIT;
    return vf;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;   // the render scale: what the frame is drawn at
static float g_al_x = 1, g_al_y = 1;   // the allocation scale: the size of scalable textures (Dynamic: the frame uses a part)
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
// The window's height (device.c): the game's full-HD pictures (its 1080-line UI and composite targets, 1.5x its 720p
// 3D) aren't made taller than it - above it they cost GPU time and are only scaled back down. At 1080p on a 1080-line
// screen they were 3600x1620 (eight passes, ~10 ms of the Field's GPU time); the 3D still gets the whole scale.
static float g_display_h = 0;
static float shadow_scale(const NvnTexture *t);
void renderer_set_display_height(float h) { g_display_h = h; }
// The game's small blur / glow levels (400 wide and below: the game's sizes follow its own render resolution, e.g.
// 336x189) at most at this share of the internal resolution: blurred, sharper is not visible (Martha's Bay at 1080p:
// -0.6 ms, frames identical). Not the first glow level (half the scene, e.g. 672x378): kept small it lost the water's
// sparkles even fed a 2x2-averaged scene (-1.7 ms, rejected). Phones: their own size (1x); LARECOMP_AUX_SCALE=<f>
// (0 = off).
static float aux_cap(void) {
    static float f = -1;
#ifdef __ANDROID__
    const float def = 1.0f;
#else
    const float def = 0.0f;
#endif
    if (f < 0) f = getenv("LARECOMP_AUX_SCALE") ? (float)atof(getenv("LARECOMP_AUX_SCALE")) : def;
    return f;
}
// Our caps on a scalable texture's scale (aux_cap, the display height), on the render scale and the allocation scale alike.
static void cap_scale(const NvnTexture *t, float *sx, float *sy) {
    float cap = aux_cap();
    bool aux = t->width <= 400;
    if (cap > 0 && aux) {
        if (*sx > cap) *sx = cap;
        if (*sy > cap) *sy = cap;
    }
    if (g_display_h > 0 && t->height >= 1000 && t->height * *sy > g_display_h * 1.01f) {
        float f = g_display_h / (t->height * *sy);
        if (f * *sy < 1) f = 1 / *sy;  // (never below the game's own size)
        *sx *= f, *sy *= f;
    }
}
// The scale a texture's contents are drawn at (sx, sy) and its image's size (ax, ay: Dynamic resolution allocates
// once at the allocation scale and draws into the top-left part); the shadow map at its fixed share (shadow_scale).
static void tex_scale(const NvnTexture *t, const TexBackend *b, float *sx, float *sy) {
    float ss = shadow_scale(t);
    if (ss < 1) { *sx = *sy = ss; return; }
    *sx = b->scalable ? g_rs_x : 1, *sy = b->scalable ? g_rs_y : 1;
    if (b->scalable) cap_scale(t, sx, sy);
}
static void tex_alloc_scale(const NvnTexture *t, const TexBackend *b, float *ax, float *ay) {
    float ss = shadow_scale(t);
    if (ss < 1) { *ax = *ay = ss; return; }
    *ax = b->scalable ? g_al_x : 1, *ay = b->scalable ? g_al_y : 1;
    if (b->scalable) cap_scale(t, ax, ay);
}
void renderer_set_alloc_scale(float ax, float ay) { g_al_x = ax; g_al_y = ay; }
// The part of a scalable texture its contents occupy (Dynamic: the render scale over the allocation scale; 1 otherwise).
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    *fx = *fy = 1;
    if (b && b->scalable && b->ax > 0 && b->ay > 0) { *fx = b->sx / b->ax; *fy = b->sy / b->ay; }
}
uint64_t renderer_present_count(void);

// The sun's shadow map (a square depth target of 2048) at a share of its size: half on phones - the shadows look the
// same on their screens (frames compared in the Field), and the pass took 3.3 ms of the Field's GPU time, 1.6 at half.
// LARECOMP_SHADOW_SCALE=<f> (1: full size).
static float shadow_scale(const NvnTexture *t) {
    if (!shared_renderer_legacy_target_policy()) return 1;
    static float f = -1;
#ifdef __ANDROID__
    /* Half-size maps were qualified for LAS's sun pass. Other titles retain
     * their original depth-map size, as in the upstream Eevee renderer. */
    const float def = 0.5f;
#else
    const float def = 1.0f;
#endif
    if (f < 0) f = getenv("LARECOMP_SHADOW_SCALE") ? (float)atof(getenv("LARECOMP_SHADOW_SCALE")) : def;
    return t->width == 2048 && t->height == 2048 && renderer_format_is_depth(t->format) && f > 0 && f < 1 ? f : 1;
}

static bool tex_scalable(const NvnTexture *t) {
    // COMPRESSIBLE(0x8) is not a render-role guarantee. The inherited heuristic
    // is qualified only by explicit adapter policy; other titles retain native
    // guest dimensions until coherent attachment-group scaling is available.
    if (!shared_renderer_legacy_target_policy()) return false;
    static int off = -1;  // (per texture lookup: cached)
    if (off < 0) off = getenv("LARECOMP_NO_SCALE") != NULL;
    if (off) return false;
    if (shadow_scale(t) < 1) return true;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    extern uint64_t renderer_exec_frame(void);
    uint64_t total_frames = renderer_exec_frame();  // (the frame being translated)
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;
void vk_level_change_check(void) {
    if (!shared_renderer_legacy_target_policy()) return;
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        if (!strncmp(lv, "TelephoneBox", 12) || getenv("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}

bool renderer_texture_frame_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    extern uint64_t renderer_exec_frame(void);
    uint64_t total_frames = renderer_exec_frame();  // (the frame being translated)
    if (!b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    // LARECOMP_NO_FRAME_CLEAR skips the clear only while the contents cover the whole target: below 1x a frame draws
    // into part of it, and the rest would show what an earlier frame left there (issue #205: the upper part flickered)
    // (Dynamic: the contents cover the whole target only at the factor 1 - its render scale is the allocation's)
    if (g_vk.no_frame_clear && b->sx >= 1.0f && b->sy >= 1.0f && b->sx >= b->ax && b->sy >= b->ay) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkBuffer buf; VkDeviceMemory bmem; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else if (dead[i].buf) { vkDestroyBuffer(g_vk.dev, dead[i].buf, NULL); vkFreeMemory(g_vk.dev, dead[i].bmem, NULL); }
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

// The sRGB / UNORM counterpart of a colour format (VK_FORMAT_UNDEFINED: none). (g_vk.lean_usage format lists)
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;
    case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;
    case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)  // (UNORM, SRGB alternate)
            return (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1 ? f - 1 : f + 1;
        return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_view_format(TexBackend *b, VkFormat want) {
    if (!b || !b->nview_fmt || want == b->format) return want;
    for (int i = 0; i < b->nview_fmt; i++) if (b->view_fmt[i] == want) return want;
    static int nlog;
    if (nlog++ < 10) LOG("[vk] lean usage: view format %d of an image of format %d refused (its own instead)\n", want, b->format);
    return b->format;
}

static bool vk_fmt_list(void) {  // LARECOMP_VK_FMTLIST=0/1 (on by default on phones: 2 ms of 25.7 at 1x, S20 FE)
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_FMTLIST");
#ifdef __ANDROID__
        on = e ? atoi(e) != 0 : 1;
#else
        on = e ? atoi(e) != 0 : 0;
#endif
    }
    return on;
}


static VkResult create_image_allocation(VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, TexBackend *b) {
    VkResult r = vmaCreateImage(g_vk.vma, ci, ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS && (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
        // Optional storage capability must not change existing sampled/rendered
        // formats. A refused usage combination gets one exact-format retry;
        // storage binding then rejects the recorded non-storage allocation.
        ci->usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        r = vmaCreateImage(g_vk.vma, ci, ai, &b->image, &b->mem, NULL);
    }
    return r;
}

static VkImageCreateFlags color_slice_create_flags(VkImageType type, VkImageUsageFlags usage,
                                                  uint32_t samples, bool depth, uint32_t api) {
    return type == VK_IMAGE_TYPE_3D && samples == 1 && !depth &&
           (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && api >= VK_API_VERSION_1_1
         ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    tex_alloc_scale(t, b, &b->ax, &b->ay);
    tex_scale(t, b, &b->sx, &b->sy);
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->ax); th = (uint32_t)lroundf(th * b->ay);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    b->bcn = BCN_NONE;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        // BC on GPUs without it (phones): decoded on the CPU, into the uncompressed format with the same channels
        static const struct { VkFormat bc, out; uint8_t kind; } BC[] = {
            {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, BCN_1}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, BCN_1},
            {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, BCN_2}, {VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, BCN_2},
            {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, BCN_3}, {VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, BCN_3},
            {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8_UNORM, BCN_4U}, {VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_R8_SNORM, BCN_4S},
            {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8_UNORM, BCN_5U}, {VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_R8G8_SNORM, BCN_5S},
            {VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, BCN_6HS},
            {VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, BCN_6HU},
            {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, BCN_7}, {VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, BCN_7}};
        for (size_t i = 0; i < sizeof BC / sizeof *BC; i++)
            if (BC[i].bc == vf) b->bcn = BC[i].kind, vf = BC[i].out;
        if (b->bcn) {
        } else if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (storage_nvn_format(t->format) && vf == mf.vf && samples == 1 &&
        vk_storage_format_kind(vf) >= 0 && format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    flags |= color_slice_create_flags(type, usage, samples, depthfmt, g_vk.api);
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // g_vk.lean_usage (integrated GPUs): colour images may only be viewed as their own format and its sRGB / UNORM
    // twin, listed in a VkImageFormatListCreateInfo (no twin: not mutable at all). Mesa's AMD / Intel drivers keep
    // DCC / CCS compression for such images; with MUTABLE_FORMAT and no list they turn it off. Views in any other
    // format fall back to the image's own (vk_view_format, logged).
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    b->nview_fmt = 0;
    if (!depthfmt && g_vk.lean_usage) {
        VkFormat twin = srgb_twin(vf);
        b->view_fmt[0] = vf;
        b->view_fmt[1] = twin;
        b->nview_fmt = twin != VK_FORMAT_UNDEFINED ? 2 : 1;
        if (b->nview_fmt == 1) flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        else fl.viewFormatCount = 2, fl.pViewFormats = b->view_fmt;
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = fl.viewFormatCount ? &fl : NULL,
                            .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    // A mutable format image without a format list is never compressed on Turnip (UBWC): render targets list their
    // own format and its sRGB / UNORM twin; views in any other format fall back to the image's own (vk_view).
    // (phones; g_vk.lean_usage, where on, has listed every colour image's formats above instead)
    VkFormat fl_fmts[2] = {vf, srgb_twin(vf)};
    VkImageFormatListCreateInfo fl2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                       .viewFormatCount = fl_fmts[1] ? 2 : 1, .pViewFormats = fl_fmts};
    b->fmt_listed = false;
    if (vk_fmt_list() && !b->nview_fmt && !depthfmt && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        ci.pNext = &fl2;
        b->fmt_listed = true;
        b->fmt_twin = fl_fmts[1];
    }
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = create_image_allocation(&ci, &ai, b);
    if (r != VK_SUCCESS) {
        // (the Vulkan side too: a driver can refuse a format / usage / tiling combination with "out of device memory",
        // issue #45 - a GTX 1060 failing a 1920x1080 image with plenty of memory free)
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u - vk format %d, usage %#x, flags %#x, "
            "%u levels, %u layers, %u samples\n", r, t->format, t->width, t->height, t->depth, t->target, ci.format,
            ci.usage, ci.flags, ci.mipLevels, ci.arrayLayers, (unsigned)ci.samples);
        vk_log_memory("image creation failed");
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.pNext = NULL;  // (g_vk.lean_usage: the fallback image is mutable as before, no format list)
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        b->nview_fmt = 0;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        b->fmt_listed = false;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->format = vf;
    b->usage = ci.usage;
    b->create_flags = ci.flags;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    // New depth / stencil images start zeroed. The game builds its depth pyramid with depth-only copies, so the stencil
    // of those images was never written: whatever the memory held, different per image and per run, then failed or
    // passed the depth-of-field passes' stencil test in patches (flicker that came and went between runs, every other
    // frame, below 1x internal resolution).
    // Scalable colour targets too (as Metal's, metal.m clear_new_target): Dynamic resolution draws into their top-left
    // part only, and the game's blurs sample a few texels past it, where a new image holds whatever its memory held -
    // NaN / infinity in a float target among it, which the bloom chain spreads and keeps (a pure green band down the
    // right edge, issue #205; every window resize reallocates these images). Copies into them write only the contents'
    // part, and with LARECOMP_NO_FRAME_CLEAR the per-frame clear is skipped. LARECOMP_NEW_TARGET_FILL=<r>,<g>,<b>,<a>
    // (test): that colour instead of zeros.
    bool clear_color = !depthfmt && b->scalable && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (depthfmt || clear_color) {
        VkCommandBuffer cb = vk_upload_cmd();
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (depthfmt) {
            VkClearDepthStencilValue zero = {0.0f, 0};
            rec_CmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        } else {
            static int fill = -1;
            static VkClearColorValue fc;
            if (fill < 0) {
                const char *e = getenv("LARECOMP_NEW_TARGET_FILL");
                float *c = fc.float32;
                fill = e && sscanf(e, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4;
            }
            VkClearColorValue zero = {{0}};
            rec_CmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, fill ? &fc : &zero, 1, &rg);
        }
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        rec_CmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }
void vk_defer_buffer(VkBuffer b, VkDeviceMemory m) { defer((Dead){.buf = b, .bmem = m}); }  // (raw memory, not VMA)

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        float wax, way;
        tex_alloc_scale(t, b, &wax, &way);
        if (b->scalable && (b->ax != wax || b->ay != way)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->scalable = tex_scalable(t);
    create_image(t, b);
    t->backend = b;
    live_add(t, b);
    return b;
}

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        extern void vk_alias_texture_destroyed(NvnTexture *t);  // (exec.c copy aliases: a source's contents handed over)
        vk_alias_texture_destroyed(t);
        live_remove(b);
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    extern void vk_alias_reset(void);
    vk_alias_reset();
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride, uv_off = b->pending_uv_off;
    bool nv12 = b->pending_nv12;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (nv12) { vk_convert_nv12(b, px, w, h, stride, uv_off); free(px); return; }
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

static void upload_texture(NvnTexture *t, TexBackend *b);

// BEGIN explicit CPU texture-write coherence (host-tested actual guards).
static bool texture_write_pending(const NvnTexture *t, const TexBackend *b, uint64_t *generation) {
    *generation = __atomic_load_n(&t->cpu_write_generation, __ATOMIC_ACQUIRE);
    return *generation != b->cpu_write_generation;
}
static bool texture_upload_skip(const NvnTexture *t, const TexBackend *b, uint64_t frame, uint64_t *generation) {
    bool written = texture_write_pending(t, b, generation);
    if ((!t->addr && !t->pool) || t->target == 10) return true;
    if (written) return false;
    return b->uploaded && (b->checked_frame == frame ||
        (!b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)));
}
static void texture_source_prepare(TexBackend *b, uint64_t address, uint64_t generation) {
    if (b->source_address == address && b->source_generation == generation) return;
    b->source_address = address;
    b->source_generation = generation;
    b->uploaded = false;
    b->checked_frame = UINT64_MAX;
    b->wgen = 0;
    b->tp = 0;
}
static bool texture_linear_span(const NvnTexture *t, uint64_t *size) {
    return nvn_texture_row_span(&t->layout, t->stride, size);
}
static void texture_write_gpu_check(const NvnTexture *t, const TexBackend *b) {
    uint64_t generation;
    if (texture_write_pending(t, b, &generation))
        FATAL("[vk] CPU WriteTexels pending for GPU-owned/rendered texture %llu; region coherence unsupported\n",
              (unsigned long long)t->debug_id);
}
static void texture_mark_gpu_owned(NvnTexture *t, TexBackend *b) {
    // Runs where the render queue changes ownership, not on an unsynchronized
    // guest-thread backend pointer. Refuse to discard unconsumed CPU writes.
    texture_write_gpu_check(t, b);
    b->gpu_owned = true;
    __atomic_store_n(&t->gpu_write_owned, 1, __ATOMIC_RELEASE);
}
static void texture_prepare_gpu_owned(NvnTexture *t, TexBackend *b) {
    uint64_t generation;
    if (!b->gpu_owned && texture_write_pending(t, b, &generation))
        upload_texture(t, b); // ordered CPU initialization before the first GPU write
    texture_mark_gpu_owned(t, b);
}
// END explicit CPU texture-write coherence.

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
static void upload_texture(NvnTexture *t, TexBackend *b) {
    uint64_t frame = vk_frame_counter(), write_generation;
    if ((!t->addr && !t->pool) || t->target == 10) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total;
    if (nvn_texture_row_linear(t->flags)) {
        if (!texture_linear_span(t, &total)) FATAL("[vk] unsupported linear texture upload layout\n");
    } else total = tegra_total_size(&t->layout);
    if (!total) return;
    uint64_t address = t->addr, mapping_generation = 0;
    if (t->pool && !nvn_pool_resolve_span(t->pool, t->pool_offset, total, &address, &mapping_generation))
        FATAL("[vk] texture %llu has no contiguous CPU backing: pool offset %#llx, bytes %#llx\n",
            (unsigned long long)t->debug_id, (unsigned long long)t->pool_offset, (unsigned long long)total);
    texture_source_prepare(b, address, mapping_generation);
    if (texture_upload_skip(t, b, frame, &write_generation)) return;
    bool written = write_generation != b->cpu_write_generation;
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)address;
    // Prompt/recolour identification also reads CPU texels. Keep the live
    // texture's pool offset identity, and give those readers the resolved view.
    NvnTexture readable = {.layout = t->layout, .addr = address, .width = t->width,
        .height = t->height, .depth = t->depth, .levels = t->levels, .format = t->format};
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = written || b->dynamic || !b->uploaded || always_full;
    bool periodic = frame - b->full_frame >= 120 && full_hash_budget >= total;  // (safety net: a full hash now and then)
    if (!full && periodic) { full = true; full_hash_budget -= total; }
    bool identified_now = !b->tp;  // (button prompts / Hero Mode icon: re-uploaded when their look changes)
    if (identified_now) b->tp = nvn_texture_row_linear(t->flags) ? -1 : texpatch_marker(&readable);
    bool restyle = b->tp > 0 && b->tp <= TEXPATCH_RECOLOUR_BASE && b->uploaded && b->tp_gen != texpatch_generation();
    // The OS's page write tracking (exec.c vk_pages_changed, shared with the vertex cache): texture memory the CPU
    // hasn't written since the last check is not hashed at all (dynamic textures were fully hashed every frame,
    // ~2.4 MB a frame in the overworld). The periodic full hash stays as a safety net. LARECOMP_VK_TEX_WATCH=0: off.
    static int watch = -1;
    if (watch < 0) { const char *e = getenv("LARECOMP_VK_TEX_WATCH"); watch = !e || atoi(e) != 0; }
    if (watch && !always_full && !written) {
        extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
        uint32_t now = 0;
        int ch = vk_pages_changed(address, total, b->wgen, &now);
        if (ch >= 0) {
            /* A sparse edit can miss every sample. Once this generation is
             * consumed, clean-page skips would hide it until a budgeted periodic
             * check. Honor positive dirty evidence with a full hash now. */
            if (ch > 0) full = true;
            b->wgen = now;
            if (!ch && b->uploaded && !restyle) {
                if (!periodic) { prof_tex_watch_skips++; return; }
                if (b->dynamic) full_hash_budget -= total;  // (the others took theirs above)
            }
        }
    }
    bool fast = tex_fast_hash();  // (runtime/nvn/tex_hash.h; LARECOMP_TEX_FASTHASH=0: the previous hashes)
    if (!full && !restyle) {
        prof_tex_samples++;
        uint64_t sh = fast ? tex_hash_sample(mem, total) : fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    extern void slow_note_full_hash(void), slow_note_upload(uint64_t bytes);  // (slowframe.h)
    slow_note_full_hash();
    uint64_t h = fast ? tex_hash_full(mem, total) : hash_full(mem, total);
    b->full_frame = frame;
    // (the sample is only compared while the texture isn't dynamic, and a dynamic one stays so: none for those)
    if (!fast) b->sample_hash = fnv_sample(mem, total);
    else if (!b->dynamic && !(b->uploaded && h != b->data_hash)) b->sample_hash = tex_hash_sample(mem, total);
    if (b->uploaded && h == b->data_hash && !restyle) {
        b->cpu_write_generation = write_generation; // same bytes already resident
        return;
    }
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    // new contents (the game reused the memory for another image, or the data arrived after the first check): which
    // known texture it is, if any, is decided again before this upload patches it
    if (!identified_now && h != b->data_hash) b->tp = nvn_texture_row_linear(t->flags) ? -1 : texpatch_marker(&readable);
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    b->data_hash = h;
    b->uploaded = true;
    if (b->format != mf.vf && !b->astc_bw && !b->bcn) return;  // unsupported format: nothing sensible to copy
    slow_note_upload(total);
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = nvn_texture_row_linear(t->flags);
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw || b->bcn) {  // blocks -> linear block rows -> uncompressed texels
                const uint8_t *src = mem + layer * layer_size + l.offset;
                uint32_t bb = b->bcn ? (uint32_t)bcn_block_bytes(b->bcn) : 16, tb = b->bcn ? (uint32_t)bcn_texel_bytes(b->bcn) : 4;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * bb);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * bb, src + (uint64_t)y * t->stride, l.wb * bb);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, bb, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * bb);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * tb, 16);
                if (b->bcn) bcn_decode(b->bcn, blocks, l.w, l.h, s.ptr);
                else astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = mem + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * t->stride, row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, lin, l.w, l.h, row);
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
    b->cpu_write_generation = write_generation;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (b->gpu_owned) texture_write_gpu_check(t, b);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) texture_prepare_gpu_owned(t, b);
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

// A GPU write (render target, copy destination): the contents are at the current render scale from here on.
void renderer_mark_gpu_owned(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    texture_mark_gpu_owned(t, b);
    if (b->scalable) tex_scale(t, b, &b->sx, &b->sy);
}
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
static void upload_rgba_now(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->pending_nv12 = false;
    texture_prepare_gpu_owned(t, b);
    pthread_mutex_unlock(&pending_lock);
}

// Decoded video as NV12 (runtime/hle/movie.c): the planes (half the bytes of RGBA) go to the GPU, which converts them
// into the texture (exec.c vk_convert_nv12) - swscale plus the RGBA upload cost mid-range CPUs most of a frame at the
// movies' 60 fps. false: a texture format this doesn't handle (the caller converts on the CPU).
static bool nv12_format_ok(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}
static bool upload_nv12_now(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);
// Movie frames (runtime/hle/movie.c, the game thread): with the render thread on they are queued behind the lists
// already submitted (rqueue.c), as a copy.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    if (!rq_on()) { upload_rgba_now(t, rgba, w, h, stride); return; }
    if (!t->backend) return;
    size_t n = (size_t)stride * h;
    void *copy = malloc(n);
    memcpy(copy, rgba, n);
    rq_upload(t, RQ_UPLOAD_RGBA, copy, w, h, stride, 0);
}
bool renderer_upload_nv12(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    if (!rq_on()) return upload_nv12_now(t, nv12, w, h, stride, uv_off);
    if (!t->backend) return true;
    if (!nv12_format_ok(vk_format(t->format))) return false;
    size_t n = (size_t)uv_off + (size_t)stride * (h / 2);
    void *copy = malloc(n);
    memcpy(copy, nv12, n);
    rq_upload(t, RQ_UPLOAD_NV12, copy, w, h, stride, uv_off);
    return true;
}
void backend_upload_now(NvnTexture *t, int kind, void *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    if (kind == RQ_UPLOAD_RGBA) upload_rgba_now(t, data, w, h, stride);
    else upload_nv12_now(t, data, w, h, stride, uv_off);
    free(data);
}
static bool upload_nv12_now(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    TexBackend *b = t->backend;
    if (!b) return true;  // (as renderer_upload_rgba: a frame before the texture is first bound is dropped)
    if (!nv12_format_ok(b->format)) return false;
    size_t n = (size_t)uv_off + (size_t)stride * (h / 2);
    void *copy = malloc(n);
    memcpy(copy, nv12, n);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->pending_nv12 = true;
    b->pending_uv_off = uv_off;
    texture_prepare_gpu_owned(t, b);
    pthread_mutex_unlock(&pending_lock);
    return true;
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; VkImageUsageFlags usage; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    // (the slots the texture's views went into - a slot since taken by another texture's view is left alone - or the
    // whole table after more than eight: per-frame image swaps searched 16384 entries twice each)
    int n = b->vslot_overflow ? VIEW_TAB : b->nvslots;
    for (int k = 0; k < n; k++) {
        int i = b->vslot_overflow ? k : b->vslots[k];
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
    }
    b->nvslots = 0;
    b->vslot_overflow = false;
}

static VkImageView image_view_usage(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                                    uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect, VkImageUsageFlags usage) {
    fmt = vk_view_format(b, fmt);  // (g_vk.lean_usage: only the listed formats)
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^
                 (k2 * 0x165667B19E3779F9ull) ^ ((uint64_t)usage * 0x9E3779B97F4A7C15ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2 && e->usage == usage) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = usage ? &use : NULL,
                                .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v, usage};
    if (b->nvslots < 8) b->vslots[b->nvslots++] = (uint16_t)slot;
    else b->vslot_overflow = true;
    return v;
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    // A mutable sRGB sampled view must not inherit STORAGE usage from its UNORM
    // image. Storage views have their own cache key and STORAGE-only usage.
    VkImageUsageFlags usage = (b->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? b->usage & ~VK_IMAGE_USAGE_STORAGE_BIT : 0;
    return image_view_usage(b, type, fmt, lv0, nlv, ly0, nly, sw, aspect, usage);
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// Format views of a VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT image must stay in the image format's compatibility class:
// ordinary colour formats of the same texel size, a compressed format only with its own class (BCn / ASTC block size:
// unorm, snorm and sRGB twins). Anything else is invalid usage (the Metal side aborted on it: a texture pool slot whose
// texture was finalized and the address reused by another format, runtime/nvn/objects.c).
static int vk_color_bytes(VkFormat f) {  // texel size of an ordinary colour format, 0 = not one
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
        return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return 4;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}
static int vk_block_class(VkFormat f) {  // compressed formats: one id per compatibility class, 0 = not compressed
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return 1 + (f - VK_FORMAT_BC1_RGB_UNORM_BLOCK) / 2;
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return 100 + (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;
    return 0;
}
static bool vk_view_compatible(VkFormat image, VkFormat view) {
    if (image == view) return true;
    int ib = vk_color_bytes(image), vb = vk_color_bytes(view);
    if (ib || vb) return ib == vb;
    int ic = vk_block_class(image);
    return ic && ic == vk_block_class(view);
}

VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_textype,
                            VkFormat *actual_format) {
    if (actual_format) *actual_format = VK_FORMAT_UNDEFINED;
    if (!t || !storage_nvn_format(t->format)) return VK_NULL_HANDLE;
    if (!has_view) v = NULL;
    if (has_view && !v) return VK_NULL_HANDLE;
    if (v && (v->setmask & 32)) {
        int vt = v->target == 0 ? TEXTYPE_1D : v->target == 1 ? TEXTYPE_2D : v->target == 2 ? TEXTYPE_3D :
                 v->target == 3 ? TEXTYPE_1D_ARRAY : v->target == 4 ? TEXTYPE_2D_ARRAY : -1;
        if (vt != want_textype) return VK_NULL_HANDLE;
    }
    uint32_t requested = v && (v->setmask & 4) ? v->format : t->format;
    if (!storage_nvn_format(requested)) return VK_NULL_HANDLE;
    VkFormat fmt = vk_format(requested);
    if (actual_format) *actual_format = fmt;
    if (vk_storage_format_kind(fmt) < 0 || !storage_format_writable(fmt))
        return VK_NULL_HANDLE;
    TexBackend *b = vk_tex(t, false);  // preserve CPU contents before the first GPU store
    if (!b || !b->image || !(b->usage & VK_IMAGE_USAGE_STORAGE_BIT) || b->samples != 1 ||
        b->aspect != VK_IMAGE_ASPECT_COLOR_BIT || b->format != vk_format(t->format) ||
        !vk_view_compatible(b->format, fmt) || (b->fmt_listed && fmt != b->format && fmt != b->fmt_twin) ||
        vk_view_format(b, fmt) != fmt) return VK_NULL_HANDLE;
    // Storage coordinates address the original texels. Scaled allocations need
    // a separate coordinate contract rather than silently storing other pixels.
    uint32_t tw = t->width ? t->width : 1;
    uint32_t th = (t->target == 0 || t->target == 3) ? 1 : (t->height ? t->height : 1);
    if (b->width != tw || b->height != th) return VK_NULL_HANDLE;
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers :
                   (ly0 < b->layers ? b->layers - ly0 : 0);
    if (lv0 >= b->levels || ly0 >= b->layers || !nly || nly > b->layers - ly0) return VK_NULL_HANDLE;
    VkImageViewType want = view_type_for(want_textype), have = b->base_type;
    switch (want_textype) {
    case TEXTYPE_1D:
        if (have != VK_IMAGE_VIEW_TYPE_1D && have != VK_IMAGE_VIEW_TYPE_1D_ARRAY) return VK_NULL_HANDLE;
        nly = 1;
        break;
    case TEXTYPE_2D:
        if (have != VK_IMAGE_VIEW_TYPE_2D && have != VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
            have != VK_IMAGE_VIEW_TYPE_CUBE && have != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) return VK_NULL_HANDLE;
        nly = 1;
        break;
    case TEXTYPE_1D_ARRAY:
        if (have != VK_IMAGE_VIEW_TYPE_1D && have != VK_IMAGE_VIEW_TYPE_1D_ARRAY) return VK_NULL_HANDLE;
        break;
    case TEXTYPE_2D_ARRAY:
        if (have != VK_IMAGE_VIEW_TYPE_2D && have != VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
            have != VK_IMAGE_VIEW_TYPE_CUBE && have != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) return VK_NULL_HANDLE;
        break;
    case TEXTYPE_3D:
        if (have != VK_IMAGE_VIEW_TYPE_3D || ly0 || nly != 1) return VK_NULL_HANDLE;
        break;
    default: return VK_NULL_HANDLE;
    }
    // Storage images require identity component mappings regardless of the
    // sampled texture's swizzle; image stores write physical format channels.
    return image_view_usage(b, want, fmt, lv0, 1, ly0, nly, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_USAGE_STORAGE_BIT);
}

// Sampling combined depth/stencil textures selects one numeric domain. Keep
// this pure policy shared with the shader-key/descriptor classifier: stencil is
// raw UINT data, while depth is a floating-point sample.
bool vk_sampled_stencil(const NvnTexture *t, const NvnTextureView *v, bool has_view) {
    if (!t) return false;
    uint32_t mode = has_view && v && (v->setmask & 16) ? v->ds_mode : t->ds_mode;
    if (mode > 1) return false;
    return t->format == 0x31 || (mode == 1 && renderer_format_has_stencil(t->format));
}

// fmt_of deliberately has an RGBA8 fallback; it is not evidence that an
// unknown guest format is compatible with a float sampler.
static bool sampled_nvn_format_known(uint32_t f) {
    return (f >= 0x01 && f <= 0x19) || (f >= 0x25 && f <= 0x50) ||
           f == 0x6d || f == 0x71 || f == 0x76 || (f >= 0x79 && f <= 0x9b);
}

bool vk_sampled_float_compatible(NvnTexture *t, const NvnTextureView *v, bool has_view) {
    if (!t || !sampled_nvn_format_known(t->format) || vk_sampled_stencil(t, v, has_view)) return false;
    if (!has_view) v = NULL;
    uint32_t mode = v && (v->setmask & 16) ? v->ds_mode : t->ds_mode;
    if (renderer_format_is_depth(t->format) && mode != 0) return false;
    const TexBackend *b = t->backend;
    VkFormat base = fmt_of(t->format).vf, fmt = b ? b->format : base;
    bool depth = b ? (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
                   : renderer_format_is_depth(t->format);
    if (v && (v->setmask & 4) && !depth) {
        if (!sampled_nvn_format_known(v->format)) return false;
        VkFormat want = fmt_of(v->format).vf;
        // Same effective-format selection as vk_view, including fallback images
        // and explicit image-format lists. No image creation or guest reads.
        if (base == fmt && want != fmt && (!b || !b->fmt_listed || want == b->fmt_twin) &&
            vk_view_compatible(fmt, want)) fmt = want;
    }
    if (vk_storage_format_kind(fmt) == 1 || vk_storage_format_kind(fmt) == 2) return false;
    if ((fmt >= VK_FORMAT_BC1_RGBA_UNORM_BLOCK && fmt <= VK_FORMAT_BC7_SRGB_BLOCK) ||
        (fmt >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) return true;
    switch (fmt) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default: return false;
    }
}

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format && vf != fmt) {  // (fallback images keep their own format)
            if (b->fmt_listed && vf != b->fmt_twin) {
                static int nlog;
                if (nlog++ < 20)
                    LOG("[vk] texture %ux%u fmt %#x (vk %d) viewed as nvn %#x (vk %d) outside its format list: its own "
                        "format instead\n", t->width, t->height, t->format, b->format, v->format, vf);
            } else if (vk_view_compatible(fmt, vf)) fmt = vf;
            else {
                static int nlog;
                if (!nlog++)
                    LOG("[vk] texture %ux%u fmt %#x (vk %d) can't be viewed as nvn %#x (vk %d): its own format instead "
                        "(logged once)\n", t->width, t->height, t->format, b->format, v->format, vf);
            }
        }
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (render && b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
        // Core1.1 / maintenance1 attachment-only slice views. A 3D image has
        // arrayLayers=1, but this view's layer indexes Z in the SELECTED mip.
        // Validate before the ordinary array-layer clamps below can erase Z.
        if (!(b->create_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) ||
            !(b->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || b->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
            b->samples != 1 || !b->depth || lv0 >= b->levels) return VK_NULL_HANDLE;
        uint32_t slices = lv0 < 32 ? b->depth >> lv0 : 0;
        if (!slices) slices = 1;
        if (ly0 >= slices) return VK_NULL_HANDLE;
        VkImageView slice = image_view_usage(b, VK_IMAGE_VIEW_TYPE_2D_ARRAY, fmt, lv0, 1, ly0, 1,
                                            IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        static unsigned logged_slices;
        if (slice && logged_slices < 8) {
            logged_slices++;
            LOG("[vk] 3D color attachment %u: texture %llu %ux%ux%u mip %u Z %u (mip depth %u)\n",
                logged_slices, (unsigned long long)t->debug_id, b->width, b->height, b->depth, lv0, ly0, slices);
        }
        return slice;
    }
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    if (renderer_format_is_depth(t->format)) {
        uint32_t mode = v && (v->setmask & 16) ? v->ds_mode : t->ds_mode;
        if (mode > 1) return VK_NULL_HANDLE;
        if (vk_sampled_stencil(t, v, v != NULL)) {
            if (!(b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) return VK_NULL_HANDLE;
            aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        } else if (mode == 1) return VK_NULL_HANDLE; // a depth-only format has no stencil plane
    }
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    rec_CmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// Depth compare (sampler*Shadow) needs a view whose format has SAMPLED_IMAGE_DEPTH_COMPARISON: the RGBA8 dummy above
// has not (VUID-vkCmdDrawIndexed-None-06479). Shadow slots with nothing usable bound get a 1x1 depth image instead,
// cleared to 1.0: the farthest depth, so a less / less-equal compare reads it as lit (no shadow), as an empty shadow
// map would. D32_SFLOAT where it can be compared, else D16_UNORM. 3D (no shadow form) and 1D (depth 1D images are not
// required) types keep the colour dummy.
bool vk_format_depth_compare(VkFormat f) {
    static VkFormat fs[16];
    static int8_t ok[16];
    static int n;
    for (int i = 0; i < n; i++) if (fs[i] == f) return ok[i];
    VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
    vkGetPhysicalDeviceFormatProperties2(g_vk.phys, f, &p2);
    bool r = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
    if (n < 16) { fs[n] = f; ok[n++] = r; }
    return r;
}

// t's sampled image can be read with a depth compare (a depth image of a comparable format)
bool vk_tex_depth_compare(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && vk_format_depth_compare(b->format);
}

VkImageView vk_dummy_depth_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    VkImageViewType vt = view_type_for(textype);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY && vt != VK_IMAGE_VIEW_TYPE_CUBE &&
        vt != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        return vk_dummy_view(textype);
    static VkFormat df;
    if (!df) {
        df = vk_format_depth_compare(VK_FORMAT_D32_SFLOAT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
        LOG("[vk] shadow slot dummy: 1x1 %s cleared to 1.0\n", df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    uint32_t layers = cube ? 6 : 1;
    TexBackend *b = &dummies[textype];
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D, .format = df, .extent = {1, 1, 1}, .mipLevels = 1,
                            .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) {
        LOG("[vk] shadow slot dummy: depth image creation failed, the colour dummy instead\n");
        return views[textype] = vk_dummy_view(textype);
    }
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue one = {1.0f, 0};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    rec_CmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &all);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    b->format = df;
    b->levels = 1;
    b->layers = layers;
    b->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    views[textype] = vk_image_view(b, vt, df, 0, 1, 0, layers, IDENTITY, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!views[textype]) views[textype] = vk_dummy_view(textype);
    return views[textype];
}

// Throwaway color attachment for passes that have none (exec.c begin_pass, Intel only): R8, at least w x h (the pass's
// render area: the attachments' level extent, which Dynamic resolution's sub-rect lies inside), never sampled; its
// contents are don't-care. One image per sample count, grown to the largest render area seen (the outgrown one is
// destroyed once the submissions that may use it are done), so changing render scales don't pile up images. Only the
// image is kept: its view comes from the view table on each use, like a game target's (a cached handle could be
// evicted from that table and destroyed under us). Called by the translating thread only (the render thread, or the
// game's GPU thread with LARECOMP_RENDER_THREAD=0), as vk_dummy_view and the view table are.
VkImageView vk_dummy_color_view(uint32_t w, uint32_t h, uint32_t samples) {
    static TexBackend imgs[5];  // samples 1, 2, 4, 8, 16
    int k = 0;
    while (k < 4 && (1u << k) < samples) k++;
    TexBackend *b = &imgs[k];
    if (!b->image || b->width < w || b->height < h || b->samples != samples) {
        if (b->width > w) w = b->width;
        if (b->height > h) h = b->height;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage img;
        VmaAllocation mem;
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
        if (b->image) { views_forget(b); defer((Dead){.img = b->image, .mem = b->mem}); }
        vk_image_init_layout(vk_upload_cmd(), img, VK_IMAGE_ASPECT_COLOR_BIT);
        *b = (TexBackend){.image = img, .mem = mem, .format = VK_FORMAT_R8_UNORM, .base_type = VK_IMAGE_VIEW_TYPE_2D,
                          .width = w, .height = h, .depth = 1, .layers = 1, .levels = 1, .samples = samples,
                          .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .gpu_owned = true};
        LOG("[vk] placeholder color attachment %ux%u (samples %u) for passes without one\n", w, h, samples);
    }
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend) return (VkSampler)d->backend;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    d->backend = (void *)s;
    return s;
}

// Integer sampled images cannot use linear filtering or anisotropy. Cache the
// specialization separately: descriptor storage can be reused or mutated, and
// d->backend remains the original floating-point sampler. Like vk_sampler, this
// cache belongs to the renderer thread and the fresh game-process lifetime.
#define UINT_SAMPLER_CAP 1024
typedef struct { NvnSamplerDesc key; VkSampler sampler; } UintSamplerEntry;
static UintSamplerEntry uint_samplers[UINT_SAMPLER_CAP];
static uint32_t uint_sampler_count;

VkSampler vk_sampler_uint(NvnSamplerDesc *d) {
    if (!d || d->compare_mode) return VK_NULL_HANDLE;
    NvnSamplerDesc key = *d;
    key.backend = NULL;
    for (uint32_t i = 0; i < uint_sampler_count; i++)
        if (!memcmp(&uint_samplers[i].key, &key, sizeof key)) return uint_samplers[i].sampler;
    // Do not evict a sampler that submitted descriptors can still reference.
    if (uint_sampler_count == UINT_SAMPLER_CAP) return VK_NULL_HANDLE;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: return VK_NULL_HANDLE;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (d->min_filter <= 1) si.maxLod = si.minLod + 0.25f;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    si.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        // NVN SetBorderColori/ui preserve these exact component bits. Sampling
        // an integer image consumes integer components, never float conversion.
        uint32_t c[4]; memcpy(c, d->border, sizeof c);
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.uint32, c, sizeof c);
            si.borderColor = VK_BORDER_COLOR_INT_CUSTOM_EXT;
            si.pNext = &cb;
        } else return VK_NULL_HANDLE;
    }
    VkSampler sampler;
    if (vkCreateSampler(g_vk.dev, &si, NULL, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    uint_samplers[uint_sampler_count++] = (UintSamplerEntry){key, sampler};
    return sampler;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}

void backend_texture_view_format(NvnTexture *t, uint32_t format) { (void)t; (void)format; }  // (images are made mutable-format)
