#include "continuous_context.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(CnceContext)==80,"control size");
#define OFF(member,value) _Static_assert(offsetof(CnceContext,member)==value,#member)
OFF(magic,0); OFF(cpu,8); OFF(host_tpidr,16); OFF(host_x18,24); OFF(host_sp,32);
OFF(event,40); OFF(mode,48); OFF(host_fpcr,56); OFF(host_fpsr,64); OFF(self,72);
_Static_assert(sizeof(CnceSite)==40,"site ABI");
int continuous_context_init(CnceContext *ctx,Cpu *cpu) {
    if(!ctx || !cpu || ((uintptr_t)ctx&15) || ((uintptr_t)cpu&15))return -1;
    memset(ctx,0,sizeof(*ctx));ctx->magic=CNCE_CONTEXT_MAGIC;ctx->cpu=cpu;ctx->self=ctx;return 0;
}
/* The host probes validate context layout and patch emission without executing
 * the ARM64 transition entries; their link lines deliberately omit
 * continuous_context.S. Keep that validation-only branch on native aarch64 host
 * probe builds, where a HOST_TEST macro is defined, so they neither reference
 * assembly-only symbols nor claim native execution. Device builds define
 * neither macro and keep the real assembly path unchanged. */
#if defined(__aarch64__) && !defined(CONTINUOUS_CONTEXT_HOST_TEST) && !defined(CONTINUOUS_SIGNALS_HOST_TEST)
#define CONTINUOUS_NATIVE_ASM 1
#else
#define CONTINUOUS_NATIVE_ASM 0
#endif
int continuous_context_run(CnceContext *ctx,const void *resume) {
    if(!ctx || ((uintptr_t)ctx&15) || ctx->magic!=CNCE_CONTEXT_MAGIC || ctx->self!=ctx || !ctx->cpu || ctx->mode || !resume || ((uintptr_t)resume&3) || (ctx->cpu->sp&15) || ctx->cpu->sp<16)return -1;
#if CONTINUOUS_NATIVE_ASM
    continuous_context_enter(ctx,resume);return ctx->mode==0?0:-1;
#else
    return -2; /* Host validation only; no simulated native-execution claim. */
#endif
}
int continuous_context_is_transition(uint64_t pc) {
#if CONTINUOUS_NATIVE_ASM
    return pc>=(uintptr_t)continuous_context_transition_begin && pc<(uintptr_t)continuous_context_transition_end;
#else
    (void)pc;return 0;
#endif
}
