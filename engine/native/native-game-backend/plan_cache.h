/* Bounded per-Cpu decode cache. Admission/execution policy remains the planner's. */
#ifndef NATIVE_GAME_PLAN_CACHE_H
#define NATIVE_GAME_PLAN_CACHE_H
#include "block_plan.h"

#define NGB_PLAN_CACHE_ENTRIES 256u
typedef struct NgbPlanCacheEntry {
    uint64_t pc;
    uint32_t bytes, limit, valid;
    uint8_t original[NGB_MAX_NATIVE_WORDS * 4u];
    NgbBlockPlan plan;
    NgbInstruction first;
} NgbPlanCacheEntry;
typedef struct NgbPlanCache {
    uint64_t hits, misses, invalidations, replacements;
    NgbPlanCacheEntry entries[NGB_PLAN_CACHE_ENTRIES];
} NgbPlanCache;

/* Zero-initialize once; one executing Cpu owns the cache, including its nested
 * calls. Every hit validates ALL supplied original bytes (4..256, aligned size).
 * Caller must still run host lookup, instruction hooks and the live native
 * limit before lookup. limit0 returns first-instruction-only metadata, never a
 * native prefix. Neither host callback pointers nor admission failures are
 * cached. first is independently correct at pc even when a native prefix later
 * receives a NOT_ENTERED response from the native executor. */
enum NgbStatus ngb_plan_cache_get(NgbPlanCache *, NgbDecoder *, const void *code,
    size_t bytes, uint64_t pc, unsigned limit, NgbBlockPlan *, NgbInstruction *first);
#endif
