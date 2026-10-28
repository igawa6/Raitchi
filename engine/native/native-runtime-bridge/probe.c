#define _GNU_SOURCE
#include "bridge.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
_Static_assert(_Generic(&rt_svc, NrSvc: 1, default: 0), "production rt_svc declaration must match bridge API");
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint64_t host_tls(void) { uint64_t v; __asm__ volatile("mrs %0,tpidr_el0":"=r"(v)); return v; }
static uint64_t host_x18(void) { uint64_t v; __asm__ volatile("mov %0,x18":"=r"(v)); return v; }
static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static uint64_t ticks(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 192 / 10000); }
static int is_rx(void *address, size_t size) {
    FILE *file = fopen("/proc/self/maps", "r"); if (!file) return 0;
    char line[512], perm[5]; unsigned long long lo, hi; int result = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) == 3 && lo <= (uintptr_t)address && hi >= (uintptr_t)address + size) {
            result = !memcmp(perm, "r-x", 3); break;
        }
    }
    fclose(file); return result;
}
static void *map_snippet(long page, uint32_t *branch) {
    uintptr_t target = (uintptr_t)nr_gateway, center = target & ~((uintptr_t)page - 1);
    const uintptr_t step = 1024 * 1024;
    for (unsigned i = 1; i <= 96; ++i) {
        for (unsigned direction = 0; direction < 2; ++direction) {
            uintptr_t delta = (uintptr_t)i * step;
            if ((direction && center < delta) || (!direction && center > UINTPTR_MAX - delta)) continue;
            uintptr_t wanted = direction ? center - delta : center + delta;
            if (nr_rewrite_svc(0xd4000001u | (0x1eu << 5), wanted, target, branch)) continue;
            void *result = mmap((void *)wanted, (size_t)page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (result == MAP_FAILED) { if (errno == EEXIST) continue; return MAP_FAILED; }
            if ((uintptr_t)result != wanted) { munmap(result, (size_t)page); return MAP_FAILED; }
            return result;
        }
    }
    return MAP_FAILED;
}
static void seed_cpu(NrContext *ctx, unsigned iteration) {
    unsigned char *bytes = (unsigned char *)ctx->seed;
    for (size_t i = 0; i < sizeof(Cpu); ++i) bytes[i] = (unsigned char)(0x5a ^ i ^ iteration);
    for (unsigned i = 0; i < 32; ++i) ctx->seed->x[i] = UINT64_C(0x1111222233334444) ^ ((uint64_t)i << 40) ^ iteration;
    ctx->seed->x[0] = UINT64_MAX - iteration;
    ctx->seed->x[16] = (uintptr_t)ctx->snippet;
    ctx->seed->x[30] = (uintptr_t)nr_resume;
    ctx->seed->sp = 0; /* filled from the host-backed caller stack before guest entry */
    ctx->seed->pc = ctx->pc_cookie;
    ctx->seed->nzcv = 0xa0000000;
    ctx->seed->fpcr = 0;
    ctx->seed->fpsr = 1;
    ctx->seed->thread = NULL; /* no invented KThread or guest TLS allocation */
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t low = UINT64_C(0x1020304050607080) ^ ((uint64_t)i << 48) ^ iteration;
        uint64_t high = UINT64_C(0xf1e2d3c4b5a69788) ^ ((uint64_t)i << 40) ^ iteration;
        ctx->seed->v[i] = (__uint128_t)high << 64 | low;
    }
    memcpy(ctx->cpu, ctx->seed, sizeof(Cpu));
}
__attribute__((visibility("default"))) int native_runtime_bridge_probe(const char *runtime_path, char *output, size_t capacity) {
    if (!runtime_path || !output || capacity < 4096) return -1;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr(), tls = host_tls(), x18 = host_x18();
    void *runtime = NULL, *snippet = MAP_FAILED;
    const char *error = "none"; int result = 1, failures = 0, rx = 0, transitions = 0, clock_bounds = 0, progress = 0;
    long page = sysconf(_SC_PAGESIZE);
    NrContext ctx = {.pc_cookie = 0x425249444745u, .host_tls = tls, .host_x18 = x18};
    if (page < 4096 || (page & (page - 1))) { error = "host-page-size"; goto done; }
    /* Runtime ENV() caches and rt_ticks scale initialize lazily. A fresh helper
     * process plus serial priming is required; this is not an in-game API. */
    if (unsetenv("LARECOMP_PROF_SVC") || unsetenv("LARECOMP_TRACE_SVC") || setenv("LARECOMP_TICK_SCALE", "1", 1)) { error = "environment"; goto done; }
    runtime = dlopen(runtime_path, RTLD_NOW | RTLD_LOCAL);
    if (!runtime) { error = "runtime-dlopen"; goto done; }
    _Static_assert(sizeof(NrSvc) == sizeof(void *), "Android LP64 function pointer");
    void *entry = dlsym(runtime, "rt_svc"); memcpy(&ctx.svc, &entry, sizeof entry);
    if (!ctx.svc) { error = "production-rt_svc-not-exported"; goto done; }
    if (posix_memalign((void **)&ctx.cpu, _Alignof(Cpu), sizeof(Cpu)) ||
        posix_memalign((void **)&ctx.seed, _Alignof(Cpu), sizeof(Cpu)) ||
        posix_memalign((void **)&ctx.before, _Alignof(Cpu), sizeof(Cpu)) ||
        posix_memalign((void **)&ctx.observed, _Alignof(Cpu), sizeof(Cpu))) { error = "Cpu-allocation"; goto done; }
    memset(ctx.cpu, 0, sizeof(Cpu)); ctx.svc(ctx.cpu, 0x1e, ctx.pc_cookie); /* serial prime */
    uint32_t branch;
    snippet = map_snippet(page, &branch);
    if (snippet == MAP_FAILED) { error = "near-gateway-noreplace-map"; goto done; }
    memcpy(snippet, &branch, sizeof branch);
    __builtin___clear_cache(snippet, (char *)snippet + sizeof branch);
    if (mprotect(snippet, (size_t)page, PROT_READ | PROT_EXEC) || !(rx = is_rx(snippet, (size_t)page))) { error = "snippet-rx"; goto done; }
    ctx.snippet = snippet;
    uint64_t previous = 0;
    for (unsigned i = 0; i < 65; ++i) {
        if (i == 64) {
            struct timespec delay = {0, 2000000};
            while (nanosleep(&delay, &delay) && errno == EINTR) {}
        }
        seed_cpu(&ctx, i);
        uint64_t before = monotonic_ns();
        nr_enter(&ctx);
        uint64_t after = monotonic_ns(), value = ctx.cpu->x[0];
        int bounded = before && after >= before && ticks(before) <= value && value <= ticks(after);
        clock_bounds += bounded;
        failures += !bounded || (i && value < previous) || value == ctx.seed->x[0] || ctx.consumed != (value ^ 1u);
        if (i == 64) { progress = value > previous; failures += !progress; }
        previous = value;
        failures += host_tls() != tls || host_x18() != x18 || rt_get_fpcr() != fpcr || rt_get_fpsr() != fpsr;
        ++transitions;
    }
    failures += ctx.materialization_failures + ctx.production_mutations + ctx.resume_failures;
    result = failures != 0;
done:
    if (snippet != MAP_FAILED) munmap(snippet, (size_t)page);
    free(ctx.cpu); free(ctx.seed); free(ctx.before); free(ctx.observed);
    if (runtime) dlclose(runtime);
    rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    int host_ok = host_tls() == tls && host_x18() == x18 && rt_get_fpcr() == fpcr && rt_get_fpsr() == fpsr;
    if (!host_ok) result = 1;
    snprintf(output, capacity,
        "{\"schema\":1,\"state\":\"%s\",\"error\":\"%s\",\"failures\":%d,\"production_api\":\"rt_svc:SystemTick\","
        "\"native_transitions\":%d,\"clock_brackets_passed\":%d,\"clock_progress_after_sleep\":%s,"
        "\"rw_to_rx_verified\":%s,\"materialization_failures\":%llu,\"production_other_byte_mutations\":%llu,"
        "\"native_resume_failures\":%llu,\"host_tls_x18_fp_preserved\":%s,\"guest_x18_x19_virtual\":true,"
        "\"arbitrary_guest_stack_switch\":false,\"hardware_exclusive_monitor_preserved\":false,\"kernel_or_guest_initializers_called\":false}",
        result ? "failed" : "passed", error, failures, transitions, clock_bounds, progress ? "true" : "false", rx ? "true" : "false",
        (unsigned long long)ctx.materialization_failures, (unsigned long long)ctx.production_mutations,
        (unsigned long long)ctx.resume_failures, host_ok ? "true" : "false");
    return result;
}
