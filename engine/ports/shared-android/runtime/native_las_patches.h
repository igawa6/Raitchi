/* LAS selected-version compatibility for original-code execution only. */
#pragma once
#include "runtime/aot_cpu.h"
/* Configure while quiescent, before original text is patched/executable.
 * No-op for C-AOT and other titles. Validate every selected opcode/target before
 * publishing callbacks; no title addresses or generated functions are embedded. */
int native_las_patches_register(void);
/* -1 unmatched; otherwise the native instruction-hook0/1/2 convention.
 * Helpers preserve Cpu state except architectural outputs of each replacement. */
int native_las_patches_apply(Cpu *cpu,uint64_t pc);
