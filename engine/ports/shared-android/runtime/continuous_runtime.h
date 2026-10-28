#pragma once
#include "runtime/aot_cpu.h"
int continuous_runtime_configure(void);
void continuous_runtime_run(Cpu *cpu);
void continuous_runtime_interrupt_all(void);
void continuous_runtime_finish_after_join(void);
