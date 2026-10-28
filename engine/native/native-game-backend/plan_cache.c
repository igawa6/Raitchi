#include "plan_cache.h"
#include <string.h>

_Static_assert((NGB_PLAN_CACHE_ENTRIES & (NGB_PLAN_CACHE_ENTRIES - 1)) == 0, "power-of-two cache");
_Static_assert(sizeof(NgbPlanCache) <= 256u * 1024u, "bounded per-Cpu metadata");

enum NgbStatus ngb_plan_cache_get(NgbPlanCache *cache, NgbDecoder *decoder, const void *code,
    size_t bytes, uint64_t pc, unsigned limit, NgbBlockPlan *out, NgbInstruction *first) {
    if (!cache || !decoder || !decoder->handle || !decoder->instruction || !code || !out || !first ||
        bytes < 4 || bytes > NGB_MAX_NATIVE_WORDS * 4u || (bytes & 3) || (pc & 3) ||
        pc > UINT64_MAX - bytes || limit > NGB_MAX_NATIVE_WORDS ||
        out->struct_size != sizeof *out || out->version != NGB_PLAN_VERSION) return NGB_ARGUMENT;
    uint64_t hash = (pc >> 2) ^ (pc >> 14) ^ ((uint64_t)bytes << 4) ^ ((uint64_t)limit * UINT64_C(0x9e3779b1));
    NgbPlanCacheEntry *entry = &cache->entries[hash & (NGB_PLAN_CACHE_ENTRIES - 1)];
    int same_key = entry->valid && entry->pc == pc && entry->bytes == bytes && entry->limit == limit;
    if (same_key && !memcmp(entry->original, code, bytes)) {
        ++cache->hits;
        *out = entry->plan;
        *first = entry->first;
        return NGB_OK;
    }
    ++cache->misses;
    if (same_key) ++cache->invalidations;
    else if (entry->valid) ++cache->replacements;
    entry->valid = 0;
    uint32_t snapshot[NGB_MAX_NATIVE_WORDS];
    memcpy(snapshot, code, bytes);
    NgbBlockPlan plan = {.struct_size = sizeof plan, .version = NGB_PLAN_VERSION,
                         .start_pc = pc, .next_pc = pc};
    NgbInstruction initial = {0};
    enum NgbStatus status;
    if (limit) {
        status = ngb_plan_block_context(decoder, snapshot, bytes, pc, limit, &plan);
        if (status == NGB_OK) {
            if (!plan.native_count && plan.exit.pc == pc) initial = plan.exit;
            else status = ngb_classify(decoder, snapshot, pc, &initial);
        }
    } else {
        status = ngb_classify(decoder, snapshot, pc, &initial);
        plan.exit = initial;
    }
    *out = plan;
    *first = initial;
    if (status != NGB_OK) return status;
    entry->pc = pc; entry->bytes = (uint32_t)bytes; entry->limit = limit;
    memcpy(entry->original, snapshot, bytes);
    entry->plan = plan;
    entry->first = initial;
    entry->valid = 1;
    return NGB_OK;
}
