/* Existing runtime instruction hooks at original native/interpreted PCs. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "runtime/aot_cpu.h"

/* Selected LAS compatibility replacements run before ordinary rt_hook callbacks.
 * 0: execute original word; 1: skip four bytes; 2: cpu->pc is the target.
 * Hook callbacks execute on the host stack, on the actual current Cpu.
 * Registration remains quiescent under the core hook-registry contract. */
int native_hooks_apply(Cpu *cpu, uint64_t actual_pc);
/* Limits a native prefix BEFORE its first registered instruction-hook site.
 * A site at actual_pc yields zero; its original word may be interpreted after
 * native_hooks_apply returns zero. Cpu is accepted for NativeEngineHooks ABI. */
size_t native_hooks_limit(Cpu *cpu, uint64_t actual_pc, size_t max_words);
