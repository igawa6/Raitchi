#include "bridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int corrupt;
static void unit_svc(Cpu *cpu, uint32_t immediate, uint64_t pc) {
    assert(immediate == 0x1e && pc == 123);
    cpu->x[0] = 87654321;
    if (corrupt == 1) cpu->v[13] ^= (__uint128_t)1 << 99;
    if (corrupt == 2) ((unsigned char *)cpu)[sizeof(Cpu)-1] ^= 1;
}
int main(void) {
    uint32_t out = 0xdeadbeef, svc = 0xd4000001u | (0x1eu << 5); unsigned assertions = 0;
    for (int64_t delta = -0x8000000; delta <= 0x7fffffc; delta += 0x100000) {
        uintptr_t from = 0x10000000, target = (uintptr_t)((int64_t)from + delta);
        assert(!nr_rewrite_svc(svc, from, target, &out));
        int64_t encoded = out & 0x03ffffff; if (encoded & 0x02000000) encoded -= 0x04000000;
        assert(encoded * 4 == delta); assertions += 2;
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        out = 0xdeadbeef; assert(nr_rewrite_svc(svc ^ (1u << bit), 0x10000000, 0x10000004, &out));
        assert(out == 0xdeadbeef); assertions += 2;
    }
    out = 0xdeadbeef;
    assert(nr_rewrite_svc(svc, 0x10000000, 0x18000000, &out)); ++assertions;
    assert(nr_rewrite_svc(svc, 0x10000004, 0x08000000, &out)); ++assertions;
    assert(nr_rewrite_svc(svc, 3, 4, &out)); ++assertions;
    assert(nr_rewrite_svc(svc, 4, 5, &out)); ++assertions;
    assert(nr_rewrite_svc(svc, 4, 8, NULL)); ++assertions;
    assert(!nr_rewrite_svc(svc, 0x10000000, 0x17fffffc, &out)); ++assertions;
    assert((out & 0x03ffffff) == 0x01ffffff); ++assertions;
    Cpu *objects = aligned_alloc(_Alignof(Cpu), 4 * sizeof(Cpu)); assert(objects);
    NrContext ctx = {.cpu=&objects[0],.seed=&objects[1],.before=&objects[2],.observed=&objects[3],
        .svc=unit_svc,.pc_cookie=123,.expected_sp=0x2000,.host_x18=456};
    NrFrame frame = {0};
    memset(ctx.seed, 0x5a, sizeof(Cpu)); ctx.seed->thread = NULL; ctx.seed->sp = ctx.expected_sp;
    for (unsigned i = 0; i < 31; ++i) frame.x[i] = ctx.seed->x[i];
    frame.x[18] = ctx.host_x18; frame.x[19] = (uintptr_t)&ctx;
    frame.sp=ctx.expected_sp;frame.nzcv=ctx.seed->nzcv;frame.fpcr=ctx.seed->fpcr;frame.fpsr=ctx.seed->fpsr;
    memcpy(frame.q,ctx.seed->v,sizeof frame.q);memcpy(ctx.cpu,ctx.seed,sizeof(Cpu));
    nr_call_production(&ctx,&frame);
    assert(!ctx.materialization_failures && !ctx.production_mutations); ++assertions;
    assert(frame.x[0]==87654321 && ctx.cpu->x[0]==87654321); ++assertions;
    frame.consumed=frame.x[0]^1u;nr_observe(&ctx,&frame);assert(!ctx.resume_failures);++assertions;
    assert(frame.x[18]==ctx.host_x18 && frame.x[19]==(uintptr_t)&ctx);++assertions;
    assert(!memcmp((char*)ctx.cpu+8,(char*)ctx.before+8,sizeof(Cpu)-8));++assertions;
    /* A mutation in an upper SIMD lane must fail, while the safe saved
     * continuation and actual changed X0 are retained for cleanup. */
    memcpy(ctx.cpu,ctx.seed,sizeof(Cpu));frame.x[0]=ctx.seed->x[0];corrupt=1;nr_call_production(&ctx,&frame);
    assert(!ctx.materialization_failures);++assertions;
    assert(ctx.production_mutations==1);++assertions;
    assert(frame.x[0]==87654321 && frame.q[13]==ctx.before->v[13]);++assertions;
    frame.consumed=frame.x[0]^1u;nr_observe(&ctx,&frame);assert(ctx.resume_failures==1);++assertions;
    memcpy(ctx.cpu,ctx.seed,sizeof(Cpu));frame.x[0]=ctx.seed->x[0];corrupt=2;nr_call_production(&ctx,&frame);
    assert(!ctx.materialization_failures && ctx.production_mutations==2);++assertions;
    frame.consumed=frame.x[0]^1u;nr_observe(&ctx,&frame);assert(ctx.resume_failures==1);++assertions;
    /* Even if every other field matches, restoring old X0 must be detected. */
    frame.x[0]=ctx.seed->x[0];frame.consumed=frame.x[0]^1u;nr_observe(&ctx,&frame);
    assert(ctx.resume_failures==2);++assertions;
    free(objects);
    printf("native/runtime marshaling and SVC policy assertions=%u\n",assertions);
    return 0;
}
