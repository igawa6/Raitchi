/* Experimental original-NSO native-block + Unicorn execution backend. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "runtime/aot_cpu.h"

typedef struct NativeEngineHooks {
    GuestFn (*host_lookup)(uint64_t actual_pc); /* HLE/overrides ONLY; never AOT. */
    /* 0 executed; positive NOT ENTERED (safe interpreter fallback); negative
     * potentially entered/failed (fatal, never replay partially executed code). */
    /* gpr_usage is the union of every original body GPR read/write. Executor
     * chooses a context register absent from this mask; X18 remains reserved. */
    int (*run_native_block)(Cpu *, const uint32_t *words, size_t count, uint32_t gpr_usage);
    void (*safe_point)(void);
    int (*instruction_hook)(Cpu *, uint64_t actual_pc); /* 0 original, 1 skip, 2 PC set */
    size_t (*native_limit)(Cpu *, uint64_t actual_pc, size_t max_words);
} NativeEngineHooks;
typedef struct NativeEngineStats {
    uint64_t invocations, native_blocks, native_instructions, interpreted_instructions, native_declines;
    uint64_t software_instructions, main_native_instructions, sdk_native_instructions, atomic_instructions;
    uint64_t hle_calls, svc_calls, instruction_hooks, mapped_pages, failures, aot_calls;
    uint64_t plan_cache_hits, plan_cache_misses, plan_cache_invalidations;
} NativeEngineStats;

/* Configure once, quiescently, before creating game threads. Contexts remain
 * process-owned so a production ExitThread longjmp never frees live state. */
int native_engine_configure(const NativeEngineHooks *hooks);
void native_engine_run(Cpu *cpu);
void native_engine_stats(NativeEngineStats *out);
