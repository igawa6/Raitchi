/* Isolated leaf-only comparison. Original ISA policy, no embedded game asset.
 * Cpu/descriptor/sidecar layouts come from the production public headers. */
#define _GNU_SOURCE
#include "runtime/aot_cpu.h"
#include "runtime/title_manifest.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ISA encodings constructed independently of candidate bytes: scalar FABS on
 * S0 and RET through X30. No other instruction/register form is executable. */
static int leaf_allowed(const void *raw, size_t length) {
    if (!raw || length != 2 * sizeof(uint32_t)) return 0;
    uint32_t word[2];
    memcpy(word, raw, sizeof word);
    uint32_t scalar_fabs_s0 = (0x1eu << 24) | (1u << 21) | (3u << 14);
    uint32_t return_x30 = (0xd6u << 24) | (0x5fu << 16) | (30u << 5);
    return word[0] == scalar_fabs_s0 && word[1] == return_x30;
}

#ifdef LEAF_HOST_TEST
#include <assert.h>
int main(void) {
    uint32_t fixture[] = {(0x1eu << 24) | (1u << 21) | (3u << 14),
                          (0xd6u << 24) | (0x5fu << 16) | (30u << 5)};
    unsigned assertions = 0;
    assert(leaf_allowed(fixture, sizeof fixture)); ++assertions;
    for (unsigned instruction = 0; instruction < 2; ++instruction) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            fixture[instruction] ^= 1u << bit;
            assert(!leaf_allowed(fixture, sizeof fixture)); ++assertions;
            fixture[instruction] ^= 1u << bit;
        }
    }
    assert(!leaf_allowed(NULL, sizeof fixture)); ++assertions;
    assert(!leaf_allowed(fixture, sizeof fixture - 1)); ++assertions;
    assert(!leaf_allowed(fixture, sizeof fixture + 1)); ++assertions;
    printf("leaf ISA whitelist assertions=%u\n", assertions);
    return 0;
}
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t leaf_host_tls(void) { uint64_t v; __asm__ volatile("mrs %0,tpidr_el0":"=r"(v)); return v; }
static uint64_t leaf_host_x18(void) { uint64_t v; __asm__ volatile("mov %0,x18":"=r"(v)); return v; }
static int leaf_rx(const void *address, size_t size) {
    FILE *file = fopen("/proc/self/maps", "r");
    if (!file) return 0;
    char line[512], permissions[5];
    unsigned long long start, end;
    int result = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3 &&
            start <= (uintptr_t)address && end >= (uintptr_t)address + size) {
            result = permissions[0] == 'r' && permissions[1] == '-' && permissions[2] == 'x';
            break;
        }
    }
    fclose(file);
    return result;
}

__attribute__((visibility("default"))) int native_las_leaf_probe(
    const char *runtime_path, const char *game_path, const void *manifest_bytes, size_t manifest_length,
    const uint8_t *contract, uint64_t actual_address, const void *native_bytes, size_t native_size,
    const uint32_t *inputs, const uint32_t *expected, size_t count, char *output, size_t capacity) {
    if (!output || capacity < 8192) return -1;
    const char *error = "none";
    int result = 1, failures = 0, rx = 0, compared = 0;
    uint64_t original_fpcr = rt_get_fpcr(), original_fpsr = rt_get_fpsr();
    uint64_t original_tls = leaf_host_tls(), original_x18 = leaf_host_x18();
    void *runtime = NULL, *game = NULL, *native = MAP_FAILED;
    Cpu *cpu = NULL;
    long page = sysconf(_SC_PAGESIZE);
    char rows[4096] = "";
    size_t used = 0;
    TitleManifest manifest;
    ModuleAbiFailure failure;
    if (!runtime_path || !game_path || !contract || !inputs || !expected || count != 12 ||
        !leaf_allowed(native_bytes, native_size) || page < 4096 || (page & (page - 1))) {
        error = "request-rejected"; goto done;
    }
    if (title_manifest_parse(manifest_bytes, manifest_length, contract, &manifest, &failure) != TITLE_MANIFEST_OK ||
        manifest.title_id != 1 || manifest.profile.identity.cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        manifest.profile.identity.cpu_size != sizeof(Cpu) || manifest.profile.identity.cpu_alignment != _Alignof(Cpu)) {
        error = "expected-profile-rejected"; goto done;
    }
    runtime = dlopen(runtime_path, RTLD_NOW | RTLD_GLOBAL);
    if (!runtime) { error = "runtime-dlopen"; goto done; }
    game = dlopen(game_path, RTLD_NOW | RTLD_LOCAL);
    if (!game) { error = "game-dlopen"; goto done; }
    const ModuleAbiDescriptor *descriptor = dlsym(game, "recomp_module_descriptor_v1");
    if (!descriptor || module_abi_validate(descriptor, &manifest.profile, &failure) != MODULE_ABI_OK) {
        error = "actual-descriptor-rejected"; goto done;
    }
    ModuleAbiGuestFn aot = NULL;
    for (uint32_t m = 0; m < descriptor->module_count; ++m) {
        const ModuleAbiModule *module = &descriptor->modules[m];
        if (module->image.module_id != MODULE_ABI_SDK) continue;
        uint64_t lo = 0, hi = module->address_count;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (module->addresses[mid] < actual_address) lo = mid + 1; else hi = mid;
        }
        if (lo < module->address_count && module->addresses[lo] == actual_address) aot = module->functions[lo];
    }
    if (!aot) { error = "SDK-function-not-found"; goto done; }
    if (posix_memalign((void **)&cpu, _Alignof(Cpu), sizeof(Cpu))) { error = "cpu-allocation"; goto done; }
    native = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (native == MAP_FAILED) { error = "native-mmap"; goto done; }
    memcpy(native, native_bytes, native_size);
    __builtin___clear_cache(native, (char *)native + native_size);
    if (mprotect(native, (size_t)page, PROT_READ | PROT_EXEC) || !(rx = leaf_rx(native, (size_t)page))) {
        error = "native-rx"; goto done;
    }
    typedef float (*NativeLeaf)(float);
    NativeLeaf call_native = (NativeLeaf)native;
    for (size_t i = 0; i < count; ++i) {
        float argument, value;
        uint32_t native_result, aot_result;
        memcpy(&argument, &inputs[i], sizeof argument);
        rt_set_fpcr(0); rt_set_fpsr(0);
        value = call_native(argument);
        memcpy(&native_result, &value, sizeof native_result);
        uint64_t native_fpsr = rt_get_fpsr(), native_fpcr = rt_get_fpcr();
        memset(cpu, 0, sizeof *cpu);
        cpu->v[0] = inputs[i];
        rt_set_fpcr(0); rt_set_fpsr(0);
        aot(cpu); /* ONLY the audited descriptor function; no guest initializer/dispatch. */
        memcpy(&aot_result, &cpu->v[0], sizeof aot_result);
        uint64_t aot_fpsr = rt_get_fpsr(), aot_fpcr = rt_get_fpcr();
        int ok = native_result == expected[i] && aot_result == expected[i] && native_result == aot_result &&
            native_fpsr == aot_fpsr && !native_fpsr && !native_fpcr && !aot_fpcr &&
            leaf_host_tls() == original_tls && leaf_host_x18() == original_x18;
        failures += !ok; ++compared;
        int written = snprintf(rows + used, sizeof rows - used,
            "%s{\"input_bits\":\"%08x\",\"native_bits\":\"%08x\",\"aot_bits\":\"%08x\","
            "\"expected_bits\":\"%08x\",\"passed\":%s}", i ? "," : "", inputs[i],
            native_result, aot_result, expected[i], ok ? "true" : "false");
        if (written < 0 || (size_t)written >= sizeof rows - used) { error = "result-capacity"; goto done; }
        used += (size_t)written;
    }
    result = failures != 0;
done:
    if (native != MAP_FAILED) munmap(native, (size_t)page);
    free(cpu);
    /* Drop the game's reference first. No SDK DT_INIT/nnMain was invoked. */
    if (game) dlclose(game);
    if (runtime) dlclose(runtime);
    rt_set_fpcr(original_fpcr); rt_set_fpsr(original_fpsr);
    int restored = rt_get_fpcr() == original_fpcr && rt_get_fpsr() == original_fpsr;
    int host_ok = leaf_host_tls() == original_tls && leaf_host_x18() == original_x18;
    if (!restored || !host_ok) result = 1;
    snprintf(output, capacity,
        "{\"schema\":1,\"state\":\"%s\",\"error\":\"%s\",\"cases\":%d,\"failures\":%d,"
        "\"rw_to_rx_verified\":%s,\"fp_state_restored\":%s,\"host_tls_x18_preserved\":%s,"
        "\"game_initializers_called\":false,\"guest_windows_mapped\":false,\"results\":[%s]}",
        result ? "failed" : "passed", error, compared, failures, rx ? "true" : "false",
        restored ? "true" : "false", host_ok ? "true" : "false", rows);
    return result;
}
#endif
