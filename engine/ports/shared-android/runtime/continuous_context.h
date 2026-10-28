/* Continuous native context seam. Architectural design follows the GPL-tagged
 * Eden/yuzu NCE reference described in docs/native-nce-next-step.md; this is an
 * independently implemented adapter for the shared Cpu ABI. No guest assets. */
#pragma once
#ifndef __ASSEMBLER__
#include "runtime/aot_cpu.h"
#include "continuous-nce/patch_plan.h"
#define CNCE_CONTEXT_MAGIC UINT64_C(0x434e434543545831)
typedef struct __attribute__((aligned(16))) CnceContext {
    uint64_t magic;
    Cpu *cpu;
    uint64_t host_tpidr,host_x18,host_sp;
    const CnceSite *event;
    uint64_t mode,host_fpcr,host_fpsr;
    struct CnceContext *self;
} CnceContext;
/* Invocation-owned; storage survives any service ExitThread longjmp. Nested
 * host->guest callbacks use another context. Never call host C with private TLS.
 * Enter/exit temporarily use16bytes BELOW guestSP; caller supplies writable,
 * guarded guest stack headroom. Resume must be a verified near-target8B stub. */
int continuous_context_init(CnceContext *context,Cpu *cpu);
int continuous_context_run(CnceContext *context,const void *resume_trampoline);
void continuous_context_enter(CnceContext *context,const void *resume_trampoline);
void continuous_context_exit(void);
/* Install only as SA_SIGINFO|SA_ONSTACK with a real guarded host sigaltstack.
 * Dispatch runs with host TLS/X18. Return1 restores private TLS before sigreturn;
 * return0 retains host TLS for an intentionally rewritten host-return context. */
void continuous_context_signal_front(int signal,void *info,void *ucontext);
int continuous_signals_dispatch(int signal,void *info,void *ucontext,CnceContext *guest);
/* Signal integration can redirect ucontext PC here and x0=context after saving
 * guest state; this restores the suspended host invocation and returns to run. */
void continuous_context_signal_return(CnceContext *context);
extern const unsigned char continuous_context_transition_begin[],continuous_context_transition_end[];
int continuous_context_is_transition(uint64_t pc);
#endif
