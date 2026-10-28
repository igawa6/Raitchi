/* Signal integration for the opt-in continuous native backend. */
#pragma once
#include <stdint.h>
#include "runtime/aot_cpu.h"
struct CnceContext;

typedef struct ContinuousSignalHooks {
    /* Async-signal-safe immutable lookups; never allocate, lock or enter guest.
     * is_guest_pc must exclude patch islands and context transition assembly. */
    int (*is_guest_pc)(uint64_t pc);
    int (*original_pc)(uint64_t native_pc, uint64_t *original_pc);
} ContinuousSignalHooks;
enum ContinuousSignalReason {
    CONTINUOUS_SIGNAL_STOP = 1u,
    CONTINUOUS_SIGNAL_PAUSE = 2u
};
typedef struct ContinuousSignalStats {
    uint64_t registered_threads, interrupt_requests, interrupted_guests;
    uint64_t deferred_interrupts, chained_faults, resumed_faults;
} ContinuousSignalStats;

int continuous_signals_configure(const ContinuousSignalHooks *hooks);
/* Only the first thread calls production_install, which registers process-wide
 * C handlers. Later threads install their guarded altstack without resetting
 * those global assembly fronts while another guest is executing. */
int continuous_signals_install_thread(void (*production_install)(void));
/* Sends SIGURG to the actual registered Cpu's host pthread. Retry while stopping
 * until acknowledged: signals in entry/exit islands are deliberately deferred.
 * A host-side blocked callback retains pending reasons until a safe boundary. */
int continuous_signals_interrupt_cpu(Cpu *cpu, uint32_t reasons);
uint32_t continuous_signals_pending(Cpu *cpu);
/* Host-side consume after native run returns, or before reentering guest. */
uint32_t continuous_signals_take_reason(Cpu *cpu);
void continuous_signals_stats(ContinuousSignalStats *out);
/* Called ONLY by assembly after restoring host TLS/X18 on the signal altstack.
 * 1: resume private guest TLS; 0: retain host TLS for host-return/host signal. */
int continuous_signals_dispatch(int signal, void *info, void *ucontext,
                                struct CnceContext *guest);
