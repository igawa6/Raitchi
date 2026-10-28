/* Actual selector, source exporter and kernel copy with a protected-page inventory. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/session_layout.h"
#include "shader.h"
#include "production_projection_types.inc"
static unsigned checks,queries;static uint64_t base,page;static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void query_memory_unlocked(uint64_t address,uint64_t output){uint64_t*v=(void*)(uintptr_t)output;queries++;memset(v,0,40);if(address>=base&&address<base+page){v[0]=base;v[1]=page;v[2]=5;v[3]=1;}else{v[0]=address;v[1]=page;}}
#include "production_kernel_copy.inc"
static struct {NvnShaderStage*program_unused;}unused;
static struct {struct {NvnShaderStage stages[6];}*program;Range ubuf[2][18];}st;
#include "production_projection.inc"
static void reject(const unsigned char*s,size_t n){uint64_t h=99;bool full=true;CHECK(!material_projection_selector(s,n,&h,&full));CHECK(h==99&&full);}
int main(void){
 (void)unused;uint64_t h=0;bool full=true;CHECK(material_projection_selector(NULL,0,&h,&full)&&h==UINT64_C(0x44b584f0d922d49d)&&!full);
 const char*valid="projection_fs=462a9ebb0e419a61";CHECK(material_projection_selector((void*)valid,strlen(valid),&h,&full)&&h==UINT64_C(0x462a9ebb0e419a61)&&full);
 const char*upper="projection_fs=ABCDEF0123456789\n";CHECK(material_projection_selector((void*)upper,strlen(upper),&h,&full)&&h==UINT64_C(0xabcdef0123456789)&&full);
 for(size_t n=1;n<30;n++)reject((void*)valid,n);unsigned char bad[66];memset(bad,'x',sizeof bad);for(size_t n=32;n<=66;n++)reject(bad,n);
 memcpy(bad,valid,30);for(unsigned i=0;i<30;i++){unsigned char old=bad[i];bad[i]='!';reject(bad,30);bad[i]=old;}bad[30]='\r';reject(bad,31);reject(NULL,1);
 page=sysconf(_SC_PAGESIZE);unsigned char*memory=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);CHECK(memory!=MAP_FAILED);base=(uintptr_t)memory;CHECK(!mprotect(memory+page,page,PROT_NONE));memcpy(memory+page-30,valid,30);CHECK(material_projection_selector(memory+page-30,30,&h,&full));
 for(unsigned i=0;i<256;i++)memory[i]=(unsigned char)i;
 SessionLayout layout={.window={base,base+page*2}};MaterialProjection cap={.meta=tmpfile(),.data=tmpfile(),.layout=&layout};CHECK(cap.meta&&cap.data);
 unsigned char*copy=material_projection_payload(&cap,"valid",base,32);CHECK(copy&&!memcmp(copy,memory,32)&&cap.saved==32);free(copy);long pos=ftell(cap.data);CHECK(pos==32);
 unsigned before=queries;CHECK(!material_projection_payload(&cap,"hole",base+page-16,32));CHECK(cap.saved==32&&ftell(cap.data)==pos);CHECK(queries>before);
 before=queries;CHECK(!material_projection_payload(&cap,"overflow",UINT64_MAX-15,32));CHECK(queries==before);CHECK(!material_projection_range(&layout,base+page*2,1));CHECK(!material_projection_range(&layout,base,0));
 cap.saved=MATERIAL_PROJECTION_PAYLOAD_BYTES+1;CHECK(!material_projection_payload(&cap,"bad-budget",base,1));CHECK(ftell(cap.data)==pos);cap.saved=MATERIAL_PROJECTION_PAYLOAD_BYTES-15;CHECK(!material_projection_payload(&cap,"over-budget",base,16));CHECK(ftell(cap.data)==pos);
 cap.saved=32;struct {NvnShaderStage stages[6];}program={0};st.program=(void*)&program;program.stages[0]=(NvnShaderStage){.code_addr=base,.const_offset=128,.const_size=16};program.stages[1]=(NvnShaderStage){.code_addr=base+page-8,.const_offset=16};Prog p={0};p.fp[0].hash=shader_code_hash(memory,144);p.fp[0].len=144;
 material_projection_shader(&cap,&p,0);CHECK(cap.saved==176);material_projection_shader(&cap,&p,1);CHECK(cap.saved==176&&ftell(cap.data)==176);
 ShaderInfo vi={.cbuf_mask=(1u<<9)|(1u<<12)|(1u<<20)},fi={.cbuf_mask=1u<<11};p.vi=&vi;p.fi=&fi;
 st.ubuf[0][6]=(Range){base,16};st.ubuf[0][17]=(Range){base+32,16};st.ubuf[0][7]=(Range){base+page,UINT64_MAX};st.ubuf[1][8]=(Range){base+64,16};
 material_projection_uniforms(&cap,&p);CHECK(cap.saved==224); // used VS9/20 and FS11, not unused huge CB10
 CHECK(!fflush(cap.meta));CHECK(!fseek(cap.meta,0,SEEK_SET));char log[8192];size_t n=fread(log,1,sizeof log-1,cap.meta);log[n]=0;CHECK(strstr(log,"name=shader-vs")&&strstr(log,"constant_bytes=16")&&strstr(log,"cached_identity_match=1"));CHECK(strstr(log,"name=shader-fs")&&strstr(log,"kernel-copy-failed"));CHECK(strstr(log,"name=ubo-0-9")&&strstr(log,"name=ubo-0-20")&&strstr(log,"name=ubo-1-11"));CHECK(!strstr(log,"name=ubo-0-10"));CHECK(strstr(log,"cbuf=12 slot=9 guest=0 bound_bytes=0 reflected_used=1"));CHECK(strstr(log,"metadata-only-not-reflected"));
 cap.saved=MATERIAL_PROJECTION_PAYLOAD_BYTES-16;material_projection_uniforms(&cap,&p);CHECK(cap.saved==MATERIAL_PROJECTION_PAYLOAD_BYTES); // VS palette takes last budget before FS
 CHECK(!fclose(cap.meta));CHECK(!fclose(cap.data));CHECK(!munmap(memory,page*2));printf("%u production selector/export/kernel-copy assertions passed\n",checks);return 0;}
