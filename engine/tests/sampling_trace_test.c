/* Verbatim production trace gate/formatter with exact current host structs. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shader.h"
#include "production_types.inc"
static unsigned checks,logs,format_calls;
static char line[4096];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void diag_log(const char*f,...){logs++;va_list a;va_start(a,f);vsnprintf(line,sizeof line,f,a);va_end(a);}
#define LOG(...) diag_log(__VA_ARGS__)
static VkFormat vk_format(uint32_t f){format_calls++;return f==0x25?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_UNDEFINED;}
static unsigned env_reads;
static char *trace_getenv(const char *name){if(!strcmp(name,"LARECOMP_SAMPLING_TRACE"))env_reads++;return getenv(name);}
static unsigned stat_calls,unlink_calls;static int fail_unlink;static uint64_t frame;
static uint64_t vk_frame_counter(void){return frame;}
static int trace_stat(const char *p,struct stat *s){stat_calls++;return stat(p,s);}
static int trace_unlink(const char *p){unlink_calls++;return fail_unlink?-1:unlink(p);}
#define getenv trace_getenv
#define stat(p,s) trace_stat(p,s)
#define unlink(p) trace_unlink(p)
#include "production_sampling_trace.inc"
static uint64_t material_probe_frame;static bool material_projection_done;
#define MATERIAL_PROBE_MAX_DRAWS 2048u
#include "production_material_gate.inc"
#undef getenv
#undef stat
#undef unlink
static void request(void){FILE*f=fopen("sampling-trace.request","w");CHECK(f);CHECK(!fclose(f));}
int main(int argc,char**argv){CHECK(argc==2);ShaderTexture image={.type=TEXTYPE_3D,.bindless=true,.bindless_cbuf=7,.bindless_offset=0x80};
 char cwd[4096],dir[]="/tmp/raitchi-sampling-trace-XXXXXX";CHECK(getcwd(cwd,sizeof cwd));CHECK(mkdtemp(dir));CHECK(!chdir(dir));
 CHECK(!setenv("LARECOMP_SAMPLING_TRACE",argv[1],1));
 if(!strcmp(argv[1],"unset"))CHECK(!unsetenv("LARECOMP_SAMPLING_TRACE"));
 CHECK(!unsetenv("LARECOMP_MATERIAL_PROBE"));
 material_probe_poll();CHECK(!material_probe_file);CHECK(access("material-probe.log",F_OK));
 FILE *request_file=fopen("material-probe.request","w");CHECK(request_file);CHECK(!fclose(request_file));frame=100;
 material_projection_done=true;material_probe_poll();
 if(!strcmp(argv[1],"1")){
  CHECK(material_probe_file&&material_probe_count==0&&material_probe_frame==100&&!material_projection_done);CHECK(material_probe_bound_enabled());CHECK(access("material-probe.request",F_OK));
  frame=101;material_probe_poll();CHECK(!material_probe_file);CHECK(!unlink("material-probe.log"));
  request_file=fopen("material-probe.request","w");CHECK(request_file);CHECK(fputs("projection_fs=462a9ebb0e419a61\n",request_file)>=0);CHECK(!fclose(request_file));frame=102;material_probe_poll();CHECK(material_probe_file&&material_projection_full_stage_hash&&material_projection_fs==UINT64_C(0x462a9ebb0e419a61));frame=103;material_probe_poll();CHECK(!material_probe_file);CHECK(!unlink("material-probe.log"));
  request_file=fopen("material-probe.request","w");CHECK(request_file);for(unsigned i=0;i<65;i++)CHECK(fputc('x',request_file)!=EOF);CHECK(!fclose(request_file));frame=104;material_probe_poll();CHECK(!material_probe_file);CHECK(access("material-probe.request",F_OK));CHECK(access("material-probe.log",F_OK));
  request_file=fopen("material-probe.request","w");CHECK(request_file);CHECK(fputs("projection_fs=../../evil",request_file)>=0);CHECK(!fclose(request_file));frame=105;material_probe_poll();CHECK(!material_probe_file&&access("material-probe.request",F_OK));
 }else{CHECK(!material_probe_file&&stat_calls==0);CHECK(!access("material-probe.request",F_OK));CHECK(!unlink("material-probe.request"));}
 logs=stat_calls=unlink_calls=0;frame=0;
 if(strcmp(argv[1],"1")){
  request();sampling_trace_poll();CHECK(stat_calls==0&&unlink_calls==0);CHECK(!access("sampling-trace.request",F_OK));
  void*hole=mmap(NULL,4096,PROT_NONE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);CHECK(hole!=MAP_FAILED);
  for(unsigned i=0;i<1000;i++)sampling_trace_binding(1,0x1234,i,&image,hole,hole,false,false);
  CHECK(logs==0&&format_calls==0);CHECK(!setenv("LARECOMP_SAMPLING_TRACE","1",1));CHECK(!sampling_trace_admit(1,9,9,2));CHECK(!munmap(hole,4096));
 }else{
  NvnSamplerDesc s={.min_filter=5,.mag_filter=1,.lod_min=0,.lod_max=1000,.compare_mode=1,.compare_func=2};
  TexBackend b={.format=VK_FORMAT_R8G8B8A8_UNORM};NvnTexture t={.format=0x25,.width=64,.height=64,.depth=64,.levels=7,.debug_id=2119,.backend=&b};NvnTexEntry e={.tex=&t,.has_view=true,.view={.setmask=1,.base_level=1,.num_levels=1}};
  sampling_trace_binding(1,0xabc,0x1070011a,&image,&e,&s,false,false);CHECK(logs==1);CHECK(strstr(line,"texture_id=2119 extent=64x64x64 levels=7"));CHECK(strstr(line,"sampler_source=pool sampler_default=0 min=5 mag=1"));CHECK(strstr(line,"mip=1+1"));
  for(unsigned i=0;i<100;i++)sampling_trace_binding(1,0xabc,0x1070011a,&image,&e,&s,false,false);CHECK(logs==1);
  sampling_trace_binding(0,0xabc,0x1070011a,&image,&e,&s,false,false);CHECK(logs==2);
  sampling_trace_binding(1,0xdef,0x1070011a,&image,&e,&s,false,false);CHECK(logs==3);
  image.type=TEXTYPE_2D;sampling_trace_binding(1,0xabc,0x1070011a,&image,&e,&s,false,false);CHECK(logs==4);
  sampling_trace_binding(1,0xabc,0,&image,NULL,NULL,true,true);CHECK(logs==5);CHECK(strstr(line,"sampler_source=renderer-default sampler_default=1 min=1 mag=1"));CHECK(strstr(line,"null_view_fallback=1"));CHECK(strstr(line,"lod=0..0.25"));
  for(unsigned i=0;i<1000;i++)sampling_trace_binding(1,0xabc,i+1,&image,&e,&s,false,false);CHECK(logs==64);
  unsigned before=format_calls;sampling_trace_binding(1,0x999,9999,&image,(void*)1,(void*)1,false,false);CHECK(logs==64&&format_calls==before);
  // Directories do not arm; a request created later in the same frame waits
  // until the next frame. A failed consume does not reset the record budget.
  CHECK(!mkdir("sampling-trace.request",0700));frame=1;sampling_trace_poll();CHECK(sampling_trace_count==64&&logs==64&&unlink_calls==0);CHECK(!rmdir("sampling-trace.request"));
  request();sampling_trace_poll();CHECK(!access("sampling-trace.request",F_OK));CHECK(stat_calls==1);
  frame=2;fail_unlink=1;sampling_trace_poll();CHECK(sampling_trace_count==64&&logs==64);CHECK(!access("sampling-trace.request",F_OK));
  fail_unlink=0;frame=3;sampling_trace_poll();CHECK(sampling_trace_count==0&&logs==65);CHECK(access("sampling-trace.request",F_OK));CHECK(strstr(line,"armed frame=3 limit=64"));
  unsigned polls=stat_calls;for(unsigned i=0;i<1000;i++)sampling_trace_poll();CHECK(stat_calls==polls);
  sampling_trace_binding(1,0xabc,0x1070011a,&image,&e,&s,false,false);CHECK(sampling_trace_count==1&&logs==66);
  for(unsigned i=0;i<1000;i++)sampling_trace_binding(1,0xabc,i+1,&image,&e,&s,false,false);CHECK(sampling_trace_count==64&&logs==129);
  frame=4;sampling_trace_poll();CHECK(sampling_trace_count==64&&logs==129);
 }
 CHECK(env_reads==1);
 // Every requested material draw now carries real pool sampler fields, not
 // only the first64 startup tuples or a shader-specific projection capture.
 FILE *meta=tmpfile();CHECK(meta);NvnSamplerDesc ao={.min_filter=5,.mag_filter=1,.wrap={1,7,5},.lod_min=1,.lod_max=1,.compare_func=4};
 material_sampler_metadata(meta,2,258,&ao,true);CHECK(!fseek(meta,0,SEEK_SET));char fields[1024];CHECK(fgets(fields,sizeof fields,meta));CHECK(strstr(fields,"source=pool min=5 mag=1 wrap=1,7,5"));CHECK(strstr(fields,"lod_min=1 lod_max=1"));CHECK(strstr(fields,"compare_mode=0 compare_func=4"));CHECK(!fclose(meta));
 meta=tmpfile();CHECK(meta);material_sampler_metadata(meta,2,258,NULL,false);CHECK(!fseek(meta,0,SEEK_SET));CHECK(fgets(fields,sizeof fields,meta));CHECK(strstr(fields,"source=unresolved")&&!strstr(fields," min="));CHECK(!fclose(meta));
 if(!access("sampling-trace.request",F_OK))CHECK(!unlink("sampling-trace.request"));CHECK(!chdir(cwd));CHECK(!rmdir(dir));
 printf("%u trace gate/dedup/metadata assertions; records=%u\n",checks,logs);return 0;}
