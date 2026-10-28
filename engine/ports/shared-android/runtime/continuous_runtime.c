/* Continuous original-code execution; services retain the existing Cpu ABI. */
#include "continuous_runtime.h"
#include "continuous_context.h"
#include "continuous_image.h"
#include "continuous_signals.h"
#include "native_hooks.h"
#include "native-game-backend/control_step.h"
#include "runtime/aot_hooks.h"
#include "rt.h"
#include "kernel.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct RunFrame { CnceContext context; struct RunFrame *next; } RunFrame;
typedef struct RunThread {
    Cpu *cpu;
    pthread_t owner;
    unsigned depth;
    RunFrame *frames;
    struct RunThread *next;
} RunThread;
static RunThread *threads;
static pthread_mutex_t threads_lock=PTHREAD_MUTEX_INITIALIZER;
static int configured;
static _Atomic uint64_t runs,exits,host_calls,svc_calls,hook_calls,tls_calls,clock_calls;
static _Atomic uint64_t original_steps,guest_ns,last_log_ns;

static int original_pc(uint64_t native_pc,uint64_t *result) {
    uint64_t pc=continuous_image_original_pc(native_pc);
    if(!result || !pc)return 0;
    *result=pc;return 1;
}
int continuous_runtime_configure(void) {
    if(configured || !aot_dispatch_is_bound())return -1;
    size_t host_count=aot_dispatch_override_count(),hook_count=aot_instruction_hook_count();
    if(host_count>AOT_MAX_DISPATCH_OVERRIDES || hook_count>AOT_MAX_INSTRUCTION_HOOKS)return -1;
    size_t count=host_count+hook_count;
    CncePatchRequest *requests=calloc(count?count:1,sizeof *requests);
    if(!requests)return -1;
    for(size_t i=0;i<host_count;i++) {
        if(!aot_dispatch_override_address(i,&requests[i].pc)){free(requests);return -1;}
        requests[i].kind=CNCE_PATCH_HOST_FUNCTION;
    }
    for(size_t i=0;i<hook_count;i++) {
        if(!aot_instruction_hook_address(i,&requests[host_count+i].pc)){free(requests);return -1;}
        requests[host_count+i].kind=CNCE_PATCH_INSTRUCTION_HOOK;
    }
    CnceImageModule modules[4];
    if(g_nmodules<=0 || g_nmodules>4){free(requests);return -1;}
    for(int i=0;i<g_nmodules;i++)modules[i]=(CnceImageModule){g_modules[i].base,g_modules[i].size,
        g_modules[i].text_off,g_modules[i].text_size};
    char error[512];
    int result=continuous_image_prepare(session_layout_current(),modules,(size_t)g_nmodules,
        requests,count,(uintptr_t)continuous_context_exit,error,sizeof error);
    free(requests);
    if(result){LOG("[continuous-nce] image preparation failed: %s\n",error);return -1;}
    ContinuousSignalHooks signals={continuous_image_is_original_guest_pc,original_pc};
    if(continuous_signals_configure(&signals)){continuous_image_finish_after_join();return -1;}
    (void)rt_ticks();
    configured=1;
    LOG("[continuous-nce] original modules executable; ordinary branches/loops stay native; host_entries=%zu instruction_hooks=%zu\n",host_count,hook_count);
    return 0;
}
static RunThread *thread_for(Cpu *cpu) {
    pthread_mutex_lock(&threads_lock);
    RunThread *thread=threads;
    while(thread && thread->cpu!=cpu)thread=thread->next;
    if(!thread) {
        thread=calloc(1,sizeof *thread);
        if(thread){thread->cpu=cpu;thread->owner=pthread_self();thread->next=threads;threads=thread;}
    }
    pthread_mutex_unlock(&threads_lock);
    if(!thread || !pthread_equal(thread->owner,pthread_self()))FATAL("continuous Cpu has no exclusive host-thread owner\n");
    return thread;
}
static RunFrame *frame_for(RunThread *thread) {
    if(thread->depth>=64)FATAL("continuous native callback nesting limit\n");
    RunFrame **slot=&thread->frames;
    for(unsigned i=0;i<thread->depth;i++) {
        if(!*slot)*slot=calloc(1,sizeof **slot);
        if(!*slot)FATAL("continuous invocation allocation failed\n");
        slot=&(*slot)->next;
    }
    if(!*slot)*slot=calloc(1,sizeof **slot);
    if(!*slot || continuous_context_init(&(*slot)->context,thread->cpu))
        FATAL("continuous invocation context rejected\n");
    ++thread->depth;
    return *slot;
}
/* Displaced PC-relative literal loads cannot be copied into a remote island.
 * This rare hook/original-entry path applies their original addressing here. */
static int literal_step(Cpu *cpu,uint32_t word,uint64_t pc) {
    if((word&UINT32_C(0x3b000000))!=UINT32_C(0x18000000))return 0;
    uint64_t imm=(word>>5)&UINT64_C(0x7ffff),sign=UINT64_C(1)<<18;
    uint64_t address=pc+(((imm^sign)-sign)<<2);
    unsigned reg=word&31,opc=word>>30,vector=(word>>26)&1;
    if(!vector) {
        if(opc==3){cpu->pc=pc+4;return 1;} /* PRFM has no architectural result. */
        uint64_t value=0;
        if(opc==1)memcpy(&value,(const void *)(uintptr_t)address,8);
        else {
            uint32_t low;memcpy(&low,(const void *)(uintptr_t)address,4);
            value=opc==2?(uint64_t)(int64_t)(int32_t)low:low;
        }
        if(reg!=31)cpu->x[reg]=value;
    } else {
        if(opc==3)FATAL("unsupported displaced SIMD literal load\n");
        memset(&cpu->v[reg],0,sizeof cpu->v[reg]);
        memcpy(&cpu->v[reg],(const void *)(uintptr_t)address,(size_t)4<<opc);
    }
    cpu->pc=pc+4;return 1;
}
static const void *original_first(Cpu *cpu,const CnceSite *site) {
    cpu->pc=site->original_pc;
    atomic_fetch_add_explicit(&original_steps,1,memory_order_relaxed);
    if((site->original_word&UINT32_C(0xffe0001f))==UINT32_C(0xd4000001)) {
        rt_svc(cpu,(site->original_word>>5)&0xffff,site->original_pc);
        cpu->pc=site->resume_pc;return NULL;
    }
    if(ngb_control_step(cpu,site->original_word,site->original_pc,rt_ticks()) ||
       literal_step(cpu,site->original_word,site->original_pc))return NULL;
    const void *entry=continuous_image_escape(cpu,site);
    if(!entry)FATAL("cannot relocate original instruction at %llx word=%08x\n",
        (unsigned long long)site->original_pc,site->original_word);
    return entry;
}
static void progress(void) {
    uint64_t now=plat_now_ns(),old=atomic_load_explicit(&last_log_ns,memory_order_relaxed);
    if(now-old<UINT64_C(2000000000) || !atomic_compare_exchange_strong_explicit(&last_log_ns,&old,now,memory_order_relaxed,memory_order_relaxed))return;
    extern uint64_t renderer_present_count(void);
    LOG("[continuous-nce-progress] runs=%llu exits=%llu hle=%llu svc=%llu hooks=%llu tls=%llu clock=%llu displaced=%llu native_wall_ns=%llu presents=%llu interpreter_instructions=0 generated_calls=%s\n",
        (unsigned long long)atomic_load(&runs),(unsigned long long)atomic_load(&exits),
        (unsigned long long)atomic_load(&host_calls),(unsigned long long)atomic_load(&svc_calls),
        (unsigned long long)atomic_load(&hook_calls),(unsigned long long)atomic_load(&tls_calls),
        (unsigned long long)atomic_load(&clock_calls),(unsigned long long)atomic_load(&original_steps),
        (unsigned long long)atomic_load(&guest_ns),(unsigned long long)renderer_present_count(),
        shared_native_manifest_current()?"none-no-game-DSO":"NX-guarded");
}
void continuous_runtime_run(Cpu *cpu) {
    if(!configured || !cpu || kernel_current_cpu()!=cpu)FATAL("continuous execution requires actual current KThread Cpu\n");
    RunThread *thread=thread_for(cpu);
    RunFrame *frame=frame_for(thread);
    uint64_t sentinel=continuous_image_return_target();
    if(!sentinel)FATAL("continuous invocation return landing missing\n");
    if(!cpu->x[30])cpu->x[30]=sentinel;
    atomic_fetch_add_explicit(&runs,1,memory_order_relaxed);
    const void *entry=NULL;
    while(cpu->pc) {
        rt_unwind_point();
        if(cpu->pc==sentinel){cpu->pc=0;cpu->x[30]=0;break;}
        (void)continuous_signals_take_reason(cpu);
        if(!entry)entry=continuous_image_resume(cpu,cpu->pc);
        if(!entry)FATAL("continuous resume outside admitted original code at %llx\n",(unsigned long long)cpu->pc);
        uint64_t before=plat_now_ns();
        int status=continuous_context_run(&frame->context,entry);
        atomic_fetch_add_explicit(&guest_ns,plat_now_ns()-before,memory_order_relaxed);
        entry=NULL;
        if(status)FATAL("continuous gateway return failed: %d\n",status);
        rt_unwind_point();
        const CnceSite *site=frame->context.event;
        if(!site) {
            if(!continuous_signals_take_reason(cpu))FATAL("continuous guest returned without exit or interrupt\n");
            continue;
        }
        if(!continuous_image_valid_event(site))FATAL("continuous exit descriptor is not immutable image metadata\n");
        atomic_fetch_add_explicit(&exits,1,memory_order_relaxed);
        if(site->original_pc==sentinel){cpu->pc=0;cpu->x[30]=0;break;}
        switch(site->kind) {
        case CNCE_PATCH_HOST_FUNCTION: {
            cpu->pc=site->original_pc;
            if(aot_original_call_take(cpu,site->original_pc)){entry=original_first(cpu,site);break;}
            GuestFn host=aot_dispatch_host_lookup(site->original_pc);
            if(!host)FATAL("unbound continuous host entry %llx\n",(unsigned long long)site->original_pc);
            uint64_t return_pc=cpu->x[30];
            atomic_fetch_add_explicit(&host_calls,1,memory_order_relaxed);
            host(cpu);
            cpu->pc=return_pc;
            break;
        }
        case CNCE_PATCH_INSTRUCTION_HOOK: {
            cpu->pc=site->original_pc;
            atomic_fetch_add_explicit(&hook_calls,1,memory_order_relaxed);
            int selected=native_hooks_apply(cpu,site->original_pc);
            if(!selected)entry=original_first(cpu,site);
            else if(selected==1)cpu->pc=site->resume_pc;
            else if(selected!=2)FATAL("invalid continuous hook selection\n");
            break;
        }
        case CNCE_PATCH_SVC:
            atomic_fetch_add_explicit(&svc_calls,1,memory_order_relaxed);
            rt_svc(cpu,site->operand,site->original_pc);cpu->pc=site->resume_pc;break;
        case CNCE_PATCH_TPIDR_READ:case CNCE_PATCH_TPIDRRO_READ:
            atomic_fetch_add_explicit(&tls_calls,1,memory_order_relaxed);
            if(site->operand!=31)cpu->x[site->operand]=site->kind==CNCE_PATCH_TPIDR_READ?cpu->tpidr:cpu->tpidrro;
            cpu->pc=site->resume_pc;break;
        case CNCE_PATCH_TPIDR_WRITE:
            atomic_fetch_add_explicit(&tls_calls,1,memory_order_relaxed);
            cpu->tpidr=site->operand==31?0:cpu->x[site->operand];cpu->pc=site->resume_pc;break;
        case CNCE_PATCH_COUNTER_READ:case CNCE_PATCH_FREQUENCY_READ:
            atomic_fetch_add_explicit(&clock_calls,1,memory_order_relaxed);
            if(site->operand!=31)cpu->x[site->operand]=site->kind==CNCE_PATCH_COUNTER_READ?rt_ticks():UINT64_C(19200000);
            cpu->pc=site->resume_pc;break;
        default:FATAL("unexpected continuous exit kind %u\n",site->kind);
        }
        progress();
    }
    --thread->depth;
}
void continuous_runtime_interrupt_all(void) {
    pthread_mutex_lock(&threads_lock);
    for(RunThread *thread=threads;thread;thread=thread->next)
        (void)continuous_signals_interrupt_cpu(thread->cpu,CONTINUOUS_SIGNAL_STOP);
    pthread_mutex_unlock(&threads_lock);
}
void continuous_runtime_finish_after_join(void) {
    continuous_image_finish_after_join();
    for(RunThread *thread=threads;thread;) {
        RunThread *next=thread->next;
        for(RunFrame *frame=thread->frames;frame;){RunFrame *following=frame->next;free(frame);frame=following;}
        free(thread);thread=next;
    }
    threads=NULL;
}
