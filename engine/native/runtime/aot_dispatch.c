/* Derived from LAS Recomp dispatch.c, Copyright (c) 2026 Peiikai.
 * MIT license: see LICENSE-LASRecomp in this directory. */
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
#include "aot_dispatch.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const ModuleAbiDescriptor *registry;
static const NativeManifest *native_registry;
static uint32_t capabilities;
static Cpu *live_cpus[AOT_MAX_CPUS];
static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t address; GuestFn function; } DispatchEntry;
static DispatchEntry overrides[AOT_MAX_DISPATCH_OVERRIDES];
static struct { uint64_t address; _Atomic(GuestFn) function; } hle[AOT_MAX_HLE_FUNCTIONS];
static unsigned override_count;
static _Atomic unsigned hle_count;
static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;
static GuestFn hle_guard;
static GuestFn execution_backend;
/* The native engine asks this seam at every instruction/block boundary. Keep
 * immutable entry indices separate from the default C-AOT dispatch/cache path.
 * Half-full open addressing guarantees an empty slot without allocating. */
#define HOST_INDEX_SIZE 8192u
_Static_assert(AOT_MAX_HLE_FUNCTIONS <= HOST_INDEX_SIZE / 2, "HLE index capacity");
_Static_assert(AOT_MAX_DISPATCH_OVERRIDES <= HOST_INDEX_SIZE / 2, "override index capacity");
static _Atomic unsigned hle_host_index[HOST_INDEX_SIZE];
static unsigned override_host_index[HOST_INDEX_SIZE];
static unsigned host_index_slot(uint64_t address) {
    address ^= address >> 33;
    address *= UINT64_C(0xff51afd7ed558ccd);
    address ^= address >> 33;
    address *= UINT64_C(0xc4ceb9fe1a85ec53);
    address ^= address >> 33;
    return (unsigned)address & (HOST_INDEX_SIZE - 1);
}
static void publish_hle_index(uint64_t address, unsigned index) {
    unsigned slot = host_index_slot(address);
    /* Called under hle_lock, only for a newly appended immutable address. */
    for (unsigned n = 0; n < HOST_INDEX_SIZE; ++n, slot = (slot + 1) & (HOST_INDEX_SIZE - 1))
        if (!atomic_load_explicit(&hle_host_index[slot], memory_order_relaxed)) {
            atomic_store_explicit(&hle_host_index[slot], index + 1, memory_order_release);
            return;
        }
    abort(); /* unreachable with the enforced half-full registry capacity */
}
static void publish_override_index(uint64_t address, unsigned index) {
    unsigned slot = host_index_slot(address);
    /* Overrides retain their existing all-guest-execution-quiescent contract. */
    for (unsigned n = 0; n < HOST_INDEX_SIZE; ++n, slot = (slot + 1) & (HOST_INDEX_SIZE - 1))
        if (!override_host_index[slot]) {
            override_host_index[slot] = index + 1;
            return;
        }
    abort();
}

enum AotStatus aot_dispatch_bind(const ModuleAbiDescriptor *descriptor,
    const ModuleAbiProfile *expected, uint32_t artifact_caps, uint32_t required_caps,
    ModuleAbiFailure *failure) {
    if (aot_dispatch_is_bound()) return AOT_ALREADY_BOUND;
    if (module_abi_validate(descriptor, expected, failure) != MODULE_ABI_OK) return AOT_ABI_MISMATCH;
    if (descriptor->identity.cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        descriptor->identity.cpu_size != sizeof(Cpu) || descriptor->identity.cpu_alignment != _Alignof(Cpu))
        return AOT_CPU_MISMATCH;
    if ((artifact_caps & ~AOT_CAP_ALL) || (required_caps & ~AOT_CAP_ALL) || artifact_caps != required_caps)
        return AOT_CAPABILITY_MISMATCH;
    capabilities = artifact_caps;
    registry = descriptor;
    return AOT_OK;
}

enum AotStatus aot_dispatch_bind_native(const NativeManifest *manifest,
    const uint8_t expected_contract[32], uint32_t caps, NativeManifestFailure *failure) {
    if (aot_dispatch_is_bound()) return AOT_ALREADY_BOUND;
    if (native_manifest_validate(manifest, expected_contract, failure) != NATIVE_MANIFEST_OK)
        return AOT_ABI_MISMATCH;
    if (caps & ~AOT_CAP_ALL) return AOT_CAPABILITY_MISMATCH;
    capabilities = caps;
    native_registry = manifest;
    return AOT_OK;
}

uint32_t aot_dispatch_capabilities(void) { return capabilities; }
int aot_dispatch_is_bound(void) { return registry != NULL || native_registry != NULL; }

GuestFn dispatch_lookup(uint64_t address) {
    if (!registry) return NULL;
    for (uint32_t m = 0; m < registry->module_count; ++m) {
        const ModuleAbiModule *module = &registry->modules[m];
        uint64_t lo = 0, hi = module->address_count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (module->addresses[mid] < address) lo = mid + 1;
            else hi = mid;
        }
        if (lo < module->address_count && module->addresses[lo] == address) return module->functions[lo];
    }
    return NULL;
}

int aot_dispatch_contains_text(uint64_t address) {
    if (native_registry) return native_manifest_is_code_address(native_registry, address);
    if (!registry || (address & 3)) return 0;
    for (uint32_t m = 0; m < registry->module_count; ++m) {
        const ModuleAbiModuleSpec *image = &registry->modules[m].image;
        uint64_t lo = image->canonical_base - registry->identity.guest_shift + image->text_offset;
        if (address >= lo && address - lo < image->text_size) return 1;
    }
    return 0;
}

unsigned aot_dispatch_cache_slot(uint64_t pc) { return (unsigned)((pc >> 4) ^ (pc >> 14)) & 4095u; }

void aot_cpu_initialize(Cpu *cpu) { if (cpu) memset(cpu, 0, sizeof *cpu); }

enum AotStatus aot_dispatch_attach_cpu(Cpu *cpu) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!cpu || ((uintptr_t)cpu & 15)) return AOT_ARGUMENT;
    pthread_mutex_lock(&cpu_lock);
    for (unsigned i = 0; i < AOT_MAX_CPUS; ++i) if (live_cpus[i] == cpu) {
        pthread_mutex_unlock(&cpu_lock);
        return AOT_OK;
    }
    for (unsigned i = 0; i < AOT_MAX_CPUS; ++i) if (!live_cpus[i]) {
        memset(cpu->dcache, 0, sizeof cpu->dcache);
        live_cpus[i] = cpu;
        pthread_mutex_unlock(&cpu_lock);
        return AOT_OK;
    }
    pthread_mutex_unlock(&cpu_lock);
    return AOT_CAPACITY;
}

enum AotStatus aot_dispatch_detach_cpu(Cpu *cpu) {
    if (!cpu) return AOT_ARGUMENT;
    pthread_mutex_lock(&cpu_lock);
    for (unsigned i = 0; i < AOT_MAX_CPUS; ++i) if (live_cpus[i] == cpu) {
        memset(cpu->dcache, 0, sizeof cpu->dcache);
        live_cpus[i] = NULL;
        pthread_mutex_unlock(&cpu_lock);
        return AOT_OK;
    }
    pthread_mutex_unlock(&cpu_lock);
    return AOT_NOT_FOUND;
}

static void invalidate_caches(void) {
    pthread_mutex_lock(&cpu_lock);
    for (unsigned i = 0; i < AOT_MAX_CPUS; ++i)
        if (live_cpus[i]) memset(live_cpus[i]->dcache, 0, sizeof live_cpus[i]->dcache);
    pthread_mutex_unlock(&cpu_lock);
}

enum AotStatus aot_dispatch_set_execution_backend(GuestFn backend) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!backend) return AOT_ARGUMENT;
    if (execution_backend) return AOT_ALREADY_BOUND;
    execution_backend=backend;
    invalidate_caches();
    return AOT_OK;
}
GuestFn aot_dispatch_execution_backend(void) { return execution_backend; }
size_t aot_dispatch_override_count(void) { return override_count; }
int aot_dispatch_override_address(size_t index, uint64_t *address) {
    if (!address || index >= override_count) return 0;
    *address=overrides[index].address;
    return 1;
}
GuestFn aot_dispatch_host_lookup(uint64_t address) {
    unsigned slot = host_index_slot(address);
    for (unsigned n = 0; n < HOST_INDEX_SIZE; ++n, slot = (slot + 1) & (HOST_INDEX_SIZE - 1)) {
        unsigned entry = atomic_load_explicit(&hle_host_index[slot], memory_order_acquire);
        if (!entry) break;
        if (hle[entry - 1].address == address)
            return atomic_load_explicit(&hle[entry - 1].function, memory_order_acquire);
    }
    slot = host_index_slot(address);
    for (unsigned n = 0; n < HOST_INDEX_SIZE; ++n, slot = (slot + 1) & (HOST_INDEX_SIZE - 1)) {
        unsigned entry = override_host_index[slot];
        if (!entry) break;
        if (overrides[entry - 1].address == address) return overrides[entry - 1].function;
    }
    return NULL;
}

enum AotStatus aot_dispatch_override(uint64_t address, GuestFn function) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!function || (!dispatch_lookup(address) && !(execution_backend && aot_dispatch_contains_text(address)))) return AOT_ARGUMENT;
    for (unsigned i = 0; i < override_count; ++i) if (overrides[i].address == address) {
        overrides[i].function = function;
        invalidate_caches();
        return AOT_OK;
    }
    if (override_count == AOT_MAX_DISPATCH_OVERRIDES) return AOT_CAPACITY;
    overrides[override_count] = (DispatchEntry){address, function};
    publish_override_index(address, override_count++);
    invalidate_caches();
    return AOT_OK;
}

enum AotStatus aot_dispatch_add_hle(uint64_t address, GuestFn function) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    if (!address || (address & 3) || !function) return AOT_ARGUMENT;
    if (native_registry) {
        for (uint32_t m = 0; m < native_registry->module_count; ++m) {
            const NativeModuleSpec *image = &native_registry->modules[m];
            uint64_t lo = image->canonical_base - native_registry->identity.guest_shift;
            if (address >= lo && address - lo < image->mapped_size) return AOT_ARGUMENT;
        }
    } else {
        for (uint32_t m = 0; m < registry->module_count; ++m) {
            const ModuleAbiModuleSpec *image = &registry->modules[m].image;
            uint64_t lo = image->canonical_base - registry->identity.guest_shift;
            if (address >= lo && address - lo < image->mapped_size) return AOT_ARGUMENT;
        }
    }
    pthread_mutex_lock(&hle_lock);
    unsigned count = atomic_load_explicit(&hle_count, memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) if (hle[i].address == address) {
        atomic_store_explicit(&hle[i].function, function, memory_order_release);
        pthread_mutex_unlock(&hle_lock);
        return AOT_OK;
    }
    if (count == AOT_MAX_HLE_FUNCTIONS) {
        pthread_mutex_unlock(&hle_lock);
        return AOT_CAPACITY;
    }
    hle[count].address = address;
    atomic_store_explicit(&hle[count].function, function, memory_order_relaxed);
    atomic_store_explicit(&hle_count, count + 1, memory_order_release);
    publish_hle_index(address, count);
    pthread_mutex_unlock(&hle_lock);
    return AOT_OK;
}

enum AotStatus aot_dispatch_set_hle_guard(GuestFn guard) {
    if (!aot_dispatch_is_bound()) return AOT_NOT_BOUND;
    hle_guard = guard;
    return AOT_OK;
}

__attribute__((noinline)) static GuestFn dispatch_slow(Cpu *cpu) {
    uint64_t pc = cpu->pc;
    unsigned count = atomic_load_explicit(&hle_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i)
        if (hle[i].address == pc) {
            if (hle_guard) hle_guard(cpu);
            return atomic_load_explicit(&hle[i].function, memory_order_acquire);
        }
    for (unsigned i = 0; i < override_count; ++i) if (overrides[i].address == pc) return overrides[i].function;
    GuestFn function = execution_backend && aot_dispatch_contains_text(pc) ? execution_backend : dispatch_lookup(pc);
    if (!function) abort(); /* no guest invocation on unresolved/unbound entry */
    unsigned slot = aot_dispatch_cache_slot(pc);
    cpu->dcache[slot].addr = pc;
    cpu->dcache[slot].fn = function;
    return function;
}

/* LAS's per-Cpu cache and mandatory tail-call chain, with no TLS cache lookup. */
__attribute__((noinline)) static void dispatch_miss(Cpu *cpu) {
    GuestFn function = dispatch_slow(cpu);
    __attribute__((musttail)) return function(cpu);
}
__attribute__((no_stack_protector)) void rt_dispatch(Cpu *cpu) {
    unsigned slot = aot_dispatch_cache_slot(cpu->pc);
    if (__builtin_expect(cpu->dcache[slot].addr == cpu->pc && cpu->dcache[slot].fn != NULL, 1))
        __attribute__((musttail)) return cpu->dcache[slot].fn(cpu);
    __attribute__((musttail)) return dispatch_miss(cpu);
}

void guest_call(Cpu *cpu, uint64_t address) {
    uint64_t lr = cpu->x[30];
    cpu->x[30] = 0;
    cpu->pc = address;
    rt_dispatch(cpu);
    cpu->x[30] = lr;
}

const char *aot_status_string(enum AotStatus status) {
    switch (status) {
    case AOT_OK: return "ok";
    case AOT_ARGUMENT: return "invalid argument";
    case AOT_ALREADY_BOUND: return "execution registry already bound";
    case AOT_NOT_BOUND: return "execution registry not bound";
    case AOT_ABI_MISMATCH: return "module ABI validation failed";
    case AOT_CPU_MISMATCH: return "shared Cpu identity required";
    case AOT_CAPABILITY_MISMATCH: return "execution capabilities mismatch";
    case AOT_CAPACITY: return "registry capacity exceeded";
    case AOT_NOT_FOUND: return "entry not found";
    default: return "unknown execution status";
    }
}
