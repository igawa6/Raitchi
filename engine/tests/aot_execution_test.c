/* Asset-free behavioral tests; each named case runs in a fresh host process.
 * Build this runner and fixture with the SAME independent fixture macros.
 * Usage: aot_execution_test <fixture.so> <case>
 */
#include "runtime/aot_hooks.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef FIXTURE_MODULE_COUNT
#error "choose independent synthetic two- or four-module profile"
#endif
#define CANONICAL_BASE UINT64_C(0x5000000000)
#if FIXTURE_MODULE_COUNT == 2
#define SHIFT UINT64_C(0x2000000000)
#else
#define SHIFT UINT64_C(0)
#endif
#define BASE (CANONICAL_BASE - SHIFT)
#define ADDR(offset) (BASE + (offset))
#define CAPS ((uint32_t)AOT_PROFILE_CAPABILITIES)
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
static const ModuleAbiDescriptor *descriptor;
static ModuleAbiModuleSpec specs[FIXTURE_MODULE_COUNT];
static ModuleAbiProfile expected;

/* Built from the fixture producer specification, never copied from descriptor. */
static void make_expected(void) {
    for (unsigned i = 0; i < FIXTURE_MODULE_COUNT; ++i) {
        specs[i] = (ModuleAbiModuleSpec){.struct_size = sizeof(ModuleAbiModuleSpec), .module_id = i + 1,
            .canonical_base = CANONICAL_BASE + UINT64_C(0x100000) * i, .mapped_size = 0x40000,
            .text_offset = 0, .text_size = 0x20000, .function_count = i ? 1 : 7};
        specs[i].nso_build_id[0] = (uint8_t)(i + 1);
        specs[i].nso_sha256[0] = (uint8_t)(i + 17);
    }
    expected = (ModuleAbiProfile){.struct_size = sizeof(ModuleAbiProfile), .module_count = FIXTURE_MODULE_COUNT,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
            .cpu_size = sizeof(Cpu), .cpu_alignment = _Alignof(Cpu), .guest_shift = SHIFT,
            .guest_window_lo = BASE, .guest_window_hi = BASE + 0x1000000}, .modules = specs};
}
static void bind_registry(void) {
    ModuleAbiFailure failure;
    CHECK(aot_dispatch_bind(descriptor, &expected, CAPS, CAPS, &failure) == AOT_OK);
    CHECK(failure.status == MODULE_ABI_OK);
    CHECK(aot_dispatch_capabilities() == CAPS);
}
static void init_cpu(Cpu *cpu) {
    aot_cpu_initialize(cpu);
    CHECK(aot_dispatch_attach_cpu(cpu) == AOT_OK);
}
static void call(Cpu *cpu, uint64_t address) { cpu->pc = address; rt_dispatch(cpu); }
static int handle_a(Cpu *cpu, uint64_t address) { (void)address; cpu->x[0] += 1000; cpu->x[20]++; return 1; }
static int pass_b(Cpu *cpu, uint64_t address) { (void)address; cpu->x[21]++; return 0; }
static int handle_wrapper(Cpu *cpu, uint64_t address) { (void)address; cpu->x[22]++; return 1; }
static int original_a(Cpu *cpu, uint64_t address) {
    cpu->x[20]++; rt_call_original(cpu, address); cpu->x[0] += 100; return 1;
}
static int original_nested(Cpu *cpu, uint64_t address) { cpu->x[23]++; rt_call_original(cpu, address); return 1; }
static void override_function(Cpu *cpu) { cpu->x[0] += 5000; }
static void hle_function(Cpu *cpu) { cpu->x[0] += 7; cpu->x[24] = cpu->x[30]; }
static void hle_guard(Cpu *cpu) { ++cpu->x[25]; }
static void native_backend_fixture(Cpu *cpu) {
    cpu->x[0]+=10000;cpu->x[26]=cpu->pc;cpu->x[24]=cpu->x[30];
    if(aot_original_call_take(cpu,cpu->pc))cpu->x[27]++;
}
static void backend_case(void) {
    bind_registry();Cpu cpu;init_cpu(&cpu);
    CHECK(!aot_dispatch_execution_backend());
    CHECK(!aot_dispatch_host_lookup(ADDR(0x100)));
    call(&cpu,ADDR(0x100));CHECK(cpu.x[0]==1);
    CHECK(aot_dispatch_set_execution_backend(NULL)==AOT_ARGUMENT);
    CHECK(aot_dispatch_set_execution_backend(native_backend_fixture)==AOT_OK);
    CHECK(aot_dispatch_execution_backend()==native_backend_fixture);
    CHECK(aot_dispatch_set_execution_backend(native_backend_fixture)==AOT_ALREADY_BOUND);
    CHECK(!cpu.dcache[aot_dispatch_cache_slot(ADDR(0x100))].fn);
    call(&cpu,ADDR(0x100));CHECK(cpu.x[0]==10001 && cpu.x[10]==1);
    cpu.x[30]=0x1234;guest_call(&cpu,ADDR(0x104));
    CHECK(cpu.x[0]==20001 && cpu.x[26]==ADDR(0x104) && cpu.x[24]==0 && cpu.x[30]==0x1234);
    CHECK(aot_dispatch_override(ADDR(0x104),override_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(ADDR(0x104))==override_function);
    call(&cpu,ADDR(0x104));CHECK(cpu.x[0]==25001);
    const uint64_t hle=BASE+0x900000;
    CHECK(aot_dispatch_add_hle(hle,hle_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(hle)==hle_function);
    call(&cpu,hle);CHECK(cpu.x[0]==25008);
    aot_native_call_original(&cpu,ADDR(0x100));
    CHECK(cpu.x[0]==35008 && cpu.x[24]==0 && cpu.x[30]==0x1234 && cpu.x[27]==1);
    if(CAPS&AOT_CAP_FUNCTION_HOOKS) {
        rt_call_original(&cpu,ADDR(0x100));
        CHECK(cpu.x[0]==45008 && cpu.x[24]==0 && cpu.x[30]==0x1234 && cpu.x[27]==2);
    }
    CHECK(aot_dispatch_detach_cpu(&cpu)==AOT_OK);
}

static void execution(void) {
    bind_registry(); Cpu cpu; init_cpu(&cpu);
    call(&cpu, ADDR(0x100)); CHECK(cpu.x[0] == 1);
    call(&cpu, ADDR(0x300)); CHECK(cpu.x[0] == 12);
    call(&cpu, ADDR(0x400)); CHECK(cpu.x[0] == 13);
    call(&cpu, ADDR(0x500)); CHECK(cpu.x[0] == 24);
    for (unsigned i = 1; i < FIXTURE_MODULE_COUNT; ++i) call(&cpu, BASE + UINT64_C(0x100000) * i + 0x100);
    CHECK(cpu.x[3] == (FIXTURE_MODULE_COUNT == 2 ? 2 : 234));
    CHECK(dispatch_lookup(ADDR(0x104)) == NULL);
    CHECK(dispatch_lookup(BASE + 0x800000) == NULL);
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
static void cache(void) {
    bind_registry(); Cpu first, second; init_cpu(&first); init_cpu(&second);
    unsigned slot = aot_dispatch_cache_slot(ADDR(0x100));
    CHECK(slot == aot_dispatch_cache_slot(ADDR(0x10140)));
    call(&first, ADDR(0x100)); CHECK(first.dcache[slot].fn == dispatch_lookup(ADDR(0x100)));
    call(&first, ADDR(0x100)); CHECK(first.x[0] == 2 && first.x[10] == 2);
    CHECK(second.dcache[slot].fn == NULL);
    call(&second, ADDR(0x10140)); CHECK(second.dcache[slot].addr == ADDR(0x10140));
    CHECK(first.dcache[slot].addr == ADDR(0x100));
    call(&first, ADDR(0x10140)); CHECK(first.dcache[slot].addr == ADDR(0x10140));
    call(&first, ADDR(0x100)); CHECK(first.x[0] == 103 && first.dcache[slot].addr == ADDR(0x100));
    CHECK(aot_dispatch_detach_cpu(&first) == AOT_OK); CHECK(aot_dispatch_detach_cpu(&second) == AOT_OK);
}
static void overrides_case(void) {
    bind_registry(); Cpu first, second; init_cpu(&first); init_cpu(&second);
    call(&first, ADDR(0x100)); call(&second, ADDR(0x100));
    unsigned slot = aot_dispatch_cache_slot(ADDR(0x100));
    CHECK(aot_dispatch_override(ADDR(0x100), override_function) == AOT_OK);
    CHECK(first.dcache[slot].fn == NULL && second.dcache[slot].fn == NULL);
    call(&first, ADDR(0x100)); call(&second, ADDR(0x100));
    CHECK(first.x[0] == 5001 && second.x[0] == 5001);
    CHECK(first.dcache[slot].fn == NULL); /* overrides deliberately remain uncached */
    CHECK(aot_dispatch_override(ADDR(0x100), hle_function) == AOT_OK);
    call(&first, ADDR(0x100)); CHECK(first.x[0] == 5008);
    CHECK(aot_dispatch_override(ADDR(0x104), hle_function) == AOT_ARGUMENT);
    CHECK(aot_dispatch_detach_cpu(&first) == AOT_OK); CHECK(aot_dispatch_detach_cpu(&second) == AOT_OK);
}
static void hle_case(void) {
    bind_registry(); Cpu cpu; init_cpu(&cpu);
    const uint64_t address = BASE + 0x900000;
    CHECK(aot_dispatch_set_hle_guard(hle_guard) == AOT_OK);
    CHECK(aot_dispatch_add_hle(address, hle_function) == AOT_OK);
    cpu.x[30] = 0x1234; guest_call(&cpu, address);
    CHECK(cpu.x[0] == 7 && cpu.x[24] == 0 && cpu.x[30] == 0x1234);
    CHECK(cpu.x[25] == 1);
    CHECK(cpu.dcache[aot_dispatch_cache_slot(address)].fn == NULL);
    CHECK(aot_dispatch_add_hle(address, override_function) == AOT_OK);
    call(&cpu, address); CHECK(cpu.x[0] == 5007);
    CHECK(cpu.x[25] == 2);
    CHECK(aot_dispatch_add_hle(ADDR(0x100), hle_function) == AOT_ARGUMENT);
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
typedef struct DynamicHleThread { unsigned id; int ok; } DynamicHleThread;
static void *dynamic_hle_worker(void *argument) {
    DynamicHleThread *thread = argument;
    thread->ok = 1;
    /* Concurrent lifecycle, publication and dispatch; no shared guest Cpu. */
    for (unsigned round = 0; round < 40; ++round) {
        Cpu cpu;
        aot_cpu_initialize(&cpu);
        if (aot_dispatch_attach_cpu(&cpu) != AOT_OK) { thread->ok = 0; break; }
        uint64_t address = BASE + 0x900000 + 16 * (thread->id * 40 + round);
        if (aot_dispatch_add_hle(address, hle_function) != AOT_OK) thread->ok = 0;
        if (aot_dispatch_host_lookup(address) != hle_function) thread->ok = 0;
        call(&cpu, address);
        if (cpu.x[0] != 7 || cpu.dcache[aot_dispatch_cache_slot(address)].fn) thread->ok = 0;
        /* Each thread also atomically replaces an already published common slot. */
        if (aot_dispatch_add_hle(BASE + 0x980000, round & 1 ? hle_function : override_function) != AOT_OK)
            thread->ok = 0;
        cpu.x[0] = 0; call(&cpu, BASE + 0x980000);
        if (cpu.x[0] != 7 && cpu.x[0] != 5000) thread->ok = 0;
        GuestFn live = aot_dispatch_host_lookup(BASE + 0x980000);
        if (live != hle_function && live != override_function) thread->ok = 0;
        if (aot_dispatch_detach_cpu(&cpu) != AOT_OK) thread->ok = 0;
    }
    return NULL;
}
static void dynamic_hle_case(void) {
    bind_registry();
    pthread_t threads[4]; DynamicHleThread results[4];
    for (unsigned i = 0; i < 4; ++i) {
        results[i] = (DynamicHleThread){i, 0};
        CHECK(!pthread_create(&threads[i], NULL, dynamic_hle_worker, &results[i]));
    }
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!pthread_join(threads[i], NULL)); CHECK(results[i].ok);
    }
}
static void host_lookup_capacity_case(void) {
    bind_registry();
    CHECK(!aot_dispatch_host_lookup(0)&&!aot_dispatch_host_lookup(UINT64_MAX));
    CHECK(!aot_dispatch_host_lookup(ADDR(0x100))); /* never a generated entry */
    CHECK(aot_dispatch_set_execution_backend(native_backend_fixture)==AOT_OK);
    const uint64_t hle_base=BASE+0x900000;
    for(unsigned i=0;i<AOT_MAX_HLE_FUNCTIONS;i++) {
        CHECK(!aot_dispatch_host_lookup(hle_base+4*i)); /* no negative caching */
        CHECK(aot_dispatch_add_hle(hle_base+4*i,i&1?hle_function:override_function)==AOT_OK);
        CHECK(aot_dispatch_override(BASE+4*i,i&1?override_function:hle_function)==AOT_OK);
    }
    for(unsigned i=0;i<AOT_MAX_HLE_FUNCTIONS;i++) {
        CHECK(aot_dispatch_host_lookup(hle_base+4*i)==(i&1?hle_function:override_function));
        CHECK(aot_dispatch_host_lookup(BASE+4*i)==(i&1?override_function:hle_function));
    }
    CHECK(aot_dispatch_add_hle(hle_base+4*AOT_MAX_HLE_FUNCTIONS,hle_function)==AOT_CAPACITY);
    CHECK(aot_dispatch_override(BASE+4*AOT_MAX_DISPATCH_OVERRIDES,hle_function)==AOT_CAPACITY);
    /* Full registries still replace existing entries, including the final one. */
    CHECK(aot_dispatch_add_hle(hle_base+4*(AOT_MAX_HLE_FUNCTIONS-1),override_function)==AOT_OK);
    CHECK(aot_dispatch_override(BASE+4*(AOT_MAX_DISPATCH_OVERRIDES-1),hle_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(hle_base+4*(AOT_MAX_HLE_FUNCTIONS-1))==override_function);
    CHECK(aot_dispatch_host_lookup(BASE+4*(AOT_MAX_DISPATCH_OVERRIDES-1))==hle_function);
    for(unsigned i=0;i<10000;i++)CHECK(!aot_dispatch_host_lookup(BASE+0x800000+4*i));
}
static void host_lookup_zero_case(void) {
    /* This ABI forbids mapped guest zero. Inserting a real first entry into
     * bucket zero must still leave a zero-address query absent. */
    bind_registry();const uint64_t bucket_zero=UINT64_C(0x1000001e6c);
    CHECK(!aot_dispatch_host_lookup(0));
    CHECK(aot_dispatch_set_execution_backend(native_backend_fixture)==AOT_OK);
    CHECK(aot_dispatch_override(0,override_function)==AOT_ARGUMENT);
    CHECK(aot_dispatch_add_hle(0,hle_function)==AOT_ARGUMENT);
    CHECK(aot_dispatch_add_hle(bucket_zero,hle_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(bucket_zero)==hle_function);
    CHECK(!aot_dispatch_host_lookup(0));
    CHECK(!aot_dispatch_host_lookup(4)&&!aot_dispatch_host_lookup(UINT64_MAX));
}
static void host_lookup_collisions_case(void) {
    bind_registry();
    /* Asset-free addresses chosen to collide at bucket8191, exercising linear
     * probing across the table end. The last key stays absent until queried. */
    const uint64_t keys[]={UINT64_C(0x1000006134),UINT64_C(0x100003d3b0),UINT64_C(0x100003e9f4),UINT64_C(0x100003ea4c),UINT64_C(0x1000041474),UINT64_C(0x100004ed04),UINT64_C(0x100005d8a4),UINT64_C(0x100006a468),UINT64_C(0x100007f8c8),UINT64_C(0x10000837c8),UINT64_C(0x100008f8b0),UINT64_C(0x1000098cac),UINT64_C(0x10000a3e88),UINT64_C(0x10000a9df4),UINT64_C(0x10000b8c90),UINT64_C(0x10000c8ccc),UINT64_C(0x10000cb7ec)};
    for(unsigned i=0;i<16;i++)CHECK(aot_dispatch_add_hle(keys[i],i&1?hle_function:override_function)==AOT_OK);
    for(unsigned round=0;round<3;round++) {
        for(unsigned i=0;i<16;i++)CHECK(aot_dispatch_host_lookup(keys[i])==(i&1?hle_function:override_function));
        CHECK(!aot_dispatch_host_lookup(keys[16]));
    }
    CHECK(aot_dispatch_add_hle(keys[16],hle_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(keys[16])==hle_function);
    CHECK(aot_dispatch_add_hle(keys[0],hle_function)==AOT_OK);
    CHECK(aot_dispatch_host_lookup(keys[0])==hle_function);
}
typedef struct HostLookupThread {unsigned id;int ok;} HostLookupThread;
static void *host_lookup_worker(void *argument) {
    HostLookupThread *thread=argument;thread->ok=1;
    const uint64_t start=BASE+0x900000,common=BASE+0x980000;
    for(unsigned i=0;i<4000;i++) {
        if(thread->id<4) {
            if(i<500&&aot_dispatch_add_hle(start+4*(thread->id*500+i),hle_function)!=AOT_OK)thread->ok=0;
            if(aot_dispatch_add_hle(common,i&1?hle_function:override_function)!=AOT_OK)thread->ok=0;
        }
        /* Readers may see absent addresses until a publisher completes, but
         * never uninitialized data or a cached old function after publication. */
        GuestFn function=aot_dispatch_host_lookup(start+4*((i*37+thread->id)%2000));
        if(function&&function!=hle_function)thread->ok=0;
        function=aot_dispatch_host_lookup(common);
        if(function&&function!=hle_function&&function!=override_function)thread->ok=0;
    }
    return NULL;
}
static void host_lookup_concurrent_case(void) {
    bind_registry();pthread_t threads[6];HostLookupThread results[6];
    for(unsigned i=0;i<6;i++){results[i]=(HostLookupThread){i,0};CHECK(!pthread_create(&threads[i],NULL,host_lookup_worker,&results[i]));}
    for(unsigned i=0;i<6;i++){CHECK(!pthread_join(threads[i],NULL));CHECK(results[i].ok);}
    for(unsigned i=0;i<2000;i++)CHECK(aot_dispatch_host_lookup(BASE+0x900000+4*i)==hle_function);
    CHECK(aot_dispatch_add_hle(BASE+0x980000,override_function)==AOT_OK);
    for(unsigned i=0;i<1000;i++)CHECK(aot_dispatch_host_lookup(BASE+0x980000)==override_function);
}
static uint64_t monotonic_ns(void) {struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static void host_lookup_workload_case(void) {
    bind_registry();CHECK(aot_dispatch_set_execution_backend(native_backend_fixture)==AOT_OK);
    const unsigned hle_entries=714,override_entries=64,queries=20000;
    uint64_t hle_addresses[714],override_addresses[64];
    for(unsigned i=0;i<hle_entries;i++){hle_addresses[i]=BASE+0x900000+4*i;CHECK(aot_dispatch_add_hle(hle_addresses[i],hle_function)==AOT_OK);}
    for(unsigned i=0;i<override_entries;i++){override_addresses[i]=BASE+4*i;CHECK(aot_dispatch_override(override_addresses[i],override_function)==AOT_OK);}
    uint64_t start=monotonic_ns();
    for(unsigned i=0;i<queries;i++)CHECK(!aot_dispatch_host_lookup(BASE+0x800000+4*i));
    uint64_t indexed_ns=monotonic_ns()-start,comparisons=0;volatile unsigned matches=0;
    start=monotonic_ns();
    for(unsigned i=0;i<queries;i++) {
        uint64_t address=BASE+0x800000+4*i;
        for(unsigned j=0;j<hle_entries;j++){comparisons++;if(hle_addresses[j]==address)matches++;}
        for(unsigned j=0;j<override_entries;j++){comparisons++;if(override_addresses[j]==address)matches++;}
    }
    uint64_t linear_ns=monotonic_ns()-start;CHECK(!matches);CHECK(comparisons==(uint64_t)queries*(hle_entries+override_entries));
    printf("synthetic_host_lookup queries=%u registered=%u linear_reference_comparisons=%" PRIu64 " indexed_ns=%" PRIu64 " linear_reference_ns=%" PRIu64 "; timing is host-only, no game performance claim\n",queries,hle_entries+override_entries,comparisons,indexed_ns,linear_ns);
}
static void hooks_case(void) {
    bind_registry(); Cpu cpu; init_cpu(&cpu);
    if (!(CAPS & AOT_CAP_FUNCTION_HOOKS)) {
        CHECK(rt_register_fnhook(ADDR(0x100), handle_a) == AOT_CAPABILITY_MISMATCH);
        CHECK(!g_fnhook); call(&cpu, ADDR(0x100)); CHECK(cpu.x[0] == 1);
    } else {
        call(&cpu, ADDR(0x100)); /* cache hit must still pass through generated FN_ENTER */
        CHECK(rt_register_fnhook(ADDR(0x100), handle_a) == AOT_OK);
        CHECK(rt_register_fnhook(ADDR(0x200), pass_b) == AOT_OK);
        CHECK(rt_register_fnhook(ADDR(0x300), handle_wrapper) == AOT_OK);
        call(&cpu, ADDR(0x300)); CHECK(cpu.x[0] == 1011 && cpu.x[20] == 1 && cpu.x[21] == 1);
        CHECK(cpu.x[22] == 0); /* forwarding wrapper has no entry hook */
        call(&cpu, ADDR(0x400)); call(&cpu, ADDR(0x100)); call(&cpu, ADDR(0x100));
        CHECK(cpu.x[0] == 4011 && cpu.x[20] == 4 && cpu.x[10] == 1);
        CHECK(rt_register_fnhook(ADDR(0x100), original_a) == AOT_OK);
        CHECK(rt_register_fnhook(ADDR(0x500), original_nested) == AOT_OK);
        cpu.x[0] = cpu.x[10] = cpu.x[20] = 0;
        call(&cpu, ADDR(0x500)); CHECK(cpu.x[0] == 111 && cpu.x[10] == 1 && cpu.x[20] == 1 && cpu.x[23] == 1);
        cpu.x[0] = cpu.x[10] = cpu.x[20] = 0; cpu.x[6] = 1;
        call(&cpu, ADDR(0x100)); CHECK(cpu.x[0] == 202 && cpu.x[10] == 2 && cpu.x[20] == 2);
        rt_call_original(&cpu, ADDR(0x300)); /* unconsumed wrapper token must not leak */
        call(&cpu, ADDR(0x300)); CHECK(cpu.x[22] == 0);
        CHECK(rt_register_fnhook(ADDR(0x104), handle_a) == AOT_ARGUMENT);
    }
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
static int instruction_mode(Cpu *cpu, uint64_t site) { (void)site; return (int)cpu->x[1]; }
static void instruction_case(void) {
    bind_registry(); Cpu cpu; init_cpu(&cpu);
    if (!(CAPS & AOT_CAP_INSTRUCTION_HOOKS)) {
        CHECK(rt_register_instruction_hook(ADDR(0x604), instruction_mode) == AOT_CAPABILITY_MISMATCH);
    } else {
        CHECK(rt_register_instruction_hook(ADDR(0x604), instruction_mode) == AOT_OK);
        call(&cpu, ADDR(0x600)); CHECK(cpu.x[0] == 1);
        cpu.x[1] = 1; call(&cpu, ADDR(0x600)); CHECK(cpu.x[0] == 1);
        cpu.x[1] = 2; call(&cpu, ADDR(0x600)); CHECK(cpu.x[0] == 21);
        cpu.x[1] = 3; call(&cpu, ADDR(0x600)); CHECK(cpu.x[0] == 51 && cpu.x[13] == 4);
        CHECK(rt_hook(&cpu, ADDR(0x608)) == 0);
        CHECK(rt_register_instruction_hook(BASE + 0x900000, instruction_mode) == AOT_ARGUMENT);
    }
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
static unsigned profile_hits;
static uint64_t last_profile_address;
static void profile(uint64_t address, uint64_t lr, uint64_t x0) {
    (void)lr; (void)x0; ++profile_hits; last_profile_address = address;
}
static void profiling_case(void) {
    bind_registry(); Cpu cpu; init_cpu(&cpu);
    if (!(CAPS & AOT_CAP_PROFILING)) {
        CHECK(aot_set_profiler(profile) == AOT_CAPABILITY_MISMATCH); CHECK(!g_fnprof);
    } else {
        CHECK(aot_set_profiler(profile) == AOT_OK);
        call(&cpu, ADDR(0x300)); CHECK(profile_hits == 2 && last_profile_address == ADDR(0x200));
        CHECK(aot_set_profiler(NULL) == AOT_OK); call(&cpu, ADDR(0x100)); CHECK(profile_hits == 2);
    }
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
static void *thread_body(void *arg) {
    Cpu *cpu = arg;
    for (unsigned i = 0; i < 2000; ++i) call(cpu, ADDR(0x100));
    return NULL;
}
static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t token_condition = PTHREAD_COND_INITIALIZER;
static int token_waiting, token_release;
static int pause_entry(Cpu *cpu, uint64_t address) {
    (void)cpu; (void)address;
    pthread_mutex_lock(&token_lock);
    token_waiting = 1; pthread_cond_broadcast(&token_condition);
    while (!token_release) pthread_cond_wait(&token_condition, &token_lock);
    pthread_mutex_unlock(&token_lock);
    return 0;
}
static void *original_wrapper_thread(void *arg) { rt_call_original(arg, ADDR(0x300)); return NULL; }
static void threads_case(void) {
    bind_registry(); Cpu first, second; init_cpu(&first); init_cpu(&second);
    if (CAPS & AOT_CAP_FUNCTION_HOOKS) CHECK(rt_register_fnhook(ADDR(0x100), original_a) == AOT_OK);
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, thread_body, &first));
    CHECK(!pthread_create(&threads[1], NULL, thread_body, &second));
    CHECK(!pthread_join(threads[0], NULL)); CHECK(!pthread_join(threads[1], NULL));
    uint64_t want = CAPS & AOT_CAP_FUNCTION_HOOKS ? 202000 : 2000;
    CHECK(first.x[0] == want && second.x[0] == want);
    CHECK(first.x[10] == 2000 && second.x[10] == 2000);
    CHECK(first.x[20] == (CAPS & AOT_CAP_FUNCTION_HOOKS ? 2000 : 0));
    CHECK(second.x[20] == first.x[20]);
    if (CAPS & AOT_CAP_FUNCTION_HOOKS) {
        /* The wrapper omits FN_ENTER, so its original-bypass token stays armed
         * while A's hook blocks. Probe that exact Cpu/address from this thread:
         * a global token would suppress the callback; TLS must not. Guest body
         * execution is paused and the callback writes an otherwise-unused reg. */
        CHECK(rt_register_fnhook(ADDR(0x100), pause_entry) == AOT_OK);
        CHECK(rt_register_fnhook(ADDR(0x300), handle_wrapper) == AOT_OK);
        CHECK(!pthread_create(&threads[0], NULL, original_wrapper_thread, &first));
        pthread_mutex_lock(&token_lock);
        while (!token_waiting) pthread_cond_wait(&token_condition, &token_lock);
        CHECK(rt_fnhook(&first, ADDR(0x300)) == 1 && first.x[22] == 1);
        token_release = 1; pthread_cond_broadcast(&token_condition);
        pthread_mutex_unlock(&token_lock);
        CHECK(!pthread_join(threads[0], NULL));
        CHECK(first.x[0] == want + 11);
        CHECK(rt_fnhook(&first, ADDR(0x300)) == 1 && first.x[22] == 2);
    }
    CHECK(aot_dispatch_detach_cpu(&first) == AOT_OK); CHECK(aot_dispatch_detach_cpu(&second) == AOT_OK);
}
static void helpers_case(void) {
    Cpu cpu; memset(&cpu, 0xa5, sizeof cpu); aot_cpu_initialize(&cpu);
    const unsigned char *bytes = (const void *)&cpu;
    for (size_t i = 0; i < sizeof cpu; ++i) CHECK(bytes[i] == 0);
    CHECK(cpu.excl_hi == 0);
    CHECK(adds64(&cpu, UINT64_MAX, 1) == 0 && FLAG_C(&cpu) && FLAG_Z(&cpu));
    CHECK(subs64(&cpu, 0, 1) == UINT64_MAX && !FLAG_C(&cpu) && FLAG_N(&cpu));
    CHECK(adds32(&cpu, INT32_MAX, 1) == UINT32_C(0x80000000) && FLAG_V(&cpu));
    cpu.nzcv = UINT64_C(1) << 29; CHECK(adcs64(&cpu, UINT64_MAX, 0) == 0 && FLAG_C(&cpu));
    CHECK(udiv64(3, 0) == 0 && sdiv64((uint64_t)INT64_MIN, UINT64_MAX) == (uint64_t)INT64_MIN);
    CHECK(sdiv32((uint32_t)INT32_MIN, UINT32_MAX) == (uint32_t)INT32_MIN);
    CHECK(clz64(0) == 64 && cls32(UINT32_MAX) == 31);
    CHECK(rbit32(1) == UINT32_C(0x80000000) && ROR64(1, 0) == 1);
    uint64_t memory = 10;
    CHECK(ldx64(&cpu, (uintptr_t)&memory) == 10);
    CHECK(stx64(&cpu, (uintptr_t)&memory, 11) == 0 && memory == 11);
    CHECK(stx64(&cpu, (uintptr_t)&memory, 12) == 1 && memory == 11);
    CHECK(ldx64(&cpu, (uintptr_t)&memory) == 11); memory = 13;
    CHECK(stx64(&cpu, (uintptr_t)&memory, 14) == 1 && memory == 13);
    stl64((uintptr_t)&memory, 15); CHECK(lda64((uintptr_t)&memory) == 15);
    unsigned char unaligned[16] = {0}; ST64((uintptr_t)(unaligned + 1), UINT64_C(0xfedcba9876543210));
    CHECK(LD64((uintptr_t)(unaligned + 1)) == UINT64_C(0xfedcba9876543210));
}
static void gate_caps(void) {
    CHECK(aot_dispatch_bind(descriptor, &expected, CAPS | 0x80, CAPS | 0x80, NULL) == AOT_CAPABILITY_MISMATCH);
    CHECK(aot_dispatch_bind(descriptor, &expected, CAPS, CAPS ^ AOT_CAP_PROFILING, NULL) == AOT_CAPABILITY_MISMATCH);
    CHECK(!aot_dispatch_is_bound() && dispatch_lookup(ADDR(0x100)) == NULL && !g_fnhook);
}
static void gate_legacy(void) {
    ModuleAbiDescriptor copy = *descriptor;
    copy.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560;
    expected.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560;
    CHECK(aot_dispatch_bind(&copy, &expected, CAPS, CAPS, NULL) == AOT_CPU_MISMATCH);
    CHECK(!aot_dispatch_is_bound());
    copy.identity.cpu_identity = expected.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024;
    copy.identity.cpu_size = expected.identity.cpu_size = 1024;
    CHECK(aot_dispatch_bind(&copy, &expected, CAPS, CAPS, NULL) == AOT_CPU_MISMATCH);
}
static void gate_metadata(void) {
    ModuleAbiFailure failure;
    specs[0].nso_sha256[31] = 1;
    CHECK(aot_dispatch_bind(descriptor, &expected, CAPS, CAPS, &failure) == AOT_ABI_MISMATCH);
    CHECK(failure.status == MODULE_ABI_METADATA_MISMATCH && failure.module_index == 0);
    CHECK(!aot_dispatch_is_bound());
}
static void second_bind(void) {
    bind_registry(); CHECK(aot_dispatch_bind(descriptor, &expected, CAPS, CAPS, NULL) == AOT_ALREADY_BOUND);
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <fixture.so> <case>\n", argv[0]); return 2; }
    make_expected();
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    descriptor = dlsym(library, "recomp_module_descriptor_v1"); CHECK(descriptor != NULL);
    struct { const char *name; void (*run)(void); } cases[] = {
        {"execution", execution}, {"cache", cache}, {"overrides", overrides_case}, {"hle", hle_case}, {"backend",backend_case},
        {"hooks", hooks_case}, {"instruction", instruction_case}, {"profiling", profiling_case},
        {"threads", threads_case}, {"helpers", helpers_case}, {"gate-caps", gate_caps},
        {"gate-legacy", gate_legacy}, {"gate-metadata", gate_metadata}, {"second-bind", second_bind},
        {"dynamic-hle", dynamic_hle_case}, {"host-lookup-capacity",host_lookup_capacity_case},
        {"host-lookup-zero",host_lookup_zero_case}, {"host-lookup-concurrent",host_lookup_concurrent_case},
        {"host-lookup-workload",host_lookup_workload_case}, {"host-lookup-collisions",host_lookup_collisions_case}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) if (!strcmp(argv[2], cases[i].name)) {
        cases[i].run(); printf("%s modules=%u caps=%u: %u checks\n", argv[2], FIXTURE_MODULE_COUNT, CAPS, checks); return 0;
    }
    fprintf(stderr, "unknown case: %s\n", argv[2]); return 2;
    /* No dlclose: one successful execution registry lasts for this process. */
}
