#define _GNU_SOURCE
#include "native_session.h"
#include "native_engine.h"
#include "native_blocks.h"
#include "native_hooks.h"
#include "continuous_runtime.h"
#include "continuous_signals.h"
#include "native_las_patches.h"
#include "kernel.h"
#include "gamelib.h"
#include "runtime/aot_hooks.h"
#include <dlfcn.h>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

static struct {void *address;size_t size;} forbidden[16];
static unsigned forbidden_count;
int native_session_requested(void) {
    const char *backend=getenv("LARECOMP_CPU_BACKEND");
    if(!backend || !*backend || !strcmp(backend,"aot"))return 0;
    if(!strcmp(backend,"continuous-nce"))return 2;
    if(strcmp(backend,"native-blocks"))FATAL("unknown CPU backend %s\n",backend);
    return 1;
}
int native_session_continuous(void) { return native_session_requested()==2; }
const char *native_session_suffix(void) {
    int mode=native_session_requested();
    return mode==2?"-nce":mode==1?"-native":"";
}
void native_session_stop_idle(void) {
    if(native_session_continuous())continuous_runtime_interrupt_all();
}
static GuestFn native_host_lookup(uint64_t pc) {
    if(aot_original_call_take(kernel_current_cpu(),pc)) return NULL;
    return aot_dispatch_host_lookup(pc);
}
static void forbid_generated_execution(void) {
    Dl_info info;
    const ModuleAbiDescriptor *descriptor=shared_gamelib_descriptor();
    if(shared_native_manifest_current()) {
        if(descriptor)FATAL("native-only setup unexpectedly bound a game DSO\n");
        LOG("[native-engine] native-only session: no generated game DSO requested or bound\n");
        return;
    }
    if(!descriptor || !dladdr(descriptor,&info) || !info.dli_fbase)FATAL("cannot locate generated game ELF\n");
    const Elf64_Ehdr *header=info.dli_fbase;
    if(memcmp(header->e_ident,ELFMAG,SELFMAG) || header->e_ident[EI_CLASS]!=ELFCLASS64 ||
       header->e_phentsize!=sizeof(Elf64_Phdr) || header->e_phnum>32)FATAL("unexpected generated ELF layout\n");
    const Elf64_Phdr *ph=(const void *)((const char*)header+header->e_phoff);
    uintptr_t page=(uintptr_t)sysconf(_SC_PAGESIZE);
    for(unsigned i=0;i<header->e_phnum;i++)if(ph[i].p_type==PT_LOAD && (ph[i].p_flags&PF_X)) {
        if((ph[i].p_flags&PF_W) || forbidden_count==16)FATAL("unexpected generated executable segment\n");
        uintptr_t lo=((uintptr_t)header+ph[i].p_vaddr)&~(page-1);
        uintptr_t hi=((uintptr_t)header+ph[i].p_vaddr+ph[i].p_memsz+page-1)&~(page-1);
        if(hi<=lo || mprotect((void*)lo,hi-lo,PROT_READ))FATAL("cannot disable generated game code execution\n");
        forbidden[forbidden_count].address=(void*)lo;forbidden[forbidden_count++].size=hi-lo;
    }
    if(!forbidden_count)FATAL("generated game code has no executable segment guard\n");
    LOG("[native-engine] generated guest code is non-executable (%u segments), descriptor data retained\n",forbidden_count);
}
void native_session_start(void) {
    if(!native_session_requested())return;
    if(native_session_continuous()) {
        if(aot_dispatch_set_execution_backend(continuous_runtime_run)!=AOT_OK)
            FATAL("cannot bind continuous native execution backend\n");
        extern void loader_native_host_definitions(void);
        loader_native_host_definitions();
        if(native_las_patches_register())FATAL("native LAS compatibility patches rejected\n");
        if(continuous_runtime_configure())FATAL("cannot configure continuous native image\n");
        forbid_generated_execution();
        LOG("[continuous-nce] continuous original ARM64 execution enabled; C-AOT execution disabled\n");
        return;
    }
    NativeEngineHooks hooks={native_host_lookup,native_blocks_run,rt_unwind_point,native_hooks_apply,native_hooks_limit};
    if(native_engine_configure(&hooks) || aot_dispatch_set_execution_backend(native_engine_run)!=AOT_OK)
        FATAL("cannot configure original-code execution backend\n");
    extern void loader_native_host_definitions(void);
    loader_native_host_definitions();
    if(native_las_patches_register())FATAL("native LAS compatibility patches rejected\n");
    forbid_generated_execution();
    LOG("[native-engine] original NSO execution enabled: native blocks plus instruction interpreter; C-AOT execution disabled\n");
}
void native_session_finish(void) {
    if(!native_session_requested())return;
    if(native_session_continuous()) {
        ContinuousSignalStats signals;continuous_signals_stats(&signals);
        LOG("[continuous-nce] shutdown joined: threads=%llu interrupts=%llu acknowledged=%llu deferred=%llu faults=%llu retried=%llu\n",
            (unsigned long long)signals.registered_threads,(unsigned long long)signals.interrupt_requests,
            (unsigned long long)signals.interrupted_guests,(unsigned long long)signals.deferred_interrupts,
            (unsigned long long)signals.chained_faults,(unsigned long long)signals.resumed_faults);
        continuous_runtime_finish_after_join();
    }
    else {
      NativeEngineStats s;native_engine_stats(&s);
      LOG("[native-engine] final native_blocks=%llu native_instructions=%llu interpreted=%llu hle=%llu svc=%llu failures=%llu\n",
        (unsigned long long)s.native_blocks,(unsigned long long)s.native_instructions,(unsigned long long)s.interpreted_instructions,
        (unsigned long long)s.hle_calls,(unsigned long long)s.svc_calls,(unsigned long long)s.failures);
    }
    for(unsigned i=0;i<forbidden_count;i++)if(mprotect(forbidden[i].address,forbidden[i].size,PROT_READ|PROT_EXEC))
        FATAL("cannot restore ELF finalizer execution after guest shutdown\n");
}
