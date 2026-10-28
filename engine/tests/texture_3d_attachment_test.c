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
static VkImageUsageFlags last_usage;static int fail_view;
#define CHECK(c) do { checks++; if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#define LOG(...) ((void)0)
static struct { VkDevice dev; } g_vk;
uint64_t prof_views_created;
typedef struct { VkImageView view; } Dead;
static void defer(Dead d){CHECK(d.view!=VK_NULL_HANDLE);deferred++;}
static VkFormat depth24_stencil8(void){return VK_FORMAT_D32_SFLOAT_S8_UINT;}
static VkImageViewCreateInfo last_view;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice dev,const VkImageViewCreateInfo *vi,const VkAllocationCallbacks *alloc,VkImageView *out){
    (void)dev;(void)alloc;if(fail_view){fail_view=0;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}last_view=*vi;last_usage=vi->pNext?((const VkImageViewUsageCreateInfo*)vi->pNext)->usage:0;*out=(VkImageView)(uintptr_t)(++created+100);return VK_SUCCESS;
}
static TexBackend *vk_tex(NvnTexture *t,bool render){(void)render;uploads++;return t->backend;}
static VkImageView vk_dummy_view(int type){(void)type;dummies++;return (VkImageView)(uintptr_t)99999;}
#include "production_formats.inc"
static VkFormat vk_format(uint32_t f){return fmt_of(f).vf;}
static void views_forget(TexBackend *);
#include "production_views.inc"

int main(void){
 for(unsigned api=0;api<3;api++)for(unsigned type=0;type<3;type++)for(unsigned sm=1;sm<=4;sm*=2)for(unsigned depth=0;depth<2;depth++)for(unsigned color=0;color<2;color++){
  uint32_t version=api==0?VK_API_VERSION_1_0:api==1?VK_API_VERSION_1_1:VK_API_VERSION_1_3;
  VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|(color?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0);
  CHECK(color_slice_create_flags(type,usage,sm,depth,version)==((type==VK_IMAGE_TYPE_3D&&api&&sm==1&&!depth&&color)?VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT:0));
 }
 NvnTexture t={.debug_id=17,.format=0x25,.target=2};
 TexBackend b={.image=(VkImage)(uintptr_t)1,.format=VK_FORMAT_R8G8B8A8_UNORM,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,.base_type=VK_IMAGE_VIEW_TYPE_3D,.width=64,.height=32,.layers=1,.levels=7,.samples=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.create_flags=VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT};t.backend=&b;
 for(unsigned depth=1;depth<=65;depth++){
  b.depth=depth;memset(view_tab,0,sizeof view_tab);b.nvslots=0;b.vslot_overflow=false;
  for(unsigned mip=0;mip<b.levels;mip++){
   unsigned slices=depth>>mip;if(!slices)slices=1;
   for(unsigned z=0;z<slices;z++){
    NvnTextureView v={.setmask=1|2,.base_level=mip,.num_levels=1,.min_layer=z,.num_layers=1};
    VkImageView view=vk_view(&t,&v,true,true,TEXTYPE_2D);CHECK(view);
    CHECK(last_view.viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY);CHECK(last_view.subresourceRange.baseMipLevel==mip);CHECK(last_view.subresourceRange.baseArrayLayer==z);
    CHECK(last_view.subresourceRange.levelCount==1&&last_view.subresourceRange.layerCount==1);CHECK(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(last_view.components.r==VK_COMPONENT_SWIZZLE_IDENTITY&&last_view.components.a==VK_COMPONENT_SWIZZLE_IDENTITY);
    CHECK(last_usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);unsigned before=created;CHECK(vk_view(&t,&v,true,true,TEXTYPE_2D)==view);CHECK(created==before);
   }
   unsigned before=created;NvnTextureView v={.setmask=1|2,.base_level=mip,.min_layer=slices};CHECK(!vk_view(&t,&v,true,true,TEXTYPE_2D));CHECK(created==before);
  }
  unsigned before=created;NvnTextureView v={.setmask=1,.base_level=b.levels};CHECK(!vk_view(&t,&v,true,true,TEXTYPE_2D));CHECK(created==before);
 }
 b.depth=17;NvnTextureView v={.setmask=1|2,.base_level=1,.min_layer=7};
 #define BAD(field,value) do{__typeof__(b.field) old=b.field;b.field=value;unsigned before=created;CHECK(!vk_view(&t,&v,true,true,TEXTYPE_2D));CHECK(created==before);b.field=old;}while(0)
 BAD(create_flags,0);BAD(usage,VK_IMAGE_USAGE_SAMPLED_BIT);BAD(samples,4);BAD(depth,0);BAD(levels,0);BAD(aspect,VK_IMAGE_ASPECT_DEPTH_BIT);BAD(aspect,VK_IMAGE_ASPECT_STENCIL_BIT);
 memset(view_tab,0,sizeof view_tab);fail_view=1;CHECK(!vk_view(&t,&v,true,true,TEXTYPE_2D));CHECK(vk_view(&t,&v,true,true,TEXTYPE_2D));
 // Sampled3D views retain Vulkan arrayLayer0/count1; slice support is attachment-only.
 memset(view_tab,0,sizeof view_tab);v=(NvnTextureView){0};CHECK(vk_view(&t,&v,true,false,TEXTYPE_3D));CHECK(last_view.viewType==VK_IMAGE_VIEW_TYPE_3D);CHECK(last_view.subresourceRange.baseArrayLayer==0&&last_view.subresourceRange.layerCount==1);
 CHECK(vk_view(&t,&v,true,false,TEXTYPE_2D)==(VkImageView)(uintptr_t)99999);
 // Existing 2D attachment path preserves its own view and broad usage policy.
 memset(view_tab,0,sizeof view_tab);b.base_type=VK_IMAGE_VIEW_TYPE_2D;b.depth=1;b.create_flags=0;CHECK(vk_view(&t,NULL,false,true,TEXTYPE_2D));CHECK(last_view.viewType==VK_IMAGE_VIEW_TYPE_2D);CHECK(last_view.subresourceRange.layerCount==1);
 CHECK(uploads>0);CHECK(dummies==1);CHECK(deferred==0);
 printf("%u actual3D slice-view/creation-flag assertions passed\n",checks);return 0;
}
