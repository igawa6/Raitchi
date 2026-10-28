/* Actual alias policy and copy recording; GPU byte storage is controlled. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "production_types.inc"
static unsigned checks,copies,ends,forgets;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define LOG(...) ((void)0)
static struct{bool rt_dirty;}st;
static uint32_t vk_view_gen;
static uint8_t pixels[8][4];
static bool ct_on(void){return false;}
static void vk_end_pass(void){ends++;}
static VkCommandBuffer vk_cmd(void){return (VkCommandBuffer)(uintptr_t)1;}
static void vk_full_barrier(VkCommandBuffer cb){CHECK(cb==vk_cmd());}
static void gp_mark(VkCommandBuffer cb,const char*fmt,...){(void)cb;(void)fmt;}
static void vk_views_forget(TexBackend*b){CHECK(b);forgets++;vk_view_gen++;}
static void rec_CmdCopyImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,unsigned nr,const VkImageCopy*c){
 CHECK(cb==vk_cmd()&&sl==VK_IMAGE_LAYOUT_GENERAL&&dl==VK_IMAGE_LAYOUT_GENERAL&&nr==1);
 CHECK(c->srcSubresource.aspectMask==c->dstSubresource.aspectMask&&c->extent.width==1&&c->extent.height==1&&c->extent.depth==1);
 unsigned si=(unsigned)(uintptr_t)src,di=(unsigned)(uintptr_t)dst;CHECK(si<8&&di<8);unsigned a=c->srcSubresource.aspectMask;
 if(a&VK_IMAGE_ASPECT_COLOR_BIT)memcpy(pixels[di],pixels[si],4);
 if(a&VK_IMAGE_ASPECT_DEPTH_BIT)pixels[di][0]=pixels[si][0];
 if(a&VK_IMAGE_ASPECT_STENCIL_BIT)pixels[di][1]=pixels[si][1];copies++;
}
static void rec_CmdBlitImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,unsigned nr,const VkImageBlit*c,VkFilter f){
 (void)cb;(void)src;(void)sl;(void)dst;(void)dl;(void)nr;(void)c;(void)f;CHECK(false); /* no format conversion in these fixtures */
}
#include "production_alias.inc"
static NvnTexture t[3];static TexBackend b[3];
static void setup(bool depth){
 memset(aliases,0,sizeof aliases);memset(stale,0,sizeof stale);memset(alias_hist,0,sizeof alias_hist);naliases=nstale=nhist=0;stat_alias_skipped=0;
 memset(t,0,sizeof t);memset(b,0,sizeof b);memset(pixels,0,sizeof pixels);
 for(unsigned i=0;i<3;i++){t[i]=(NvnTexture){.width=1,.height=1,.backend=&b[i]};b[i]=(TexBackend){.image=(VkImage)(uintptr_t)(i+1),.mem=(void*)(uintptr_t)(i+1),.format=depth?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM,.width=1,.height=1,.depth=1,.layers=1,.levels=1,.samples=1,.base_type=VK_IMAGE_VIEW_TYPE_2D,.gpu_owned=true,.aspect=depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT};}
}
int main(void){
 setup(false);
 // Two previously unread frames do not revoke the third-party snapshot lifetime.
 for(unsigned frame=0;frame<2;frame++){
  memset(pixels[(uintptr_t)b[0].image],0x31+frame,4);CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));
  alias_before_write(&t[0],0,"next frame source overwrite");memset(pixels[(uintptr_t)b[0].image],0xa0+frame,4);
 }
 alias_catch_up(&t[1]);NvnTexture*sample=alias_read(&t[1]);
 for(unsigned i=0;i<4;i++)CHECK(pixels[(uintptr_t)((TexBackend*)sample->backend)->image][i]==0x32);
 CHECK(nstale==0&&naliases==0&&stat_alias_skipped==0);
 // Reads can still resolve to the source without eagerly recording a GPU copy.
 setup(false);memset(pixels[1],0x45,4);unsigned before=copies;
 CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));CHECK(copies==before&&alias_read(&t[1])==&t[0]);
 alias_before_write(&t[0],VK_IMAGE_ASPECT_COLOR_BIT,"full source clear");memset(pixels[1],0,4);CHECK(pixels[2][0]==0x45);
 // A destination partial write first materializes all original bytes.
 setup(false);memset(pixels[1],0x56,4);CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));alias_before_write(&t[1],0,"partial destination write");pixels[2][0]=0xff;CHECK(pixels[2][1]==0x56&&pixels[2][0]==0xff);
 // Multiple destinations preserve the same source generation.
 setup(false);memset(pixels[1],0x67,4);CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));CHECK(alias_make(&t[0],&t[2],&b[0],&b[2]));alias_before_write(&t[0],0,"source write");memset(pixels[1],0xee,4);CHECK(pixels[2][0]==0x67&&pixels[3][0]==0x67);
 // Whole DS clears keep the safe image handover optimization (no transfer).
 setup(true);pixels[1][0]=0x71;pixels[1][1]=0x72;CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));before=copies;
 alias_before_write(&t[0],VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,"full DS clear");CHECK(copies==before&&b[1].image==(VkImage)(uintptr_t)1&&b[0].image==(VkImage)(uintptr_t)2&&pixels[1][0]==0x71&&pixels[1][1]==0x72);
 // Depth-only destination clears copy only the retained stencil plane.
 setup(true);pixels[1][0]=0x81;pixels[1][1]=0x82;pixels[2][0]=0x99;CHECK(alias_make(&t[0],&t[1],&b[0],&b[1]));alias_before_write(&t[1],VK_IMAGE_ASPECT_DEPTH_BIT,"depth only clear");CHECK(pixels[2][1]==0x82&&pixels[2][0]==0x99);pixels[2][0]=0;CHECK(pixels[2][1]==0x82);
 CHECK(ends>0&&forgets==2&&vk_view_gen>0);
 printf("{\"state\":\"passed\",\"checks\":%u,\"actual_helpers\":true,\"controlled_gpu_bytes\":true,\"safe_handover_retained\":true}\n",checks);
}
