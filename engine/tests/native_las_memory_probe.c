/* Original differential harness; no embedded original game bytes or addresses. */
#define _GNU_SOURCE
#include "runtime/aot_cpu.h"
#include "runtime/title_manifest.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int memory_register(unsigned r) { return r == 0 || (r >= 8 && r <= 11); }
static int internal_target(size_t index, uint32_t word, size_t words) {
    int32_t immediate = (int32_t)((word >> 5) & 0x7ffff);
    if (immediate & 0x40000) immediate -= 0x80000;
    int64_t target = (int64_t)index + immediate;
    return target >= 0 && (uint64_t)target < words;
}
/* Restricted ISA forms used by the independently audited memory leaf. No
 * stores, stack, TLS, x18, indirect calls, literals or PC-relative addresses.
 * Runtime inputs are additionally bound to the reviewed original SHA by Python. */
static int memory_allowed(const void *raw, size_t size) {
    if (!raw || size < 8 || size > 256 || size % 4) return 0;
    const unsigned char *bytes = raw;
    size_t words = size / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t w; memcpy(&w, bytes + 4 * i, 4);
        unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        if (w == ((0xd6u << 24) | (0x5fu << 16) | (30u << 5))) continue;
        if ((w & 0xff00001fu) == 0x54000000u && internal_target(i, w, words)) continue; /* B.EQ */
        if ((w & 0xfe000000u) == 0x34000000u && memory_register(rd) && internal_target(i, w, words)) continue; /* CBZ/CBNZ W */
        if ((w & 0xffe0ffe0u) == 0xaa0003e0u && memory_register(rd) && memory_register(rm)) continue; /* MOV X */
        if ((w & 0xff800000u) == 0xf2800000u && rd == 9) continue; /* MOVK X9 */
        if ((w & 0x9f800000u) == 0x92000000u) { /* scalar logical immediate */
            unsigned op = (w >> 29) & 3;
            if (op == 3 && rd == 31 && memory_register(rn)) continue; /* TST */
            if (op == 1 && rn == 31 && rd == 9) continue; /* MOV bitmask X9 */
        }
        if ((w & 0x9f800000u) == 0x12000000u && ((w >> 29) & 3) == 3 && rd == 31 && rn == 10) continue; /* TST W10 */
        if ((w & 0xbfe0fc00u) == 0x8a200000u && memory_register(rd) && memory_register(rn) && memory_register(rm)) continue; /* BIC X */
        if ((w & 0xbfe0fc00u) == 0x8b000000u && memory_register(rd) && memory_register(rn) && memory_register(rm)) continue; /* ADD/SUB X reg */
        if ((w & 0xbf800000u) == 0x91000000u && memory_register(rd) && memory_register(rn)) continue; /* ADD/SUB X imm */
        if ((w & 0xfffffc00u) == 0x39400000u && memory_register(rd) && rn == 0) continue; /* LDRB [X0] */
        uint32_t load = w & 0xffe00c00u;
        unsigned imm = (w >> 12) & 511;
        if (rn == 8 && memory_register(rd) &&
            ((load == 0xf8400c00u && imm == 8) ||
             ((load == 0x38400400u || load == 0x38400c00u) && imm == 1))) continue;
        return 0;
    }
    uint32_t last; memcpy(&last, bytes + size - 4, 4);
    return last == ((0xd6u << 24) | (0x5fu << 16) | (30u << 5));
}

#ifdef MEMORY_HOST_TEST
#include <assert.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    uint32_t code[65]; size_t size = fread(code, 1, sizeof code, file); fclose(file);
    assert(memory_allowed(code, size));
    unsigned checks = 1;
    const uint32_t unsafe[] = {0xd4000001, 0xd53bd040, 0xd53bd060, 0xaa0003f2, 0x90000000,
                              0x58000000, 0xf9000000, 0xa9bf7bfd, 0x94000000, 0x14000000,
                              0x547fffe0, 0x347fffe0};
    for (size_t i = 0; i < size / 4; ++i) {
        uint32_t original = code[i];
        for (size_t j = 0; j < sizeof unsafe / sizeof *unsafe; ++j) {
            code[i] = unsafe[j]; assert(!memory_allowed(code, size)); ++checks;
        }
        code[i] = original;
    }
    assert(!memory_allowed(NULL, size)); ++checks;
    assert(!memory_allowed(code, size - 1)); ++checks;
    assert(!memory_allowed(code, 260)); ++checks;
    printf("memory ISA policy assertions=%u\n", checks);
    return 0;
}
#else
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint64_t memory_tls(void) { uint64_t v; __asm__ volatile("mrs %0,tpidr_el0":"=r"(v)); return v; }
static uint64_t memory_x18(void) { uint64_t v; __asm__ volatile("mov %0,x18":"=r"(v)); return v; }
static int memory_permissions(const void *address, size_t size, const char *wanted) {
    FILE *file = fopen("/proc/self/maps", "r"); if (!file) return 0;
    unsigned long long lo, hi; char line[512], perm[5]; int result = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) == 3 && lo <= (uintptr_t)address && hi >= (uintptr_t)address + size) {
            result = !memcmp(perm, wanted, 3); break;
        }
    }
    fclose(file); return result;
}
static uint32_t memory_random(uint32_t *state) { uint32_t v = *state; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *state = v; }

__attribute__((visibility("default"))) int native_las_memory_probe(
    const char *runtime_path, const char *game_path, const void *manifest_bytes, size_t manifest_size,
    const uint8_t *contract, uint64_t address, const void *code, size_t code_size, char *output, size_t capacity) {
    if (!output || capacity < 4096) return -1;
    const char *error = "none"; int result = 1, failures = 0, cases = 0, rx = 0, guards = 0, readonly = 0, unchanged = 1;
    unsigned alignment_mask = 0, near_guard_cases = 0;
    uint64_t saved_fpcr = rt_get_fpcr(), saved_fpsr = rt_get_fpsr(), saved_tls = memory_tls(), saved_x18 = memory_x18();
    long page = sysconf(_SC_PAGESIZE);
    void *runtime = NULL, *game = NULL, *native = MAP_FAILED, *mapping = MAP_FAILED;
    Cpu *cpu = NULL; unsigned char *snapshot = NULL, *data = NULL;
    TitleManifest manifest; ModuleAbiFailure failure;
    if (!runtime_path || !game_path || !contract || !memory_allowed(code, code_size) || page < 4096 || (page & (page - 1))) { error = "request-policy"; goto done; }
    if (title_manifest_parse(manifest_bytes, manifest_size, contract, &manifest, &failure) != TITLE_MANIFEST_OK || manifest.title_id != 1 ||
        manifest.profile.identity.cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 || manifest.profile.identity.cpu_size != sizeof(Cpu)) { error = "expected-profile"; goto done; }
    runtime = dlopen(runtime_path, RTLD_NOW | RTLD_GLOBAL); if (!runtime) { error = "runtime-dlopen"; goto done; }
    game = dlopen(game_path, RTLD_NOW | RTLD_LOCAL); if (!game) { error = "game-dlopen"; goto done; }
    const ModuleAbiDescriptor *descriptor = dlsym(game, "recomp_module_descriptor_v1");
    if (!descriptor || module_abi_validate(descriptor, &manifest.profile, &failure) != MODULE_ABI_OK) { error = "descriptor"; goto done; }
    ModuleAbiGuestFn aot = NULL;
    uint64_t first = manifest.profile.identity.guest_window_lo;
    for (uint32_t m = 0; m < descriptor->module_count; ++m) {
        const ModuleAbiModule *module = &descriptor->modules[m];
        uint64_t end = module->image.canonical_base - manifest.profile.identity.guest_shift + module->image.mapped_size;
        if (end > first) first = end;
        if (module->image.module_id == MODULE_ABI_SDK) {
            uint64_t lo = 0, hi = module->address_count;
            while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (module->addresses[mid] < address) lo = mid + 1; else hi = mid; }
            if (lo < module->address_count && module->addresses[lo] == address) aot = module->functions[lo];
        }
    }
    if (!aot) { error = "SDK-function"; goto done; }
    first = (first + (uint64_t)page - 1) & ~((uint64_t)page - 1);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        uint64_t wanted = first + (16 + 8 * (uint64_t)attempt) * (uint64_t)page;
        if (wanted >= manifest.profile.identity.guest_window_hi || 4 * (uint64_t)page > manifest.profile.identity.guest_window_hi - wanted) break;
        mapping = mmap((void *)(uintptr_t)wanted, 4 * (size_t)page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapping == MAP_FAILED) { if (errno == EEXIST) continue; break; }
        if ((uintptr_t)mapping != wanted) { munmap(mapping, 4 * (size_t)page); mapping = MAP_FAILED; break; }
        break;
    }
    if (mapping == MAP_FAILED) { error = "low-guest-noreplace-mapping"; goto done; }
    data = (unsigned char *)mapping + page;
    guards = memory_permissions(mapping, (size_t)page, "---") && memory_permissions(data + 2 * page, (size_t)page, "---");
    if (!guards || mprotect(data, 2 * (size_t)page, PROT_READ | PROT_WRITE)) { error = "guard-pages"; goto done; }
    if (posix_memalign((void **)&cpu, _Alignof(Cpu), sizeof *cpu) || !(snapshot = malloc(2 * (size_t)page))) { error = "allocations"; goto done; }
    native = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (native == MAP_FAILED) { error = "native-mmap"; goto done; }
    memcpy(native, code, code_size); __builtin___clear_cache(native, (char *)native + code_size);
    if (mprotect(native, (size_t)page, PROT_READ | PROT_EXEC) || !(rx = memory_permissions(native, (size_t)page, "r-x"))) { error = "native-rx"; goto done; }
    typedef size_t (*NativeMemory)(const char *);
    NativeMemory original = (NativeMemory)native;
    static const unsigned lengths[] = {0,1,2,7,8,9,15,16,31,63,127,255};
    static const unsigned edge_lengths[] = {0,1,7,8,63,255};
    uint32_t sequence = 0x6b8b4567;
    for (unsigned i = 0; i < 180; ++i) {
        size_t length, start;
        if (i < 96) { length = lengths[i / 8]; start = 32 + i % 8; }
        else if (i < 144) { unsigned edge = i - 96; length = edge_lengths[edge / 8]; start = 2 * (size_t)page - 1 - edge % 8 - length; ++near_guard_cases; }
        else if (i < 176) { length = memory_random(&sequence) % 2048; start = 16 + memory_random(&sequence) % 128; }
        else { length = 2 * (size_t)page - 1 - (i - 176); start = i - 176; ++near_guard_cases; }
        if (start + length >= 2 * (size_t)page || mprotect(data, 2 * (size_t)page, PROT_READ | PROT_WRITE)) { error = "case-bounds"; goto done; }
        uint32_t seed = 0x9e3779b9u ^ i;
        for (size_t j = 0; j < 2 * (size_t)page; ++j) data[j] = 1 + memory_random(&seed) % 255;
        data[start + length] = 0; memcpy(snapshot, data, 2 * (size_t)page);
        if (mprotect(data, 2 * (size_t)page, PROT_READ) || !memory_permissions(data, 2 * (size_t)page, "r--")) { error = "readonly-input"; goto done; }
        readonly = 1;
        uint64_t actual = (uintptr_t)(data + start), canonical = actual + manifest.profile.identity.guest_shift;
        if (canonical < actual || canonical - manifest.profile.identity.guest_shift != actual) { error = "pointer-shift"; goto done; }
        alignment_mask |= 1u << (actual & 7);
        rt_set_fpcr(saved_fpcr); rt_set_fpsr(saved_fpsr);
        size_t native_result = original((const char *)(uintptr_t)actual);
        int native_unchanged = !memcmp(data, snapshot, 2 * (size_t)page);
        int native_state = rt_get_fpcr() == saved_fpcr && rt_get_fpsr() == saved_fpsr && memory_tls() == saved_tls && memory_x18() == saved_x18;
        memset(cpu, 0, sizeof *cpu);
        cpu->x[0] = canonical - manifest.profile.identity.guest_shift; /* dynamic pointers are already actual, no double shift */
        rt_set_fpcr(saved_fpcr); rt_set_fpsr(saved_fpsr); aot(cpu);
        int aot_unchanged = !memcmp(data, snapshot, 2 * (size_t)page);
        int state = rt_get_fpcr() == saved_fpcr && rt_get_fpsr() == saved_fpsr && memory_tls() == saved_tls && memory_x18() == saved_x18;
        unchanged &= native_unchanged && aot_unchanged;
        failures += native_result != length || cpu->x[0] != length || !native_unchanged || !aot_unchanged || !native_state || !state;
        ++cases;
    }
    result = failures || alignment_mask != 255 || !unchanged;
done:
    if (native != MAP_FAILED) munmap(native, (size_t)page);
    if (mapping != MAP_FAILED) munmap(mapping, 4 * (size_t)page);
    free(snapshot); free(cpu);
    if (game) dlclose(game); if (runtime) dlclose(runtime);
    rt_set_fpcr(saved_fpcr); rt_set_fpsr(saved_fpsr);
    int restored = rt_get_fpcr() == saved_fpcr && rt_get_fpsr() == saved_fpsr;
    int host_ok = memory_tls() == saved_tls && memory_x18() == saved_x18;
    if (!restored || !host_ok) result = 1;
    snprintf(output, capacity, "{\"schema\":1,\"state\":\"%s\",\"error\":\"%s\",\"cases\":%d,\"failures\":%d,"
        "\"alignment_mask\":%u,\"near_guard_cases\":%u,\"rw_to_rx_verified\":%s,\"guard_pages_verified\":%s,"
        "\"readonly_inputs_verified\":%s,\"full_buffers_unchanged\":%s,\"actual_low_guest_buffers\":%s,"
        "\"fp_state_restored\":%s,\"host_tls_x18_preserved\":%s,\"game_initializers_called\":false,\"full_guest_window_reserved\":false}",
        result ? "failed" : "passed", error, cases, failures, alignment_mask, near_guard_cases, rx ? "true" : "false", guards ? "true" : "false",
        readonly ? "true" : "false", unchanged ? "true" : "false", data ? "true" : "false", restored ? "true" : "false", host_ok ? "true" : "false");
    return result;
}
#endif
