/* Shared host atomics for original guest instructions. No Unicorn fallback. */
#ifndef NATIVE_GAME_ATOMIC_STEP_H
#define NATIVE_GAME_ATOMIC_STEP_H
#include "runtime/aot_cpu.h"
typedef struct NgbAtomicMonitor {
    uint64_t address, value, high;
    uint32_t width, valid; /* byte width including both registers for pairs */
} NgbAtomicMonitor;
/* Monitor belongs to the actual executing Cpu/context. HLE/SVC and other
 * exceptional host transitions invalidate it. Value-CAS approximates the
 * existing CPU3 exclusive model; it does not detect same-value ABA writes. */
void ngb_atomic_monitor_clear(NgbAtomicMonitor *monitor);
/* 1 handled, PC/output registers changed; 0 not atomic, Cpu and monitor unchanged;
 * -1 recognized unsupported/invalid/misaligned, no partial architectural update.
 * Guest GPR31 is SP for an address, ZR for a source/destination/status as allowed.
 * Reservation and strong store-CAS width/address must match exactly.
 * Every store-exclusive/CAS/RMW uses one actual host atomic operation, including
 * failure (which must perform no write). No host lock is retained on return. */
int ngb_atomic_step(Cpu *cpu,uint32_t word,uint64_t pc,NgbAtomicMonitor *monitor);
#endif
