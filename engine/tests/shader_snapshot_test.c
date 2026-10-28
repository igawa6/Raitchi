#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include "nvn/shader_snapshot.h"
#include "nvn/maxwell/shader.h"
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
#define LOG(...) ((void)0)
#define GUEST_LO UINT64_C(1)
#define GUEST_HI UINT64_MAX
#define HEAP_BASE UINT64_C(0x1200000000)
#define KERNEL_READABLE_COPY_MAX (UINT64_C(16)*1024*1024)
typedef struct{uint64_t code_addr;uint32_t const_offset,const_size;}NvnShaderStage;
typedef struct{NvnShaderStage stages[2];}NvnProgram;
typedef struct{int state;uint64_t hash;}TestFp;
typedef struct{uint64_t code[2],cmax[2],csize[2],chash;const uint8_t*read_code[2];ShaderKey vkey,fkey;TestFp fp[2];}Prog;
static uint64_t readable_begin,readable_size;
static unsigned readable_calls;
static uint64_t kernel_heap_size(void){return 4096;}
static bool kernel_copy_readable(void*dst,uint64_t source,uint64_t size){++readable_calls;if(!size||size>KERNEL_READABLE_COPY_MAX||source<readable_begin||source-readable_begin>readable_size||size>readable_size-(source-readable_begin))return false;memcpy(dst,(void*)(uintptr_t)source,(size_t)size);return true;}
#include "shader_snapshot_production.h"
static unsigned fp_calls;
static void shader_fp_check(TestFp*fp,const NvnShaderStage*s,const char*stage){(void)stage;++fp_calls;fp->hash=shader_code_hash((void*)(uintptr_t)s->code_addr,(size_t)s->const_offset+s->const_size);fp->state=1;}
#include "shader_snapshot_fingerprint.inc"
static Prog actual_capture(NvnProgram*np){ShaderKey key={0},*vkey=&key,*fkey=&key;Prog value={0},*p=&value;
#include "shader_snapshot_admission.inc"
#include "shader_snapshot_assignment.inc"
 return value;
}
static bool copy_bytes(void*dst,uint64_t source,uint64_t size){memcpy(dst,(void*)(uintptr_t)source,(size_t)size);return true;}
static bool copy_fails(void*dst,uint64_t source,uint64_t size){(void)source;memset(dst,0xa5,(size_t)size);return false;}
static void bounded(void){
 uint8_t data[256],other[256];memset(data,0x4d,sizeof data);memcpy(other,data,sizeof data);NvnShaderSnapshotStore*s=nvn_shader_snapshots_create(640,256);CHECK(s);enum NvnShaderSnapshotResult why;
 const NvnShaderSnapshot*a=nvn_shader_snapshot_capture(s,(uintptr_t)data,128,20,copy_bytes,&why);CHECK(a&&why==NVN_SNAPSHOT_OK);uint64_t charged=nvn_shader_snapshots_used(s);CHECK(charged>=148&&charged<=640);
 for(unsigned variant=0;variant<10000;++variant)CHECK(nvn_shader_snapshot_capture(s,(uintptr_t)other,128,20,copy_bytes,&why)==a);CHECK(nvn_shader_snapshots_used(s)==charged);
 data[147]^=1;const NvnShaderSnapshot*b=nvn_shader_snapshot_capture(s,(uintptr_t)data,128,20,copy_bytes,&why);CHECK(b&&b!=a&&nvn_shader_snapshot_data(a)[147]!=nvn_shader_snapshot_data(b)[147]);CHECK(nvn_shader_snapshots_used(s)==charged*2);
 const NvnShaderSnapshot*boundary=nvn_shader_snapshot_capture(s,(uintptr_t)other,144,4,copy_bytes,&why);CHECK(boundary&&boundary!=a);CHECK(nvn_shader_snapshots_used(s)==charged*3);data[146]^=1;CHECK(!nvn_shader_snapshot_capture(s,(uintptr_t)data,128,20,copy_bytes,&why)&&why==NVN_SNAPSHOT_LIMIT);CHECK(nvn_shader_snapshots_used(s)==charged*3);CHECK(nvn_shader_snapshot_capture(s,(uintptr_t)other,128,20,copy_bytes,&why)==a);
 CHECK(!nvn_shader_snapshot_capture(s,(uintptr_t)data,128,20,copy_fails,&why)&&why==NVN_SNAPSHOT_COPY);CHECK(nvn_shader_snapshots_used(s)==charged*3);CHECK(!nvn_shader_snapshot_capture(s,UINT64_MAX-4,128,20,copy_bytes,&why)&&why==NVN_SNAPSHOT_ARGUMENT);CHECK(!nvn_shader_snapshot_capture(s,(uintptr_t)data,256,1,copy_bytes,&why)&&why==NVN_SNAPSHOT_LIMIT);CHECK(!nvn_shader_snapshot_capture(s,(uintptr_t)data,0,20,copy_bytes,&why));CHECK(!nvn_shader_snapshot_capture(NULL,(uintptr_t)data,128,20,copy_bytes,&why));CHECK(!nvn_shader_snapshots_create(0,256));CHECK(!nvn_shader_snapshots_create(640,UINT64_MAX));nvn_shader_snapshots_destroy(s);
}
static void admission(void){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);uint8_t*map=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);CHECK(!mprotect(map+page,page,PROT_NONE));uint8_t*code=map+page-276;memset(code,0,276);uint64_t brx=UINT64_C(0xe250000000000000);memcpy(code+0x88,&brx,8);for(unsigned i=0;i<20;++i)code[256+i]=(uint8_t)(i+1);
 readable_begin=(uintptr_t)code;readable_size=276;NvnProgram np={.stages={{(uintptr_t)code,256,20},{0,0,0}}};CHECK(!hs_constants_valid((uintptr_t)code,256,20,HEAP_BASE,kernel_heap_size()));CHECK(!mprotect(map,page,PROT_READ));Prog p=actual_capture(&np);CHECK(p.chash&&p.csize[0]==20&&p.cmax[0]==256&&p.code[0]==(uintptr_t)code&&p.read_code[0]);CHECK(p.read_code[0]!=(void*)code&&!memcmp(p.read_code[0],code,276));uint64_t stage_hash=hs_snapshot_stage_hash(&np.stages[0],p.read_code[0]);CHECK(stage_hash==shader_code_hash(code,276));CHECK(!hs_snapshot_stage_hash(&np.stages[0],NULL));uint64_t used=nvn_shader_snapshots_used(hs_snapshot_store);Prog same=actual_capture(&np);CHECK(same.chash==p.chash&&same.read_code[0]==p.read_code[0]&&nvn_shader_snapshots_used(hs_snapshot_store)==used);
 CHECK(!mprotect(map,page,PROT_READ|PROT_WRITE));code[275]^=1;Prog changed=actual_capture(&np);CHECK(changed.chash&&changed.chash!=p.chash&&changed.read_code[0]!=p.read_code[0]);CHECK(p.read_code[0][275]!=code[275]);p.code[0]=UINT64_MAX;CHECK(prog_code(&p,0)==p.read_code[0]);CHECK(!mprotect(map,page,PROT_NONE));CHECK(prog_code(&p,0)[275]!=changed.read_code[0][275]);CHECK(hs_snapshot_stage_hash(&np.stages[0],p.read_code[0])==stage_hash); // delayed consumers own actual immutable bytes
 Prog seed={0};const uint8_t*inputs[2]={p.read_code[0],NULL};prog_fp_snapshot_init(&seed,&np,inputs);CHECK(seed.fp[0].state==1&&seed.fp[0].hash==stage_hash&&seed.fp[1].state==2&&fp_calls==1);prog_fp_snapshot_init(&seed,&np,inputs);CHECK(fp_calls==1);Prog failed={0};const uint8_t*none[2]={NULL,NULL};prog_fp_snapshot_init(&failed,&np,none);CHECK(failed.fp[0].state==2&&failed.fp[1].state==2&&fp_calls==1);
 CHECK(!mprotect(map,page,PROT_READ|PROT_WRITE));readable_size=275;Prog hole=actual_capture(&np);CHECK(!hole.chash&&!hole.read_code[0]&&!hole.csize[0]&&hole.code[0]==(uintptr_t)code);readable_size=276;
 np.stages[0].const_size=0;Prog no_constants=actual_capture(&np);CHECK(!no_constants.chash&&!no_constants.read_code[0]&&!no_constants.csize[0]); // original BRX fallback retained
 np.stages[0].const_size=20;unsigned calls=readable_calls;np.stages[0].code_addr=UINT64_MAX-16;Prog invalid=actual_capture(&np);CHECK(!invalid.chash&&!invalid.read_code[0]&&readable_calls==calls);
 CHECK(!munmap(map,page*2));nvn_shader_snapshots_destroy(hs_snapshot_store);hs_snapshot_store=NULL;
}
static NvnShaderSnapshotStore*threads_store;static uint8_t shared[148];static const NvnShaderSnapshot*thread_results[8];static _Atomic unsigned active,max_active;
static bool counted_copy(void*d,uint64_t s,uint64_t n){unsigned now=atomic_fetch_add(&active,1)+1,old=atomic_load(&max_active);while(old<now&&!atomic_compare_exchange_weak(&max_active,&old,now)){}memcpy(d,(void*)(uintptr_t)s,(size_t)n);atomic_fetch_sub(&active,1);return true;}
static void*thread_capture(void*arg){uintptr_t id=(uintptr_t)arg;for(unsigned i=0;i<1000;++i)thread_results[id]=nvn_shader_snapshot_capture(threads_store,(uintptr_t)shared,128,20,counted_copy,NULL);return NULL;}
static void concurrency(void){threads_store=nvn_shader_snapshots_create(1024,256);CHECK(threads_store);pthread_t threads[8];for(uintptr_t i=0;i<8;++i)CHECK(!pthread_create(&threads[i],NULL,thread_capture,(void*)i));for(unsigned i=0;i<8;++i)CHECK(!pthread_join(threads[i],NULL));for(unsigned i=0;i<8;++i)CHECK(thread_results[i]&&thread_results[i]==thread_results[0]);CHECK(atomic_load(&max_active)==1);CHECK(nvn_shader_snapshots_used(threads_store)<256);nvn_shader_snapshots_destroy(threads_store);}
int main(void){bounded();admission();concurrency();printf("state=passed checks=%u; actual foreground C1 admission, immutable content dedup, guarded copies and bounded resources\n",checks);}
