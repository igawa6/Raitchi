/* Actual production view/cache policy; Vulkan calls are explicit controlled stand-ins. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "production_types.inc"
enum {TEXTYPE_1D,TEXTYPE_2D,TEXTYPE_3D,TEXTYPE_CUBE,TEXTYPE_2D_ARRAY,TEXTYPE_CUBE_ARRAY,TEXTYPE_1D_ARRAY,TEXTYPE_BUFFER,TEXTYPE_2D_MS};
static unsigned checks, created, uploads, deferred;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
#define LOG(...) ((void)0)
static struct { VkPhysicalDevice phys; VkDevice dev; uint32_t api; void *vma; } g_vk;
static VkFormat refused = VK_FORMAT_UNDEFINED;
static bool no_storage, no_formatless, fail_create;
static VkImageViewCreateInfo last_view;
static VkImageViewUsageCreateInfo last_usage;
uint64_t prof_views_created;
typedef struct { VkImageView view; } Dead;
static void defer(Dead d) { CHECK(d.view != VK_NULL_HANDLE); deferred++; }
static VkFormat depth24_stencil8(void) { return VK_FORMAT_D32_SFLOAT_S8_UINT; }
static bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out) {
    (void)p; memset(out,0,sizeof *out);
    if(f != refused) out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | (no_storage ? 0 : VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice p, VkFormat f, VkFormatProperties2 *out) {
    (void)p; VkFormatProperties3 *p3=out->pNext;
    CHECK(p3 && p3->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);
    p3->optimalTilingFeatures = f == refused ? 0 : (no_storage ? 0 : VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) |
        (no_formatless ? 0 : VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice dev,const VkImageViewCreateInfo *vi,const VkAllocationCallbacks *alloc,VkImageView *out) {
    (void)dev;(void)alloc; last_view=*vi; memset(&last_usage,0,sizeof last_usage);
    if(vi->pNext) last_usage=*(const VkImageViewUsageCreateInfo *)vi->pNext;
    if(fail_create) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=(VkImageView)(uintptr_t)(++created+100); return VK_SUCCESS;
}
static TexBackend *vk_tex(NvnTexture *t,bool render) { CHECK(!render); uploads++; return t->backend; }
typedef struct { unsigned unused; } VmaAllocationCreateInfo;
static unsigned allocation_calls;
static VkResult allocation_result[2];
static VkImageCreateInfo allocation_inputs[2];
static VkResult vmaCreateImage(void *allocator,const VkImageCreateInfo *ci,const VmaAllocationCreateInfo *ai,
                               VkImage *image,VmaAllocation *mem,void *allocation_info) {
    (void)allocator;(void)ai;(void)image;(void)mem;(void)allocation_info;
    CHECK(allocation_calls<2); allocation_inputs[allocation_calls]=*ci;
    return allocation_result[allocation_calls++];
}
#include "production_allocation.inc"
#include "production_formats.inc"
static VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
static void views_forget(TexBackend *);
#include "production_views.inc"
static VkImageUsageFlags creation_usage(NvnTexture *t,VkFormat vf,uint32_t samples) {
    VkFmt mf=fmt_of(t->format);
#include "production_usage.inc"
}
static TexBackend backend;
static NvnTexture texture;
/* Independent Vulkan packed-format reference: PACK16 names run MSB to LSB.
 * NVN RGB565's original file format puts R in bits0..4. No game bytes needed. */
static float packed_alpha(VkFormat format,uint16_t pixel,float outline_alpha) {
    CHECK(format==VK_FORMAT_R5G6B5_UNORM_PACK16||format==VK_FORMAT_B5G6R5_UNORM_PACK16);
    unsigned red=format==VK_FORMAT_B5G6R5_UNORM_PACK16?pixel&31:pixel>>11;
    unsigned green=(pixel>>5)&63;
    float alpha=red/31.f+green/63.f*(1.f-outline_alpha);
    return alpha>1.f?1.f:alpha;
}
static void reset_texture(void) {
    memset(view_tab,0,sizeof view_tab); memset(&backend,0,sizeof backend); memset(&texture,0,sizeof texture);
    texture=(NvnTexture){.backend=&backend,.format=0x25,.width=64,.height=32,.levels=4,.samples=1,.target=1};
    backend=(TexBackend){.image=(VkImage)(uintptr_t)1,.format=VK_FORMAT_R8G8B8A8_UNORM,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .base_type=VK_IMAGE_VIEW_TYPE_2D,.width=64,.height=32,.depth=1,.layers=1,.levels=4,.samples=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    g_vk.api=VK_API_VERSION_1_3; refused=VK_FORMAT_UNDEFINED; no_storage=no_formatless=fail_create=false;
}
int main(void) {
    reset_texture(); VkFormat actual;
    VkFormat rgb565=vk_format(0x3c);
    CHECK(rgb565==VK_FORMAT_B5G6R5_UNORM_PACK16);
    CHECK(vk_format(0x3a)==VK_FORMAT_R5G6B5_UNORM_PACK16); /* separate format unchanged */
    CHECK(vk_format(0x0d)==VK_FORMAT_R8G8_UNORM);
    CHECK(vk_format(0x87)==VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    CHECK(packed_alpha(rgb565,0x001f,0)==1.f); /* red low bits: filled */
    CHECK(packed_alpha(rgb565,0xf800,0)==0.f); /* blue high bits: transparent */
    CHECK(packed_alpha(rgb565,0x07e0,0)==1.f); /* green edge */
    CHECK(packed_alpha(rgb565,0x07e0,1)==0.f);
    CHECK(packed_alpha(VK_FORMAT_R5G6B5_UNORM_PACK16,0x001f,0)==0.f);
    CHECK(packed_alpha(VK_FORMAT_R5G6B5_UNORM_PACK16,0xf800,0)==1.f); /* old mapping reverses alpha */
    VkImageCreateInfo allocation={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.format=VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,.extent={64,32,1},.mipLevels=4,.arrayLayers=2};
    VmaAllocationCreateInfo ai={0};
    allocation_result[0]=VK_ERROR_FORMAT_NOT_SUPPORTED; allocation_result[1]=VK_SUCCESS;
    CHECK(create_image_allocation(&allocation,&ai,&backend)==VK_SUCCESS && allocation_calls==2);
    CHECK(!(allocation.usage&VK_IMAGE_USAGE_STORAGE_BIT));
    VkImageCreateInfo expected=allocation_inputs[0];expected.usage &=~VK_IMAGE_USAGE_STORAGE_BIT;
    CHECK(!memcmp(&expected,&allocation_inputs[1],sizeof expected));
    allocation_calls=0;allocation_result[0]=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(create_image_allocation(&allocation,&ai,&backend)==VK_ERROR_OUT_OF_DEVICE_MEMORY && allocation_calls==1);
    allocation_calls=0;allocation.usage |=VK_IMAGE_USAGE_STORAGE_BIT;allocation_result[1]=VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(create_image_allocation(&allocation,&ai,&backend)==VK_ERROR_OUT_OF_HOST_MEMORY && allocation_calls==2);
    allocation_calls=0;allocation.usage |=VK_IMAGE_USAGE_STORAGE_BIT;allocation_result[0]=VK_SUCCESS;
    CHECK(create_image_allocation(&allocation,&ai,&backend)==VK_SUCCESS && allocation_calls==1);
    CHECK(vk_storage_format_kind(VK_FORMAT_R16_SFLOAT)==0);
    CHECK(vk_storage_format_kind(VK_FORMAT_R8_SNORM)==0);
    CHECK(vk_storage_format_kind(VK_FORMAT_R32_SINT)==1);
    CHECK(vk_storage_format_kind(VK_FORMAT_R8G8B8A8_UINT)==2);
    CHECK(vk_storage_format_kind(VK_FORMAT_R8G8B8A8_SRGB)==-1);
    CHECK(vk_storage_format_kind(VK_FORMAT_BC1_RGBA_UNORM_BLOCK)==-1);
    CHECK(creation_usage(&texture,backend.format,1)&VK_IMAGE_USAGE_STORAGE_BIT);
    no_storage=true; CHECK(!(creation_usage(&texture,backend.format,1)&VK_IMAGE_USAGE_STORAGE_BIT)); no_storage=false;
    CHECK(!(creation_usage(&texture,VK_FORMAT_R16_SFLOAT,1)&VK_IMAGE_USAGE_STORAGE_BIT));
    CHECK(!(creation_usage(&texture,backend.format,4)&VK_IMAGE_USAGE_STORAGE_BIT));
    texture.format=0xffff; CHECK(!(creation_usage(&texture,backend.format,1)&VK_IMAGE_USAGE_STORAGE_BIT)); reset_texture();
    VkImageView stored=vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual);
    CHECK(stored && actual==backend.format && last_usage.usage==VK_IMAGE_USAGE_STORAGE_BIT);
    CHECK(last_view.subresourceRange.levelCount==1 && last_view.subresourceRange.layerCount==1);
    CHECK(last_view.components.r==VK_COMPONENT_SWIZZLE_IDENTITY && last_view.components.a==VK_COMPONENT_SWIZZLE_IDENTITY);
    unsigned before=created; CHECK(vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)==stored && created==before);
    VkImageView sampled=vk_image_view(&backend,VK_IMAGE_VIEW_TYPE_2D,backend.format,0,1,0,1,IDENTITY,VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(sampled && sampled!=stored && !(last_usage.usage&VK_IMAGE_USAGE_STORAGE_BIT));
    CHECK(vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)==stored);
    backend.fmt_listed=true; backend.fmt_twin=VK_FORMAT_R8G8B8A8_SRGB;
    CHECK(vk_image_view(&backend,VK_IMAGE_VIEW_TYPE_2D,backend.fmt_twin,0,1,0,1,IDENTITY,VK_IMAGE_ASPECT_COLOR_BIT));
    CHECK(last_view.format==VK_FORMAT_R8G8B8A8_SRGB && !(last_usage.usage&VK_IMAGE_USAGE_STORAGE_BIT));
    unsigned old=created; views_forget(&backend); CHECK(deferred>=3 && !backend.nvslots);
    CHECK(vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual) && created==old+1);
    NvnTextureView v={.setmask=1,.base_level=2,.num_levels=2};
    CHECK(vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));
    CHECK(last_view.subresourceRange.baseMipLevel==2 && last_view.subresourceRange.levelCount==1);
    v.base_level=4; CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));
    CHECK(!vk_storage_view(NULL,NULL,false,TEXTYPE_2D,&actual));
    CHECK(!vk_storage_view(&texture,NULL,true,TEXTYPE_2D,&actual));
    no_formatless=true; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)); no_formatless=false;
    no_storage=true; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)); no_storage=false;
    g_vk.api=VK_API_VERSION_1_2; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)); g_vk.api=VK_API_VERSION_1_3;
    backend.width=128; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)); backend.width=64;
    backend.samples=4; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual)); backend.samples=1;
    backend.usage &=~VK_IMAGE_USAGE_STORAGE_BIT; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual));backend.usage |=VK_IMAGE_USAGE_STORAGE_BIT;
    backend.aspect=VK_IMAGE_ASPECT_DEPTH_BIT; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual));reset_texture();
    for(unsigned fmt=0;fmt<0xa0;fmt++) {
        if(storage_nvn_format(fmt)) continue;
        texture.format=fmt; CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual));
    }
    reset_texture(); v=(NvnTextureView){.setmask=4,.format=0x27};
    CHECK(vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual) && vk_storage_format_kind(actual)==2);
    v.format=0x05; CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));
    v.format=0x38; CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));
    backend.fmt_listed=true;v.format=0x27; CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));reset_texture();
    v=(NvnTextureView){.setmask=32,.target=2};CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D,&actual));
    for(int type=0;type<9;type++) {
        reset_texture();
        if(type==TEXTYPE_1D||type==TEXTYPE_1D_ARRAY){texture.target=3;texture.height=4;backend.height=1;backend.layers=4;backend.base_type=VK_IMAGE_VIEW_TYPE_1D_ARRAY;}
        if(type==TEXTYPE_2D_ARRAY){texture.target=4;backend.layers=5;backend.base_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;}
        if(type==TEXTYPE_3D){texture.target=2;texture.depth=4;backend.depth=4;backend.base_type=VK_IMAGE_VIEW_TYPE_3D;}
        VkImageView image=vk_storage_view(&texture,NULL,false,type,&actual);
        bool supported=type==TEXTYPE_1D||type==TEXTYPE_2D||type==TEXTYPE_3D||type==TEXTYPE_1D_ARRAY||type==TEXTYPE_2D_ARRAY;
        CHECK(!!image==supported);
    }
    reset_texture();backend.base_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;backend.layers=5;
    v=(NvnTextureView){.setmask=2,.min_layer=2,.num_layers=3};CHECK(vk_storage_view(&texture,&v,true,TEXTYPE_2D_ARRAY,&actual));
    CHECK(last_view.subresourceRange.baseArrayLayer==2&&last_view.subresourceRange.layerCount==3);
    v.num_layers=4;CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D_ARRAY,&actual));v.num_layers=1;v.min_layer=5;
    CHECK(!vk_storage_view(&texture,&v,true,TEXTYPE_2D_ARRAY,&actual));
    reset_texture();fail_create=true;CHECK(!vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual));
    fail_create=false;CHECK(vk_storage_view(&texture,NULL,false,TEXTYPE_2D,&actual));
    printf("{\"state\":\"passed\",\"checks\":%u,\"views_created\":%u,\"cpu_upload_calls\":%u,\"usage_cache_separation\":true,\"unsupported_never_dummy\":true}\n",checks,created,uploads);
    return 0;
}
