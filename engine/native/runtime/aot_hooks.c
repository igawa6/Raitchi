/*
 * Derived from LAS Recomp runtime/cpu.h and dispatch.c (MIT).
 * Copyright (c) 2026 Peiikai
 * Function-entry hook protocol derived from Let's Go Recomp (MIT).
 * Copyright (c) 2026 raulzito12345rrs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "aot_hooks.h"
#include <stdlib.h>

volatile int g_fnhook;
volatile int g_fnprof;
typedef struct { uint64_t address; AotFunctionHook function; } FunctionHookEntry;
typedef struct { uint64_t site; AotInstructionHook function; } InstructionHookEntry;
static FunctionHookEntry hooks[AOT_MAX_FUNCTION_HOOKS];
static InstructionHookEntry instruction_hooks[AOT_MAX_INSTRUCTION_HOOKS];
static unsigned instruction_count;
static AotProfiler profiler;
static _Thread_local struct { uint64_t address; Cpu *cpu; int armed; } bypass;

enum AotStatus rt_register_fnhook(uint64_t address, AotFunctionHook hook) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!(aot_dispatch_capabilities() & AOT_CAP_FUNCTION_HOOKS)) return AOT_CAPABILITY_MISMATCH;
    if (!address || !hook || !dispatch_lookup(address)) return AOT_ARGUMENT;
    unsigned slot = (unsigned)((address >> 2) * 0x9E3779B1u) & (AOT_MAX_FUNCTION_HOOKS - 1);
    for (unsigned n = 0; n < AOT_MAX_FUNCTION_HOOKS; ++n, slot = (slot + 1) & (AOT_MAX_FUNCTION_HOOKS - 1)) {
        if (!hooks[slot].address || hooks[slot].address == address) {
            hooks[slot] = (FunctionHookEntry){address, hook};
            g_fnhook = 1;
            return AOT_OK;
        }
    }
    return AOT_CAPACITY;
}

int rt_fnhook(Cpu *cpu, uint64_t address) {
    if (bypass.armed && bypass.address == address && bypass.cpu == cpu) {
        bypass.armed = 0;
        return 0;
    }
    unsigned slot = (unsigned)((address >> 2) * 0x9E3779B1u) & (AOT_MAX_FUNCTION_HOOKS - 1);
    for (unsigned n = 0; n < AOT_MAX_FUNCTION_HOOKS && hooks[slot].address;
         ++n, slot = (slot + 1) & (AOT_MAX_FUNCTION_HOOKS - 1))
        if (hooks[slot].address == address) return hooks[slot].function(cpu, address);
    return 0;
}

int aot_original_call_take(Cpu *cpu, uint64_t address) {
    if (!bypass.armed || bypass.address!=address || bypass.cpu!=cpu) return 0;
    bypass.armed=0;
    return 1;
}

void aot_native_call_original(Cpu *cpu, uint64_t address) {
    GuestFn backend=aot_dispatch_execution_backend();
    if(!cpu || !backend || !aot_dispatch_contains_text(address)) abort();
    uint64_t saved_address=bypass.address,saved_lr=cpu->x[30];
    Cpu *saved_cpu=bypass.cpu;
    int saved_armed=bypass.armed;
    bypass.address=address;bypass.cpu=cpu;bypass.armed=1;
    cpu->pc=address;cpu->x[30]=0;
    backend(cpu);
    cpu->x[30]=saved_lr;
    bypass.address=saved_address;bypass.cpu=saved_cpu;bypass.armed=saved_armed;
}

void rt_call_original(Cpu *cpu, uint64_t address) {
    if (!cpu || !(aot_dispatch_capabilities() & AOT_CAP_FUNCTION_HOOKS)) abort();
    if(aot_dispatch_execution_backend()) { aot_native_call_original(cpu,address);return; }
    GuestFn function=dispatch_lookup(address);
    if(!function) abort();
    uint64_t saved_address=bypass.address;
    Cpu *saved_cpu=bypass.cpu;
    int saved_armed=bypass.armed;
    bypass.address=address;bypass.cpu=cpu;bypass.armed=1;
    function(cpu);
    bypass.address=saved_address;bypass.cpu=saved_cpu;bypass.armed=saved_armed;
}

enum AotStatus rt_register_instruction_hook(uint64_t site, AotInstructionHook hook) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!(aot_dispatch_capabilities() & AOT_CAP_INSTRUCTION_HOOKS)) return AOT_CAPABILITY_MISMATCH;
    if (!hook || !aot_dispatch_contains_text(site)) return AOT_ARGUMENT;
    for (unsigned i = 0; i < instruction_count; ++i) if (instruction_hooks[i].site == site) {
        instruction_hooks[i].function = hook;
        return AOT_OK;
    }
    if (instruction_count == AOT_MAX_INSTRUCTION_HOOKS) return AOT_CAPACITY;
    instruction_hooks[instruction_count++] = (InstructionHookEntry){site, hook};
    return AOT_OK;
}

int aot_instruction_hook_registered(uint64_t site) {
    for(unsigned i=0;i<instruction_count;i++) if(instruction_hooks[i].site==site) return 1;
    return 0;
}
size_t aot_instruction_hook_count(void) { return instruction_count; }
int aot_instruction_hook_address(size_t index,uint64_t *address) {
    if(!address || index>=instruction_count)return 0;
    *address=instruction_hooks[index].site;
    return 1;
}
size_t aot_instruction_hook_limit(uint64_t pc,size_t max_words) {
    if(pc&3) return 0;
    size_t result=max_words;
    for(unsigned i=0;i<instruction_count;i++) {
        uint64_t site=instruction_hooks[i].site;
        if(site>=pc && (site-pc)/4<result) result=(size_t)((site-pc)/4);
    }
    return result;
}

int rt_hook(Cpu *cpu, uint64_t site) {
    for (unsigned i = 0; i < instruction_count; ++i)
        if (instruction_hooks[i].site == site) return instruction_hooks[i].function(cpu, site);
    return 0;
}

enum AotStatus aot_set_profiler(AotProfiler callback) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!(aot_dispatch_capabilities() & AOT_CAP_PROFILING)) return AOT_CAPABILITY_MISMATCH;
    profiler = callback;
    g_fnprof = callback != NULL;
    return AOT_OK;
}
void fnprof_hit(uint64_t address, uint64_t lr, uint64_t x0) {
    if (profiler) profiler(address, lr, x0);
}
