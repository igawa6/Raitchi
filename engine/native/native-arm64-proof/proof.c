#define _GNU_SOURCE
#include "proof.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

_Static_assert(offsetof(NapContext, flags) == 64, "assembly flags");
_Static_assert(offsetof(NapContext, initial_fpcr) == 112, "assembly fpcr");
_Static_assert(offsetof(NapContext, ready) == 144, "assembly ready");
_Static_assert(offsetof(NapContext, release) == 148, "assembly release");

static pthread_mutex_t nap_run_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int nap_start;
static _Atomic unsigned nap_atomic_ready;
static uint64_t nap_scalar;
static _Alignas(16) uint64_t nap_pair[2];
static _Atomic(NapContext *) nap_signaling;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "signal-safe atomics");
enum { NAP_CALLBACKS = 128, NAP_INCREMENTS = 20000 };

static inline uint64_t nap_host_tls(void) {
    uint64_t value;
    __asm__ volatile("mrs %0,tpidr_el0" : "=r"(value));
    return value;
}
static inline uint64_t nap_host_x18(void) {
    uint64_t value;
    __asm__ volatile("mov %0,x18" : "=r"(value));
    return value;
}

/* No libc, TLS lookup, locks or allocations in the signal handler. */
static void nap_signal(int number, siginfo_t *info, void *raw) {
    (void)number; (void)info;
    NapContext *ctx = atomic_load_explicit(&nap_signaling, memory_order_acquire);
    if (!ctx) return;
    ucontext_t *uc = raw;
    uint64_t pc = uc->uc_mcontext.pc;
    ctx->signal_ok = pc >= (uintptr_t)nap_signal_begin && pc < (uintptr_t)nap_signal_end
        && uc->uc_mcontext.regs[18] == ctx->host_x18
        && nap_host_x18() == ctx->host_x18 && nap_host_tls() == ctx->host_tls;
    ctx->signal_seen = 1;
    atomic_store_explicit(&ctx->release, 1, memory_order_release);
}

static void nap_callback(NapContext *ctx) {
    if (nap_host_tls() != ctx->host_tls || nap_host_x18() != ctx->host_x18) ++ctx->thread_failures;
    uint64_t desired = ctx->guest_x18 + 7;
    if (nap_call(ctx, ctx->write_code, desired) != desired) ++ctx->thread_failures;
    ctx->nested_result = nap_call(ctx, ctx->read_code, 0);
    if (ctx->nested_result != ctx->guest_tls + desired || ctx->guest_x18 != desired) ++ctx->thread_failures;
    ++ctx->callbacks;
    /* Deliberately destroy tested caller state. The assembly gate must restore it. */
    __asm__ volatile("movi v0.16b,#0x11\n\t"
                     "movi v31.16b,#0x22\n\t"
                     "mrs x9,fpcr\n\t"
                     "eor x9,x9,#0x400000\n\t"
                     "msr fpcr,x9\n\t"
                     "mov x9,#1\n\t"
                     "msr fpsr,x9\n\t"
                     "cmp x9,#0" ::: "x9", "v0", "v31", "cc", "memory");
}

static void *nap_worker(void *argument) {
    NapContext *ctx = argument;
    int go;
    while (!(go = atomic_load_explicit(&nap_start, memory_order_acquire))) sched_yield();
    if (go < 0) return NULL;
    ctx->host_tls = nap_host_tls();
    ctx->host_x18 = nap_host_x18();
    for (unsigned i = 0; i < NAP_CALLBACKS; ++i) {
        if (nap_call(ctx, ctx->read_code, 0) != ctx->guest_tls + ctx->guest_x18) ++ctx->thread_failures;
        nap_live(ctx, nap_callback);
        if (ctx->flags != 0x60000000 || ctx->live_reg != 0x1234 ||
            ctx->v0[0] != UINT64_C(0x5a5a5a5a5a5a5a5a) || ctx->v0[1] != ctx->v0[0] ||
            ctx->v31[0] != UINT64_C(0xa5a5a5a5a5a5a5a5) || ctx->v31[1] != ctx->v31[0] ||
            ctx->initial_fpcr != ctx->final_fpcr || ctx->initial_fpsr != ctx->final_fpsr ||
            nap_host_tls() != ctx->host_tls || nap_host_x18() != ctx->host_x18) ++ctx->thread_failures;
    }
    atomic_fetch_add_explicit(&nap_atomic_ready, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&nap_atomic_ready, memory_order_acquire) != 2) sched_yield();
    for (unsigned i = 0; i < NAP_INCREMENTS; ++i) {
        uint64_t scalar = nap_atomic_increment(&nap_scalar), pair = nap_pair_increment(nap_pair);
        if (scalar == UINT64_MAX || pair == UINT64_MAX) ++ctx->thread_failures;
        else { ctx->scalar_retries += scalar; ctx->pair_retries += pair; }
    }
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGUSR2);
    if (pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL)) ++ctx->thread_failures;
    nap_signal_wait(ctx);
    if (nap_host_tls() != ctx->host_tls || nap_host_x18() != ctx->host_x18) ++ctx->thread_failures;
    return NULL;
}

/* Read back the actual published mapping permissions, rather than only claiming
 * the requested mprotect flags. Linux procfs is part of this Android-only proof. */
static int nap_mapping_rx(const void *address, size_t size) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], permissions[5];
    unsigned long long start, end;
    int ok = 0;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3 &&
            start <= (uintptr_t)address && end >= (uintptr_t)address + size) {
            ok = permissions[0] == 'r' && permissions[1] == '-' && permissions[2] == 'x';
            break;
        }
    }
    fclose(maps);
    return ok;
}

__attribute__((visibility("default")))
int native_arm64_proof_run(char *output, size_t capacity) {
    if (!output || capacity < 1024) return -1;
    if (pthread_mutex_trylock(&nap_run_lock)) {
        snprintf(output, capacity, "{\"state\":\"busy\"}");
        return -1;
    }
    int result = 1, failures = 0, created = 0, installed = 0, rx = 0;
    const char *error = "none";
    uint64_t caller_tls = nap_host_tls(), caller_x18 = nap_host_x18();
    long page = sysconf(_SC_PAGESIZE);
    void *code = MAP_FAILED;
    struct sigaction old_action, action = {0};
    pthread_t threads[2];
    NapContext contexts[2] = {{0}};
    const uint32_t read_source[] = {0xd53bd049, 0xaa1203ea, 0x8b0a0120, 0xd65f03c0};
    const uint32_t write_source[] = {0xaa0003f2, 0xaa1203e0, 0xd65f03c0};
    uint32_t read_words[4], write_words[3];
    if (page < 4096 || (page & (page - 1))) { error = "host-page-size"; goto done; }
    if (nap_rewrite(read_source, 4, read_words) || nap_rewrite(write_source, 3, write_words)) {
        error = "rewrite"; goto done;
    }
    code = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { error = "mmap"; goto done; }
    memcpy(code, read_words, sizeof read_words);
    memcpy((char *)code + 64, write_words, sizeof write_words);
    __builtin___clear_cache(code, (char *)code + page);
    if (mprotect(code, (size_t)page, PROT_READ | PROT_EXEC)) { error = "mprotect-rx"; goto done; }
    rx = nap_mapping_rx(code, (size_t)page);
    if (!rx) { error = "mapping-not-rx"; goto done; }
    action.sa_sigaction = nap_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, &old_action)) { error = "sigaction"; goto done; }
    installed = 1;
    nap_scalar = nap_pair[0] = nap_pair[1] = 0;
    atomic_store(&nap_start, 0);
    atomic_store(&nap_atomic_ready, 0);
    for (unsigned i = 0; i < 2; ++i) {
        contexts[i].guest_tls = UINT64_C(0xabc00000) + i * 0x10000;
        contexts[i].guest_x18 = UINT64_C(0x18000000) + i * 0x100;
        contexts[i].read_code = (NapCode)code;
        contexts[i].write_code = (NapCode)((char *)code + 64);
        if (pthread_create(&threads[i], NULL, nap_worker, &contexts[i])) { error = "pthread-create"; break; }
        ++created;
    }
    atomic_store_explicit(&nap_start, created == 2 ? 1 : -1, memory_order_release);
    if (created == 2) {
        for (unsigned i = 0; i < 2; ++i) {
            unsigned ticks = 0;
            while (!atomic_load_explicit(&contexts[i].ready, memory_order_acquire) && ticks++ < 5000) usleep(1000);
            if (!atomic_load_explicit(&contexts[i].ready, memory_order_acquire)) {
                error = "worker-timeout"; ++failures;
                atomic_store_explicit(&contexts[i].release, 1, memory_order_release);
                continue;
            }
            atomic_store_explicit(&nap_signaling, &contexts[i], memory_order_release);
            if (pthread_kill(threads[i], SIGUSR2)) {
                error = "pthread-kill"; ++failures;
                atomic_store_explicit(&contexts[i].release, 1, memory_order_release);
            }
            ticks = 0;
            while (!atomic_load_explicit(&contexts[i].release, memory_order_acquire) && ticks++ < 5000) usleep(1000);
            if (!atomic_load_explicit(&contexts[i].release, memory_order_acquire)) {
                error = "signal-timeout"; ++failures;
                atomic_store_explicit(&contexts[i].release, 1, memory_order_release);
            }
            if (pthread_join(threads[i], NULL)) { error = "pthread-join"; ++failures; }
            threads[i] = (pthread_t)0;
        }
    }
    for (unsigned i = 0; i < (unsigned)created; ++i) {
        atomic_store_explicit(&contexts[i].release, 1, memory_order_release);
        if (threads[i] && pthread_join(threads[i], NULL)) ++failures;
    }
    atomic_store_explicit(&nap_signaling, NULL, memory_order_release);
    for (unsigned i = 0; i < 2; ++i) {
        failures += (int)contexts[i].thread_failures;
        failures += contexts[i].callbacks != NAP_CALLBACKS || !contexts[i].signal_seen || !contexts[i].signal_ok;
        failures += contexts[i].guest_tls != UINT64_C(0xabc00000) + i * 0x10000;
        failures += contexts[i].guest_x18 != UINT64_C(0x18000000) + i * 0x100 + NAP_CALLBACKS * 7;
    }
    failures += nap_scalar != 2 * NAP_INCREMENTS || nap_pair[0] != 2 * NAP_INCREMENTS || nap_pair[1] != 6 * NAP_INCREMENTS;
    failures += caller_tls != nap_host_tls() || caller_x18 != nap_host_x18();
    result = failures || created != 2;
done:
    if (installed) sigaction(SIGUSR2, &old_action, NULL);
    if (code != MAP_FAILED) munmap(code, (size_t)page);
    snprintf(output, capacity,
        "{\"schema\":1,\"state\":\"%s\",\"error\":\"%s\",\"failures\":%d,"
        "\"host_page_size\":%ld,\"rw_to_rx_verified\":%s,\"threads\":%d,"
        "\"callbacks\":%llu,\"signals\":%d,\"signal_contexts_ok\":%d,"
        "\"scalar_increments\":%llu,\"pair_low\":%llu,\"pair_high\":%llu,"
        "\"scalar_exclusive_retries\":%llu,\"pair_exclusive_retries\":%llu,"
        "\"host_tls_preserved\":%s,\"host_x18_preserved\":%s}",
        result ? "failed" : "passed", error, failures, page, rx ? "true" : "false", created,
        (unsigned long long)(contexts[0].callbacks + contexts[1].callbacks),
        contexts[0].signal_seen + contexts[1].signal_seen, contexts[0].signal_ok + contexts[1].signal_ok,
        (unsigned long long)nap_scalar, (unsigned long long)nap_pair[0], (unsigned long long)nap_pair[1],
        (unsigned long long)(contexts[0].scalar_retries + contexts[1].scalar_retries),
        (unsigned long long)(contexts[0].pair_retries + contexts[1].pair_retries),
        caller_tls == nap_host_tls() ? "true" : "false", caller_x18 == nap_host_x18() ? "true" : "false");
    pthread_mutex_unlock(&nap_run_lock);
    return result;
}
