/* Actual descriptor binding body: typed stencil and bounded legacy comparison. */
#include "shader.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint64_t VkImageView,VkSampler;
typedef uint32_t VkImageAspectFlags,VkFormat;
typedef struct{VkSampler sampler;VkImageView imageView;unsigned imageLayout;}VkDescriptorImageInfo;
typedef struct{uint32_t optimalTilingFeatures;}VkFormatProperties;
#define VK_NULL_HANDLE 0
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_IMAGE_ASPECT_DEPTH_BIT 2
#define VK_IMAGE_ASPECT_STENCIL_BIT 4
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT 1
#define VK_IMAGE_LAYOUT_GENERAL 1
typedef struct{uint32_t format,aspect,samples;}TexBackend;
typedef struct{uint32_t setmask,ds_mode,format,other;}NvnTextureView;
typedef struct{uint32_t format,ds_mode,width,height,depth,target;uint64_t addr;TexBackend*backend;}NvnTexture;
typedef struct{NvnTexture*tex;NvnTextureView view;bool has_view;uint32_t dead_format;}NvnTexEntry;
typedef struct{unsigned min_filter,mag_filter,compare_mode;float max_aniso;}NvnSamplerDesc;
typedef struct{int count;void*entries;}Pool;
typedef struct{uint64_t addr,size;}Range;
static struct{Pool*tpool,*spool;uint64_t tex[6][32];Range ubuf[6][18];}st;
static struct{int phys;}g_vk;
uint32_t vk_view_gen;
static unsigned checks,view_calls,legacy_logs,shadow_checks;
static int naliases,nstale,nfeedback;
static NvnTexture*feedback_tex[4];
static bool no_view,dummy_view,no_compare;
static jmp_buf failure;
static bool expect_failure;
static char error[256];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static void fatal(const char*s){snprintf(error,sizeof error,"%s",s);if(expect_failure)longjmp(failure,1);fputs(s,stderr);abort();}
#define FATAL(...) fatal(__VA_ARGS__)
#define LOG(...) (legacy_logs++)
static uint64_t hash_words(uint64_t h,const void*p,size_t n){const unsigned char*b=p;for(size_t i=0;i<n;i++)h=(h^b[i])*1099511628211ull;return h;}
static uint64_t vk_frame_counter(void){return 1;}
static NvnTexture*alias_read(NvnTexture*t){return t;}
static bool vk_sampled_stencil(const NvnTexture*t,const NvnTextureView*v,bool has){unsigned mode=has&&(v->setmask&16)?v->ds_mode:t->ds_mode;return t->format==0x31||((t->format==0x35||t->format==0x36)&&mode==1);}
// These legacy stencil fixtures are bound handles. The separately tested
// bindless float-format gate must never be invoked by this path.
static bool vk_sampled_float_compatible(NvnTexture*t,const NvnTextureView*v,bool has){(void)t;(void)v;(void)has;CHECK(false);return false;}
static VkImageView vk_view(NvnTexture*t,const NvnTextureView*v,bool has,bool render,int type){(void)render;(void)type;view_calls++;return no_view?0:dummy_view?999:vk_sampled_stencil(t,v,has)?222:111;}
static void renderer_texture_scale(NvnTexture*t,float*x,float*y){(void)t;*x=*y=1;}
static void renderer_texture_fraction(NvnTexture*t,float*x,float*y){(void)t;*x=*y=1;}
static VkImageView feedback_sample_view(int i,int type,bool stencil){(void)i;(void)type;return stencil?222:111;}
static void nvn_dead_slot_sampled(NvnTexEntry*e,uint32_t tic){(void)e;(void)tic;}
static VkSampler vk_sampler_uint(NvnSamplerDesc*d){return d->compare_mode?0:333;}
static VkSampler vk_sampler(NvnSamplerDesc*d){(void)d;return 444;}
static VkSampler vk_default_sampler(bool linear){return linear?555:556;}
static TexBackend*vk_tex(NvnTexture*t,bool r){(void)r;return t->backend;}
static void vkGetPhysicalDeviceFormatProperties(int p,VkFormat f,VkFormatProperties*out){(void)p;(void)f;out->optimalTilingFeatures=1;}
static bool vk_tex_depth_compare(NvnTexture*t){return t&&!no_compare&&(t->backend->aspect&2);}
static VkImageView vk_dummy_view(int type){(void)type;return 999;}
static void shadow_slot_check(int stage,ShaderTexture*t,int i,uint64_t h,uint32_t tic,NvnTexEntry*e,NvnTexture*tex,VkImageView*v){(void)stage;(void)t;(void)i;(void)h;(void)tic;(void)e;(void)tex;(void)v;shadow_checks++;}
#include "stencil_binding.production.h"
static VkDescriptorImageInfo bind(ShaderInfo*s){VkDescriptorImageInfo d={0};float sc[2],fr[2];uint32_t wh[2];texture_info(1,s,0,&d,sc,wh,fr,0x1234);return d;}
#define REJECT(expr,why) do{expect_failure=true;if(!setjmp(failure)){expr;CHECK(0);}expect_failure=false;CHECK(strstr(error,why));}while(0)
int main(void){
    TexBackend b={130,6,1};NvnTexture t={.format=0x35,.ds_mode=1,.width=64,.height=64,.backend=&b};
    NvnTexEntry e[2]={{0},{.tex=&t,.view={.setmask=16,.ds_mode=1},.has_view=true}};
    NvnSamplerDesc samplers[2]={{0},{.min_filter=2,.max_aniso=1}};Pool tp={2,e},sp={2,samplers};st.tpool=&tp;st.spool=&sp;st.tex[1][0]=(1u<<20)|1;
    ShaderInfo raw={.ntex=1};raw.tex[0]=(ShaderTexture){.handle_offset=0x20,.type=TEXTYPE_2D,.is_uint=true};
    ShaderInfo legacy=raw;legacy.tex[0].is_uint=false;legacy.tex[0].shadow=true;legacy.tex[0].legacy_stencil_compare=true;
    VkDescriptorImageInfo d=bind(&raw);CHECK(d.imageView==222&&d.sampler==333&&view_calls==1);CHECK(!legacy_logs&&!shadow_checks);
    NvnTextureView original=e[1].view;
    d=bind(&legacy);CHECK(d.imageView==111&&d.sampler==444&&view_calls==2);CHECK(legacy_logs==1&&!shadow_checks);
    CHECK(!memcmp(&original,&e[1].view,sizeof original));
    d=bind(&raw);CHECK(d.imageView==222&&d.sampler==333);d=bind(&legacy);CHECK(d.imageView==111&&legacy_logs==1);
    nfeedback=1;feedback_tex[0]=&t;CHECK(bind(&raw).imageView==222);CHECK(bind(&legacy).imageView==111);nfeedback=0;
    t.format=0x31;REJECT(bind(&legacy),"real combined");t.format=0x35;
    b.aspect=1;REJECT(bind(&legacy),"refusing dummy");b.aspect=6;
    b.samples=4;REJECT(bind(&legacy),"refusing dummy");b.samples=1;
    no_compare=true;REJECT(bind(&legacy),"refusing dummy");no_compare=false;
    e[1].tex=NULL;REJECT(bind(&legacy),"refusing dummy");e[1].tex=&t;
    dummy_view=true;vk_view_gen++;REJECT(bind(&legacy),"refusing dummy");dummy_view=false;
    no_view=true;vk_view_gen++;REJECT(bind(&legacy),"refusing dummy");no_view=false;
    vk_view_gen++;no_view=true;REJECT(bind(&raw),"refusing float dummy");no_view=false;
    vk_view_gen++;samplers[1].compare_mode=1;REJECT(bind(&raw),"unsupported sampler");samplers[1].compare_mode=0;
    CHECK(bind(&legacy).imageView==111&&legacy_logs==1);
    printf("{\"state\":\"passed\",\"checks\":%u,\"legacy_real_depth_only\":true,\"accuracy\":\"unqualified\"}\n",checks);return 0;
}
