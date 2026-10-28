/* Actual command recorder with allocator canaries and synthetic guest objects. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/aot_cpu.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); exit(1); } } while(0)
static jmp_buf fatal_env;
static int expecting_fatal;
static unsigned fatal_count;
static char fatal_text[1024];
static void fatal(const char *format,...) __attribute__((noreturn));
static void fatal(const char *format,...) {
    ++fatal_count;
    va_list ap; va_start(ap,format); vsnprintf(fatal_text,sizeof fatal_text,format,ap); va_end(ap);
    if (expecting_fatal) longjmp(fatal_env,1);
    fputs(fatal_text,stderr); exit(1);
}
typedef struct Allocation {void *raw,*p;size_t size;struct Allocation *next;} Allocation;
static Allocation *allocations;
static size_t live_allocations;
static unsigned fail_alloc;
static void *checked_malloc(size_t size) {
    if (fail_alloc && !--fail_alloc) return NULL;
    if (size > SIZE_MAX-64) return NULL;
    unsigned char *raw=malloc(size+64);if(!raw)return NULL;
    Allocation *a=malloc(sizeof *a);if(!a){free(raw);return NULL;}
    memset(raw,0x9a,32);memset(raw+32+size,0xbc,32);
    *a=(Allocation){raw,raw+32,size,allocations};allocations=a;++live_allocations;return a->p;
}
static void *checked_calloc(size_t n,size_t size) {
    if(n&&size>SIZE_MAX/n)return NULL;
    void *p=checked_malloc(n*size);if(p)memset(p,0,n*size);return p;
}
static void checked_free(void *p) {
    if(!p)return;
    Allocation **a=&allocations;while(*a&&(*a)->p!=p)a=&(*a)->next;
    CHECK(*a!=NULL);Allocation *old=*a;unsigned char *raw=old->raw;
    for(unsigned i=0;i<32;i++)CHECK(raw[i]==0x9a&&raw[32+old->size+i]==0xbc);
    *a=old->next;--live_allocations;free(old->raw);free(old);
}
static bool capture_trace_log;
static char trace_log[65536];
static size_t trace_log_used;
void shadow_log(const char *format,...) {
    if(!capture_trace_log || trace_log_used>=sizeof trace_log-1)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(trace_log+trace_log_used,sizeof trace_log-trace_log_used,format,args);
    va_end(args);
    if(n>0)trace_log_used+=(size_t)n<sizeof trace_log-trace_log_used?(size_t)n:sizeof trace_log-trace_log_used-1;
}
#define malloc checked_malloc
#define calloc checked_calloc
#define free checked_free
static uint64_t shadow_lookup_probes;
#define NVN_SHADOW_LOOKUP_PROBE() (++shadow_lookup_probes)
#include "production_cmdbuf.c"
#undef malloc
#undef calloc
#undef free

/* This fixture covers the production recorder only with the experimental
 * command lifetime adapter disabled. command_recording.h is the current
 * production interface; fail on any accidental route into its ON-only work.
 * The separate command-lifetime checker exercises the production core/arena. */
static unsigned lifetime_finalize_calls;
bool nvn_command_lifetime_enabled(void) { return false; }
void nvn_command_record_add(NvnCmdBuf *cb, NvnPool *pool, uint64_t offset, uint64_t nominal, uint64_t effective) {
    (void)cb; (void)pool; (void)offset; (void)nominal; (void)effective;
    fatal("legacy recorder unexpectedly routed command add into lifetime mode\n");
}
CmdRec *nvn_command_record_budget(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra, bool begin) {
    (void)c; (void)cb; (void)op; (void)extra; (void)begin;
    fatal("legacy recorder unexpectedly routed record budget into lifetime mode\n");
}
uint64_t nvn_command_record_end(NvnCmdBuf *cb) {
    (void)cb; fatal("legacy recorder unexpectedly routed End into lifetime mode\n");
}
void nvn_command_record_finalize(NvnCmdBuf *cb) { (void)cb; ++lifetime_finalize_calls; }
void nvn_command_record_call(Cpu *c) { (void)c; fatal("legacy recorder unexpectedly routed CALL into lifetime mode\n"); }
void nvn_command_record_copy(Cpu *c) { (void)c; fatal("legacy recorder unexpectedly routed COPY into lifetime mode\n"); }
void nvn_command_submit(Cpu *c) { (void)c; fatal("legacy recorder unexpectedly routed submit into lifetime mode\n"); }
NvnClLease *nvn_command_reader_enter(NvnClLease *lease) { (void)lease; fatal("lifetime reader called in OFF fixture\n"); }
void nvn_command_reader_leave(NvnClLease *lease) { (void)lease; fatal("lifetime reader called in OFF fixture\n"); }
NvnClLease *nvn_command_reader_current(void) { fatal("lifetime reader called in OFF fixture\n"); }
uint64_t nvn_command_resolve(uint64_t handle) { (void)handle; fatal("lifetime resolver called in OFF fixture\n"); }
NvnClLeaseFlags nvn_command_flags(uint64_t handle) { (void)handle; fatal("lifetime flags called in OFF fixture\n"); }
void nvn_command_lifetime_finish_after_consumers(void) {
    /* Production shadow cleanup calls this shared finalizer; lifetime OFF is a no-op. */
    CHECK(!nvn_command_lifetime_enabled());
}
bool nvn_command_lifetime_stats(NvnCommandLifetimeStats *out) { (void)out; fatal("lifetime stats called in OFF fixture\n"); }

static _Alignas(16) unsigned char guest_chunk[65536];
static _Alignas(16) uint64_t guest_object[16],pool_object[16];
static NvnPool pool;
static Cpu cpu;
static unsigned callback_calls;
static uint64_t callback_need,callback_next,callback_size=256;
static bool callback_nest_once;
static unsigned callback_diagnostic_mode;
static void *trace_foreign_control(void *unused) {
    (void)unused;Cpu other={0};other.x[0]=(uintptr_t)guest_object;other.x[1]=0x20;other.x[2]=0x40;other.x[30]=0xfeed;
    hle_nvnCommandBufferAddControlMemory(&other);return NULL;
}
void guest_call(Cpu *c,uint64_t address) {
    CHECK(address==0x42);++callback_calls;callback_need=c->x[2];
    CHECK(c->x[0]==(uintptr_t)guest_object&&c->x[1]==0&&c->x[3]==0x678);
    if(callback_nest_once) {
        uint64_t nested_values[29]={0};
        callback_nest_once=false;c->x[0]=(uintptr_t)guest_object;c->x[1]=2;c->x[2]=3;c->x[3]=29;c->x[4]=(uintptr_t)nested_values;
        hle_nvnCommandBufferBindTextures(c);return;
    }
    if(callback_diagnostic_mode) {
        c->x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemorySize(c);
        c->x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryUsed(c);
        c->x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryFree(c);
        if(callback_diagnostic_mode==1 && memory_trace_callback->origin.depth==1) {
            pthread_t worker;CHECK(!pthread_create(&worker,NULL,trace_foreign_control,NULL));CHECK(!pthread_join(worker,NULL));
            uint64_t lr=c->x[30];c->x[0]=(uintptr_t)guest_object;c->x[1]=2;c->x[2]=3;c->x[3]=0x123;c->x[30]=0x2222;
            hle_nvnCommandBufferBindTexture(c);c->x[30]=lr;
            for(unsigned i=0;i<8;i++)c->x[i]=0xdead0000+i;
            return;
        }
    }
    c->x[0]=(uintptr_t)guest_object;c->x[1]=(uintptr_t)pool_object;c->x[2]=callback_next;c->x[3]=callback_size;
    hle_nvnCommandBufferAddCommandMemory(c);
    for(unsigned i=0;i<8;i++)c->x[i]=0xdead0000+i;
}
static void setup(void) {
    CHECK(!nvn_command_lifetime_enabled());
    callback_size=256;callback_nest_once=false;
    memset(&cpu,0,sizeof cpu);memset(guest_object,0,sizeof guest_object);memset(pool_object,0,sizeof pool_object);
    memset(guest_chunk,0xa5,sizeof guest_chunk);pool=(NvnPool){.storage=(uintptr_t)guest_chunk,.size=sizeof guest_chunk};pool_object[0]=(uintptr_t)&pool;
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferInitialize(&cpu);CHECK(cpu.x[0]==1);
}
static NvnCmdBuf *cb(void) {return (void *)(uintptr_t)guest_object[0];}
static void add(uint64_t offset,uint64_t size) {
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=(uintptr_t)pool_object;cpu.x[2]=offset;cpu.x[3]=size;
    hle_nvnCommandBufferAddCommandMemory(&cpu);
}
static void begin(void){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBeginRecording(&cpu);}
static uint64_t end(void){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferEndRecording(&cpu);return cpu.x[0];}
static void finalize(void){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferFinalize(&cpu);CHECK(!guest_object[0]);}
static void canaries(void){for(unsigned i=0;i<sizeof guest_chunk;i++)CHECK(guest_chunk[i]==0xa5);}
#define REJECT(code) do {unsigned before=fatal_count;expecting_fatal=1; \
    if(!setjmp(fatal_env)){code;CHECK(0);} expecting_fatal=0;CHECK(fatal_count==before+1); \
} while(0)
static unsigned walk(uint64_t handle,unsigned wanted,bool fence);
static void basic(void) {
    setup();add(0,2048);CHECK(cb()->mem==(uintptr_t)guest_chunk&&cb()->host!=(uintptr_t)guest_chunk);
    CHECK(cb()->hcap==SHADOW_INITIAL_LIMIT&&cb()->size==2048);begin();
    uint64_t handle=cb()->start;CHECK(handle==cb()->host);
    for(unsigned i=0;i<100;i++){cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=i;hle_nvnCommandBufferBarrier(&cpu);}
    CHECK(cb()->host!=handle&&cb()->used+JUMP_RESERVE<cb()->hcap); // actual records grow beyond the4KiB root
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryUsed(&cpu);CHECK(cpu.x[0]==1200 && !(cpu.x[0]&3));
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryFree(&cpu);CHECK(cpu.x[0]==844 && !(cpu.x[0]&3));
    cb()->fences=true;CHECK(end()==handle);CHECK(((CmdRec *)(uintptr_t)handle)->x[6]==CMD_LIST_FENCES);
    CHECK(walk(handle,101,true)==1); // all100 payload records survive the root/continuation boundary
    canaries();finalize();CHECK(((CmdRec *)(uintptr_t)handle)->op==CMD_BARRIER);
    CHECK(live_allocations>0);nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void reuse_and_growth(void) {
    setup();add(0,256);uint64_t first=cb()->host;begin();uint64_t handle=end();
    CmdRec prior=*(CmdRec *)(uintptr_t)handle;add(0,256);CHECK(cb()->host==first);add(0,128);CHECK(cb()->host==first&&cb()->hcap==128*8+HOST_FRAMING);
    add(0,512);CHECK(cb()->host!=first&&cb()->hcap==SHADOW_INITIAL_LIMIT);CHECK(!memcmp((void *)(uintptr_t)handle,&prior,sizeof prior));
    uint64_t grown=cb()->host;finalize();CHECK(!memcmp((void *)(uintptr_t)handle,&prior,sizeof prior));
    setup();add(0,512);CHECK(cb()->host==grown);finalize();canaries();
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void chaining_and_callback(void) {
    setup();add(0,256);NvnCmdBuf *b=cb();b->callback=0x42;b->callback_data=0x678;callback_next=512;callback_calls=0;begin();
    uint64_t first=b->host;
    while(b->guest_used+2*12+END_BUDGET<=b->size){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    uint64_t old_used=b->used;uint64_t saved[8]={(uintptr_t)guest_object,0x11,0x22,0x33,0x44,0x55,0x66,0x77};memcpy(cpu.x,saved,sizeof saved);
    hle_nvnCommandBufferBarrier(&cpu);CHECK(!callback_calls);
    old_used=b->used;memcpy(cpu.x,saved,sizeof saved);hle_nvnCommandBufferBarrier(&cpu);
    CHECK(callback_calls==1&&callback_need==16);
    CHECK(!memcmp(cpu.x,saved,sizeof saved));CHECK(b->host!=first);
    CmdRec *jump=(void *)(uintptr_t)(first+old_used);CHECK(jump->op==CMD_JUMP&&jump->x[0]==b->host);
    CmdRec *record=(void *)(uintptr_t)b->host;CHECK(record->op==CMD_BARRIER&&record->x[0]==0x11&&record->x[6]==0x77);
    CHECK(end()==first);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void invalid_ranges(void) {
    setup();add(0,256);uint64_t original=cb()->host;size_t count=live_allocations;
    REJECT(add(sizeof guest_chunk+4,8));REJECT(add(sizeof guest_chunk,4));REJECT(add(0,0));REJECT(add(UINT64_MAX,256));REJECT(add(1,256));REJECT(add(0,255));
    pool.storage=UINT64_MAX-3;pool.size=UINT64_MAX;REJECT(add(8,256));
    pool.storage=16;REJECT(add(0,UINT64_MAX/8+1));
    pool.storage=(uintptr_t)guest_chunk;pool.size=sizeof guest_chunk;
    CHECK(cb()->host==original&&live_allocations==count);
    begin();cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);REJECT(add(0,256));CHECK(cb()->host==original);
    REJECT((void)rec(&cpu,cb(),CMD_BARRIER,UINT32_MAX));
    cb()->used=cb()->hcap;REJECT(add(512,256));cb()->used=sizeof(CmdRec);(void)end();
    fail_alloc=1;REJECT(add(512,256));CHECK(cb()->host==original);
    uint64_t slab_used=g_shadow_slabs->used;g_shadow_slabs->used=g_shadow_slabs->capacity;
    fail_alloc=2;REJECT(add(512,256));CHECK(cb()->host==original);g_shadow_slabs->used=slab_used;
    fail_alloc=0;canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static uint64_t logical_used(void) {
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryUsed(&cpu);return cpu.x[0];
}
static uint64_t logical_free(void) {
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemoryFree(&cpu);return cpu.x[0];
}
static unsigned walk(uint64_t handle,unsigned wanted,bool fence) {
    unsigned records=0,jumps=0;uint64_t at=handle;
    CHECK(((CmdRec*)(uintptr_t)handle)->x[6]==(fence?CMD_LIST_FENCES:0));
    for(unsigned guard=0;guard<10000;guard++) {
        CmdRec *r=(void*)(uintptr_t)at;
        CHECK(r->size>=sizeof(CmdRec) && !(r->size&7));
        if(r->op==CMD_END){CHECK(records==wanted);return jumps;}
        if(r->op==CMD_JUMP){CHECK(r->x[0] && r->x[0]!=at);at=r->x[0];jumps++;continue;}
        CHECK(r->op==CMD_BARRIER);records++;at+=r->size;
    }
    CHECK(!"unterminated host list");return 0;
}
static void logical_and_spills(void) {
    setup();add(0,128);NvnCmdBuf *b=cb();uint64_t handles[32];unsigned spills=0;
    b->callback=0x42;b->callback_data=0x678;callback_calls=0;
    // An empty list advances only its explicit End word. Repeated host framing
    // exhausts backing before the logical128B, producing retained spills.
    for(unsigned i=0;i<32;i++) {
        uint64_t before_host=b->host;begin();CHECK(logical_used()==i*4);CHECK(logical_free()==128-i*4-4);
        if(before_host!=b->host)spills++;
        b->fences=(i&1)!=0;handles[i]=end();CHECK(logical_used()==(i+1)*4);
        if(before_host!=b->host)spills++;
        for(unsigned j=0;j<i;j++)CHECK(handles[j]!=handles[i]);
        (void)walk(handles[i],1,(i&1)!=0);
    }
    CHECK(spills>0 && !callback_calls && logical_used()==128 && !logical_free());
    for(unsigned i=0;i<32;i++)(void)walk(handles[i],1,(i&1)!=0);
    canaries();finalize();for(unsigned i=0;i<32;i++)(void)walk(handles[i],1,(i&1)!=0);
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);

    setup();add(0,256);b=cb();b->callback=0x42;b->callback_data=0x678;callback_calls=0;
    // Force small, still valid host backing to exercise multiple active-list
    // continuations independently of the guest budget/callback path.
    b->hcap=2*sizeof(CmdRec)+JUMP_RESERVE;begin();uint64_t first=b->start;
    for(unsigned i=0;i<8;i++) {
        b->hcap=b->used+JUMP_RESERVE;
        uint64_t before=b->guest_used;cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);
        CHECK(b->guest_used==before+12 && !callback_calls && b->start==first);
    }
    b->hcap=b->used+JUMP_RESERVE;b->fences=true;CHECK(end()==first);
    CHECK(logical_used()==8*12+4);CHECK(walk(first,9,true)==9);
    // Same guest chunk is still self-chaining after a host-only spill, even
    // though current host pointer differs from the cached first generation.
    begin();b->hcap=b->used+JUMP_RESERVE;cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);
    REJECT(add(0,256));(void)end();(void)walk(first,9,true);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void empty_payload_then_guest_callback(void) {
    setup();add(0,512);NvnCmdBuf *b=cb();b->callback=0x42;b->callback_data=0x678;
    callback_calls=0;callback_next=1024;uint64_t empty[24];
    for(unsigned i=0;i<24;i++){begin();empty[i]=end();}
    CHECK(b->guest_used==96 && !callback_calls);
    begin();uint64_t first=b->start,before_host=b->host;
    // Here initial backing is naturally nearly full of host-only framing;
    // logical budget still has ample room for this real480B record.
    (void)rec(&cpu,b,CMD_BARRIER,400);
    CHECK(b->host!=before_host && b->start==first && b->guest_used==156 && !callback_calls);
    unsigned payloads=1;
    while(b->guest_used+16<=b->size) {
        cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);payloads++;
    }
    CHECK(b->guest_used==504);
    uint64_t saved[8]={(uintptr_t)guest_object,3,4,5,6,7,8,9};memcpy(cpu.x,saved,sizeof saved);
    hle_nvnCommandBufferBarrier(&cpu);payloads++;
    CHECK(callback_calls==1 && callback_need==16 && b->guest_used==12 && b->start==first);
    CHECK(!memcmp(cpu.x,saved,sizeof saved));b->fences=true;CHECK(end()==first);
    CHECK(logical_used()==16 && walk(first,payloads+1,true)==2);
    for(unsigned i=0;i<24;i++)(void)walk(empty[i],1,false);
    canaries();finalize();(void)walk(first,payloads+1,true);
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void spill_failure_and_word_costs(void) {
    setup();add(0,256);begin();NvnCmdBuf *b=cb();b->hcap=b->used+JUMP_RESERVE;
    uint64_t host=b->host,used=b->used,guest_used=b->guest_used;size_t allocated=live_allocations;
    fail_alloc=1;REJECT((void)rec(&cpu,b,CMD_BARRIER,0));
    CHECK(b->host==host&&b->used==used&&b->guest_used==guest_used&&live_allocations==allocated);
    uint64_t slab_used=g_shadow_slabs->used;g_shadow_slabs->used=g_shadow_slabs->capacity;
    fail_alloc=2;REJECT((void)rec(&cpu,b,CMD_BARRIER,0));g_shadow_slabs->used=slab_used;
    CHECK(b->host==host&&b->used==used&&b->guest_used==guest_used&&live_allocations==allocated);
    fail_alloc=0;(void)rec(&cpu,b,CMD_BARRIER,0);CHECK(b->guest_used==12);
    (void)rec(&cpu,b,CMD_BARRIER,1);CHECK(b->guest_used==24); //88B also rounds to12B
    (void)rec(&cpu,b,CMD_BARRIER,17);CHECK(b->guest_used==40); //104B rounds to16B
    (void)end();CHECK(logical_used()==44);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    setup();add(0,4);begin();CHECK(!logical_used()&&!logical_free());uint64_t empty=end();CHECK(logical_used()==4);(void)walk(empty,1,false);finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void nominal_over_reservation(void) {
    for(unsigned tail_index=0;tail_index<3;tail_index++) {
        unsigned tail=(unsigned[]){4,8,472}[tail_index];setup();pool.size=1024;
        add(1024-tail,512);NvnCmdBuf *b=cb();uint64_t host=b->host;
        CHECK(b->size==512 && b->guest_capacity==tail && b->guest_used==0);
        CHECK(b->hcap==tail*8+HOST_FRAMING);cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferGetCommandMemorySize(&cpu);CHECK(cpu.x[0]==512);
        CHECK(logical_free()==tail-4);b->callback=0x42;b->callback_data=0x678;callback_calls=0;callback_next=0;
        uint64_t old_handles[118];
        for(unsigned i=0;i<tail/4;i++){begin();old_handles[i]=end();CHECK(logical_used()==(i+1)*4 && logical_used()<=tail);}
        CHECK(!callback_calls && !logical_free());
        for(unsigned i=0;i<tail/4;i++)(void)walk(old_handles[i],1,false);
        // Next Begin cannot consume any of the nominal but unbacked bytes. It
        // requests real new command memory before recording, preserves old End.
        begin();CHECK(callback_calls==1 && callback_need==4);CHECK(b->mem==(uintptr_t)guest_chunk && b->guest_capacity==256 && b->size==256);
        CHECK(!b->guest_used && b->host!=host);(void)end();
        for(unsigned i=0;i<tail/4;i++)(void)walk(old_handles[i],1,false);
        canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    }
    setup();pool.size=1024;add(1016,512);begin();NvnCmdBuf *b=cb();
    b->callback=0x42;b->callback_data=0x678;callback_calls=0;callback_next=0;
    uint64_t first=b->start;cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);
    CHECK(callback_calls==1 && callback_need==16 && b->guest_used==12 && b->guest_capacity==256 && b->start==first);
    CHECK(end()==first);CHECK(walk(first,2,false)==1);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
typedef struct {uint64_t addr,size;} TestRange;
static struct {TestRange ubuf[6][18],sbuf[6][16];uint64_t tex[6][32],img[6][8];} st;
static void replay_bind(CmdRec *r){void*x=r->extra;switch(r->op){
#include "bind_replay_production.inc"
default: CHECK(false);
}}
static void deferred_bindings(void){
    size_t page=(size_t)sysconf(_SC_PAGESIZE);uint8_t*memory=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED);CHECK(!mprotect(memory+page,page,PROT_NONE));
    void (*handlers[])(Cpu*)={hle_nvnCommandBufferBindTexturesDeferred,hle_nvnCommandBufferBindUniformBuffersDeferred,hle_nvnCommandBufferBindStorageBuffersDeferred};
    unsigned ops[]={CMD_BIND_TEXTURES_DEFERRED,CMD_BIND_UNIFORM_BUFFERS_DEFERRED,CMD_BIND_STORAGE_BUFFERS_DEFERRED},limits[]={32,18,16};
    for(unsigned kind=0;kind<3;++kind){
      setup();add(0,4096);begin();uint64_t array=(uintptr_t)(memory+page); // recording must not read this protected array
      cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=5;cpu.x[2]=limits[kind]-2;cpu.x[3]=2;cpu.x[4]=array;uint64_t at=cb()->host+cb()->used;handlers[kind](&cpu);CmdRec*r=(void*)(uintptr_t)at;
      CHECK(r->op==ops[kind]&&r->size==sizeof(CmdRec));CHECK(r->x[0]==5&&r->x[1]==limits[kind]-2&&r->x[2]==2&&r->x[3]==array);
      CHECK(!mprotect(memory+page,page,PROT_READ|PROT_WRITE));uint64_t values[4]={0x123400,32,0x567800,64};memcpy((void*)(uintptr_t)array,values,sizeof values);memset(&st,0x2b,sizeof st);replay_bind(r);
      if(!kind)CHECK(st.tex[5][30]==values[0]&&st.tex[5][31]==values[1]&&st.tex[5][29]==UINT64_C(0x2b2b2b2b2b2b2b2b));
      else {TestRange *ranges=kind==1?st.ubuf[5]:st.sbuf[5];CHECK(ranges[limits[kind]-2].addr==values[0]&&ranges[limits[kind]-2].size==values[1]);CHECK(ranges[limits[kind]-1].addr==values[2]&&ranges[limits[kind]-1].size==values[3]);CHECK(ranges[limits[kind]-3].addr==UINT64_C(0x2b2b2b2b2b2b2b2b));}
      values[0]=0xabcdef;memcpy((void*)(uintptr_t)array,values,sizeof values);replay_bind(r);CHECK(!kind?st.tex[5][30]==values[0]:kind==1?st.ubuf[5][16].addr==values[0]:st.sbuf[5][14].addr==values[0]);
      CHECK(!mprotect(memory+page,page,PROT_NONE));cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;cpu.x[2]=0;cpu.x[3]=0;cpu.x[4]=0;uint64_t used=cb()->used;handlers[kind](&cpu);CHECK(cb()->used==used);
      cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=6;cpu.x[2]=0;cpu.x[3]=1;cpu.x[4]=array;REJECT(handlers[kind](&cpu));cpu.x[1]=0;cpu.x[2]=limits[kind]-1;cpu.x[3]=2;REJECT(handlers[kind](&cpu));cpu.x[2]=0;cpu.x[3]=UINT32_MAX;REJECT(handlers[kind](&cpu));cpu.x[3]=1;cpu.x[4]=UINT64_MAX-1;REJECT(handlers[kind](&cpu));cpu.x[4]=0;REJECT(handlers[kind](&cpu));CHECK(cb()->used==used);
      uint64_t handle=end();CHECK(nvn_command_has_deferred(handle));canaries();finalize();CHECK(nvn_command_has_deferred(handle));nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    }
    CHECK(!munmap(memory,page*2));
}
static uint64_t call_list(uint64_t child){begin();cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=1;cpu.x[2]=(uintptr_t)&child;hle_nvnCommandBufferCallCommands(&cpu);return end();}
static void deferred_nested(void){
    setup();add(0,4096);begin();uint64_t plain=end();CHECK(!nvn_command_has_deferred(plain));uint64_t parent=call_list(plain);CHECK(!nvn_command_has_deferred(parent));
    begin();NvnCmdBuf*b=cb();b->hcap=b->used+JUMP_RESERVE; // force a retained host-only continuation
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;cpu.x[2]=0;cpu.x[3]=1;cpu.x[4]=0x10000;hle_nvnCommandBufferBindTexturesDeferred(&cpu);uint64_t leaf=end();CHECK(nvn_command_has_deferred(leaf));CHECK(((CmdRec*)(uintptr_t)leaf)->op==CMD_BARRIER);
    uint64_t nested=leaf;for(unsigned i=0;i<8;++i){nested=call_list(nested);CHECK(nvn_command_has_deferred(nested));}uint64_t ignored=call_list(nested);CHECK(!nvn_command_has_deferred(ignored)); // replay ignores calls beyond depth8 too
    parent=call_list(leaf);CmdRec*first=(void*)(uintptr_t)parent,*call=(void*)((uint8_t*)first+first->size);CHECK(call->op==CMD_CALL);uint64_t count=call->x[0];call->x[0]=UINT64_MAX;REJECT((void)nvn_command_has_deferred(parent));call->x[0]=count;CHECK(nvn_command_has_deferred(parent));
    uint64_t child;memcpy(&child,call->extra,8);uint64_t invalid=UINT64_MAX-7;memcpy(call->extra,&invalid,8);REJECT((void)nvn_command_has_deferred(parent));memcpy(call->extra,&child,8);
    uint32_t size=call->size;call->size=sizeof(CmdRec)-8;REJECT((void)nvn_command_has_deferred(parent));call->size=size;CHECK(nvn_command_has_deferred(parent));
    uint32_t op=call->op;call->op=CMD_JUMP;call->x[0]=(uintptr_t)call;uint32_t budget=5;pthread_mutex_lock(&g_shadow_lock);CHECK(command_deferred_locked(parent,0,&budget)==-1&&!budget);pthread_mutex_unlock(&g_shadow_lock);call->op=op;call->x[0]=count;
    REJECT((void)nvn_command_has_deferred(0x10000));REJECT((void)nvn_command_has_deferred(parent+1));CHECK(nvn_command_has_deferred(parent));canaries();finalize();CHECK(nvn_command_has_deferred(parent));nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static bool queue_on;
static uint64_t queued[16],queue_count,queue_done;
static char queue_events[64];static unsigned queue_event_count,queue_waits,queue_executes;
static __thread uint64_t g_submit_ns;
static void event(char c){CHECK(queue_event_count<sizeof queue_events);queue_events[queue_event_count++]=c;}
static bool rq_on(void){return queue_on;}
static uint64_t plat_now_ns(void){static uint64_t tick;return ++tick;}
static uint64_t rq_submit(uint64_t handle){CHECK(queue_count<16);event('S');queued[queue_count++]=handle;return queue_count;}
static void queue_replay(uint64_t handle,unsigned depth){
 for(unsigned guard=0;guard<1000;++guard){CmdRec*r=(void*)(uintptr_t)handle;switch(r->op){
 case CMD_END:return;case CMD_JUMP:handle=r->x[0];continue;case CMD_BARRIER:break;
 case CMD_CALL:if(depth<8)for(uint64_t i=0;i<r->x[0];++i){uint64_t child;memcpy(&child,r->extra+i*8,8);queue_replay(child,depth+1);}break;
 default:replay_bind(r);break;
 }handle+=r->size;}CHECK(false);
}
void backend_execute(uint64_t handle){event('D');++queue_executes;queue_replay(handle,0);}
static uint64_t rq_wait_item(uint64_t item){event('W');++queue_waits;CHECK(item<=queue_count);while(queue_done<item)backend_execute(queued[queue_done++]);return 123;}
#include "queue_submit_production.inc"
static void queue_reset(bool on){queue_on=on;queue_count=queue_done=queue_waits=queue_executes=queue_event_count=0;memset(queue_events,0,sizeof queue_events);memset(&st,0,sizeof st);}
static void submit_lifetime(void){
 setup();add(0,4096);begin();uint64_t plain=end();uint64_t values[2]={11,22};begin();cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;cpu.x[2]=0;cpu.x[3]=2;cpu.x[4]=(uintptr_t)values;hle_nvnCommandBufferBindTexturesDeferred(&cpu);uint64_t child=end(),parent=call_list(child);values[0]=99;
 uint64_t handles[3]={plain,parent,plain};Cpu submit={0};submit.x[1]=3;submit.x[2]=(uintptr_t)handles;queue_reset(true);hle_nvnQueueSubmitCommands(&submit);
 CHECK(queue_count==3&&queue_done==2&&queue_waits==1&&queue_executes==2);CHECK(queue_event_count==6&&!memcmp(queue_events,"SSWDDS",6));CHECK(st.tex[0][0]==99&&st.tex[0][1]==22);values[0]=111;CHECK(st.tex[0][0]==99); // consumed before return, later source reuse cannot alter the binding
 (void)rq_wait_item(3);CHECK(st.tex[0][0]==99&&queue_done==3);
 queue_reset(true);submit.x[1]=1;hle_nvnQueueSubmitCommands(&submit);CHECK(queue_count==1&&!queue_done&&!queue_waits&&!queue_executes&&queue_event_count==1&&queue_events[0]=='S');(void)rq_wait_item(1);
 queue_reset(false);submit.x[1]=2;hle_nvnQueueSubmitCommands(&submit);CHECK(!queue_count&&!queue_waits&&queue_executes==2&&queue_event_count==2&&!memcmp(queue_events,"DD",2));CHECK(st.tex[0][0]==111&&g_submit_ns>=3);
 canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void bounded_backing_and_diagnostics(void){
    const uint64_t bytes=UINT64_C(16)*1024*1024;size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t*memory=mmap(NULL,(size_t)bytes+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED);CHECK(!mprotect(memory+page,(size_t)bytes,PROT_READ|PROT_WRITE));
    setup();pool.storage=(uintptr_t)(memory+page);pool.size=bytes;
    add(0xc00fe0,0x3ff020);CHECK(cb()->hcap==SHADOW_INITIAL_LIMIT&&cb()->guest_capacity==0x3ff020);CHECK(g_shadow_stats.bytes==SHADOW_INITIAL_LIMIT&&g_shadow_stats.allocations==1&&g_shadow_stats.largest==SHADOW_INITIAL_LIMIT);begin();uint64_t old=end();
    uint64_t handles[96],legacy_reservations=0;
    for(unsigned i=0;i<96;++i){uint64_t offset=i*256,remaining=bytes-offset;legacy_reservations+=remaining*CMD_SHADOW+HOST_FRAMING;add(offset,remaining);CHECK(cb()->hcap==SHADOW_INITIAL_LIMIT&&cb()->guest_capacity==remaining);begin();handles[i]=end();CHECK(g_shadow_stats.bytes==(i+2)*SHADOW_INITIAL_LIMIT);CHECK(g_shadow_stats.spills==0);}
    CHECK(legacy_reservations>UINT64_C(8)*1024*1024*1024);CHECK(g_shadow_stats.bytes<UINT64_C(400)*1024);CHECK(g_shadow_stats.reports<=12);
    for(unsigned i=0;i<96;++i)(void)walk(handles[i],1,false);(void)walk(old,1,false);
    // A real64KiB inline uniform update grows to its actual payload+framing,
    // independently of the multi-MiB guest reservation and without a callback.
    add(96*256,bytes-96*256);begin();uint64_t handle=cb()->start,base=cb()->host;uint64_t before=g_shadow_stats.bytes,allocs=g_shadow_stats.allocations,spills=g_shadow_stats.spills;
    uint8_t*payload=malloc(65536);CHECK(payload);for(unsigned i=0;i<65536;++i)payload[i]=(uint8_t)(i*31+7);
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x10000;cpu.x[2]=65536;cpu.x[3]=0;cpu.x[4]=65536;cpu.x[5]=(uintptr_t)payload;hle_nvnCommandBufferUpdateUniformBuffer(&cpu);
    CmdRec*r=(void*)(uintptr_t)cb()->host;CHECK(cb()->host!=base&&r->op==CMD_UPDATE_UNIFORM_BUFFER&&r->size==sizeof(CmdRec)+65536);CHECK(cb()->hcap==r->size+HOST_FRAMING);CHECK(!memcmp(r->extra,payload,65536));CHECK(g_shadow_stats.bytes==before+cb()->hcap&&g_shadow_stats.allocations==allocs+1&&g_shadow_stats.spills==spills+1);CHECK(g_shadow_stats.largest==cb()->hcap);
    uint64_t active=cb()->host;CHECK(end()==handle&&cb()->host==active&&g_shadow_stats.spills==spills+1);free(payload);for(unsigned i=0;i<65536;++i)CHECK(r->extra[i]==(uint8_t)(i*31+7));
    CmdRec*first=(void*)(uintptr_t)handle,*jump=(void*)((uint8_t*)first+first->size);CHECK(first->op==CMD_BARRIER&&jump->op==CMD_JUMP&&jump->x[0]==(uintptr_t)r);CHECK(((CmdRec*)((uint8_t*)r+r->size))->op==CMD_END);
    uint64_t retained=g_shadow_stats.bytes,nalloc=g_shadow_stats.allocations,nstarts=g_nshadow;size_t live=live_allocations;
    uint64_t slab_used=g_shadow_slabs->used;g_shadow_slabs->used=g_shadow_slabs->capacity;
    fail_alloc=1;REJECT(add(1000*256,bytes-1000*256));CHECK(strstr(fatal_text,"slab allocation")&&strstr(fatal_text,"host capacity 4096"));CHECK(g_shadow_stats.bytes==retained&&g_shadow_stats.allocations==nalloc&&g_nshadow==nstarts&&live_allocations==live);
    fail_alloc=2;REJECT(add(1000*256,bytes-1000*256));CHECK(strstr(fatal_text,"allocation metadata")&&strstr(fatal_text,"retained "));CHECK(g_shadow_stats.bytes==retained&&g_shadow_stats.allocations==nalloc&&g_nshadow==nstarts&&live_allocations==live);g_shadow_slabs->used=slab_used;
    size_t actual_starts=g_nshadow;g_nshadow=g_shadow_cap/2;fail_alloc=1;REJECT(add(1000*256,bytes-1000*256));CHECK(strstr(fatal_text,"guest table allocation"));g_nshadow=actual_starts;CHECK(g_shadow_stats.bytes==retained&&live_allocations==live);fail_alloc=0;
    canaries();finalize();for(unsigned i=0;i<96;++i)(void)walk(handles[i],1,false);(void)walk(old,1,false);CHECK(r->extra[65535]==(uint8_t)(65535u*31+7));
    nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations&&!g_shadow_stats.bytes&&!g_shadow_stats.peak&&!g_shadow_stats.allocations&&!g_shadow_stats.spills&&!g_shadow_stats.reports);CHECK(!munmap(memory,(size_t)bytes+2*page));
}
static void indexed_lookup_complexity(void){
    setup();add(0,4096);begin();
    for(unsigned i=0;i<200;++i){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;cpu.x[2]=0;cpu.x[3]=1;cpu.x[4]=0x10000;
    hle_nvnCommandBufferBindTexturesDeferred(&cpu);uint64_t leaf=end(),parent=call_list(leaf);begin();uint64_t plain=end();
    pthread_mutex_lock(&g_shadow_lock);size_t initial_blocks=g_shadow_index_count;
    for(unsigned i=0;i<20000;++i){
        ShadowFailure failure={0};uint64_t memory=shadow_allocate_locked(128,true,&failure);CHECK(memory&&!failure.reason);
        raw_rec(memory,CMD_BARRIER,sizeof(CmdRec));
    }
    CHECK(g_shadow_index_count==initial_blocks+20000&&g_shadow_index_count==g_shadow_stats.allocations);
    for(size_t i=0;i<g_shadow_index_count;++i){
        const ShadowAllocation*a=g_shadow_index[i];uint64_t base=(uintptr_t)a->memory;
        if(i)CHECK((uintptr_t)g_shadow_index[i-1]->memory+g_shadow_index[i-1]->capacity<=base);
        shadow_lookup_probes=0;CHECK(shadow_allocation_locked(base)==a);CHECK(shadow_lookup_probes<=15);
        shadow_lookup_probes=0;CHECK(shadow_allocation_locked(base+a->capacity-1)==a);CHECK(shadow_lookup_probes<=15);
        const ShadowAllocation*current=NULL;CHECK(shadow_record_locked(base,&current)&&current==a);
        CHECK(!shadow_record_locked(base+a->capacity-8,&current)); // partial header must not be read
        CHECK(!shadow_record_locked(base+1,&current));
    }
    // Force index growth to fail after successful data/node allocations. No
    // generation/index entry/stat becomes visible and the old handles survive.
    size_t capacity=g_shadow_index_capacity,count=g_shadow_index_count,live=live_allocations;
    uint64_t retained=g_shadow_stats.bytes;ShadowAllocation**index=g_shadow_index;ShadowAllocation*head=g_shadow_allocations;
    g_shadow_index_capacity=count;fail_alloc=2;ShadowFailure failure={0};
    CHECK(!shadow_allocate_locked(128,true,&failure));CHECK(failure.reason&&!strcmp(failure.reason,"allocation index"));
    CHECK(g_shadow_index==index&&g_shadow_index_count==count&&g_shadow_allocations==head&&g_shadow_stats.bytes==retained&&live_allocations==live);
    g_shadow_index_capacity=capacity;fail_alloc=0;
    pthread_mutex_unlock(&g_shadow_lock);
    shadow_lookup_probes=0;CHECK(nvn_command_has_deferred(leaf));CHECK(shadow_lookup_probes<=30); //201 records cross only the initial4KiB root and one continuation
    shadow_lookup_probes=0;CHECK(nvn_command_has_deferred(parent));CHECK(shadow_lookup_probes<=45); // parent plus those two child blocks
    shadow_lookup_probes=0;CHECK(!nvn_command_has_deferred(plain));CHECK(shadow_lookup_probes<=15);
    canaries();finalize();CHECK(nvn_command_has_deferred(parent));nvn_command_shadow_finish_after_gpu_idle();
    CHECK(!live_allocations&&!g_shadow_index&&!g_shadow_index_count&&!g_shadow_index_capacity);
    REJECT((void)nvn_command_has_deferred(leaf)); // stale handle cannot survive idle cleanup
}
static void forced_spills(unsigned count){
    for(unsigned i=0;i<count;++i){NvnCmdBuf*b=cb();b->hcap=b->used+JUMP_RESERVE;cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x100+i;hle_nvnCommandBufferBarrier(&cpu);}
}
static uint64_t list_digest(uint64_t handle){
    uint64_t hash=UINT64_C(1469598103934665603);
    for(unsigned guard=0;guard<10000;++guard){
        CmdRec*r=(void*)(uintptr_t)handle;for(unsigned i=0;i<r->size;++i){hash^=((uint8_t*)r)[i];hash*=UINT64_C(1099511628211);}
        if(r->op==CMD_END)return hash;handle=r->op==CMD_JUMP?r->x[0]:handle+r->size;
    }
    CHECK(false);return 0;
}
static ShadowRoot root_snapshot(uint64_t host){
    pthread_mutex_lock(&g_shadow_lock);ShadowRoot*r=shadow_root_locked(host,false,NULL);CHECK(r);ShadowRoot copy=*r;pthread_mutex_unlock(&g_shadow_lock);return copy;
}
static void ring_prefix_and_empty_rebind(void) {
    for(unsigned nested=0;nested<2;nested++) {
        setup();add(0,1024);uint64_t handles[32],digests[32];
        for(unsigned i=0;i<32;i++) {
            begin();cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=i;hle_nvnCommandBufferBarrier(&cpu);
            handles[i]=end();digests[i]=list_digest(handles[i]);
        }
        NvnCmdBuf *b=cb();CHECK(b->guest_used==512);begin();uint64_t root=b->start;
        CHECK(b->recording_guest_floor==(uintptr_t)guest_chunk+512);
        unsigned barriers=0;
        while(b->guest_used+44<=b->guest_capacity) {
            cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x800+barriers++;hle_nvnCommandBufferBarrier(&cpu);
        }
        CHECK(b->guest_used==992);NvnCmdBuf before=*b;uint8_t chain_before[sizeof(CmdRec)];
        memcpy(chain_before,(void*)(uintptr_t)(b->host+b->used),sizeof chain_before);
        size_t live=live_allocations;fail_alloc=1;REJECT(add(0,508));fail_alloc=0;
        CHECK(!memcmp(b,&before,sizeof before) && live_allocations==live &&
              !memcmp(chain_before,(void*)(uintptr_t)(b->host+b->used),sizeof chain_before));
        b->callback=0x42;b->callback_data=0x678;callback_next=0;callback_size=508;
        callback_calls=0;callback_nest_once=nested!=0;
        uint64_t textures[29];for(unsigned i=0;i<29;i++)textures[i]=UINT64_C(0x1234000000000000)+i;
        cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=2;cpu.x[2]=3;cpu.x[3]=29;cpu.x[4]=(uintptr_t)textures;
        hle_nvnCommandBufferBindTextures(&cpu);
        CHECK(callback_calls==1+nested && b->start==root && b->host!=before.host &&
              b->guest_capacity==508 && b->guest_used==40+40*nested && b->recording_guest_floor==(uintptr_t)guest_chunk);
        const CmdRec *jump=(void*)(uintptr_t)(before.host+before.used);
        CHECK(jump->op==CMD_JUMP && jump->x[0]==b->host && root_snapshot(root).tail->owner_root==root);
        const CmdRec *payload=(void*)(uintptr_t)(b->host+(nested?312:0));
        CHECK(payload->op==CMD_BIND_TEXTURES && payload->size==312 && payload->x[2]==29 && !memcmp(payload->extra,textures,sizeof textures));
        memset(textures,0,sizeof textures);
        for(unsigned i=0;i<29;i++){uint64_t word;memcpy(&word,payload->extra+i*8,8);CHECK(word==UINT64_C(0x1234000000000000)+i);}
        NvnCmdBuf accepted=*b;REJECT(add(0,4));CHECK(!memcmp(b,&accepted,sizeof accepted));
        CHECK(end()==root);uint64_t digest=list_digest(root);
        for(unsigned i=0;i<32;i++)CHECK(list_digest(handles[i])==digests[i]);
        begin();CHECK(b->recording_guest_floor==(uintptr_t)guest_chunk+44+40*nested);
        add(0,40);CHECK(b->recording_guest_floor==(uintptr_t)guest_chunk && b->start!=root);(void)end();
        CHECK(list_digest(root)==digest);
        for(unsigned i=0;i<32;i++)CHECK(list_digest(handles[i])==digests[i]);
        /* A later chunk resetting guest_used must not erase an earlier span. */
        add(128,1024);begin();cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);
        uint64_t floor=b->recording_guest_floor;add(2048,256);
        CHECK(!b->guest_used && b->recording_guest_floor==floor);REJECT(add(2048,4));
        add(64,256);CHECK(b->recording_guest_floor==(uintptr_t)guest_chunk+64);REJECT(add(64,4));(void)end();
        canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    }
    setup();add(0,4);begin();NvnCmdBuf *b=cb();uint64_t root=b->start,host=b->host,hcap=b->hcap,used=b->used;
    add(0,8192);CHECK(b->host==host && b->start==root && b->used==used && b->hcap==hcap && b->guest_capacity==8192 && !b->guest_used);
    add(1024,8192);CHECK(b->host==host && b->start==root && b->used==used && b->hcap==hcap && b->recording_guest_floor==(uintptr_t)guest_chunk+1024);
    for(unsigned i=0;i<8;i++){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    CHECK(b->host!=host); /* Growth uses actual backing, never the new guest reservation as host capacity. */
    REJECT(add(1024,8192));CHECK(end()==root);CHECK(walk(root,9,false)>0);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void continuation_reuse(void){
    setup();add(0,4096);begin();uint64_t root=cb()->start;forced_spills(8);CHECK(end()==root);
    uint64_t bytes=g_shadow_stats.bytes,nalloc=g_shadow_stats.allocations;
    for(unsigned cycle=0;cycle<64;++cycle){
        add(0,4096);begin();CHECK(cb()->start==root);forced_spills(8);CHECK(end()==root);
        CHECK(g_shadow_stats.bytes==bytes&&g_shadow_stats.allocations==nalloc);CHECK(walk(root,9,false)==8);
    }
    CHECK(g_shadow_stats.reused==64*8&&!g_shadow_stats.pinned);
    uint64_t old_digest=list_digest(root);ShadowRoot old=root_snapshot(root);
    pthread_mutex_lock(&g_shadow_lock);ShadowFailure rehash_failure={0};uint64_t extra=shadow_allocate_locked(256*1024,false,&rehash_failure);CHECK(extra);pthread_mutex_unlock(&g_shadow_lock);
    for(unsigned i=0;i<2048;++i){ShadowFailure failure={0};CHECK(shadow_begin_root(extra+(uint64_t)i*sizeof(CmdRec),&failure));}
    CHECK(g_shadow_root_count==2049&&g_shadow_root_capacity>=4096);ShadowRoot rehashed=root_snapshot(root);CHECK(!memcmp(&old,&rehashed,sizeof old));
    add(0,8192);begin();uint64_t grown=cb()->start;CHECK(grown!=root);forced_spills(8);CHECK(end()==grown);
    CHECK(list_digest(root)==old_digest&&root_snapshot(root).head==old.head&&root_snapshot(root).tail==old.tail);
    CHECK(root_snapshot(grown).head!=old.head);(void)walk(root,9,false);(void)walk(grown,9,false);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations&&!g_shadow_roots&&!g_shadow_root_count&&!g_shadow_root_capacity);
}
static void continuation_child_pinning(void){
    setup();add(0,4096);begin();uint64_t parent=cb()->start;forced_spills(2);CHECK(end()==parent);
    uint64_t pinned_host=cb()->host;ShadowAllocation*pinned=(ShadowAllocation*)shadow_allocation_locked(pinned_host);
    CHECK(pinned&&pinned->owner_root==parent&&!pinned->pinned);
    begin();uint64_t child=cb()->start;CHECK(child>pinned_host&&child<pinned_host+pinned->capacity);CHECK(pinned->pinned&&g_shadow_stats.pinned==1);
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0xabcdef;hle_nvnCommandBufferBarrier(&cpu);CHECK(end()==child);uint64_t child_digest=list_digest(child);
    add(0,4096);begin();forced_spills(2);CHECK(cb()->host!=pinned_host);CHECK(end()==parent);uint64_t bytes=g_shadow_stats.bytes,nalloc=g_shadow_stats.allocations;
    for(unsigned cycle=0;cycle<16;++cycle){
        add(0,4096);begin();forced_spills(2);CHECK(cb()->host!=pinned_host);CHECK(end()==parent);
        CHECK(pinned->pinned&&g_shadow_stats.pinned==1&&list_digest(child)==child_digest);
        CHECK(g_shadow_stats.bytes==bytes&&g_shadow_stats.allocations==nalloc);
    }
    // Resetting the child itself never unpins its hosting block for the parent.
    ShadowFailure failure={0};CHECK(shadow_begin_root(child,&failure));CHECK(pinned->pinned&&list_digest(child)==child_digest);
    // A parent calling an independent child must preserve both the child root
    // and its continuations when only the parent is rerecorded.
    add(8192,4096);begin();uint64_t independent=cb()->start;forced_spills(3);CHECK(end()==independent);uint64_t independent_digest=list_digest(independent);
    for(unsigned cycle=0;cycle<8;++cycle){add(0,4096);begin();forced_spills(2);cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=1;cpu.x[2]=(uintptr_t)&independent;hle_nvnCommandBufferCallCommands(&cpu);CHECK(end()==parent);CHECK(!nvn_command_has_deferred(parent));CHECK(list_digest(independent)==independent_digest&&list_digest(child)==child_digest);}
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void continuation_callback_owner(void){
    setup();add(1024,256);begin();uint64_t child=cb()->start;forced_spills(1);CHECK(end()==child);ShadowRoot child_root=root_snapshot(child);uint64_t child_block=(uintptr_t)child_root.head->memory;
    uint8_t child_bytes[sizeof(CmdRec)];memcpy(child_bytes,(void*)(uintptr_t)child_block,sizeof child_bytes);
    add(0,256);begin();uint64_t parent=cb()->start;forced_spills(1);NvnCmdBuf*b=cb();b->callback=0x42;b->callback_data=0x678;callback_calls=0;callback_next=1024;
    while(b->guest_used+16<=b->guest_capacity){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);CHECK(callback_calls==1&&b->start==parent&&b->mem==(uintptr_t)guest_chunk+1024);
    forced_spills(1);ShadowAllocation*after=(ShadowAllocation*)shadow_allocation_locked(b->host);CHECK(after&&after->owner_root==parent&&b->host!=child_block);CHECK(end()==parent);
    CHECK(!memcmp(child_bytes,(void*)(uintptr_t)child_block,sizeof child_bytes));ShadowRoot still=root_snapshot(child);CHECK(still.head==child_root.head&&still.tail==child_root.tail&&still.cursor==child_root.cursor);
    // Only a genuine Begin at B may make B's own continuation reusable.
    add(1024,256);begin();CHECK(cb()->start==child);forced_spills(1);CHECK(cb()->host==child_block);CHECK(end()==child);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void continuation_submit_reuse_and_failure(void){
    setup();add(0,4096);uint64_t host=cb()->host;ShadowFailure failure={0};size_t live=live_allocations;
    memset((void*)(uintptr_t)host,0x3d,sizeof(CmdRec));uint8_t before_begin[sizeof(CmdRec)];memcpy(before_begin,(void*)(uintptr_t)host,sizeof before_begin);
    fail_alloc=1;REJECT(begin());CHECK(strstr(fatal_text,"root table allocation")&&!g_shadow_roots&&!g_shadow_root_count&&live_allocations==live);CHECK(!cb()->used&&!cb()->recording&&!memcmp(before_begin,(void*)(uintptr_t)host,sizeof before_begin));fail_alloc=0;
    uint64_t value=11,root=0,bytes=0,nalloc=0;
    for(unsigned cycle=0;cycle<16;++cycle){
        add(0,4096);begin();if(!cycle)root=cb()->start;else CHECK(cb()->start==root);forced_spills(2);
        value=11+cycle;cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;cpu.x[2]=0;cpu.x[3]=1;cpu.x[4]=(uintptr_t)&value;hle_nvnCommandBufferBindTexturesDeferred(&cpu);CHECK(end()==root);
        Cpu submit={0};submit.x[1]=1;submit.x[2]=(uintptr_t)&root;queue_reset(true);hle_nvnQueueSubmitCommands(&submit);
        CHECK(queue_done==1&&queue_waits==1&&st.tex[0][0]==value); // consumed before the next legal rerecord
        if(!cycle){bytes=g_shadow_stats.bytes;nalloc=g_shadow_stats.allocations;}else CHECK(g_shadow_stats.bytes==bytes&&g_shadow_stats.allocations==nalloc);
    }
    pthread_mutex_lock(&g_shadow_lock);size_t count=g_shadow_root_count,capacity=g_shadow_root_capacity;ShadowRoot*table=g_shadow_roots;ShadowRoot before=*shadow_root_locked(root,false,NULL);live=live_allocations;
    g_shadow_root_count=capacity/2;fail_alloc=1;failure=(ShadowFailure){0};CHECK(!shadow_root_locked(root+sizeof(CmdRec)*2,true,&failure));CHECK(!strcmp(failure.reason,"root table allocation"));
    CHECK(g_shadow_roots==table&&g_shadow_root_capacity==capacity&&live_allocations==live);g_shadow_root_count=count;CHECK(!memcmp(&before,shadow_root_locked(root,false,NULL),sizeof before));fail_alloc=0;pthread_mutex_unlock(&g_shadow_lock);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void continuation_mixed_capacity(void){
    setup();add(0,sizeof guest_chunk);begin();uint64_t root=cb()->start;forced_spills(1);
    uint64_t small=cb()->host;(void)rec(&cpu,cb(),CMD_BARRIER,65536);uint64_t large=cb()->host;CHECK(large!=small&&end()==root);
    // The large first record must skip the earlier undersized block. Later
    // small spills cannot lease the just-used large block a second time.
    add(0,sizeof guest_chunk);begin();(void)rec(&cpu,cb(),CMD_BARRIER,65536);CHECK(cb()->host==large);
    forced_spills(1);uint64_t next=cb()->host;CHECK(next!=large&&next!=small);
    forced_spills(1);CHECK(cb()->host!=large&&cb()->host!=next&&cb()->host!=small);CHECK(end()==root);
    uint64_t bytes=g_shadow_stats.bytes,nalloc=g_shadow_stats.allocations;
    for(unsigned cycle=0;cycle<16;++cycle){
        add(0,sizeof guest_chunk);begin();(void)rec(&cpu,cb(),CMD_BARRIER,65536);CHECK(cb()->host==large);
        forced_spills(2);CHECK(end()==root&&g_shadow_stats.bytes==bytes&&g_shadow_stats.allocations==nalloc);
    }
    // Direct root admission also rejects gaps/alignment without table changes.
    ShadowFailure failure={0};size_t roots=g_shadow_root_count;CHECK(!shadow_begin_root(1,&failure));CHECK(!strcmp(failure.reason,"invalid root address")&&g_shadow_root_count==roots);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void initial_capacity_and_natural_growth(void){
    const uint64_t sizes[]={4,128,480,484,512,4096,65536};
    for(unsigned i=0;i<sizeof sizes/sizeof sizes[0];++i){
        setup();add(0,sizes[i]);uint64_t host=cb()->host,ceiling=sizes[i]*CMD_SHADOW+HOST_FRAMING;
        uint64_t expected=ceiling<4096?ceiling:4096;
        CHECK(cb()->hcap==expected&&cb()->guest_capacity==sizes[i]&&g_shadow_stats.bytes==expected);
        const ShadowAllocation*block=shadow_allocation_locked(host);CHECK(block&&(uintptr_t)block->memory==host&&block->capacity==cb()->hcap);
        begin();uint64_t handle=end();CHECK(handle==host);CHECK(walk(handle,1,false)==0);
        // A smaller subsequent reservation keeps its address, while exposing
        // only the capacity proven for that reservation to the recorder.
        add(0,4);CHECK(cb()->host==host&&cb()->hcap<=block->capacity&&cb()->hcap==4*CMD_SHADOW+HOST_FRAMING);
        CHECK(((CmdRec*)(uintptr_t)handle)->op==CMD_BARRIER);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
    }
    uint64_t capacity=0;CHECK(!shadow_initial_capacity(UINT64_MAX/CMD_SHADOW+1,&capacity));CHECK(!shadow_capacity(UINT64_MAX/CMD_SHADOW+1,&capacity));
    setup();uint64_t root=0,bytes=0,nalloc=0;
    for(unsigned cycle=0;cycle<16;++cycle){
        add(0,sizeof guest_chunk);CHECK(cb()->hcap==4096);begin();if(!cycle)root=cb()->start;else CHECK(cb()->start==root);
        unsigned payloads=0;
        while(cb()->used+sizeof(CmdRec)+JUMP_RESERVE<=cb()->hcap){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);++payloads;}
        CHECK(cb()->host==root);uint64_t before_used=cb()->used;
        cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);++payloads;
        CHECK(cb()->host!=root&&cb()->hcap==32768&&cb()->guest_used==payloads*12);
        CHECK(shadow_allocation_locked(cb()->host)->capacity==cb()->hcap);
        CmdRec*jump=(void*)(uintptr_t)(root+before_used);CHECK(jump->op==CMD_JUMP&&jump->x[0]==cb()->host);
        CHECK(end()==root&&walk(root,payloads+1,false)==1);
        if(!cycle){bytes=g_shadow_stats.bytes;nalloc=g_shadow_stats.allocations;CHECK(bytes==4096+32768&&nalloc==2);}
        else CHECK(g_shadow_stats.bytes==bytes&&g_shadow_stats.allocations==nalloc);
    }
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void slab_failure_rollback(void){
    setup();size_t live=live_allocations;
    const char*reasons[]={"slab allocation","allocation metadata","allocation index"};
    pthread_mutex_lock(&g_shadow_lock);
    for(unsigned fail=1;fail<=3;++fail){
        ShadowFailure failure={0};fail_alloc=fail;CHECK(!shadow_allocate_locked(128,false,&failure));CHECK(!strcmp(failure.reason,reasons[fail-1]));
        CHECK(!g_shadow_slabs&&!g_shadow_index&&!g_shadow_index_count&&!g_shadow_index_capacity&&!g_shadow_allocations&&!g_shadow_stats.bytes&&!g_shadow_stats.slabs&&!g_shadow_stats.slab_bytes&&live_allocations==live);
    }
    fail_alloc=0;ShadowFailure failure={0};uint64_t first=shadow_allocate_locked(128,false,&failure);CHECK(first&&!(first&15));raw_rec(first,CMD_BARRIER,sizeof(CmdRec));
    ShadowSlab*slab=g_shadow_slabs;ShadowAllocation*head=g_shadow_allocations;ShadowAllocation**index=g_shadow_index;
    uint64_t used=slab->used,bytes=g_shadow_stats.bytes,slab_bytes=g_shadow_stats.slab_bytes;size_t count=g_shadow_index_count,capacity=g_shadow_index_capacity;live=live_allocations;
    for(unsigned fresh=0;fresh<2;++fresh){
        if(fresh)slab->used=slab->capacity;
        uint64_t cursor=slab->used;g_shadow_index_capacity=count;
        for(unsigned fail=1;fail<=(fresh?3u:2u);++fail){
            failure=(ShadowFailure){0};fail_alloc=fail;CHECK(!shadow_allocate_locked(128,false,&failure));
            CHECK(!strcmp(failure.reason,reasons[fail-1+(fresh?0:1)]));
            CHECK(g_shadow_slabs==slab&&slab->used==cursor&&g_shadow_index==index&&g_shadow_index_count==count&&g_shadow_allocations==head);
            CHECK(g_shadow_stats.bytes==bytes&&g_shadow_stats.slab_bytes==slab_bytes&&g_shadow_stats.slabs==1&&live_allocations==live);
        }
        g_shadow_index_capacity=capacity;slab->used=used;fail_alloc=0;
    }
    failure=(ShadowFailure){0};CHECK(!shadow_allocate_locked(UINT64_MAX,false,&failure));CHECK(!strcmp(failure.reason,"retained-size overflow"));
    g_shadow_stats.bytes=0;failure=(ShadowFailure){0};CHECK(!shadow_allocate_locked(UINT64_MAX,false,&failure));CHECK(!strcmp(failure.reason,"slab size overflow"));g_shadow_stats.bytes=bytes;
    slab->used=slab->capacity;g_shadow_stats.slab_bytes=UINT64_MAX;failure=(ShadowFailure){0};CHECK(!shadow_allocate_locked(128,false,&failure));CHECK(!strcmp(failure.reason,"slab size overflow"));slab->used=used;g_shadow_stats.slab_bytes=slab_bytes;
    const ShadowAllocation*current=NULL;CHECK(shadow_record_locked(first,&current));pthread_mutex_unlock(&g_shadow_lock);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations&&!g_shadow_slabs&&!g_shadow_stats.slabs&&!g_shadow_stats.slab_bytes);
}
static void slab_packing_and_block_bounds(void){
    setup();uint64_t handles[4096];pthread_mutex_lock(&g_shadow_lock);
    for(unsigned i=0;i<4096;++i){
        ShadowFailure failure={0};handles[i]=shadow_allocate_locked(4096,false,&failure);CHECK(handles[i]&&!(handles[i]&15));
        raw_rec(handles[i],CMD_BARRIER,sizeof(CmdRec));memset((void*)(uintptr_t)(handles[i]+sizeof(CmdRec)),i&255,4096-sizeof(CmdRec));
    }
    CHECK(g_shadow_stats.allocations==4096&&g_shadow_stats.slabs==4&&g_shadow_stats.bytes==UINT64_C(16)*1024*1024);
    CHECK(g_shadow_stats.slab_bytes==4*(SHADOW_SLAB_CAPACITY+offsetof(ShadowSlab,data)));
    for(unsigned i=0;i<4096;++i){
        const ShadowAllocation*current=NULL;const CmdRec*r=shadow_record_locked(handles[i],&current);CHECK(r&&r->size==sizeof(CmdRec)&&current->capacity==4096);
        CHECK(*(uint8_t*)(uintptr_t)(handles[i]+4095)==(i&255));
        ((CmdRec*)r)->size=4104;CHECK(!shadow_record_locked(handles[i],&current));((CmdRec*)r)->size=sizeof(CmdRec);
    }
    // Packed neighboring blocks are physically contiguous but retain distinct
    // validation boundaries. Neither a partial header nor a record may borrow
    // bytes from the next valid block.
    CHECK(handles[1]==handles[0]+4096);const ShadowAllocation*current=NULL;CHECK(!shadow_record_locked(handles[0]+4096-8,&current));CHECK(shadow_record_locked(handles[1],&current));
    pthread_mutex_unlock(&g_shadow_lock);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations&&!g_shadow_slabs);
}
static void slab_oversized_actual_record(void){
    const size_t guest_bytes=1024*1024;uint8_t*memory=malloc(guest_bytes);CHECK(memory);memset(memory,0xa5,guest_bytes);
    setup();pool.storage=(uintptr_t)memory;pool.size=guest_bytes;uint64_t root=0,large=0,bytes=0,slab_bytes=0;
    const uint32_t extra=(uint32_t)SHADOW_SLAB_CAPACITY+17;
    for(unsigned cycle=0;cycle<8;++cycle){
        add(0,guest_bytes);begin();if(!cycle)root=cb()->start;else CHECK(cb()->start==root);
        CmdRec*r=rec(&cpu,cb(),CMD_BARRIER,extra);memset(r->extra,0x6b,extra);
        CHECK(r->size>=sizeof(CmdRec)+extra&&cb()->hcap==r->size+HOST_FRAMING&&cb()->hcap>SHADOW_SLAB_CAPACITY);
        if(!cycle){large=(uintptr_t)r;bytes=g_shadow_stats.bytes;slab_bytes=g_shadow_stats.slab_bytes;CHECK(g_shadow_stats.slabs==2&&g_shadow_slabs->capacity==cb()->hcap);}
        else CHECK((uintptr_t)r==large&&g_shadow_stats.bytes==bytes&&g_shadow_stats.slab_bytes==slab_bytes&&g_shadow_stats.slabs==2);
        CHECK(end()==root&&walk(root,2,false)==1);CHECK(r->extra[0]==0x6b&&r->extra[extra-1]==0x6b);
    }
    for(size_t i=0;i<guest_bytes;++i)CHECK(memory[i]==0xa5);
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations&&!g_shadow_slabs);free(memory);
}
static void clear_color_kinds(void) {
    setup();add(0,4096);begin();
    const uint64_t poison[]={1,2,UINT64_MAX,UINT64_C(0xffffffff80001eb0)};
    void(*wrappers[])(Cpu*)={hle_nvnCommandBufferClearColor,hle_nvnCommandBufferClearColori,hle_nvnCommandBufferClearColorui};
    uint64_t word_cost=0;
    for(unsigned p=0;p<4;p++)for(unsigned kind=0;kind<3;kind++)for(unsigned partial=0;partial<2;partial++){
        uint32_t colors[]={0x3e800000u,0xbf000000u,0x7fc01234u,0xffffffffu};
        uint32_t mask=partial?5:15;uint64_t saved[]={ (uintptr_t)guest_object,2,(uintptr_t)colors,mask,poison[p],poison[p],poison[p],poison[p] };
        memcpy(cpu.x,saved,sizeof saved);NvnCmdBuf*b=cb();uint64_t host=b->host,used=b->used,guest_used=b->guest_used;
        wrappers[kind](&cpu);CmdRec*r=(void*)(uintptr_t)(b->host==host?host+used:b->host);
        CHECK(r->op==CMD_CLEAR_COLOR&&r->x[0]==2&&r->x[2]==mask&&r->x[5]==kind);
        CHECK(r->size>=sizeof(CmdRec)+16&&!memcmp(r->extra,colors,16));CHECK(!memcmp(cpu.x,saved,sizeof saved));
        if(!word_cost)word_cost=b->guest_used-guest_used;else CHECK(b->guest_used-guest_used==word_cost);
        memset(colors,0,sizeof colors);const uint32_t expected[]={0x3e800000u,0xbf000000u,0x7fc01234u,0xffffffffu};CHECK(!memcmp(r->extra,expected,16));
    }
    CHECK(end()!=0);canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void memory_callback_diagnostics(void) {
    if(!memory_trace_enabled) {
        CHECK(!memory_trace_sequence && !memory_trace_callback);
        Cpu invalid={0}; /* Previously inert APIs remain inert with tracing off. */
        hle_nvnCommandBufferAddControlMemory(&invalid);hle_nvnCommandBufferSetCommandMemoryCallbackEnabled(&invalid);
        CHECK(!memory_trace_sequence);return;
    }
    setup();add(0,256);NvnCmdBuf *b=cb();
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x42;hle_nvnCommandBufferSetMemoryCallback(&cpu);
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x678;hle_nvnCommandBufferSetMemoryCallbackData(&cpu);
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0;hle_nvnCommandBufferSetCommandMemoryCallbackEnabled(&cpu);
    uint64_t sequence=memory_trace_sequence;
    (void)logical_free();CHECK(memory_trace_sequence==sequence); /* No per-frame query events. */
    begin();sequence=memory_trace_sequence;
    while(b->guest_used+16<=b->guest_capacity){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    CHECK(memory_trace_sequence==sequence); /* Ordinary payloads do no trace work. */
    callback_calls=0;callback_next=512;callback_diagnostic_mode=1;
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=2;cpu.x[2]=3;cpu.x[3]=0x123;cpu.x[30]=0xabcdef;
    hle_nvnCommandBufferBindTexture(&cpu);
    CHECK(callback_calls==2 && callback_need==16 && !memory_trace_callback && b->guest_used==24);
    bool outer=false,nested=false,foreign=false,query=false,disabled=false;
    uint64_t first=memory_trace_sequence>COMMAND_MEMORY_TRACE_EVENTS?memory_trace_sequence-COMMAND_MEMORY_TRACE_EVENTS:0;
    if(first<sequence)first=sequence; /* Ignore older instances that reused the host allocation. */
    for(uint64_t i=first;i<memory_trace_sequence;i++) {
        const MemoryTraceEvent *e=&memory_trace_events[i%COMMAND_MEMORY_TRACE_EVENTS];
        if(e->identity!=b)continue;
        if(e->kind==MT_CALLBACK) {
            CHECK(e->origin.op==CMD_BIND_TEXTURES && e->origin.requested==16 && e->origin.bytes==88 && e->origin.cost==12);
            CHECK(e->same_cb && e->thread && e->callback_thread==e->thread);
            CHECK(e->chunk_seen && e->chunk[2]==sizeof guest_chunk && e->chunk[3]==0 && e->chunk[4]==256 && e->chunk[5]==256);
            if(e->origin.depth==1){CHECK(e->origin.caller==0xabcdef);outer=true;}
            if(e->origin.depth==2){CHECK(e->origin.caller==0x2222);nested=true;}
        }
        if(e->kind==MT_CONTROL && e->foreign_callback){CHECK(!e->origin.depth && e->callback_depth==1);foreign=true;}
        if(e->kind==MT_QUERY_FREE && e->args[0]==0 && e->same_cb)query=true;
        if(e->kind==MT_ADDED && e->origin.depth==2 && e->enabled_seen && !e->enabled)disabled=true;
    }
    CHECK(outer&&nested&&foreign&&query&&disabled); /* Observe disabled flag; do not change its current no-op semantics. */
    callback_diagnostic_mode=0;(void)end();add(0,256);begin();
    while(b->guest_used+16<=b->guest_capacity){cpu.x[0]=(uintptr_t)guest_object;hle_nvnCommandBufferBarrier(&cpu);}
    callback_next=0;callback_diagnostic_mode=2;
    trace_log_used=0;trace_log[0]=0;capture_trace_log=true;
    cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=2;cpu.x[2]=3;cpu.x[3]=0x123;cpu.x[30]=0xabcdef;
    REJECT(hle_nvnCommandBufferBindTexture(&cpu));capture_trace_log=false;
    CHECK(!strcmp(fatal_text,"nvn: recording cannot chain a command chunk to itself\n"));
    CHECK(strstr(trace_log,"reason=same-guest-chunk") && strstr(trace_log,"same_cb=1") &&
          strstr(trace_log,"requested=16") && strstr(trace_log,"origin_lr=abcdef") &&
          strstr(trace_log,"enabled_seen=1 enabled=0") && strstr(trace_log,"query-free"));
    /* Production FATAL never returns. The fixture's deliberate longjmp must
     * discard its interrupted stack-only callback context explicitly. */
    memory_trace_callback=NULL;callback_diagnostic_mode=0;
    CHECK(b->mem==(uintptr_t)guest_chunk && b->guest_used==252);(void)end();
    for(unsigned i=0;i<2*COMMAND_MEMORY_TRACE_EVENTS;i++) {
        cpu.x[0]=(uintptr_t)guest_object;cpu.x[1]=0x20;cpu.x[2]=0x40;hle_nvnCommandBufferAddControlMemory(&cpu);
    }
    trace_log_used=0;trace_log[0]=0;capture_trace_log=true;memory_trace_dump(b,"bounded-fixture");capture_trace_log=false;
    CHECK(strstr(trace_log,"history=64/64"));
    canaries();finalize();nvn_command_shadow_finish_after_gpu_idle();CHECK(!live_allocations);
}
static void acceptance_log_limits(void) {
    if(!memory_trace_enabled){CHECK(!memory_trace_accept_reports[0] && !memory_trace_accept_reports[1]);return;}
    __atomic_store_n(&memory_trace_accept_reports[0],0,__ATOMIC_RELAXED);
    __atomic_store_n(&memory_trace_accept_reports[1],0,__ATOMIC_RELAXED);
    NvnCmdBuf value={.guest=0x42,.mem=0x1000,.guest_capacity=256,.size=256,.host=0x2000,.start=0x2100};
    trace_log_used=0;trace_log[0]=0;capture_trace_log=true;
    for(unsigned i=0;i<12;i++) {
        memory_trace_accepted(&value,true,false,false,0x2000,12,0x3000,0x55,0);
        memory_trace_accepted(&value,false,true,true,0x2000,12,0x3000,0x55,0);
    }
    capture_trace_log=false;unsigned empty=0,prefix=0;const char *at=trace_log;
    while((at=strstr(at,"accepted-empty"))){++empty;++at;}at=trace_log;
    while((at=strstr(at,"accepted-prefix"))){++prefix;++at;}
    CHECK(empty==8 && prefix==8 && memory_trace_accept_reports[0]==8 && memory_trace_accept_reports[1]==8);
    CHECK(strstr(trace_log,"mode=off") && strstr(trace_log,"mode=on") &&
          strstr(trace_log,"old_floor=2000 old_used=12 new_guest=1000 new_end=1100") &&
          strstr(trace_log,"old_host=3000 new_host=2000 root=2100"));
}
int main(void) {
    CHECK(!nvn_command_lifetime_enabled());
    clear_color_kinds();
    basic();reuse_and_growth();chaining_and_callback();invalid_ranges();logical_and_spills();empty_payload_then_guest_callback();spill_failure_and_word_costs();nominal_over_reservation();deferred_bindings();deferred_nested();submit_lifetime();bounded_backing_and_diagnostics();indexed_lookup_complexity();continuation_reuse();continuation_child_pinning();continuation_callback_owner();continuation_submit_reuse_and_failure();continuation_mixed_capacity();initial_capacity_and_natural_growth();slab_failure_rollback();slab_packing_and_block_bounds();slab_oversized_actual_record();
    CHECK(lifetime_finalize_calls > 0);
    ring_prefix_and_empty_rebind();
    memory_callback_diagnostics();
    acceptance_log_limits();
    printf("{\"state\":\"passed\",\"checks\":%u,\"command_lifetime_mode\":\"off\",\"guest_chunks_unchanged\":true,\"payload_estimate_scale\":8,\"allocations_remaining\":%zu}\n",checks,live_allocations);return 0;
}
