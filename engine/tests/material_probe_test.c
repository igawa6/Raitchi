/* Actual bounded CPU material-probe bodies with synthetic draw metadata. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/types.h>
#include "nvn/maxwell/shader.h"
#include "runtime/session_layout.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define VK_NULL_HANDLE 0
typedef struct{uint64_t sampler,imageView;int imageLayout;}VkDescriptorImageInfo;
typedef struct{int format;uint32_t width,height;uint64_t image;bool gpu_owned;}TexBackend;
typedef struct{uint64_t storage,size;uint32_t flags;}NvnPool;
typedef struct{uint8_t bpb,bw,bh,kind;const char*name;}FormatInfo;
typedef struct{uint32_t width,height,depth,layers,levels;const FormatInfo*fmt;uint32_t block_height_log2,block_depth_log2;uint64_t bytes;}TexLayout;
typedef struct{TexLayout layout;uint32_t format,width,height,depth,levels,samples,target,flags;uint8_t swizzle[4];uint64_t addr;NvnPool*pool;uint64_t pool_offset,stride;void *backend;uint64_t gpu_write_owned,debug_id;}NvnTexture;
typedef struct{uint32_t setmask;uint16_t base_level,num_levels,min_layer,num_layers;uint32_t format,target,ds_mode;uint8_t swizzle[4];}NvnTextureView;
typedef struct{NvnTexture*tex;NvnTextureView view;bool has_view;uint32_t dead_format;}NvnTexEntry;
typedef struct{void*entries;int count;}NvnDescPool;
typedef struct{uint32_t min_filter,mag_filter,wrap[3],compare_mode,compare_func,reduction;float lod_min,lod_max,lod_bias,max_aniso,border[4];void*backend;}NvnSamplerDesc;
typedef struct{uint64_t code_addr;uint32_t const_offset,const_size;}NvnShaderStage;
typedef struct{NvnShaderStage stages[6];}NvnProgram;
typedef struct{uint64_t hash;uint32_t len;}Fingerprint;
typedef struct{ShaderInfo*fi,*vi;ShaderKey vkey;Fingerprint fp[2];}Prog;
typedef struct{uint8_t blend[8][8],blend_on;uint32_t write_mask[8],attr_kind[32];int color[8],depth,stencil;uint32_t ncolor;}PipeKey;
typedef struct{uint64_t x[8];}CmdRec;
typedef struct{uint64_t addr,size;}Range;
typedef struct{uint32_t attr_mask,used_streams,attr_fmt[32],attr_off[32],attr_bind[32],stride[17],divisor[17];}VtxLayout;
typedef struct{int32_t x,y;}ProbeOffset;
typedef struct{uint32_t width,height;}ProbeExtent;
typedef struct{ProbeOffset offset;ProbeExtent extent;}VkRect2D;
static struct{struct{int ncolor;NvnTexture*color[8],*depth;NvnTextureView cview[8],dview;bool has_cview[8],has_dview;}rt;NvnProgram*program;uint8_t color_state[4],blend[8][8],ds[8],poly[4];uint32_t channel_mask,stencil_ref[2],stencil_mask[2],stencil_vmask[2];float viewport[16][4],poly_offset[3],depth_range[16][2];int32_t scissor[16][4];uint64_t tex[6][32],img[6][8];NvnDescPool*tpool,*spool;Range ubuf[6][18],vbuf[16];uint32_t attrib[16],stream[16][2];int nattrib,nstream;bool prim_restart;uint32_t restart_index;}st;
static uint32_t pass_w,pass_h;static float pass_sx=1,pass_sy=1;
static SessionLayout selected_layout;
static const SessionLayout *test_layout;
const SessionLayout *session_layout_current(void){return test_layout;}
static uint64_t g_cur_fs_hash;
static struct{uint32_t known,cull,ff,dbe,dt,dw,dop;float db[3];struct{float minDepth,maxDepth;}vp;}ss;
static uint64_t frame;
static unsigned frame_queries,logged;
static uint64_t vk_frame_counter(void){frame_queries++;return frame;}
static uint64_t tegra_total_size(const TexLayout*l){return l->bytes;}
static const FormatInfo probe_format={4,1,1,0,"probe"};
static bool nvn_format_known(uint32_t f){return f==0x3f||f==0x25;}
static const FormatInfo *nvn_format_info(uint32_t f){return nvn_format_known(f)?&probe_format:NULL;}
static bool nvn_texture_row_linear(uint32_t flags){return (flags&1)!=0;}
static bool nvn_texture_row_span(const TexLayout*l,uint64_t stride,uint64_t*out){if(!l||!out||!stride||l->height>UINT64_MAX/stride)return false;*out=stride*l->height;return true;}
static bool kernel_copy_readable(void *dst,uint64_t address,uint64_t size){
 if(!dst||!address||!size||size>SIZE_MAX)return false;
 struct iovec local={dst,(size_t)size},remote={(void*)(uintptr_t)address,(size_t)size};
 return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)size;
}
static bool sampled_texture_handle(int stage,const ShaderTexture*image,uint64_t*handle){
 if(!image||!handle||stage<0||stage>=6)return false;
 if(image->bindless_cbuf==2){unsigned slot=(image->bindless_offset-0x20)/8;if(image->bindless_offset<0x20||slot>=32)return false;*handle=st.tex[stage][slot];return true;}
 if(image->handle_offset<0x20||(image->handle_offset-0x20)%8)return false;
 unsigned slot=(image->handle_offset-0x20)/8;if(slot>=32)return false;*handle=st.tex[stage][slot];return true;
}
static uint64_t shader_code_hash(const void*data,size_t size){const uint8_t*p=data;uint64_t h=UINT64_C(1469598103934665603);for(size_t i=0;i<size;i++)h=(h^p[i])*UINT64_C(1099511628211);return h;}
static bool sampling_trace_on(void){return false;}
static bool clear_scope_full_test(void){return true;}
static int attr_kind(uint32_t f){return (int)(f&255u);}
static int vertex_format(uint32_t f){return (int)f;}
#define VK_FORMAT_UNDEFINED 0
static int blend_factor(uint8_t v){return 100+v;}
static int blend_op(uint8_t v){return 200+v;}
#define LOG(...) do{logged++;}while(0)
#include "material_probe_fragments.h"
static bool exists(const char*p){struct stat sb;return !stat(p,&sb);}
static void request(void){FILE*f=fopen("material-probe.request","w");CHECK(f);CHECK(!fclose(f));}
static char *contents(void){FILE*f=fopen("material-probe.log","rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>=0);CHECK(!fseek(f,0,SEEK_SET));char*p=calloc(1,(size_t)n+1);CHECK(p);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);CHECK(!fclose(f));return p;}
static unsigned count(const char*p,const char*needle){unsigned n=0;while((p=strstr(p,needle))){n++;p+=strlen(needle);}return n;}
static char *projection_text(void){FILE*f=fopen("material-probe-projection.log","rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>=0&&n<65536);CHECK(!fseek(f,0,SEEK_SET));char*p=calloc(1,(size_t)n+1);CHECK(p);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);CHECK(!fclose(f));return p;}
static void projection_test(Prog *prog,PipeKey *key,VkDescriptorImageInfo *images){
 CHECK(!exists("material-probe-projection.log")&&!exists("material-probe-projection.bin"));
 long page=sysconf(_SC_PAGESIZE);CHECK(page>0);size_t size=(size_t)page*4;
 unsigned char*memory=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED);
 selected_layout.window=(SessionRange){(uint64_t)(uintptr_t)memory,(uint64_t)(uintptr_t)memory+size};test_layout=&selected_layout;
 uint64_t base=(uint64_t)(uintptr_t)memory;
 CHECK(material_projection_range(test_layout,base,size));CHECK(!material_projection_range(NULL,base,1));CHECK(!material_projection_range(test_layout,base,0));
 CHECK(!material_projection_range(test_layout,base-1,1));CHECK(!material_projection_range(test_layout,base+size,1));CHECK(!material_projection_range(test_layout,base+size-1,2));CHECK(!material_projection_range(test_layout,UINT64_MAX-1,4));
 for(unsigned stage=0;stage<2;stage++)for(unsigned slot=0;slot<6;slot++){
  unsigned index=stage*6+slot;memset(memory+index*64,(int)(index+1),64);st.ubuf[stage][slot].addr=base+index*64;st.ubuf[stage][slot].size=64;
 }
 // A real high sampler index must not be confused with the texture slot.
 NvnSamplerDesc samplers[268]={0};samplers[267]=(NvnSamplerDesc){.min_filter=5,.mag_filter=1,.wrap={2,7,3},.compare_mode=0,.compare_func=4,.reduction=2,.lod_min=-2,.lod_max=8,.lod_bias=.25f,.max_aniso=4,.border={1,.5f,0,-1}};
 NvnDescPool samplerpool={samplers,268};st.spool=&samplerpool;st.tex[1][0]=267ull<<20;
 st.ubuf[0][9].addr=base+768;st.ubuf[0][9].size=16;memset(memory+768,0x7b,16);
 const uint16_t indices[3]={2,0,1};memcpy(memory+page,indices,sizeof indices);memset(memory+2*page,0x5a,512);
 st.nattrib=1;st.attrib[0]=0x2e;st.nstream=1;st.stream[0][0]=12;st.stream[0][1]=0;st.vbuf[0].addr=base+2*(uint64_t)page;st.vbuf[0].size=512;prog->vi->attr_mask=1;
 CmdRec draw={.x={4,1,3,base+(uint64_t)page,0,0,1}};
 request();frame++;material_probe_poll();CHECK(material_probe_file&&material_probe_bound_enabled());g_cur_fs_hash=0x44b584f0d922d49dull;
 VtxLayout layout={0};material_probe_draw(prog,key,&draw,true,images,1,&layout);CHECK(material_projection_done);CHECK(exists("material-probe-projection.log")&&exists("material-probe-projection.bin"));
 char*text=projection_text();CHECK(strstr(text,"source=cpu-guest-memory not_exact_gpu_cache=1"));CHECK(strstr(text,"index_bounds minimum=0 maximum=2 effective_low=0 effective_high=2 usable=1"));CHECK(strstr(text,"format=46 stream=0 relative_offset=0"));CHECK(strstr(text,"payload name=indices")&&strstr(text,"payload name=vertices-0"));CHECK(count(text,"status=captured")==15);
 CHECK(strstr(text,"ubo stage=0 cbuf=12 slot=9")&&strstr(text,"payload name=ubo-0-12"));
 CHECK(strstr(text,"sampled tex=0 binding=32 handle_known=1 handle=10b00000 tic=0 sampler_id=267"));
 CHECK(strstr(text,"sampler tex=0 id=267 source=pool min=5 mag=1 wrap=2,7,3 compare_mode=0 compare_func=4 reduction=2"));
 CHECK(strstr(text,"lod_min=-2 lod_max=8 lod_bias=0.25 max_aniso=4 border=1,0.5,0,-1 border_bits=3f800000,3f000000,00000000,bf800000"));
 CHECK(strstr(text,"sampler tex=1 id=0 source=unresolved"));free(text);
 FILE*f=fopen("material-probe-projection.bin","rb");CHECK(f);unsigned char payload[4096];size_t n=fread(payload,1,sizeof payload,f);CHECK(!ferror(f)&&feof(f));CHECK(!fclose(f));CHECK(n==12*64+16+6+292);for(unsigned i=0;i<6*64;i++)CHECK(payload[i]==i/64+1);for(unsigned i=6*64;i<6*64+16;i++)CHECK(payload[i]==0x7b);for(unsigned i=6*64+16;i<12*64+16;i++)CHECK(payload[i]==(i-(6*64+16))/64+7);CHECK(!memcmp(payload+12*64+16,indices,6));for(unsigned i=12*64+16+6;i<n;i++)CHECK(payload[i]==0x5a);
 memset(memory,0xaa,64);material_probe_draw(prog,key,&draw,true,images,1,&layout);f=fopen("material-probe-projection.bin","rb");CHECK(f&&fgetc(f)==1&&!fclose(f));
 frame++;CHECK(!material_probe_bound_enabled());material_probe_poll();CHECK(!material_probe_file);
 // Boundary failures occur before reading source memory or writing payload.
 MaterialProjection cap={.meta=tmpfile(),.data=tmpfile(),.layout=test_layout};CHECK(cap.meta&&cap.data);
 CHECK(!mprotect(memory+3*page,(size_t)page,PROT_NONE));CHECK(!material_projection_payload(&cap,"guard",base+3*(uint64_t)page,16));CHECK(cap.saved==0);
 selected_layout.window.hi=base+MATERIAL_PROJECTION_MAX_BYTES+4096;CHECK(!material_projection_payload(&cap,"large",base,MATERIAL_PROJECTION_PAYLOAD_BYTES+1));CHECK(cap.saved==0);selected_layout.window.hi=base+size;
 cap.saved=MATERIAL_PROJECTION_PAYLOAD_BYTES;CHECK(!material_projection_payload(&cap,"full",base,1));CHECK(cap.saved==MATERIAL_PROJECTION_PAYLOAD_BYTES);cap.saved=0;
 CHECK(!material_projection_payload(&cap,"outside",base+size,16));CHECK(!fseek(cap.meta,0,SEEK_SET));char errors[2048]={0};CHECK(fread(errors,1,sizeof errors-1,cap.meta)>0);CHECK(strstr(errors,"kernel-copy-failed")&&strstr(errors,"payload-budget")&&strstr(errors,"outside-active-guest-window"));CHECK(!fclose(cap.meta)&&!fclose(cap.data));
 // Verbose sampler metadata cannot consume the geometry reserve, even with
 // all 64 reflected resources. The next item is explicitly suppressed.
 FILE *budget=tmpfile();CHECK(budget);CHECK(!fseek(budget,48*1024,SEEK_SET));CHECK(fputc('X',budget)=='X');
 int previous_ntex=prog->fi->ntex;prog->fi->ntex=64;material_projection_samplers(budget,prog,images);prog->fi->ntex=previous_ntex;CHECK(ftell(budget)<49*1024);CHECK(!fseek(budget,48*1024+1,SEEK_SET));
 char suppressed[128]={0};CHECK(fread(suppressed,1,sizeof suppressed-1,budget)>0);CHECK(strstr(suppressed,"samplers status=metadata-budget next_tex=0 remaining=64"));CHECK(!fclose(budget));
 // A legal index buffer with an invalid signed base cannot select a VBO range.
 st.ubuf[0][9].addr=0;st.ubuf[0][9].size=0;st.tex[1][0]=0;
 material_projection_done=false;draw.x[4]=(uint32_t)-1;material_projection_capture(prog,&draw,true,123,images);text=projection_text();CHECK(strstr(text,"effective_low=-1")&&strstr(text,"usable=0")&&strstr(text,"unbounded-or-overflow"));CHECK(!strstr(text,"payload name=vertices-0"));CHECK(strstr(text,"status=unbound-zero")&&!strstr(text,"payload name=ubo-0-12"));CHECK(strstr(text,"sampler tex=0 id=0 source=pool"));free(text);
 material_projection_done=false;draw.x[2]=UINT64_MAX;material_projection_capture(prog,&draw,true,124,images);text=projection_text();CHECK(strstr(text,"unsupported-draw-range")&&!strstr(text,"payload name=indices"));free(text);
 CHECK(!munmap(memory,size));st.spool=NULL;test_layout=NULL;g_cur_fs_hash=0;CHECK(!unlink("material-probe-projection.log")&&!unlink("material-probe-projection.bin"));
}
int main(void){
 char old[4096];CHECK(getcwd(old,sizeof old));char temp[]="/tmp/material-probe-XXXXXX";CHECK(mkdtemp(temp)&&!chdir(temp));
 NvnProgram program={.stages={{.code_addr=0x111000,.const_offset=0x200,.const_size=0x80},{.code_addr=0x222000,.const_offset=0x300,.const_size=0x40}}};
 NvnPool pool={.storage=0x810000,.size=0x4000};TexBackend backend={.format=37,.width=32,.height=16,.image=0x44,.gpu_owned=true};NvnTexture texture={.layout={.width=32,.height=16,.depth=1,.layers=1,.levels=1,.fmt=&probe_format,.bytes=2048},.format=0x3f,.width=32,.height=16,.depth=1,.levels=1,.samples=1,.target=1,.swizzle={1,2,3,4},.addr=0x812000,.pool=&pool,.pool_offset=0x2000,.stride=128,.backend=&backend};
 NvnTexEntry entries[2]={{.tex=&texture,.view={.setmask=8,.swizzle={4,3,2,1}},.has_view=true},{0}};NvnDescPool descriptors={entries,2};ShaderInfo info={.ntex=2,.cbuf_mask=0x1f8,.out_mask=0x050f,.uses_discard=true,.writes_depth=true,.tex={{.bindless=true,.bindless_cbuf=2,.bindless_offset=0x20,.type=TEXTYPE_2D},{.bindless=true,.bindless_cbuf=7,.bindless_offset=0x100}}};Prog prog={.fi=&info,.fp={{0xabc,0x280},{0xdef,0x340}}};PipeKey key={.blend={{0,5,6,2,1,1,1}},.blend_on=1,.write_mask={15,0,5},.color={37,0,98},.depth=130,.stencil=130,.ncolor=3};CmdRec draw={.x={4,0,6}};VkDescriptorImageInfo images[2]={{.imageView=0x88,.sampler=0x99,.imageLayout=1},{.imageView=0x66,.sampler=0x77,.imageLayout=1}};
 st.program=&program;st.rt.ncolor=3;st.rt.color[0]=&texture;st.rt.color[2]=&texture;st.rt.depth=&texture;st.rt.has_dview=true;st.rt.dview.setmask=16;st.rt.dview.ds_mode=1;st.rt.has_cview[0]=true;st.rt.cview[0].setmask=4;st.rt.cview[0].format=0x3f;st.tpool=&descriptors;st.channel_mask=15;memcpy(st.blend[0],key.blend[0],8);st.color_state[0]=1;st.color_state[1]=2;st.color_state[2]=3;st.color_state[3]=4;
 const uint8_t ds[8]={1,0,3,1,0x42,0x52,0x41,0x63};memcpy(st.ds,ds,sizeof ds);
 st.stencil_ref[0]=0x12;st.stencil_ref[1]=0x34;st.stencil_mask[0]=0xf;st.stencil_mask[1]=0xf0;st.stencil_vmask[0]=0x55;st.stencil_vmask[1]=0xaa;
 ShaderInfo vertex={.clip_mask=3,.cbuf_mask=0x11f8};prog.vi=&vertex;prog.vkey.flip_y=true;
 st.poly[0]=2;st.poly[1]=1;st.poly[2]=2;st.poly[3]=4;
 st.poly_offset[0]=2.5f;st.poly_offset[1]=12;st.poly_offset[2]=-3;
 st.depth_range[0][0]=-1;st.depth_range[0][1]=2;
 ss.known=0xdead;ss.cull=2;ss.ff=1;ss.dbe=1;ss.db[0]=7.5f;ss.db[1]=-8;ss.db[2]=4;
 ss.dt=1;ss.dw=0;ss.dop=3;ss.vp.minDepth=0;ss.vp.maxDepth=1;
 unsigned char unchanged_ss[sizeof ss];memcpy(unchanged_ss,&ss,sizeof ss);
 for(unsigned i=0;i<18;i++){st.ubuf[0][i].addr=0x900000+i*256;st.ubuf[0][i].size=128;st.ubuf[1][i].addr=0xa00000+i*256;st.ubuf[1][i].size=64;}
 VtxLayout layout={0};unsigned char unchanged[sizeof st];memcpy(unchanged,&st,sizeof st);request();CHECK(!unsetenv("LARECOMP_MATERIAL_PROBE"));material_probe_poll();material_probe_draw(&prog,&key,&draw,false,images,1,&layout);CHECK(exists("material-probe.request")&&!exists("material-probe.log")&&!frame_queries&&!logged);
 CHECK(!setenv("LARECOMP_MATERIAL_PROBE","1",1));material_probe_enabled=-1;frame=10;material_probe_poll();CHECK(!exists("material-probe.request")&&material_probe_file);material_probe_draw(&prog,&key,&draw,false,images,1,&layout);material_probe_draw(&prog,&key,&draw,true,images,2,&layout);pass_w=1280;pass_h=720;VkRect2D rect={{0,0},{1280,720}};float clear_color[4]={1,.5f,0,1};material_probe_clear(0,1,clear_color,15,true,1.0f,true,3,255,false,rect,rect,"clear");CHECK(!memcmp(unchanged,&st,sizeof st));frame=11;material_probe_poll();CHECK(!material_probe_file&&logged==1);
 char*text=contents();CHECK(count(text,"draw=")==2&&strstr(text,"reason=present-ended"));CHECK(strstr(text,"vs=111000 fs=222000")&&strstr(text,"vs_hash=0000000000000abc")&&strstr(text,"color=01020304"));CHECK(strstr(text,"src_color=105 dst_color=106")&&strstr(text,"descriptor_view=88 descriptor_sampler=99"));CHECK(strstr(text,"rt_format=37")&&strstr(text,"pipe_blend=1 pipe_mask=f"));CHECK(strstr(text,"handle_known=1 handle=0 tic=0")&&strstr(text,"texture=")&&strstr(text,"guest=812000 bytes=2048"));CHECK(strstr(text,"swizzle=01020304")&&strstr(text,"swizzle=04030201")&&strstr(text,"handle_known=0"));CHECK(count(text," ubo stage=")==24&&!strstr(text,"slot=6 cbuf="));
 CHECK(strstr(text,"schema=4")&&strstr(text,"max_draws=2048"));CHECK(strstr(text,"ds=0100030142524163"));
 CHECK(strstr(text,"raster polygon=02010204 offset_factor=2.5 offset_units=12 offset_clamp=-3 depth_range=-1,2"));
 CHECK(strstr(text,"state_known=0000dead resolved_cull=2 resolved_front=1 resolved_bias_enable=1"));
 CHECK(strstr(text,"resolved_bias_constant=-8 resolved_bias_slope=7.5 resolved_bias_clamp=4"));
 CHECK(strstr(text,"resolved_depth_enable=1 resolved_depth_write=0 resolved_depth_compare=3 resolved_depth_range=0,1"));
 CHECK(strstr(text,"flip_y=1 depth_minus_one=0 vertex_clip_mask=03"));CHECK(!memcmp(unchanged_ss,&ss,sizeof ss));
 CHECK(strstr(text,"requested_depth_test=1 requested_depth_write=0 depth_func=3 requested_stencil_test=1 attachment_depth_format=130 attachment_stencil_format=130"));
 CHECK(strstr(text,"stencil face=front ref=00000012 compare_mask=00000055 write_mask=0000000f func=2 fail=2 depth_fail=5 depth_pass=1"));
 CHECK(strstr(text,"stencil face=back ref=00000034 compare_mask=000000aa write_mask=000000f0 func=4 fail=4 depth_fail=3 depth_pass=6"));
 CHECK(strstr(text,"fragment out_mask=0000050f uses_discard=1 writes_depth=1 alpha_test_func=3 logic_op=2 alpha_ref_available=0"));
 CHECK(strstr(text,"attachments color_count=3 pipeline_color_count=3"));CHECK(strstr(text,"rt0 attachment_format=37 write_mask=f"));
 CHECK(strstr(text,"rt1 attachment_format=0 write_mask=0 texture=0"));CHECK(strstr(text,"rt2 attachment_format=98 write_mask=5"));
 CHECK(strstr(text,"depth_attachment texture=")&&strstr(text,"view has=1 setmask=10"));CHECK(strstr(text,"clear=0")&&strstr(text,"full_scope_test=1 selected_path=clear"));free(text);
 // Poll once per logical frame: a mid-frame request starts next frame.
 request();material_probe_poll();CHECK(exists("material-probe.request")&&!material_probe_file);frame++;material_probe_poll();CHECK(material_probe_file&&!exists("material-probe.request"));
 for(unsigned i=0;i<MATERIAL_PROBE_MAX_DRAWS+20;i++)material_probe_draw(&prog,&key,&draw,false,images,1,&layout);CHECK(!material_probe_file&&material_probe_count==MATERIAL_PROBE_MAX_DRAWS);text=contents();CHECK(count(text,"draw=")==MATERIAL_PROBE_MAX_DRAWS&&strstr(text,"reason=draw-limit"));free(text);
 request();material_probe_poll();CHECK(exists("material-probe.request")&&!material_probe_file);frame++;material_probe_poll();CHECK(material_probe_file&&material_probe_count==0);
 ss.db[1]=0;ss.dbe=0;ss.vp.maxDepth=.5f;memcpy(unchanged_ss,&ss,sizeof ss);
 material_probe_draw(&prog,&key,&draw,false,images,1,&layout);frame++;material_probe_poll();CHECK(!material_probe_file);text=contents();CHECK(count(text,"draw=")==1&&strstr(text,"draw=0"));
 CHECK(strstr(text,"offset_units=12")&&strstr(text,"resolved_bias_enable=0 resolved_bias_constant=0"));CHECK(strstr(text,"resolved_depth_range=0,0.5"));CHECK(!memcmp(unchanged_ss,&ss,sizeof ss));
 free(text);CHECK(!memcmp(unchanged,&st,sizeof st));
 projection_test(&prog,&key,images);memcpy(&st,unchanged,sizeof st);
 CHECK(!unlink("material-probe.log")&&!chdir(old)&&!rmdir(temp));printf("{\"state\":\"passed\",\"checks\":%u,\"default_off\":true,\"finite_capture\":true,\"state_unchanged\":true,\"registered_zero_visible\":true}\n",checks);return 0;
}
