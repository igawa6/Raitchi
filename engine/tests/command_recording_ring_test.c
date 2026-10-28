/* Synthetic integration of actual recorder + lifetime bridge/core. No GPU or guest code. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "runtime/aot_cpu.h"
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static jmp_buf rejected;
static bool rejecting;
static char fatal_message[512];
static void fatal(const char *format,...) __attribute__((noreturn));
static void fatal(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(fatal_message,sizeof fatal_message,format,args);va_end(args);
    if(rejecting)longjmp(rejected,1);
    fputs(fatal_message,stderr);exit(1);
}
void shadow_log(const char *format,...){(void)format;}
#define raw_rec shadow_raw_rec
#include "production_cmdbuf.c"
#undef raw_rec
#include "production_command_recording.c"
#define REJECT(code) do {rejecting=true;if(!setjmp(rejected)){code;CHECK(false);}rejecting=false;}while(0)
__thread uint64_t g_submit_ns;
bool kernel_copy_readable(void *out,uint64_t in,uint64_t bytes){memcpy(out,(const void*)(uintptr_t)in,(size_t)bytes);return true;}
uint64_t plat_now_ns(void){return 0;}
bool rq_on(void){return false;}
uint64_t rq_submit_leased(uint64_t handle,NvnClLease *lease){(void)handle;(void)lease;fatal("queue entered\n");}
uint64_t rq_wait_item(uint64_t item){(void)item;fatal("queue entered\n");}
bool rq_command_readers_stopped(void){return true;}
void backend_execute(uint64_t handle){(void)handle;fatal("GPU entered\n");}
static _Alignas(16) unsigned char storage[65536];
static _Alignas(16) uint64_t object[16],pool_object[16],other_pool_object[16];
static NvnPool pool,other_pool;
static Cpu cpu;
static uint64_t callback_size;
static unsigned callbacks;
static bool nested_once;
static NvnCmdBuf *cb(void){return (void*)(uintptr_t)object[0];}
static void add_pool(uint64_t *which,uint64_t offset,uint64_t size){
    cpu.x[0]=(uintptr_t)object;cpu.x[1]=(uintptr_t)which;cpu.x[2]=offset;cpu.x[3]=size;hle_nvnCommandBufferAddCommandMemory(&cpu);
}
static void add(uint64_t offset,uint64_t size){add_pool(pool_object,offset,size);}
static void begin(void){cpu.x[0]=(uintptr_t)object;hle_nvnCommandBufferBeginRecording(&cpu);}
static uint64_t end(void){cpu.x[0]=(uintptr_t)object;hle_nvnCommandBufferEndRecording(&cpu);return cpu.x[0];}
static void barrier(uint64_t value){cpu.x[0]=(uintptr_t)object;cpu.x[1]=value;hle_nvnCommandBufferBarrier(&cpu);}
void guest_call(Cpu *c,uint64_t address){
    CHECK(address==0x42 && c->x[0]==(uintptr_t)object && c->x[1]==0 && c->x[2]==44 && c->x[3]==0x678);
    ++callbacks;
    if(nested_once){
        nested_once=false;c->x[0]=(uintptr_t)object;c->x[1]=2;c->x[2]=3;c->x[3]=29;
        uint64_t values[29]={0};c->x[4]=(uintptr_t)values;hle_nvnCommandBufferBindTextures(c);return;
    }
    c->x[0]=(uintptr_t)object;c->x[1]=(uintptr_t)pool_object;c->x[2]=0;c->x[3]=callback_size;
    hle_nvnCommandBufferAddCommandMemory(c);
}
static uint64_t digest(const NvnClBodyView *view){
    uint64_t hash=UINT64_C(1469598103934665603);
    for(size_t i=0;i<view->bytes;i++){hash^=((const uint8_t*)view->records)[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
static NvnClBodyView body(NvnClLease *lease,uint64_t handle){NvnClBodyView v;CHECK(nvn_cl_lease_body(lease,handle,&v));return v;}
static NvnClLease *lease(uint64_t handle){NvnClLease *out=NULL;CHECK(nvn_cl_acquire(nvn_command_store,&handle,1,&out)==NVN_CL_OK);return out;}
static void prefix_case(bool nested){
    add(0,1024);uint64_t first=0;
    for(unsigned i=0;i<32;i++){begin();barrier(i);uint64_t h=end();if(!i)first=h;}
    NvnClLease *old=lease(first);NvnClBodyView old_body=body(old,first);uint64_t old_digest=digest(&old_body);
    begin();NvnCmdBuf *b=cb();uint64_t root=b->start,root_offset=recording(b)->root_offset;
    CHECK(b->guest_used==512 && b->recording_guest_floor==(uintptr_t)storage+512);
    while(b->guest_used+44<=b->guest_capacity)barrier(0x900+b->guest_used);
    CHECK(b->guest_used==992 && recording(b)->nspans==1);
    /* Same CPU backing through an unregistered pool must fail before state/floor commit. */
    NvnCmdBuf before=*b;other_pool=pool;other_pool_object[0]=(uintptr_t)&other_pool;
    REJECT(add_pool(other_pool_object,0,508));CHECK(strstr(fatal_message,"unregistered command pool") && !memcmp(&before,b,sizeof before));
    b->callback=0x42;b->callback_data=0x678;callback_size=508;callbacks=0;nested_once=nested;
    uint64_t values[29];for(unsigned i=0;i<29;i++)values[i]=UINT64_C(0x55aa000000000000)+i;
    cpu.x[0]=(uintptr_t)object;cpu.x[1]=2;cpu.x[2]=3;cpu.x[3]=29;cpu.x[4]=(uintptr_t)values;
    hle_nvnCommandBufferBindTextures(&cpu);
    CHECK(callbacks==1+(unsigned)nested && b->start==root && b->guest_used==(nested?80:40) &&
          b->recording_guest_floor==(uintptr_t)storage && recording(b)->root_offset==root_offset && recording(b)->nspans==2);
    before=*b;REJECT(add(0,4));CHECK(!memcmp(&before,b,sizeof before));
    uint64_t handle=end();NvnClLease *current=lease(handle);NvnClBodyView view=body(current,handle);
    NvnClSpan prefix,tail;CHECK(nvn_cl_lease_span(current,handle,0,&prefix) && nvn_cl_lease_span(current,handle,1,&tail));
    CHECK(prefix.begin==0 && prefix.end==(nested?84:44) && tail.begin==512 && tail.end==992);
    size_t at=0;unsigned payloads=0;
    while(at<view.bytes){const CmdRec *r=(const void*)((const uint8_t*)view.records+at);CHECK(r->op!=CMD_JUMP);
        if(r->op==CMD_BIND_TEXTURES){CHECK(r->size==312);if(++payloads==1+(unsigned)nested)CHECK(!memcmp(r->extra,values,sizeof values));}
        at+=r->size;
    }
    CHECK(payloads==1+(unsigned)nested && digest(&old_body)==old_digest);
    CHECK(nvn_cl_lease_release_completed(current,true)==NVN_CL_OK && nvn_cl_lease_release_completed(old,true)==NVN_CL_OK);
    /* Reset Begin's floor from the new chunk cursor; earlier lower chunks cannot disappear. */
    add(128,1024);begin();barrier(1);uint64_t floor=b->recording_guest_floor;
    add(2048,256);CHECK(!b->guest_used && b->recording_guest_floor==floor);REJECT(add(2048,4));
    add(64,256);CHECK(b->recording_guest_floor==(uintptr_t)storage+64);REJECT(add(64,4));(void)end();
}
static void empty_rebind(void){
    add(0,4);begin();NvnCmdBuf *b=cb();uint64_t host=b->host,start=b->start,used=b->used,hcap=b->hcap;
    CHECK(!recording(b)->nspans);add(0,8192);
    CHECK(b->host==host && b->start==start && b->used==used && b->hcap==hcap);
    /* A distinct pool generation with the same backing must select its own logical root. */
    other_pool=pool;CHECK(nvn_command_pool_created(&other_pool));other_pool_object[0]=(uintptr_t)&other_pool;
    add_pool(other_pool_object,0,8192);
    uint64_t domain=proof_of(&other_pool).logical;CHECK(recording(b)->root_domain==domain && !recording(b)->root_offset);
    barrier(0x123);CHECK(recording(b)->nspans==1);uint64_t handle=end();NvnClLease *l=lease(handle);NvnClSpan span;
    CHECK(nvn_cl_lease_span(l,handle,0,&span) && span.domain==domain && span.begin==0 && span.end==16);
    CHECK(nvn_cl_lease_release_completed(l,true)==NVN_CL_OK);nvn_command_pool_destroyed(&other_pool);
}
int main(void){
    CHECK(nvn_command_lifetime_enabled());memset(storage,0xa5,sizeof storage);
    pool=(NvnPool){.storage=(uintptr_t)storage,.size=sizeof storage};pool_object[0]=(uintptr_t)&pool;
    CHECK(nvn_command_pool_created(&pool));cpu.x[0]=(uintptr_t)object;hle_nvnCommandBufferInitialize(&cpu);CHECK(cpu.x[0]==1);
    prefix_case(false);prefix_case(true);empty_rebind();
    for(size_t i=0;i<sizeof storage;i++)CHECK(storage[i]==0xa5);
    cpu.x[0]=(uintptr_t)object;hle_nvnCommandBufferFinalize(&cpu);nvn_command_pool_destroyed(&pool);
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!nvn_command_store && !atomic_load(&scratch_capacity));
    printf("{\"state\":\"passed\",\"checks\":%u,\"command_lifetime_mode\":\"on\",\"actual_recording_bridge\":true}\n",checks);return 0;
}
