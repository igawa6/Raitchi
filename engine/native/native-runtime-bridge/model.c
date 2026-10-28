#include "bridge.h"

int nr_rewrite_svc(uint32_t source, uintptr_t from, uintptr_t target, uint32_t *output) {
    /* Synthetic SVC SystemTick only; production guest bytes never enter here. */
    if (!output || source != (0xd4000001u | (0x1eu << 5)) || (from & 3) || (target & 3)) return -1;
    int64_t distance;
    if (target >= from) {
        if (target - from >= (1u << 27)) return -1;
        distance = (int64_t)(target - from);
    } else {
        if (from - target > (1u << 27)) return -1;
        distance = -(int64_t)(from - target);
    }
    *output = 0x14000000u | ((uint32_t)(distance / 4) & 0x03ffffffu);
    return 0;
}

static void materialize(Cpu *cpu, const NrFrame *frame) {
    for (unsigned i = 0; i < 31; ++i) if (i != 18 && i != 19) cpu->x[i] = frame->x[i];
    cpu->sp = frame->sp;
    cpu->nzcv = frame->nzcv;
    cpu->fpcr = frame->fpcr;
    cpu->fpsr = frame->fpsr;
    memcpy(cpu->v, frame->q, sizeof cpu->v);
}
static void restore(NrFrame *frame, const Cpu *cpu) {
    for (unsigned i = 0; i < 31; ++i) if (i != 18 && i != 19) frame->x[i] = cpu->x[i];
    frame->sp = cpu->sp;
    frame->nzcv = cpu->nzcv;
    frame->fpcr = cpu->fpcr;
    frame->fpsr = cpu->fpsr;
    memcpy(frame->q, cpu->v, sizeof frame->q);
}
void nr_call_production(NrContext *ctx, NrFrame *frame) {
    materialize(ctx->cpu, frame);
    ctx->seed->sp = ctx->expected_sp;
    if (memcmp(ctx->cpu, ctx->seed, sizeof(Cpu)) || frame->sp != ctx->expected_sp || (frame->sp & 15) ||
        frame->x[18] != ctx->host_x18 || frame->x[19] != (uintptr_t)ctx) ++ctx->materialization_failures;
    memcpy(ctx->before, ctx->cpu, sizeof(Cpu));
    ctx->svc(ctx->cpu, 0x1e, ctx->pc_cookie); /* actual dlsym rt_svc in device run */
    int mutated = memcmp((unsigned char *)ctx->cpu + sizeof(uint64_t),
                         (const unsigned char *)ctx->before + sizeof(uint64_t), sizeof(Cpu) - sizeof(uint64_t)) != 0;
    ctx->production_mutations += mutated;
    /* Unexpected metadata writes fail the proof. Use the saved continuation
     * for safe cleanup rather than following a corrupted SP/LR. X0 remains the
     * actual production result; no pre-call-X0 restoration can hide a failure. */
    restore(frame, mutated ? ctx->before : ctx->cpu);
    frame->x[0] = ctx->cpu->x[0];
}
void nr_observe(NrContext *ctx, NrFrame *frame) {
    memcpy(ctx->observed, ctx->cpu, sizeof(Cpu));
    materialize(ctx->observed, frame);
    ctx->consumed = frame->consumed;
    if (memcmp(ctx->observed, ctx->cpu, sizeof(Cpu)) || frame->x[18] != ctx->host_x18 ||
        frame->x[19] != (uintptr_t)ctx || frame->consumed != (ctx->cpu->x[0] ^ 1u)) ++ctx->resume_failures;
}
