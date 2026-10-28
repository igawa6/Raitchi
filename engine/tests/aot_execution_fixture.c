/* Asset-free synthetic generated-code stand-in; no game code or title addresses. */
#include "runtime/aot_hooks.h"
#ifndef FIXTURE_MODULE_COUNT
#error "choose independent synthetic two- or four-module profile"
#endif
#if FIXTURE_MODULE_COUNT != 2 && FIXTURE_MODULE_COUNT != 4
#error "fixture module count"
#endif
#define CANONICAL_BASE UINT64_C(0x5000000000)
#if FIXTURE_MODULE_COUNT == 2
#define SHIFT UINT64_C(0x2000000000)
#else
#define SHIFT UINT64_C(0)
#endif
#define BASE (CANONICAL_BASE - SHIFT)
#define ADDRESS(module, offset) (BASE + UINT64_C(0x100000) * (module) + (offset))

static void function_a(Cpu *CPU_RESTRICT c) {
    FN_ENTER(ADDRESS(0, 0x100)); c->x[0] += 1; c->x[10]++;
    if (c->x[6]) { c->x[6]--; function_a(c); } /* bypass applies to one entry, not recursion */
}
static void function_b(Cpu *CPU_RESTRICT c) { FN_ENTER(ADDRESS(0, 0x200)); c->x[0] += 10; c->x[11]++; }
/* Generated HLE forwarding wrappers intentionally have no FN_ENTER of their own. */
static void direct_wrapper(Cpu *CPU_RESTRICT c) { function_a(c); function_b(c); }
static void indirect_chain(Cpu *CPU_RESTRICT c) {
    FN_ENTER(ADDRESS(0, 0x400));
    c->pc = ADDRESS(0, 0x100);
    __attribute__((musttail)) return rt_dispatch(c);
}
static void nested(Cpu *CPU_RESTRICT c) {
    FN_ENTER(ADDRESS(0, 0x500));
    function_a(c); function_b(c); c->x[12]++;
}
static void instruction(Cpu *CPU_RESTRICT c) {
    FN_ENTER(ADDRESS(0, 0x600));
    /* Mirrors the generator's original/handled/local-target protocol. */
#if (AOT_PROFILE_CAPABILITIES & AOT_CAP_INSTRUCTION_HOOKS)
    int h = rt_hook(c, ADDRESS(0, 0x604));
#else
    int h = 0;
#endif
    if (h == 0) c->x[0] += 1;
    else if (h == 2) goto first;
    else if (h == 3) goto second;
    goto end;
first: c->x[0] += 20; goto end;
second: c->x[0] += 30;
end: c->x[13]++;
}
static void collision(Cpu *CPU_RESTRICT c) { FN_ENTER(ADDRESS(0, 0x10140)); c->x[0] += 100; c->x[14]++; }
static void sdk(Cpu *CPU_RESTRICT c) { FN_ENTER(ADDRESS(1, 0x100)); c->x[3] = c->x[3] * 10 + 2; }
#if FIXTURE_MODULE_COUNT == 4
static void subsdk0(Cpu *CPU_RESTRICT c) { FN_ENTER(ADDRESS(2, 0x100)); c->x[3] = c->x[3] * 10 + 3; }
static void subsdk1(Cpu *CPU_RESTRICT c) { FN_ENTER(ADDRESS(3, 0x100)); c->x[3] = c->x[3] * 10 + 4; }
#endif
static const uint64_t main_addresses[] = {
    ADDRESS(0, 0x100), ADDRESS(0, 0x200), ADDRESS(0, 0x300), ADDRESS(0, 0x400),
    ADDRESS(0, 0x500), ADDRESS(0, 0x600), ADDRESS(0, 0x10140)
};
static const ModuleAbiGuestFn main_functions[] = {function_a, function_b, direct_wrapper, indirect_chain, nested, instruction, collision};
static const uint64_t sdk_addresses[] = {ADDRESS(1, 0x100)};
static const ModuleAbiGuestFn sdk_functions[] = {sdk};
#if FIXTURE_MODULE_COUNT == 4
static const uint64_t subsdk0_addresses[] = {ADDRESS(2, 0x100)};
static const ModuleAbiGuestFn subsdk0_functions[] = {subsdk0};
static const uint64_t subsdk1_addresses[] = {ADDRESS(3, 0x100)};
static const ModuleAbiGuestFn subsdk1_functions[] = {subsdk1};
#endif
#define SPEC(index, count) { .struct_size = sizeof(ModuleAbiModuleSpec), .module_id = (index) + 1, \
    .nso_build_id = {(index) + 1}, .nso_sha256 = {(index) + 17}, \
    .canonical_base = CANONICAL_BASE + UINT64_C(0x100000) * (index), .mapped_size = 0x40000, \
    .text_offset = 0, .text_size = 0x20000, .function_count = (count) }
#define MODULE(index, name, count) { .struct_size = sizeof(ModuleAbiModule), .image = SPEC(index, count), \
    .address_count = count, .addresses = name##_addresses, .functions = name##_functions }
static const ModuleAbiModule modules[] = {
    MODULE(0, main, 7), MODULE(1, sdk, 1),
#if FIXTURE_MODULE_COUNT == 4
    MODULE(2, subsdk0, 1), MODULE(3, subsdk1, 1),
#endif
};
__attribute__((visibility("default"))) const ModuleAbiDescriptor recomp_module_descriptor_v1 = {
    .struct_size = sizeof(ModuleAbiDescriptor), .module_count = FIXTURE_MODULE_COUNT,
    .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
        .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
        .cpu_size = sizeof(Cpu), .cpu_alignment = _Alignof(Cpu), .guest_shift = SHIFT,
        .guest_window_lo = BASE, .guest_window_hi = BASE + 0x1000000},
    .modules = modules
};
