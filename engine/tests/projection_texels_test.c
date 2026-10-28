#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "runtime/session_layout.h"
#include "shader.h"
#include "tegra.h"
typedef void*VmaAllocation;
#include "types.inc"
static unsigned checks,queries,resolutions;static uint64_t base,page,readable;static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void query_memory_unlocked(uint64_t a,uint64_t out){uint64_t*v=(void*)(uintptr_t)out;queries++;memset(v,0,40);if(a>=base&&a<base+readable){v[0]=base;v[1]=readable;v[2]=5;v[3]=1;}else{v[0]=a;v[1]=page;}}
#include "kernel.inc"
typedef struct{ShaderInfo*vi,*fi;}Prog;
static struct{NvnDescPool*tpool;}st;
static bool sampled_texture_handle(unsigned stage,const ShaderTexture*i,uint64_t*h){(void)stage;resolutions++;if(i->handle_offset==UINT32_MAX)return false;*h=i->handle_offset;return true;}
static void material_probe_texture(FILE*f,const NvnTexture*t){(void)f;(void)t;}
static void material_probe_view(FILE*f,const char*n,const NvnTextureView*v,bool h){(void)f;(void)n;(void)v;(void)h;}
#include "export.inc"
static void rejected(NvnTexture*t,const char*why){uint64_t n=123;const char*s=material_projection_texture_span(t,&n);CHECK(s&&!strcmp(s,why)&&n==123);}
int main(void){page=sysconf(_SC_PAGESIZE);readable=8*page;unsigned char*mem=mmap(NULL,readable+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mem!=MAP_FAILED);base=(uintptr_t)mem;CHECK(!mprotect(mem+readable,page,PROT_NONE));for(uint64_t i=0;i<readable;i++)mem[i]=(unsigned char)(i*37);
 NvnPool pool={.storage=base,.size=readable};NvnTexture t={.pool=&pool,.addr=base,.format=0x29,.width=64,.height=1,.depth=1,.levels=1,.stride=4096,.flags=0x40,.debug_id=41};t.layout=(TexLayout){.width=64,.height=1,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x29)};tegra_layout_init(&t.layout);uint64_t n;CHECK(!material_projection_texture_span(&t,&n)&&n==4096); // LRT is proven pitched storage, no GPU readback.
 NvnTexture copy=t;copy.flags=0x10;CHECK(!material_projection_texture_span(&copy,&n)&&n==4096);copy.stride=511;rejected(&copy,"linear-stride");copy=t;copy.gpu_write_owned=1;rejected(&copy,"gpu-owned");TexBackend backend={.gpu_owned=true};copy=t;copy.backend=&backend;rejected(&copy,"gpu-owned");copy=t;copy.addr=0;rejected(&copy,"no-cpu-backing");pool.flags=0x200;rejected(&t,"virtual-pool");pool.flags=0;copy=t;copy.addr++;rejected(&copy,"pool-address-mismatch");pool.size=UINT64_MAX;rejected(&t,"pool-address-mismatch");pool.size=readable;copy=t;copy.pool_offset=UINT64_MAX;rejected(&copy,"pool-address-mismatch");copy=t;copy.layout.levels=32;rejected(&copy,"unproven-layout");copy=t;copy.layout.block_depth_log2=32;rejected(&copy,"unproven-layout");copy=t;copy.layout.width=65537;rejected(&copy,"unproven-layout");copy=t;copy.format=UINT32_MAX;rejected(&copy,"unproven-layout");copy=t;copy.flags=0x10;copy.stride=UINT64_MAX;copy.layout.height=2;rejected(&copy,"linear-stride");copy=t;copy.flags=0x10;copy.stride=65537;rejected(&copy,"texture-byte-limit");pool.size=4095;rejected(&t,"pool-range");pool.size=readable;
 NvnTexEntry entries[10]={0};for(unsigned i=0;i<10;i++)entries[i].tex=&t;NvnDescPool desc={.count=10,.entries=entries};st.tpool=&desc;ShaderInfo fi={.ntex=4},vi={0};for(unsigned i=0;i<4;i++)fi.tex[i].handle_offset=i+1;Prog p={.vi=&vi,.fi=&fi};SessionLayout layout={.window={base,base+readable+page}};MaterialProjection cap={.meta=tmpfile(),.data=tmpfile(),.layout=&layout};CHECK(cap.meta&&cap.data);material_projection_texels(&cap,&p);CHECK(cap.saved==4*4096&&queries==4);rewind(cap.data);unsigned char output[4096];for(unsigned i=0;i<4;i++){CHECK(fread(output,1,sizeof output,cap.data)==sizeof output);CHECK(!memcmp(output,mem,sizeof output));}
 // Tail fails kernel proof atomically; no partial file payload. Budget fails before any read.
 t.addr=base+readable-2048;t.pool_offset=readable-2048;pool.size=readable+page;uint64_t saved=cap.saved;unsigned before=queries;material_projection_texels(&cap,&p);CHECK(cap.saved==saved&&queries>before);t.addr=base;t.pool_offset=0;pool.size=readable;cap.saved=MATERIAL_PROJECTION_PAYLOAD_BYTES-4095;before=queries;material_projection_texels(&cap,&p);CHECK(queries==before&&cap.saved==MATERIAL_PROJECTION_PAYLOAD_BYTES-4095);
 cap.saved=0;fi.ntex=10;for(unsigned i=0;i<10;i++)fi.tex[i].handle_offset=i;fi.tex[0].bindless=true;before=resolutions;material_projection_texels(&cap,&p);CHECK(cap.saved==8*4096&&resolutions-before==8); // Valid bindless0, bounded entries.
 fi.ntex=1;desc.count=-1;before=queries;material_projection_texels(&cap,&p);CHECK(queries==before);desc.count=10;fi.tex[0].handle_offset=UINT32_MAX;before=queries;material_projection_texels(&cap,&p);CHECK(queries==before);fi.tex[0].handle_offset=10;material_projection_texels(&cap,&p);CHECK(queries==before);fi.tex[0].is_image=true;before=resolutions;material_projection_texels(&cap,&p);CHECK(resolutions==before);
 CHECK(!fflush(cap.meta));rewind(cap.meta);char log[30000];size_t z=fread(log,1,sizeof log-1,cap.meta);log[z]=0;CHECK(strstr(log,"name=texels-1-0")&&strstr(log,"storage_layout=linear stride=4096 storage_bytes=4096"));CHECK(strstr(log,"not_exact_gpu_cache=1")&&strstr(log,"kernel-copy-failed")&&strstr(log,"payload-budget")&&strstr(log,"entry-limit"));CHECK(!fclose(cap.meta)&&!fclose(cap.data));CHECK(!munmap(mem,readable+page));printf("%u production texture-export/span/proof assertions passed\n",checks);}
