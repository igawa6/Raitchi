#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include "types.inc"
static struct {struct{int ncolor;NvnTexture*color[8],*depth;NvnTextureView cview[8],dview;bool has_cview[8],has_dview;}rt;int32_t scissor[1][4];float viewport[1][4];}st;
static uint32_t pass_w=1920,pass_h=1080,pass_ncolor=1;static VkFormat pass_color[8]={VK_FORMAT_R8G8B8A8_UNORM},pass_depth=VK_FORMAT_D24_UNORM_S8_UINT,pass_stencil=VK_FORMAT_D24_UNORM_S8_UINT;static float pass_sx=1,pass_sy=1,vp_sx=1,vp_sy=1,vp_ox,vp_oy;
#include "scope.inc"
static uint64_t frame;static bool material_projection_done;static uint64_t material_probe_frame;static bool sample_gate;
#define MATERIAL_PROBE_MAX_DRAWS 2048u
#define LOG(...) ((void)0)
static uint64_t vk_frame_counter(void){return frame;}
static bool sampling_trace_on(void){return sample_gate;}
static void material_probe_texture(FILE*f,const NvnTexture*t){if(t)fprintf(f," backend-metadata-existing=1");}
#include "gate.inc"
static unsigned checks,commands,draws,begins;static VkClearAttachment last_a[2];static uint32_t last_n;static VkRect2D last_rect;static uint32_t stencil_mask;static float pushed[8];
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void begin_pass(void){begins++;}
static VkCommandBuffer vk_cmd(void){return (void*)1;}
static void rec_CmdClearAttachments(VkCommandBuffer cb,uint32_t n,const VkClearAttachment*a,uint32_t nr,const VkClearRect*r){(void)cb;CHECK(n<=2&&nr==1);memcpy(last_a,a,n*sizeof*a);last_n=n;last_rect=r->rect;commands++;}
static VkPipeline clear_pipeline(int i,int k,uint32_t m,bool d,bool s){(void)i;(void)k;(void)m;(void)d;(void)s;return(void*)1;}
static VkPipelineLayout pc_layout(void){return(void*)1;}
#define rec_CmdBindPipeline(...) ((void)0)
#define rec_CmdSetDepthTestEnable(...) ((void)0)
#define rec_CmdSetDepthWriteEnable(...) ((void)0)
#define rec_CmdSetStencilTestEnable(...) ((void)0)
#define rec_CmdSetStencilOp(...) ((void)0)
#define rec_CmdSetStencilReference(...) ((void)0)
#define rec_CmdSetViewport(...) ((void)0)
#define rec_CmdSetScissor(cb,i,n,r) (last_rect=*(r))
#define rec_CmdSetStencilWriteMask(cb,faces,m) (stencil_mask=(m))
#define rec_CmdPushConstants(cb,l,s,o,n,p) memcpy(pushed,p,n)
#define rec_CmdDraw(...) (draws++)
#define ss_forget() ((void)0)
#define dyn_defaults(cb) ((void)0)
#include "clear.inc"
static void scope(const char*s){if(s)CHECK(!setenv("LARECOMP_CLEAR_SCOPE_TEST",s,1));else CHECK(!unsetenv("LARECOMP_CLEAR_SCOPE_TEST"));clear_scope_test_enabled=-1;}
static void sc(int x,int y,int w,int h){st.scissor[0][0]=x;st.scissor[0][1]=y;st.scissor[0][2]=w;st.scissor[0][3]=h;}
static bool rect(VkRect2D r,int x,int y,uint32_t w,uint32_t h){return r.offset.x==x&&r.offset.y==y&&r.extent.width==w&&r.extent.height==h;}
static void request(void){FILE*f=fopen("material-probe.request","w");CHECK(f);CHECK(!fclose(f));}
int main(void){char cwd[4096],dir[]="/tmp/raitchi-clear-probe-XXXXXX";CHECK(getcwd(cwd,sizeof cwd));CHECK(mkdtemp(dir));CHECK(!chdir(dir));CHECK(!unsetenv("LARECOMP_MATERIAL_PROBE"));NvnTexture t={.debug_id=91,.width=1920,.height=1080,.levels=4};st.rt.ncolor=1;st.rt.color[0]=st.rt.depth=&t;st.rt.has_cview[0]=true;st.rt.cview[0]=(NvnTextureView){.setmask=3,.base_level=2,.min_layer=7};float c[4]={.25f,-.5f,1,0};
 // Disabled trace does not consume a request, even when actual clear runs.
 request();scope(NULL);sc(10,20,100,200);clear(0,0,c,15,false,0,false,0,0,false);CHECK(!material_probe_file&&!access("material-probe.request",F_OK));CHECK(commands==1&&rect(last_rect,10,20,100,200));CHECK(!memcmp(last_a[0].clearValue.color.float32,c,16));CHECK(!unlink("material-probe.request"));
 // Strict scope value; cache once; test full pass excludes explicit ClearTexture.
 scope("FULL");CHECK(!clear_scope_full_test());scope("full");CHECK(clear_scope_full_test());CHECK(!setenv("LARECOMP_CLEAR_SCOPE_TEST","invalid",1));CHECK(clear_scope_full_test());clear(0,1,c,15,false,0,false,0,0,false);CHECK(rect(last_rect,0,0,1920,1080));clear(0,2,c,15,false,0,false,0,0,true);CHECK(rect(last_rect,10,20,100,200));
 clear(0,0,c,5,false,0,false,0,0,false);CHECK(draws==1&&rect(last_rect,0,0,1920,1080)&&!memcmp(pushed,c,16));clear(-1,0,NULL,0,true,.75f,true,0x1234,0x5a,false);CHECK(draws==2&&stencil_mask==0x5a&&pushed[4]==.75f);clear(-1,0,NULL,0,true,.625f,true,0x12,255,false);CHECK(last_n==1&&last_a[0].aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));CHECK(last_a[0].clearValue.depthStencil.depth==.625f&&last_a[0].clearValue.depthStencil.stencil==0x12);
 CHECK(clear_scope_covers(1920,1080,st.scissor[0]));CHECK(clear_scope_attachment_covers(1920,1080,0,1920,1080));CHECK(!clear_scope_attachment_covers(1920,1080,1,1920,1080));CHECK(!clear_scope_attachment_covers(UINT_MAX,UINT_MAX,32,1,1));
 scope(NULL);CHECK(!clear_scope_covers(1920,1080,st.scissor[0]));sc(0,0,1920,1080);CHECK(clear_scope_covers(1920,1080,st.scissor[0]));sc(-10,-20,100,200);CHECK(rect(scissor_rect(),0,0,90,180));sc(0,0,0,0);CHECK(rect(scissor_rect(),0,0,0,0));unsigned old=commands;clear(0,0,c,15,false,0,false,0,0,false);CHECK(commands==old);sc(INT_MIN,INT_MIN,INT_MIN,INT_MIN);CHECK(rect(scissor_rect(),0,0,0,0));sc(1,1,INT_MAX,INT_MAX);CHECK(rect(scissor_rect(),1,1,1919,1079));sc(INT_MAX,INT_MAX,INT_MAX,INT_MAX);CHECK(rect(scissor_rect(),0,0,0,0));int32_t huge[4]={INT_MAX,INT_MAX,INT_MAX,INT_MAX};CHECK(!clear_scope_covers(UINT_MAX,UINT_MAX,huge));
 // Request observed by a leading clear, before any draw. Shared explicit gate.
 sample_gate=true;material_probe_enabled=-1;frame++;request();sc(10,20,100,200);clear(0,2,c,15,false,0,false,0,0,false);CHECK(material_probe_file&&material_probe_count==0&&material_probe_clear_count==1);CHECK(access("material-probe.request",F_OK));for(unsigned i=0;i<300;i++)clear(-1,0,NULL,0,true,.5f,true,0x34,0xff,false);CHECK(material_probe_clear_count==256&&material_probe_clear_omitted==45);material_probe_clear_omitted=UINT64_MAX;clear(-1,0,NULL,0,true,.5f,false,0,0,false);CHECK(material_probe_clear_omitted==UINT64_MAX);material_probe_finish("test");FILE*f=fopen("material-probe.log","r");CHECK(f);char text[200000];size_t n=fread(text,1,sizeof text-1,f);text[n]=0;CHECK(!fclose(f));CHECK(strstr(text,"draws_before=0 origin=color index=0 kind=2"));CHECK(strstr(text,"color_bits=3e800000,bf000000,3f800000,00000000"));CHECK(strstr(text,"target_id=91 mip=2 layer_or_z=7 mip_extent=480x270"));CHECK(strstr(text,"resolved=10,20,100,200"));CHECK(strstr(text,"stencil_value=00000034 stencil_mask=000000ff"));CHECK(strstr(text,"clears=256 clears_omitted=18446744073709551615"));CHECK(!strstr(text,"clear=256 "));frame++;request();clear(0,0,c,15,false,0,false,0,0,false);CHECK(material_probe_clear_count==1&&material_probe_clear_omitted==0);material_probe_finish("test-reset");CHECK(!unlink("material-probe.log"));CHECK(!chdir(cwd));CHECK(!rmdir(dir));printf("%u actual clear diagnostic/policy assertions passed\n",checks);}
