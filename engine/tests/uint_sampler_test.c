/* Execute the real float and UINT sampler paths with a recording Vulkan stand-in. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "production_types.inc"
static unsigned checks,created,calls;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define VK_CHECK(c) CHECK((c)==VK_SUCCESS)
static struct{VkDevice dev;bool custom_border;}g_vk;
static VkSamplerCreateInfo last;
static VkSamplerCustomBorderColorCreateInfoEXT custom;
static bool fail_create;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice dev,const VkSamplerCreateInfo *ci,const VkAllocationCallbacks *a,VkSampler *s){
    (void)dev;(void)a;calls++;last=*ci;memset(&custom,0,sizeof custom);if(ci->pNext)custom=*(const VkSamplerCustomBorderColorCreateInfoEXT *)ci->pNext;
    if(fail_create)return VK_ERROR_OUT_OF_HOST_MEMORY;*s=(VkSampler)(uintptr_t)(++created+100);return VK_SUCCESS;
}
#include "production_samplers.inc"
static void reset(void){memset(uint_samplers,0,sizeof uint_samplers);uint_sampler_count=0;g_vk.custom_border=false;fail_create=false;}
static NvnSamplerDesc descriptor(void){return (NvnSamplerDesc){.min_filter=5,.mag_filter=1,.wrap={0,1,6},.lod_min=2,.lod_max=7,.lod_bias=0.5f,.max_aniso=16};}
int main(void){
    reset();NvnSamplerDesc d=descriptor(),before=d;VkSampler u=vk_sampler_uint(&d);CHECK(u);
    CHECK(last.magFilter==VK_FILTER_NEAREST&&last.minFilter==VK_FILTER_NEAREST&&last.mipmapMode==VK_SAMPLER_MIPMAP_MODE_NEAREST);
    CHECK(!last.anisotropyEnable&&last.maxAnisotropy==0&&!last.compareEnable);
    CHECK(last.addressModeU==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE&&last.addressModeV==VK_SAMPLER_ADDRESS_MODE_REPEAT&&last.addressModeW==VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
    CHECK(last.minLod==2&&last.maxLod==7&&last.mipLodBias==0.5f);
    CHECK(last.borderColor==VK_BORDER_COLOR_INT_TRANSPARENT_BLACK);CHECK(!memcmp(&before,&d,sizeof d));
    unsigned count=calls;CHECK(vk_sampler_uint(&d)==u&&calls==count);
    NvnSamplerDesc other=d;other.backend=(void *)(uintptr_t)77;CHECK(vk_sampler_uint(&other)==u&&calls==count);
    VkSampler f=vk_sampler(&d);CHECK(f&&f!=u&&d.backend==(void *)f);
    CHECK(last.magFilter==VK_FILTER_LINEAR&&last.minFilter==VK_FILTER_LINEAR&&last.mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR&&last.anisotropyEnable);
    count=calls;CHECK(vk_sampler_uint(&d)==u&&calls==count);CHECK(vk_sampler(&d)==f&&calls==count);
    d.lod_bias=0.75f;VkSampler changed=vk_sampler_uint(&d);CHECK(changed&&changed!=u&&last.mipLodBias==0.75f&&d.backend==(void *)f);
    d.lod_bias=0.5f;CHECK(vk_sampler_uint(&d)==u); /* descriptor-address reuse never stale */
    for(unsigned wrap=0;wrap<=7;wrap++){
        reset();d=descriptor();d.wrap[0]=d.wrap[1]=d.wrap[2]=wrap;CHECK(vk_sampler_uint(&d));
        VkSamplerAddressMode expected=wrap==0||wrap==7?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:wrap==1?VK_SAMPLER_ADDRESS_MODE_REPEAT:wrap==2||wrap==3?VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:wrap==6?VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        CHECK(last.addressModeU==expected&&last.addressModeV==expected&&last.addressModeW==expected);
    }
    reset();d=descriptor();d.wrap[0]=4;uint32_t border[4]={0,0,0,1};memcpy(d.border,border,sizeof border);
    CHECK(vk_sampler_uint(&d)&&last.borderColor==VK_BORDER_COLOR_INT_OPAQUE_BLACK&&!last.pNext);
    for(unsigned i=0;i<4;i++)border[i]=1;memcpy(d.border,border,sizeof border);
    CHECK(vk_sampler_uint(&d)&&last.borderColor==VK_BORDER_COLOR_INT_OPAQUE_WHITE);
    border[0]=0xdeadbeef;border[1]=255;border[2]=0x80000000;border[3]=16;memcpy(d.border,border,sizeof border);
    count=calls;CHECK(!vk_sampler_uint(&d)&&calls==count);g_vk.custom_border=true;
    CHECK(vk_sampler_uint(&d)&&last.borderColor==VK_BORDER_COLOR_INT_CUSTOM_EXT&&last.pNext);
    CHECK(!memcmp(custom.customBorderColor.uint32,border,sizeof border));
    CHECK(custom.format==VK_FORMAT_UNDEFINED&&custom.sType==VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);
    reset();d=descriptor();d.lod_max=1001;CHECK(vk_sampler_uint(&d)&&last.maxLod==VK_LOD_CLAMP_NONE);
    d.min_filter=1;CHECK(vk_sampler_uint(&d)&&last.maxLod==d.lod_min+0.25f);
    d.min_filter=2;d.lod_max=0;CHECK(vk_sampler_uint(&d)&&last.maxLod==d.lod_min);
    reset();d=descriptor();d.compare_mode=1;count=calls;CHECK(!vk_sampler_uint(&d)&&calls==count&&uint_sampler_count==0);
    CHECK(!vk_sampler_uint(NULL));d.compare_mode=0;d.wrap[0]=8;CHECK(!vk_sampler_uint(&d)&&calls==count);
    d=descriptor();fail_create=true;CHECK(!vk_sampler_uint(&d)&&uint_sampler_count==0);fail_create=false;CHECK(vk_sampler_uint(&d)&&uint_sampler_count==1);
    reset();d=descriptor();for(unsigned i=0;i<UINT_SAMPLER_CAP;i++){d.lod_bias=(float)i;CHECK(vk_sampler_uint(&d));}
    count=calls;d.lod_bias=5000;CHECK(!vk_sampler_uint(&d)&&calls==count&&uint_sampler_count==UINT_SAMPLER_CAP);
    d.lod_bias=0;CHECK(vk_sampler_uint(&d)&&calls==count); /* no eviction of in-flight objects */
    printf("{\"state\":\"passed\",\"checks\":%u,\"controlled_vulkan\":true,\"typed_cache_separate\":true,\"raw_uint_border_preserved\":true}\n",checks);
}
