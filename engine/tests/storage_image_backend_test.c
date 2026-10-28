/* Actual exec.c storage-image bodies with a controlled texture/command backend. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "nvn/maxwell/shader.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef uint64_t VkImageView, VkCommandBuffer;
typedef int VkFormat;
#define VK_NULL_HANDLE 0
#define VK_FORMAT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_GENERAL 1
typedef struct {uint64_t sampler; VkImageView imageView; int imageLayout;} VkDescriptorImageInfo;
typedef struct {uint64_t addr,size;} Range;
typedef struct {int identity;} NvnTextureView;
typedef struct {unsigned id,cpu,gpu;bool owned,uploaded;} NvnTexture;
typedef struct {NvnTexture *tex;NvnTextureView view;bool has_view;} NvnTexEntry;
typedef struct {void *entries;int count;} NvnDescPool;
static struct {Range ubuf[6][18];uint64_t img[6][8];NvnDescPool *tpool;} st;
static bool in_pass, reject_view;
static int format_kind;
static unsigned nview, nalias, nbarrier, nowned, uploads;
static NvnTexture textures[65];
static NvnTexEntry entries[65];
static NvnDescPool pool={entries,65};
static jmp_buf fatal_env;
static bool expected_fatal;
static void fatal(const char *message,...) __attribute__((noreturn));
static void fatal(const char *message,...) {if(expected_fatal)longjmp(fatal_env,1);fprintf(stderr,"%s\n",message);exit(1);}
#define FATAL(...) fatal(__VA_ARGS__)
static void alias_before_write(NvnTexture *t,int aspect,const char *why){CHECK(t&&aspect==0&&why);CHECK(!in_pass);nalias++;}
static VkImageView vk_storage_view(NvnTexture *t,const NvnTextureView *view,bool has_view,int type,VkFormat *format){
 CHECK(t&&view&&type==TEXTYPE_2D);CHECK(!in_pass);(void)has_view;nview++;*format=1;
 if(reject_view)return 0;
 if(!t->owned&&!t->uploaded){t->gpu=t->cpu;t->uploaded=true;uploads++;}
 return t->id;
}
static int vk_storage_format_kind(VkFormat format){CHECK(format==1);return format_kind;}
static void renderer_mark_gpu_owned(NvnTexture *t){CHECK(t&&!in_pass);t->owned=true;nowned++;}
static void vk_full_barrier(VkCommandBuffer cb){CHECK(cb==7&&!in_pass);nbarrier++;}
#include "storage_image_fragments.h"
static void reset(void){memset(&st,0,sizeof st);memset(textures,0,sizeof textures);memset(entries,0,sizeof entries);for(unsigned i=0;i<65;i++){textures[i].id=i+1;textures[i].cpu=10+i;entries[i].tex=&textures[i];}st.tpool=&pool;pool.count=65;pool.entries=entries;in_pass=reject_view=false;format_kind=0;nview=nalias=nbarrier=nowned=uploads=0;storage_nwritten=0;}
static ShaderInfo shader(void){ShaderInfo s={0};s.ntex=1;s.tex[0]=(ShaderTexture){.type=TEXTYPE_2D,.is_image=true,.bindless=true,.bindless_cbuf=2,.bindless_offset=0x120};return s;}
#define REJECT(code) do {expected_fatal=true;if(!setjmp(fatal_env)){code;CHECK(0);}expected_fatal=false;} while(0)
static void handle_sources(void){
 reset();ShaderInfo s=shader();
 for(unsigned i=0;i<8;i++){s.tex[0].bindless_offset=0x120+8*i;st.img[5][i]=i+1;CHECK(storage_image_entry(5,&s.tex[0])==&entries[i+1]);}
 s.tex[0].bindless=false;
 for(unsigned i=0;i<8;i++){s.tex[0].handle_offset=i*4;CHECK(storage_image_entry(5,&s.tex[0])==&entries[i+1]);}
 // Unaligned host buffers are legal. Exactly four available bytes suffice.
 unsigned char raw[13]={0};uint32_t h=17;memcpy(raw+9,&h,4);
 s.tex[0].bindless=true;s.tex[0].bindless_offset=9;
 for(unsigned k=3;k<21;k++){s.tex[0].bindless_cbuf=k;st.ubuf[5][k-3]=(Range){(uintptr_t)raw,sizeof raw};CHECK(storage_image_entry(5,&s.tex[0])==&entries[17]);}
 h=0xfff00012u;memcpy(raw+9,&h,4);CHECK(storage_image_entry(5,&s.tex[0])==&entries[18]);
}
static void failures(void){
 reset();ShaderInfo s=shader();st.img[5][0]=1;
 REJECT(storage_image_entry(0,&s.tex[0]));s.tex[0].is_image=false;REJECT(storage_image_entry(5,&s.tex[0]));s=shader();
 uint32_t invalid_offsets[]={0,0x11f,0x121,0x124,0x160,UINT32_MAX};
 for(unsigned i=0;i<sizeof invalid_offsets/sizeof *invalid_offsets;i++){s.tex[0].bindless_offset=invalid_offsets[i];REJECT(storage_image_entry(5,&s.tex[0]));}
 s=shader();for(unsigned k=0;k<2;k++){s.tex[0].bindless_cbuf=k;REJECT(storage_image_entry(5,&s.tex[0]));}s.tex[0].bindless_cbuf=21;REJECT(storage_image_entry(5,&s.tex[0]));
 s=shader();s.tex[0].bindless_cbuf=3;s.tex[0].bindless_offset=0;
 REJECT(storage_image_entry(5,&s.tex[0]));uint32_t handle=1;st.ubuf[5][0]=(Range){(uintptr_t)&handle,3};REJECT(storage_image_entry(5,&s.tex[0]));
 st.ubuf[5][0]=(Range){UINT64_MAX-1,8};REJECT(storage_image_entry(5,&s.tex[0]));s.tex[0].bindless_offset=7;REJECT(storage_image_entry(5,&s.tex[0]));
 s=shader();s.tex[0].bindless=false;s.tex[0].handle_offset=1;REJECT(storage_image_entry(5,&s.tex[0]));s.tex[0].handle_offset=32;REJECT(storage_image_entry(5,&s.tex[0]));
 s=shader();st.img[5][0]=0;entries[0].tex=NULL;REJECT(storage_image_entry(5,&s.tex[0]));entries[0].tex=&textures[0];st.img[5][0]=65;REJECT(storage_image_entry(5,&s.tex[0]));st.img[5][0]=1;
 st.tpool=NULL;REJECT(storage_image_entry(5,&s.tex[0]));st.tpool=&pool;pool.entries=NULL;REJECT(storage_image_entry(5,&s.tex[0]));pool.entries=entries;pool.count=-1;REJECT(storage_image_entry(5,&s.tex[0]));pool.count=65;entries[1].tex=NULL;REJECT(storage_image_entry(5,&s.tex[0]));
 reset();s=shader();st.img[5][0]=1;REJECT(storage_image_info(5,&s,1));s.ntex=65;REJECT(storage_image_info(5,&s,64));s.ntex=1;
 in_pass=true;REJECT(storage_image_info(5,&s,0));in_pass=false;CHECK(!nview&&!nalias&&!storage_nwritten);
 reject_view=true;REJECT(storage_image_info(5,&s,0));reject_view=false;
 for(int kind=-1;kind<=2;kind++){if(!kind)continue;format_kind=kind;REJECT(storage_image_info(5,&s,0));CHECK(!storage_nwritten&&!nowned);}
}
static void writes_and_cache_reuse(void){
 reset();ShaderInfo s=shader();st.img[5][0]=1;
 VkDescriptorImageInfo image=storage_image_info(5,&s,0);CHECK(!image.sampler&&image.imageView==textures[1].id&&image.imageLayout==VK_IMAGE_LAYOUT_GENERAL);
 CHECK(storage_nwritten==1&&!textures[1].owned&&uploads==1);
 CHECK(storage_image_info(5,&s,0).imageView==image.imageView);CHECK(storage_nwritten==1);
 // Simulated physical dispatch; ownership changes only after it is recorded.
 textures[1].gpu=99;storage_images_finish(7);CHECK(textures[1].owned&&textures[1].gpu==99&&nowned==1&&nbarrier==1&&!storage_nwritten);
 // Same descriptor may be reused by the set cache, but each operation still
 // executes image preparation and records its writes independently.
 image=storage_image_info(5,&s,0);CHECK(image.imageView==textures[1].id&&uploads==1&&storage_nwritten==1);
 textures[1].gpu++;storage_images_finish(7);CHECK(textures[1].gpu==100&&nowned==2&&nbarrier==2);
 // Later sampled access must not overwrite this result with unchanged CPU10.
 CHECK(textures[1].cpu!=textures[1].gpu);if(!textures[1].owned)textures[1].gpu=textures[1].cpu;CHECK(textures[1].gpu==100);
 unsigned before=nbarrier;storage_images_finish(7);CHECK(nbarrier==before);
 reset();s=shader();for(unsigned i=0;i<64;i++){st.img[5][0]=i+1;storage_image_info(5,&s,0);}CHECK(storage_nwritten==64);storage_images_finish(7);CHECK(nowned==64&&nbarrier==1);
}
static void registered_zero(void){
 reset();ShaderInfo s=shader();
 // GetImageHandle(0) is zero, and pool entry0 can be genuinely registered.
 CHECK(storage_image_entry(5,&s.tex[0])==&entries[0]);
 s.tex[0].bindless=false;CHECK(storage_image_entry(5,&s.tex[0])==&entries[0]);
 uint32_t handle=0;s.tex[0].bindless=true;s.tex[0].bindless_cbuf=3;s.tex[0].bindless_offset=0;st.ubuf[5][0]=(Range){(uintptr_t)&handle,sizeof handle};
 CHECK(storage_image_entry(5,&s.tex[0])==&entries[0]);
 VkDescriptorImageInfo image=storage_image_info(5,&s,0);CHECK(image.imageView==textures[0].id&&storage_nwritten==1);
 textures[0].gpu=123;storage_images_finish(7);CHECK(textures[0].owned&&textures[0].gpu==123);
 entries[0].tex=NULL;REJECT(storage_image_entry(5,&s.tex[0]));CHECK(!storage_nwritten);
}
int main(void){handle_sources();failures();writes_and_cache_reuse();registered_zero();printf("{\"state\":\"passed\",\"checks\":%u,\"actual_storage_helper\":true,\"driver_and_bound_handles\":true,\"cache_independent_ownership\":true,\"registered_zero\":true}\n",checks);return 0;}
