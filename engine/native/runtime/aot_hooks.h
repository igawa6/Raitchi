#ifndef SHARED_RECOMP_AOT_HOOKS_H
#define SHARED_RECOMP_AOT_HOOKS_H
#include "aot_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AOT_MAX_FUNCTION_HOOKS 1024u
#define AOT_MAX_INSTRUCTION_HOOKS 256u
typedef int (*AotFunctionHook)(Cpu *, uint64_t actual_address);
typedef int (*AotInstructionHook)(Cpu *, uint64_t actual_site);
typedef void (*AotProfiler)(uint64_t actual_address, uint64_t lr, uint64_t x0);

/* Same quiescent configuration rule as dispatch. No build maps in core.
 * Function callback0 runs body, nonzero returns immediately from generated entry.
 * Instruction callback0 runs original instruction,1 handles it,2+ select the
 * site's generated local branches. Original-word/branch-list validation belongs
 * to generation/producer metadata, not this callback registry. */
AOT_API enum AotStatus rt_register_fnhook(uint64_t actual_address, AotFunctionHook hook);
AOT_API enum AotStatus rt_register_instruction_hook(uint64_t actual_site, AotInstructionHook hook);
/* Read-only queries for an alternate execution backend. Registration/replacement
 * still requires all guest execution to be quiescent; queries add no callbacks
 * and never mutate/cache the registry. Limit excludes the first registered site. */
AOT_API int aot_instruction_hook_registered(uint64_t actual_site);
AOT_API size_t aot_instruction_hook_limit(uint64_t actual_pc, size_t max_words);
AOT_API size_t aot_instruction_hook_count(void);
AOT_API int aot_instruction_hook_address(size_t index, uint64_t *address);
/* Alternate backend entry only: consume the exact next explicit original-call
 * bypass, before resolving HLE/overrides. Uses the same thread/Cpu/PC token as
 * FN_ENTER and restores outer tokens when nested rt_call_original returns. */
AOT_API int aot_original_call_take(Cpu *cpu, uint64_t actual_address);
/* Native passthrough HLE only: active backend and current module text required.
 * Bypasses its next exact host-override entry, uses LR0 as the native invocation
 * return boundary, then restores guest LR and any outer bypass token. This is
 * independent of generated FN_ENTER capabilities (LAS has instruction hooks).
 * No allocation, locks or resources are held across a possible guest unwind. */
AOT_API void aot_native_call_original(Cpu *cpu, uint64_t actual_address);
/* Bypasses exactly the intended next FN_ENTER on this host thread/Cpu. Nested
 * original calls restore the prior token. Calls the selected execution backend
 * when active, otherwise the generated table; both bypass HLE and overrides. Missing target
 * or missing function-hook capability aborts before any guest invocation. */
AOT_API void rt_call_original(Cpu *cpu, uint64_t actual_address);
AOT_API enum AotStatus aot_set_profiler(AotProfiler profiler);

#ifdef __cplusplus
}
#endif
#endif
