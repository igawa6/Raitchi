/* Actual production sample-aspect policy/cache with controlled Vulkan calls. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "production_types.inc"
enum {TEXTYPE_1D,TEXTYPE_2D,TEXTYPE_3D,TEXTYPE_CUBE,TEXTYPE_2D_ARRAY,TEXTYPE_CUBE_ARRAY,TEXTYPE_1D_ARRAY,TEXTYPE_BUFFER,TEXTYPE_2D_MS};
static unsigned checks,created,deferred,dummies,uploads;
#define CHECK(c) do { checks++; if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#define LOG(...) ((void)0)
static struct { VkDevice dev; } g_vk;
uint64_t prof_views_created;
typedef struct { VkImageView view; } Dead;
static void defer(Dead d){CHECK(d.view!=VK_NULL_HANDLE);deferred++;}
static VkFormat depth24_stencil8(void){return VK_FORMAT_D32_SFLOAT_S8_UINT;}
static VkImageViewCreateInfo last_view;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice dev,const VkImageViewCreateInfo *vi,const VkAllocationCallbacks *alloc,VkImageView *out){
    (void)dev;(void)alloc;last_view=*vi;*out=(VkImageView)(uintptr_t)(++created+100);return VK_SUCCESS;
}
static TexBackend *vk_tex(NvnTexture *t,bool render){(void)render;uploads++;return t->backend;}
static VkImageView vk_dummy_view(int type){(void)type;dummies++;return (VkImageView)(uintptr_t)99999;}
#include "production_formats.inc"
static VkFormat vk_format(uint32_t f){return fmt_of(f).vf;}
static void views_forget(TexBackend *);
#include "production_views.inc"
static NvnTexture texture;
static TexBackend backend;
static void reset(uint32_t f){
    memset(view_tab,0,sizeof view_tab);memset(&texture,0,sizeof texture);memset(&backend,0,sizeof backend);
    texture=(NvnTexture){.backend=&backend,.format=f,.width=64,.height=32,.levels=3,.samples=1,.target=1};
    backend=(TexBackend){.image=(VkImage)(uintptr_t)1,.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .base_type=VK_IMAGE_VIEW_TYPE_2D,.width=64,.height=32,.depth=1,.layers=1,.levels=3,.samples=1,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT};
}
static VkImageView sample(NvnTextureView *v,bool has,int type){return vk_view(&texture,v,has,false,type);}
int main(void){
    for(unsigned f=0x35;f<=0x36;f++){
        reset(f);NvnTextureView v={.setmask=16,.ds_mode=1};
        CHECK(!vk_sampled_stencil(&texture,NULL,false));
        VkImageView depth=sample(NULL,false,TEXTYPE_2D);CHECK(depth&&last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageView stencil=sample(&v,true,TEXTYPE_2D);CHECK(stencil&&stencil!=depth);
        CHECK(vk_sampled_stencil(&texture,&v,true));
        CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT&&last_view.format==VK_FORMAT_D32_SFLOAT_S8_UINT);
        CHECK(last_view.components.r==VK_COMPONENT_SWIZZLE_IDENTITY&&last_view.components.a==VK_COMPONENT_SWIZZLE_IDENTITY);
        unsigned before=created;
        for(unsigned i=0;i<8;i++){CHECK(sample(NULL,false,TEXTYPE_2D)==depth);CHECK(sample(&v,true,TEXTYPE_2D)==stencil);}
        CHECK(created==before);
        texture.ds_mode=1;CHECK(sample(NULL,false,TEXTYPE_2D)==stencil);
        v.ds_mode=0;CHECK(!vk_sampled_stencil(&texture,&v,true));CHECK(sample(&v,true,TEXTYPE_2D)==depth);
        CHECK(vk_sampled_stencil(&texture,&v,false));CHECK(sample(&v,false,TEXTYPE_2D)==stencil);
        v.setmask=0;CHECK(vk_sampled_stencil(&texture,&v,true));CHECK(sample(&v,true,TEXTYPE_2D)==stencil);
        v=(NvnTextureView){.setmask=16,.ds_mode=0};
        VkImageView attachment=vk_view(&texture,&v,true,true,TEXTYPE_2D);
        CHECK(attachment&&attachment!=depth&&attachment!=stencil);
        CHECK(last_view.subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));
        CHECK(last_view.subresourceRange.levelCount==1&&last_view.subresourceRange.layerCount==1);
        v.ds_mode=1;CHECK(vk_view(&texture,&v,true,true,TEXTYPE_2D)==attachment);
        v=(NvnTextureView){.setmask=16,.ds_mode=1};
        VkImageView array=sample(&v,true,TEXTYPE_2D_ARRAY);CHECK(array&&array!=stencil);
        CHECK(last_view.viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY&&last_view.subresourceRange.layerCount==1);
        CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT);
        backend.base_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;backend.layers=5;
        v=(NvnTextureView){.setmask=16|1|2,.ds_mode=1,.base_level=1,.num_levels=2,.min_layer=2,.num_layers=3};
        CHECK(sample(&v,true,TEXTYPE_2D_ARRAY));
        CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT&&last_view.subresourceRange.baseMipLevel==1&&last_view.subresourceRange.levelCount==2);
        CHECK(last_view.subresourceRange.baseArrayLayer==2&&last_view.subresourceRange.layerCount==3);
        before=created;v.ds_mode=2;CHECK(!vk_sampled_stencil(&texture,&v,true));CHECK(!sample(&v,true,TEXTYPE_2D));CHECK(created==before);
        v.ds_mode=1;backend.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;CHECK(!sample(&v,true,TEXTYPE_2D));CHECK(created==before);
        backend.aspect=VK_IMAGE_ASPECT_COLOR_BIT;CHECK(!sample(&v,true,TEXTYPE_2D));CHECK(created==before);
    }
    reset(0x31);backend.format=VK_FORMAT_S8_UINT;backend.aspect=VK_IMAGE_ASPECT_STENCIL_BIT;
    CHECK(vk_sampled_stencil(&texture,NULL,false));CHECK(sample(NULL,false,TEXTYPE_2D));CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT);
    NvnTextureView v={.setmask=16,.ds_mode=0};CHECK(vk_sampled_stencil(&texture,&v,true));v.ds_mode=1;CHECK(vk_sampled_stencil(&texture,&v,true));
    for(unsigned f=0x32;f<=0x34;f++){
        reset(f);backend.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;backend.format=VK_FORMAT_D32_SFLOAT;
        CHECK(sample(NULL,false,TEXTYPE_2D));CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
        unsigned before=created;v=(NvnTextureView){.setmask=16,.ds_mode=1};
        CHECK(!vk_sampled_stencil(&texture,&v,true));CHECK(!sample(&v,true,TEXTYPE_2D));CHECK(created==before);
    }
    reset(0x25);backend.format=VK_FORMAT_R8G8B8A8_UNORM;backend.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    texture.ds_mode=1;texture.swizzle[0]=4;texture.swizzle[1]=3;texture.swizzle[2]=2;texture.swizzle[3]=5;
    CHECK(!vk_sampled_stencil(&texture,NULL,false));CHECK(sample(NULL,false,TEXTYPE_2D));
    CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&last_view.components.r==VK_COMPONENT_SWIZZLE_B&&last_view.components.b==VK_COMPONENT_SWIZZLE_R);
    CHECK(!vk_sampled_stencil(NULL,NULL,false));CHECK(dummies==0);CHECK(uploads>0);
    unsigned before=deferred;vk_views_forget(&backend);CHECK(deferred>before);
    printf("{\"state\":\"passed\",\"checks\":%u,\"views_created\":%u,\"depth_stencil_cache_separate\":true,\"controlled_vulkan\":true}\n",checks,created);
}
