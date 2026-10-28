#define _GNU_SOURCE
#include "bridge.h"
#include "common_session.h"
#include "runtime/aot_dispatch.h"
#include "runtime/session_layout.h"
#include <dlfcn.h>
#include <sched.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint64_t tls_value(void) { uint64_t v; __asm__ volatile("mrs %0,tpidr_el0":"=r"(v)); return v; }
static uint64_t x18_value(void) { uint64_t v; __asm__ volatile("mov %0,x18":"=r"(v)); return v; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
static SharedGuestThreadSnapshot snapshot(void) {
    SharedGuestThreadSnapshot q={.struct_size=sizeof q,.version=SHARED_GUEST_API_VERSION}; return q;
}
static void entry_callback(Cpu *cpu) {
    NtContext *c=(NtContext *)(uintptr_t)cpu->x[0];
    c->cpu=cpu; c->host_tls=tls_value(); c->host_x18=x18_value();
    uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    if(c->stack_enabled) {
        pthread_attr_t attributes;void *base=NULL;size_t size=0;
        if(pthread_getattr_np(pthread_self(),&attributes)) { atomic_store(&c->failed,1);return; }
        int error=pthread_attr_getstack(&attributes,&base,&size);
        pthread_attr_destroy(&attributes);
        if(error || !base || size<16384 || (uintptr_t)base>UINTPTR_MAX-size) { atomic_store(&c->failed,1);return; }
        c->host_stack_lo=(uintptr_t)base;c->host_stack_hi=(uintptr_t)base+size;
    }
    SharedGuestThreadSnapshot q=snapshot();
    if(c->current(cpu,&q)!=SHARED_GUEST_OK || q.tls_base!=cpu->tpidrro || q.thread_id!=c->expected_id ||
       q.handle!=c->expected_handle || q.cpu_address!=(uintptr_t)cpu || !(q.flags&SHARED_GUEST_THREAD_CURRENT)) {
        atomic_store(&c->failed,1); return;
    }
    if(c->sdk_enabled) {
        /* Run the validated descriptor entry on this attached, actual Cpu.
         * Restore its full original image before native architectural seeding. */
        memcpy(c->before,cpu,sizeof *cpu);
        c->aot_fn(cpu);c->aot_calls++;c->aot_seen=cpu->x[0];
        c->aot_failures+=c->aot_seen!=c->expected_core || tls_value()!=c->host_tls ||
            x18_value()!=c->host_x18 || !nt_opaque_equal(cpu,c->before);
        memcpy(cpu,c->before,sizeof *cpu);
        rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
    }
    c->original_guest_sp=cpu->sp;
    /* Preserve the real KThread, TLS, exclusive state and dispatch cache. */
    for(unsigned i=0;i<31;i++) cpu->x[i]=UINT64_C(0x1122334455667788)^((uint64_t)i<<40)^c->expected_id;
    cpu->x[30]=(uintptr_t)nt_resume; cpu->nzcv=0xa0000000; cpu->fpcr=0; cpu->fpsr=1;
    for(unsigned i=0;i<32;i++) cpu->v[i]=((__uint128_t)(UINT64_C(0x9988776655443322)^i)<<64)|(UINT64_C(0x123456789abcdef0)^i);
    memcpy(c->seed,cpu,sizeof *cpu); memcpy(c->opaque,cpu,sizeof *cpu);
    *(uint64_t *)(uintptr_t)(c->expected_tlr+0x100)=c->marker;
    atomic_store_explicit(&c->entered,1,memory_order_release);
    atomic_fetch_add_explicit(c->barrier,1,memory_order_acq_rel);
    uint64_t deadline=now_ns()+UINT64_C(5000000000);
    while(atomic_load_explicit(c->barrier,memory_order_acquire)!=2) {
        if(now_ns()>deadline) { atomic_store(&c->failed,1); return; } sched_yield();
    }
    nt_enter(c); /* ExitThread deliberately abandons this frame through thread_main's setjmp. */
    if(c->exit_mode || tls_value()!=c->host_tls || x18_value()!=c->host_x18 || rt_get_fpcr()!=fpcr || rt_get_fpsr()!=fpsr)
        atomic_store(&c->failed,1);
    cpu->sp=c->original_guest_sp;
    atomic_store_explicit(&c->returned,1,memory_order_release);
}
static uint32_t ldr(unsigned rt,unsigned rn,size_t offset) { return 0xf9400000u|((uint32_t)(offset/8)<<10)|(rn<<5)|rt; }
static uint32_t str(unsigned rt,unsigned rn,size_t offset) { return 0xf9000000u|((uint32_t)(offset/8)<<10)|(rn<<5)|rt; }
static int direct_branch(uintptr_t pc,uintptr_t target,uint32_t *out) {
    return nt_patch_branch(0xd4000201u,pc,target,out);
}
static int build_code(NtContext *c,const NtSdkInput *sdk,void **allocation,size_t *allocation_size) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE), island=(size_t)(nt_island_end-nt_island_begin);
    size_t count=NT_BLOCKS*4u+c->exit_mode+c->sdk_enabled, total=(4096+count*island+page-1)&~(page-1);
    unsigned char *map=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(map==MAP_FAILED) return -1;
    uint32_t *p=(uint32_t *)map;
    *p++=ldr(17,19,offsetof(NtContext,cpu)); *p++=ldr(16,17,offsetof(Cpu,x)+16*8); *p++=ldr(17,17,offsetof(Cpu,x)+17*8);
#define EMIT_TRAP(word,kind_,svc_) do { \
    size_t index=c->site_count++; unsigned char *gateway=map+4096+index*island; \
    memcpy(gateway,nt_island_begin,island); \
    NtSite site={(uintptr_t)p,kind_,svc_}; c->sites[index]=site; \
    memcpy(gateway+(nt_island_site-nt_island_begin),&site,sizeof site); \
    uint32_t branch; if(nt_patch_branch(word,(uintptr_t)p,(uintptr_t)gateway,&branch)) return -1; \
    *p=branch; if(direct_branch((uintptr_t)gateway+(nt_island_return-nt_island_begin),(uintptr_t)(p+1),&branch)) return -1; \
    memcpy(gateway+(nt_island_return-nt_island_begin),&branch,4); p++; \
} while(0)
    if(c->stack_enabled) {
        *p++=0xd10083ffu; /* SUB SP,SP,#32 */
        *p++=0xa90057f4u; /* STP X20,X21,[SP] */
        *p++=0xa9015ff6u; /* STP X22,X23,[SP,#16] */
    }
    for(unsigned i=0;i<NT_BLOCKS;i++) {
        EMIT_TRAP(0xd53bd060u,NT_SITE_TLS_RO,0);
        *p++=ldr(0,0,0x100); *p++=str(0,19,offsetof(NtContext,marker_seen));
        EMIT_TRAP(0xd4000201u,NT_SITE_SVC,0x10); *p++=str(0,19,offsetof(NtContext,core_seen));
        *p++=0x52900001u; /* MOV W1,#0x8000 */
        *p++=0x72bfffe1u; /* MOVK W1,#0xffff,LSL#16: current-thread pseudo handle */
        EMIT_TRAP(0xd40004a1u,NT_SITE_SVC,0x25);
        *p++=str(0,19,offsetof(NtContext,id_rc)); *p++=str(1,19,offsetof(NtContext,id_seen));
        EMIT_TRAP(0xd40003c1u,NT_SITE_SVC,0x1e); *p++=str(0,19,offsetof(NtContext,tick_seen));
    }
    if(c->sdk_enabled) {
        /* Private original wrapper and thunk occupy separate reviewed slots.
         * Relocate only wrapper B; the original RET remains byte-for-byte. */
        uint32_t *wrapper=(uint32_t *)(map+2048),*thunk=(uint32_t *)(map+2056);
        memcpy(wrapper,&sdk->words[0],4);memcpy(thunk,&sdk->words[1],8);
        uint32_t branch;
        if(direct_branch((uintptr_t)wrapper,(uintptr_t)thunk,&branch)) return -1;
        *wrapper=branch;
        *p++=str(30,19,offsetof(NtContext,sdk_saved_lr));
        if(direct_branch((uintptr_t)p,(uintptr_t)wrapper,&branch)) return -1;
        *p++=branch|0x80000000u; /* BL: only the link bit differs from B */
        c->sdk_lr=(uintptr_t)p;
        *p++=0x10000001u; /* ADR X1, current PC: exact nested return marker */
        *p++=str(1,19,offsetof(NtContext,sdk_resume_seen));
        *p++=str(0,19,offsetof(NtContext,sdk_seen));
        *p++=ldr(1,19,offsetof(NtContext,sdk_returns));
        *p++=0x91000421u; /* ADD X1,X1,#1, preserving NZCV */
        *p++=str(1,19,offsetof(NtContext,sdk_returns));
        *p++=ldr(30,19,offsetof(NtContext,sdk_saved_lr));
        uint32_t *continuation=p;p=thunk;
        EMIT_TRAP(sdk->words[1],NT_SITE_SDK,0x10);
        p=continuation;
    }
    if(c->exit_mode) { EMIT_TRAP(0xd4000141u,NT_SITE_SVC,0x0a); }
    if(c->stack_enabled) {
        *p++=0xa94057f4u; /* LDP X20,X21,[SP] */
        *p++=0xa9415ff6u; /* LDP X22,X23,[SP,#16] */
        *p++=0x910083ffu; /* ADD SP,SP,#32 */
    }
    *p++=0xd65f03c0u;
#undef EMIT_TRAP
    if((unsigned char*)p>map+2048) return -1;
    __builtin___clear_cache((char*)map,(char*)map+total);
    if(mprotect(map,total,PROT_READ|PROT_EXEC)) return -1;
    c->entry=map; *allocation=map; *allocation_size=total; return 0;
}
static int map_permissions(void *address,size_t size,const char *expected) {
    FILE *f=fopen("/proc/self/maps","r"); if(!f) return 0;
    char line[512],perm[5]; unsigned long long lo,hi; int ok=0;
    while(fgets(line,sizeof line,f)) if(sscanf(line,"%llx-%llx %4s",&lo,&hi,perm)==3 && lo<=(uintptr_t)address && hi>=(uintptr_t)address+size) { ok=!memcmp(perm,expected,3); break; }
    fclose(f); return ok;
}
static int is_rx(void *address,size_t size) { return map_permissions(address,size,"r-x"); }
static int prepare_guest_stack(NtContext *c,const SessionLayout *layout,unsigned index,void **slot,size_t *slot_size) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    if(page<4096 || (page&(page-1))) return -1;
    size_t usable=(131072+page-1)&~(page-1),total=usable+2*page;
    SessionRange region=layout->regions[SESSION_STACK];
    if(region.hi<=region.lo || (region.lo&(page-1)) || total>(region.hi-region.lo)/2) return -1;
    uintptr_t address=region.lo+index*total;
    if(!map_permissions((void*)address,total,"rw-")) return -1;
    /* This fresh headless session has no original startup/stack allocations.
     * Claim a subrange of its existing reservation; never MAP_FIXED over it. */
    *slot=(void*)address;*slot_size=total;
    if(mprotect((void*)address,page,PROT_NONE) || mprotect((void*)(address+page+usable),page,PROT_NONE)) return -1;
    c->guest_stack_lo=address+page;c->guest_stack_hi=address+page+usable;
    c->guest_entry_sp=c->guest_stack_hi-128;c->stack_cookie=c->marker^UINT64_C(0x57acda7a90123456);
    uint64_t *low=(uint64_t*)(uintptr_t)c->guest_stack_lo,*high=(uint64_t*)(uintptr_t)(c->guest_stack_hi-64);
    for(unsigned i=0;i<8;i++) {low[i]=c->stack_cookie^i;high[i]=~c->stack_cookie^i;}
    return map_permissions((void*)address,page,"---") && map_permissions((void*)(address+page+usable),page,"---") &&
           map_permissions((void*)(address+page),usable,"rw-") && nt_stack_canaries_good(c) ? 0 : -1;
}
#define LOAD(name,type) type name=NULL; do { void *symbol=dlsym(runtime,#name); memcpy(&name,&symbol,sizeof symbol); if(!name) { error="missing-" #name; goto done; } } while(0)
__attribute__((visibility("default"))) int native_thread_bridge_probe(const char *runtime_path,const char *root,const char *game,const char *sidecar,char *output,size_t capacity,const NtSdkInput *sdk,uint32_t stack_mode) {
    if(!runtime_path||!root||!game||!sidecar||!output||capacity<4096 || stack_mode>1 || (sdk&&!nt_sdk_input_valid(sdk))) return -1;
    const char *error="none"; int result=1,failures=0,joined=0,rx=0,prepared=0,guards=0;
    NtContext *contexts=calloc(2,sizeof *contexts); _Atomic uint32_t *barrier=calloc(1,sizeof *barrier);
    Cpu *driver=calloc(1,sizeof *driver); void *maps[2]={0},*stacks[2]={0};size_t sizes[2]={0},stack_sizes[2]={0};
    uint64_t main_tls=tls_value(),main_x18=x18_value(),main_fpcr=rt_get_fpcr(),main_fpsr=rt_get_fpsr();
    void *runtime=dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);
    if(!runtime || !contexts || !barrier || !driver) { error="allocation-or-dlopen"; goto done; }
    typedef bool (*Prepare)(const char*,const char*,const char*,char*,size_t);
    typedef enum SharedGuestStatus (*GuestPrepare)(const char*,char*,size_t);
    typedef enum SharedGuestStatus (*Query)(uint32_t,SharedGuestThreadSnapshot*);
    typedef enum SharedGuestStatus (*Shutdown)(void);
    typedef enum AotStatus (*AddHle)(uint64_t,GuestFn);
    typedef const SessionLayout *(*Layout)(void);
    LOAD(shared_session_prepare,Prepare);LOAD(shared_guest_prepare,GuestPrepare);LOAD(shared_guest_current,NtCurrent);
    LOAD(shared_guest_query,Query);LOAD(shared_guest_join_finished,Query);LOAD(shared_guest_shutdown,Shutdown);
    typedef const ModuleAbiDescriptor *(*Descriptor)(void);
    LOAD(shared_gamelib_descriptor,Descriptor);
    LOAD(rt_svc,NtSvc);LOAD(aot_dispatch_add_hle,AddHle);LOAD(session_layout_current,Layout);
    char why[512],exefs[4096];
    if(!shared_session_prepare(root,game,sidecar,why,sizeof why)) { error="session-prepare"; goto done; }
    if(snprintf(exefs,sizeof exefs,"%s/exefs",root)>=(int)sizeof exefs || shared_guest_prepare(exefs,why,sizeof why)!=SHARED_GUEST_OK) { error="guest-prepare"; goto done; }
    prepared=1; const SessionLayout *layout=session_layout_current();
    if(!layout) { error="layout"; goto done; }
    ModuleAbiGuestFn aot_fn=NULL;
    if(sdk && !(aot_fn=nt_sdk_descriptor_entry(shared_gamelib_descriptor(),sdk->aot_address))) { error="sdk-descriptor-entry";goto done; }
    uint64_t hle=layout->regions[SESSION_HLE].hi-16;
    if(aot_dispatch_add_hle(hle,entry_callback)!=AOT_OK) { error="register-entry"; goto done; }
    rt_svc(driver,0x1e,0); /* serial prime of production clock/config cache */
    for(unsigned i=0;i<2;i++) {
        NtContext *c=&contexts[i]; c->call=nt_call;c->current=shared_guest_current;c->svc=rt_svc;c->barrier=barrier;c->exit_mode=i;c->sdk_enabled=sdk!=NULL;c->aot_fn=aot_fn;c->stack_enabled=stack_mode;
        c->marker=UINT64_C(0xfeed765432100000)+i;
        if(stack_mode) { if(prepare_guest_stack(c,layout,i,&stacks[i],&stack_sizes[i])) {error="guest-stack-guards";goto done;} guards+=2; }
        if(posix_memalign((void**)&c->seed,_Alignof(Cpu),sizeof(Cpu)) || posix_memalign((void**)&c->before,_Alignof(Cpu),sizeof(Cpu)) || posix_memalign((void**)&c->opaque,_Alignof(Cpu),sizeof(Cpu)) || build_code(c,sdk,&maps[i],&sizes[i])) { error="context-code"; goto done; }
        rx+=is_rx(maps[i],sizes[i]);
        driver->x[1]=hle;driver->x[2]=(uintptr_t)c;driver->x[3]=stack_mode?c->guest_entry_sp:((layout->regions[SESSION_HOST_HEAP].hi-(i+1)*0x10000)&~15ull);driver->x[4]=44;driver->x[5]=i;
        rt_svc(driver,0x08,0); if(driver->x[0]) { error="create-thread";goto done; }
        c->expected_handle=(uint32_t)driver->x[1];
        SharedGuestThreadSnapshot q=snapshot();
        if(shared_guest_query(c->expected_handle,&q)!=SHARED_GUEST_OK) { error="query-unstarted";goto done; }
        c->expected_id=q.thread_id;c->expected_tlr=q.tls_base;c->expected_core=(uint32_t)q.core;
    }
    failures+=contexts[0].expected_id==contexts[1].expected_id || contexts[0].expected_tlr==contexts[1].expected_tlr;
    for(unsigned i=0;i<2;i++) { driver->x[0]=contexts[i].expected_handle;rt_svc(driver,0x09,0);if(driver->x[0]) { error="start-thread";goto done; } }
    uint64_t deadline=now_ns()+UINT64_C(10000000000);
    for(unsigned i=0;i<2;i++) {
        SharedGuestThreadSnapshot q;
        do { q=snapshot();if(shared_guest_query(contexts[i].expected_handle,&q)!=SHARED_GUEST_OK) { error="query-started";goto done; }
            if(q.flags&SHARED_GUEST_THREAD_FINISHED) break;
            if(now_ns()>deadline) { error="thread-timeout";goto done; }
            struct timespec delay={0,1000000};nanosleep(&delay,NULL);
        } while(1);
        q=snapshot();if(shared_guest_join_finished(contexts[i].expected_handle,&q)!=SHARED_GUEST_OK || !(q.flags&SHARED_GUEST_THREAD_JOINED) || !(q.flags&SHARED_GUEST_THREAD_SIGNALED)) { error="join-finished";goto done; } joined++;
    }
    for(unsigned i=0;i<2;i++) {
        NtContext *c=&contexts[i];failures+=!atomic_load(&c->entered)||atomic_load(&c->failed)||c->state_failures||c->identity_failures||c->opaque_failures;
        failures+=c->marker_seen!=c->marker||*(uint64_t*)(uintptr_t)(c->expected_tlr+0x100)!=c->marker||c->core_seen!=c->expected_core||c->id_rc||c->id_seen!=c->expected_id||!c->tick_seen;
        if(stack_mode) {
            failures+=c->stack_failures || c->entry_rejections || !nt_stack_canaries_good(c);
            failures+=c->entry_depth!=i || c->gateway_active!=i;
            failures+=c->stack_checks!=NT_BLOCKS*4+c->sdk_enabled+1 ||
                c->native_stack_canary_checks!=NT_BLOCKS*4+c->sdk_enabled+i;
        }
        c->sdk_resume_failures+=!nt_sdk_resume_good(c);
        failures+=c->sdk_resume_failures || c->aot_failures || c->sdk_lr_failures;
        failures+=c->transitions!=NT_BLOCKS*3+c->sdk_enabled || c->tls_reads!=NT_BLOCKS || c->exit_returned;
        failures+=i ? (!c->exit_entered||atomic_load(&c->returned)||c->returned_native) : (c->exit_entered||!atomic_load(&c->returned)||c->returned_native!=1);
    }
    if(shared_guest_shutdown()!=SHARED_GUEST_OK) { error="shutdown";goto done; }
    prepared=0;failures+=rx!=2;result=failures!=0;
done:
    /* On any live-thread failure the helper process must exit; leave context and code live. */
    if(tls_value()!=main_tls||x18_value()!=main_x18||rt_get_fpcr()!=main_fpcr||rt_get_fpsr()!=main_fpsr) { failures++;result=1; }
    NtTotals totals=nt_totals(contexts,(unsigned)joined);
    snprintf(output,capacity,"{\"schema\":1,\"state\":\"%s\",\"error\":\"%s\",\"failures\":%d,\"real_threads_joined\":%d,\"rx_mappings\":%d,\"counters_known\":%s,\"state_failures\":%llu,\"identity_failures\":%llu,\"identity_reasons\":%llu,\"opaque_failures\":%llu,\"svc_transitions\":%llu,\"tls_reads\":%llu,\"exit_thread_entered\":%llu,\"exit_thread_returned\":%llu,\"original_wrapper_executed\":%s,\"original_sdk_wrapper_calls\":%llu,\"original_sdk_wrapper_returns\":%llu,\"sdk_nested_lr_checks\":%llu,\"sdk_nested_lr_failures\":%llu,\"sdk_resume_failures\":%llu,\"aot_descriptor_calls\":%llu,\"aot_comparison_failures\":%llu,\"guarded_guest_stack\":%s,\"guard_pages_verified\":%d,\"host_c_stack_checks\":%llu,\"native_stack_canary_checks\":%llu,\"stack_failures\":%llu,\"entry_rejections\":%llu,\"host_backed_sp\":%s,\"guest_x18_x19_virtual\":true,\"original_sdk_started\":false,\"arbitrary_native_game_execution\":false}",result?"failed":"passed",error,failures,joined,rx,totals.known?"true":"false",(unsigned long long)totals.state_failures,(unsigned long long)totals.identity_failures,(unsigned long long)totals.identity_reasons,(unsigned long long)totals.opaque_failures,(unsigned long long)totals.transitions,(unsigned long long)totals.tls_reads,(unsigned long long)totals.exit_entered,(unsigned long long)totals.exit_returned,(totals.known&&totals.sdk_calls)?"true":"false",(unsigned long long)totals.sdk_calls,(unsigned long long)totals.sdk_returns,(unsigned long long)totals.sdk_lr_checks,(unsigned long long)totals.sdk_lr_failures,(unsigned long long)totals.sdk_resume_failures,(unsigned long long)totals.aot_calls,(unsigned long long)totals.aot_failures,stack_mode?"true":"false",guards,(unsigned long long)totals.stack_checks,(unsigned long long)totals.native_stack_canary_checks,(unsigned long long)totals.stack_failures,(unsigned long long)totals.entry_rejections,stack_mode?"false":"true");
    if(!prepared || joined==2) { if(contexts) for(unsigned i=0;i<2;i++) {if(maps[i])munmap(maps[i],sizes[i]);if(stacks[i])mprotect(stacks[i],stack_sizes[i],PROT_READ|PROT_WRITE);free(contexts[i].seed);free(contexts[i].before);free(contexts[i].opaque);}free(contexts);free(barrier);free(driver); }
    /* Runtime, title library and registered HLE callback stay loaded until process exit. */
    return result;
}
