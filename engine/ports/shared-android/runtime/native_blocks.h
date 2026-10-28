#pragma once
#ifndef __ASSEMBLER__
#include "runtime/aot_cpu.h"
#include "native-game-backend/block_plan.h"
/* 0: executed; 1: not executed (cache capacity/resource/alignment fallback).
 * Only planner-approved ordinary instructions may reach this trusted seam. */
/* The usage mask is the planner's union of ALL body GPR reads/writes.
 * X18 is forbidden. X19 is supported when another whole-body-unused register
 * can carry this invocation's context; a negative selector means fallback. */
int native_blocks_context_register(uint32_t gpr_usage);
int native_blocks_run(Cpu *cpu, const uint32_t *words, size_t count, uint32_t gpr_usage);
typedef struct NativeBlocksCacheStats {
    uint64_t global_pages,global_page_limit,overflow_slots_per_cpu,overflow_cpu_records_per_thread;
    uint64_t overflow_live_pages,overflow_peak_pages,overflow_mappings,overflow_rewrites;
} NativeBlocksCacheStats;
/* Shared entries are immutable. Bounded per-Cpu/per-host-thread overflow slots
 * are private and rewritten only between invocations; no admitted host callout.
 * pthread exit releases idle overflow pages automatically. Explicit cleanup is
 * optional and must be called quiescently on their owning host thread. */
void native_blocks_cache_stats(NativeBlocksCacheStats *out);
int native_blocks_thread_cleanup(void);
void native_blocks_enter(void *frame);
extern const unsigned char native_blocks_exit_begin[], native_blocks_exit_end[];
#endif
