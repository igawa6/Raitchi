#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native-game-backend/plan_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned assertions;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "plan cache line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
static NgbBlockPlan fetch(NgbPlanCache *cache, NgbDecoder *decoder, const void *code,
                          size_t bytes, uint64_t pc, unsigned limit, NgbInstruction *first) {
    NgbBlockPlan plan = {.struct_size = sizeof plan, .version = NGB_PLAN_VERSION};
    CHECK(ngb_plan_cache_get(cache, decoder, code, bytes, pc, limit, &plan, first) == NGB_OK);
    CHECK(first->pc == pc && first->word == *(const uint32_t *)code);
    return plan;
}
static void compare_uncached(NgbDecoder *decoder, const void *code, size_t bytes,
                             uint64_t pc, unsigned limit, const NgbBlockPlan *cached, const NgbInstruction *first) {
    NgbBlockPlan direct = {.struct_size = sizeof direct, .version = NGB_PLAN_VERSION};
    NgbInstruction initial;
    CHECK(ngb_classify(decoder, code, pc, &initial) == NGB_OK);
    CHECK(!memcmp(&initial, first, sizeof initial));
    if (limit) {
        CHECK(ngb_plan_block_context(decoder, code, bytes, pc, limit, &direct) == NGB_OK);
        CHECK(!memcmp(&direct, cached, sizeof direct));
    } else CHECK(!cached->native_count && cached->start_pc == pc && cached->next_pc == pc);
}
int main(void) {
    NgbDecoder decoder = {0};
    CHECK(ngb_decoder_init(&decoder) == NGB_OK);
    NgbPlanCache *cache = calloc(1, sizeof *cache), *other = calloc(1, sizeof *other);
    CHECK(cache && other && sizeof *cache <= 256u * 1024u);
    uint32_t code[] = {0x91000400u, 0xaa0003f3u, 0xd503201fu, 0xd65f03c0u};
    NgbInstruction first;
    NgbBlockPlan plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(plan.native_count == 3 && plan.next_pc == 0x100c && plan.exit.kind == NGB_EXIT_RETURN);
    CHECK(plan.gpr_usage & (UINT32_C(1) << 19));
    compare_uncached(&decoder, code, sizeof code, 0x1000, 64, &plan, &first);
    for (unsigned i = 0; i < 10000; ++i) {
        NgbInstruction initial;
        NgbBlockPlan warm = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &initial);
        CHECK(!memcmp(&warm, &plan, sizeof warm) && !memcmp(&initial, &first, sizeof initial));
    }
    CHECK(cache->misses == 1 && cache->hits == 10000 && !cache->invalidations);

    const uint32_t replacements[] = {0xaa0003f2u, 0x14000001u, 0xc85f7c20u};
    const enum NgbExitKind kinds[] = {NGB_EXIT_RESERVED_REGISTER, NGB_EXIT_BRANCH, NGB_EXIT_ATOMIC};
    for (unsigned i = 0; i < 3; ++i) {
        code[0] = replacements[i];
        uint64_t misses = cache->misses;
        plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
        CHECK(cache->misses == misses + 1 && plan.native_count == 0 && plan.exit.kind == kinds[i]);
        compare_uncached(&decoder, code, sizeof code, 0x1000, 64, &plan, &first);
        uint64_t hits = cache->hits;
        plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
        CHECK(cache->hits == hits + 1 && first.kind == kinds[i]);
    }
    CHECK(cache->invalidations == 3);
    code[0] = 0x91000400u;
    code[1] = 0xaa0003f2u;
    plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(plan.native_count == 1 && plan.exit.kind == NGB_EXIT_RESERVED_REGISTER && plan.exit.pc == 0x1004);
    CHECK(first.pc == 0x1000 && first.kind == NGB_EXIT_LIMIT); /* NOT_ENTERED fallback is FIRST, not exit. */
    code[1] = 0xaa0003f3u;
    code[3] = 0xd4000201u;
    plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(plan.native_count == 3 && plan.exit.kind == NGB_EXIT_SVC && plan.exit.word == code[3]);
    code[3] = 0xc85f7c20u;
    plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(plan.native_count == 3 && plan.exit.kind == NGB_EXIT_ATOMIC && plan.exit.word == code[3]);
    CHECK(plan.exit.flags & NGB_EXCLUSIVE_LOAD);

    /* A live hook can shorten the native prefix, including a hook that returns
     * 0 and still requires the original instruction. Its limit is never cached. */
    for (unsigned limit = 0; limit <= 4; ++limit) {
        size_t bytes = limit ? limit * 4 : 4;
        plan = fetch(cache, &decoder, code, bytes, 0x1000, limit, &first);
        CHECK(plan.native_count <= limit);
        compare_uncached(&decoder, code, bytes, 0x1000, limit, &plan, &first);
        uint64_t hits = cache->hits;
        (void)fetch(cache, &decoder, code, bytes, 0x1000, limit, &first);
        CHECK(cache->hits == hits + 1);
    }
    plan = fetch(cache, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(plan.native_count == 3); /* Growing the limit restores the full admitted prefix. */

    /* Validate every supplied input byte, even beyond an early branch exit. */
    code[0] = 0x14000001u;
    (void)fetch(cache, &decoder, code, sizeof code, 0x2000, 64, &first);
    uint64_t invalidations = cache->invalidations;
    code[3] ^= 32;
    plan = fetch(cache, &decoder, code, sizeof code, 0x2000, 64, &first);
    CHECK(cache->invalidations == invalidations + 1 && !plan.native_count);

    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    unsigned char *guarded = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guarded != MAP_FAILED && !mprotect(guarded + page, (size_t)page, PROT_NONE));
    uint32_t *last_word = (uint32_t *)(guarded + page - 4);
    *last_word = 0xd65f03c0u;
    for (unsigned i = 0; i < 10; ++i) {
        plan = fetch(cache, &decoder, last_word, 4, (uintptr_t)last_word, 64, &first);
        CHECK(!plan.native_count && plan.exit.kind == NGB_EXIT_RETURN);
    }
    *last_word = 0x91000400u;
    plan = fetch(cache, &decoder, last_word, 4, (uintptr_t)last_word, 64, &first);
    CHECK(plan.native_count == 1 && plan.exit.kind == NGB_EXIT_INPUT_END);
    CHECK(!munmap(guarded, (size_t)page * 2));

    /* Independent per-Cpu objects, then guaranteed direct-map replacement via
     * more distinct keys than the fixed entry count. No unbounded growth. */
    code[0] = 0x91000400u;
    (void)fetch(other, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(other->misses == 1 && !other->hits);
    for (unsigned i = 0; i <= NGB_PLAN_CACHE_ENTRIES * 2; ++i)
        (void)fetch(cache, &decoder, code, sizeof code, 0x10000 + i * 4, 64, &first);
    CHECK(cache->replacements > 0 && other->misses == 1 && !other->hits);
    (void)fetch(other, &decoder, code, sizeof code, 0x1000, 64, &first);
    CHECK(other->misses == 1 && other->hits == 1);
    NgbBlockPlan invalid = {.struct_size = sizeof invalid, .version = NGB_PLAN_VERSION};
    CHECK(ngb_plan_cache_get(cache, &decoder, code, 0, 0x1000, 64, &invalid, &first) == NGB_ARGUMENT);
    CHECK(ngb_plan_cache_get(cache, &decoder, code, 4, 0x1000, 65, &invalid, &first) == NGB_ARGUMENT);
    CHECK(ngb_plan_cache_get(cache, &decoder, code, 4, UINT64_MAX - 3, 1, &invalid, &first) == NGB_ARGUMENT);
    ngb_decoder_destroy(&decoder); free(cache); free(other);
    printf("native plan cache assertions=%u; actual Capstone; no guest execution\n", assertions);
    return 0;
}
