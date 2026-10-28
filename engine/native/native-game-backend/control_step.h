/* Original-PC software control flow for the native/Unicorn engine. */
#ifndef NATIVE_GAME_CONTROL_STEP_H
#define NATIVE_GAME_CONTROL_STEP_H
#include "runtime/aot_cpu.h"
/* 1: handled, only architectural outputs/PC changed. 0: unhandled/invalid,
 * entire Cpu unchanged. This function never dereferences guest memory or runs
 * host syscalls. Caller supplies the current 19.2MHz guest tick value.
 * FPCR/FPSR writes use the existing frozen CPU3 lifter's masks.
 * Branch register31 is reserved; it is never interpreted as SP or x[31]. */
int ngb_control_step(Cpu *cpu,uint32_t word,uint64_t pc,uint64_t ticks);
#endif
