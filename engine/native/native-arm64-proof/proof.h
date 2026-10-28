/* Asset-free Android native execution experiment. No game/backend ABI. */
#ifndef NAP_PROOF_H
#define NAP_PROOF_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>

typedef uint64_t (*NapCode)(void);
typedef struct __attribute__((aligned(16))) NapContext {
    uint64_t guest_tls, guest_x18;
    NapCode read_code, write_code;
    uint64_t host_tls, host_x18, nested_result, callbacks;
    uint64_t flags, live_reg, v0[2], v31[2];
    uint64_t initial_fpcr, initial_fpsr, final_fpcr, final_fpsr;
    _Atomic uint32_t ready, release;
    volatile sig_atomic_t signal_seen, signal_ok;
    uint64_t thread_failures, scalar_retries, pair_retries;
} NapContext;

/* Whitelist only: MRS X0..X17,TPIDR_EL0; MOV from/to virtual X18 with
 * X0..X17; exact ADD X0,X9,X10; final RET. X19 is dedicated context.
 * All-or-nothing: output remains untouched on rejection. Max 32 words. */
int nap_rewrite(const uint32_t *input, size_t count, uint32_t *output);
uint64_t nap_call(NapContext *, NapCode, uint64_t);
void nap_live(NapContext *, void (*)(NapContext *));
void nap_signal_wait(NapContext *);
uint64_t nap_atomic_increment(uint64_t *);
uint64_t nap_pair_increment(uint64_t *);
extern const unsigned char nap_signal_begin[], nap_signal_end[];
__attribute__((visibility("default"))) int native_arm64_proof_run(char *, size_t);
#endif
