/* Isolated synthetic native caller / real production CPU3 SVC experiment. */
#ifndef NATIVE_RUNTIME_BRIDGE_H
#define NATIVE_RUNTIME_BRIDGE_H
#include "runtime/aot_cpu.h"

typedef void (*NrSvc)(Cpu *, uint32_t, uint64_t);
typedef struct __attribute__((aligned(16))) NrFrame {
    uint64_t x[31], sp, nzcv, fpcr, fpsr, consumed;
    __uint128_t q[32];
} NrFrame;
typedef struct NrContext {
    Cpu *cpu, *seed, *before, *observed;
    NrSvc svc;
    void *snippet;
    uint64_t pc_cookie, expected_sp, host_tls, host_x18, consumed;
    uint64_t materialization_failures, production_mutations, resume_failures;
} NrContext;

/* Physical X19 is this context, physical X18 remains Android's. Their guest
 * X-register images are virtual Cpu slots; other GP registers are live native.
 * Synthetic caller SP stays on a valid aligned host-backed test stack. */
void nr_enter(NrContext *);
void nr_gateway(void);
extern const unsigned char nr_resume[];
void nr_call_production(NrContext *, NrFrame *);
void nr_observe(NrContext *, NrFrame *);
int nr_rewrite_svc(uint32_t source, uintptr_t from, uintptr_t target, uint32_t *output);
__attribute__((visibility("default"))) int native_runtime_bridge_probe(const char *, char *, size_t);
#endif
