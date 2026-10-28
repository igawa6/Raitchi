#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "continuous_signals.h"
#include "continuous_context.h"
#include "kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define CONTINUOUS_MAX_SIGNAL_THREADS 512u
#define CONTINUOUS_ALTSTACK_BYTES (1024u * 1024u)
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal reasons require lock-free atomics");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "signal counters require lock-free atomics");
typedef struct SignalThread {
    Cpu *cpu;
    pthread_t host;
    _Atomic uint32_t pending, delivered;
} SignalThread;
static SignalThread threads[CONTINUOUS_MAX_SIGNAL_THREADS];
static _Atomic unsigned thread_count;
static _Thread_local SignalThread *current_slot;
static _Thread_local void *altstack_mapping;
static pthread_mutex_t install_lock = PTHREAD_MUTEX_INITIALIZER;
static ContinuousSignalHooks hooks;
static atomic_bool configured, installed;
static int (*kernel_action)(int,const struct sigaction *,struct sigaction *)=sigaction;
#if defined(__ANDROID__)
static void *signal_libc;
#endif
#if defined(__aarch64__)
static int fault_trace=-1;
static _Atomic unsigned fault_records;
#endif
static const int managed[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGURG};
static struct sigaction production_actions[sizeof managed / sizeof managed[0]];
#define SIGNAL_COUNTERS(X) X(interrupt_requests) X(interrupted_guests) \
    X(deferred_interrupts) X(chained_faults) X(resumed_faults)
#define DECLARE_COUNTER(name) static _Atomic unsigned long long counter_##name;
SIGNAL_COUNTERS(DECLARE_COUNTER)
#undef DECLARE_COUNTER

static SignalThread *slot_for(Cpu *cpu) {
    unsigned count = atomic_load_explicit(&thread_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) if (threads[i].cpu == cpu) return &threads[i];
    return NULL;
}

int continuous_signals_configure(const ContinuousSignalHooks *requested) {
    if (!requested || !requested->is_guest_pc || !requested->original_pc) return EINVAL;
    pthread_mutex_lock(&install_lock);
    if (atomic_load_explicit(&configured, memory_order_relaxed)) {
        pthread_mutex_unlock(&install_lock);
        return EALREADY;
    }
    /* ART interposes sigaction and inspects Android TLS before its user chain.
     * Install our assembly front at the kernel boundary, then chain the old ART
     * action only after restoring host TLS. Keep its registered user callback. */
#if defined(__ANDROID__)
    signal_libc=dlopen("libc.so",RTLD_NOW|RTLD_LOCAL);
    void *symbol=signal_libc?dlsym(signal_libc,"sigaction"):NULL;
    Dl_info provider={0};
    if(!symbol || !dladdr(symbol,&provider) || !provider.dli_fname ||
       !strstr(provider.dli_fname,"/libc.so")) {
        pthread_mutex_unlock(&install_lock);return ENOSYS;
    }
    kernel_action=(int (*)(int,const struct sigaction *,struct sigaction *))symbol;
#endif
    hooks = *requested;
#if defined(__aarch64__)
    const char *trace=getenv("LARECOMP_NCE_FAULT_TRACE");
    if(trace && *trace)fault_trace=open(trace,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
#endif
    atomic_store_explicit(&configured, true, memory_order_release);
    pthread_mutex_unlock(&install_lock);
    return 0;
}

static int install_altstack(void) {
    if (altstack_mapping) return 0;
    long raw_page = sysconf(_SC_PAGESIZE);
    if (raw_page < 4096 || ((unsigned long)raw_page & ((unsigned long)raw_page - 1))) return EINVAL;
    size_t page = (size_t)raw_page;
    size_t body = (CONTINUOUS_ALTSTACK_BYTES + page - 1) & ~(page - 1);
    void *mapping = mmap(NULL, body + page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return errno;
    void *stack = (unsigned char *)mapping + page;
    if (mprotect(stack, body, PROT_READ | PROT_WRITE)) {
        int error = errno; munmap(mapping, body + page * 2); return error;
    }
    stack_t description = {.ss_sp = stack, .ss_size = body, .ss_flags = 0};
    if (sigaltstack(&description, NULL)) {
        int error = errno; munmap(mapping, body + page * 2); return error;
    }
    /* Process-owned like the production per-KThread crash stack. It cannot be
     * freed by ExitThread's nonlocal unwind or while a signal uses its frames. */
    altstack_mapping = mapping;
    return 0;
}

int continuous_signals_install_thread(void (*production_install)(void)) {
    if (!production_install || !atomic_load_explicit(&configured, memory_order_acquire)) return EINVAL;
    pthread_mutex_lock(&install_lock);
    int error = 0;
    if (!atomic_load_explicit(&installed, memory_order_relaxed)) {
        production_install();
        for (size_t i = 0; i < sizeof managed / sizeof managed[0]; ++i) {
            if (kernel_action(managed[i], NULL, &production_actions[i])) { error = errno; break; }
            if ((void *)production_actions[i].sa_sigaction == (void *)continuous_context_signal_front) {
                error = ELOOP; break;
            }
        }
        if (!error) error = install_altstack();
        if (!error) {
            struct sigaction action = {0};
            action.sa_sigaction = (void (*)(int, siginfo_t *, void *))continuous_context_signal_front;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&action.sa_mask);
            for (size_t i = 0; i < sizeof managed / sizeof managed[0]; ++i) sigaddset(&action.sa_mask, managed[i]);
            /* Publish immutable actions before the first assembly front can run. */
            atomic_store_explicit(&installed, true, memory_order_release);
            for (size_t i = 0; i < sizeof managed / sizeof managed[0]; ++i)
                if (kernel_action(managed[i], &action, NULL)) { error = errno; break; }
        }
    } else error = install_altstack();
    Cpu *cpu = kernel_current_cpu();
    if (!error && cpu && !current_slot) {
        unsigned count = atomic_load_explicit(&thread_count, memory_order_relaxed);
        if (count == CONTINUOUS_MAX_SIGNAL_THREADS) error = ENOSPC;
        else {
            SignalThread *slot = &threads[count];
            slot->cpu = cpu; slot->host = pthread_self();
            current_slot = slot;
            atomic_store_explicit(&thread_count, count + 1, memory_order_release);
        }
    } else if (!error && cpu && current_slot && current_slot->cpu != cpu) error = EINVAL;
    pthread_mutex_unlock(&install_lock);
    return error;
}

int continuous_signals_interrupt_cpu(Cpu *cpu, uint32_t reasons) {
    if (!cpu || !reasons || (reasons & ~(CONTINUOUS_SIGNAL_STOP | CONTINUOUS_SIGNAL_PAUSE))) return EINVAL;
    SignalThread *slot = slot_for(cpu);
    if (!slot) return ESRCH;
    atomic_fetch_or_explicit(&slot->pending, reasons, memory_order_release);
    atomic_fetch_add_explicit(&counter_interrupt_requests, 1, memory_order_relaxed);
    return pthread_kill(slot->host, SIGURG);
}
uint32_t continuous_signals_pending(Cpu *cpu) {
    SignalThread *slot = slot_for(cpu);
    return slot ? atomic_load_explicit(&slot->pending, memory_order_acquire) : 0;
}
uint32_t continuous_signals_take_reason(Cpu *cpu) {
    SignalThread *slot = slot_for(cpu);
    if (!slot) return 0;
    return atomic_exchange_explicit(&slot->delivered, 0, memory_order_acq_rel) |
        atomic_exchange_explicit(&slot->pending, 0, memory_order_acq_rel);
}
void continuous_signals_stats(ContinuousSignalStats *out) {
    if (!out) return;
    out->registered_threads = atomic_load_explicit(&thread_count, memory_order_acquire);
#define READ_COUNTER(name) out->name = atomic_load_explicit(&counter_##name, memory_order_relaxed);
    SIGNAL_COUNTERS(READ_COUNTER)
#undef READ_COUNTER
}

__attribute__((noreturn)) static void fatal_default(int signal_number) {
    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)kernel_action(signal_number, &action, NULL);
    sigset_t unblock;
    sigemptyset(&unblock); sigaddset(&unblock, signal_number);
    (void)sigprocmask(SIG_UNBLOCK, &unblock, NULL);
    (void)syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal_number);
    _exit(128 + signal_number);
}

#if defined(__aarch64__)
typedef struct SignalRecord { uint32_t magic, size; } SignalRecord;
typedef struct __attribute__((aligned(16))) SignalFpsimd {
    SignalRecord head;
    uint32_t fpsr, fpcr;
    __uint128_t v[32];
} SignalFpsimd;
static int capture_guest(CnceContext *guest, void *raw_context) {
    ucontext_t *context = raw_context;
    unsigned char *reserved = (unsigned char *)context->uc_mcontext.__reserved;
    size_t remaining = sizeof context->uc_mcontext.__reserved;
    const SignalFpsimd *floating = NULL;
    while (remaining >= sizeof(SignalRecord)) {
        const SignalRecord *record = (const SignalRecord *)reserved;
        if (!record->magic && !record->size) break;
        if (record->size < sizeof *record || (record->size & 15) || record->size > remaining) return 0;
        if (record->magic == UINT32_C(0x46508001) && record->size >= sizeof(SignalFpsimd)) {
            floating = (const SignalFpsimd *)record; break;
        }
        reserved += record->size; remaining -= record->size;
    }
    if (!floating) return 0;
    Cpu *cpu = guest->cpu;
    memcpy(cpu->x, context->uc_mcontext.regs, 31 * sizeof(uint64_t));
    cpu->sp = context->uc_mcontext.sp;
    cpu->pc = context->uc_mcontext.pc;
    cpu->nzcv = context->uc_mcontext.pstate & UINT64_C(0xf0000000);
    cpu->fpcr = floating->fpcr; cpu->fpsr = floating->fpsr;
    memcpy(cpu->v, floating->v, sizeof cpu->v);
    /* Hardware TPIDR identifies private control storage. Both virtual guest TLS
     * values, opaque thread state and dispatch cache remain in Cpu untouched. */
    return 1;
}
#endif

int continuous_signals_dispatch(int signal_number, void *raw_info, void *raw_context, CnceContext *guest) {
#if defined(__aarch64__)
    /* Explicit diagnostic capture, before production fault routing. Fixed-size
     * records and pwrite remain usable if the ordinary log thread cannot run. */
    if(signal_number!=SIGURG && fault_trace>=0 && raw_context && raw_info) {
        unsigned slot=atomic_fetch_add_explicit(&fault_records,1,memory_order_relaxed);
        if(slot<64) {
            ucontext_t *uc=raw_context;
            uint64_t record[40]={UINT64_C(0x434e434546415531),signal_number,
                (uint64_t)(int64_t)((siginfo_t*)raw_info)->si_code,
                (uintptr_t)((siginfo_t*)raw_info)->si_addr,uc->uc_mcontext.pc,
                uc->uc_mcontext.sp,(uintptr_t)guest,(uint64_t)syscall(SYS_gettid),
                uc->uc_mcontext.pstate};
            memcpy(record+9,uc->uc_mcontext.regs,31*sizeof(uint64_t));
            (void)pwrite(fault_trace,record,sizeof record,(off_t)slot*sizeof record);
        }
    }
#endif
    if (!atomic_load_explicit(&installed, memory_order_acquire) || !hooks.is_guest_pc || !hooks.original_pc ||
        !raw_info || !raw_context)
        fatal_default(signal_number);
    if (signal_number == SIGURG) {
        SignalThread *slot = current_slot;
        if (!slot || !atomic_load_explicit(&slot->pending, memory_order_acquire)) return guest != NULL;
        int safe = 0;
#if defined(__aarch64__)
        ucontext_t *context = raw_context;
        if (guest && guest->cpu == slot->cpu && guest->magic == CNCE_CONTEXT_MAGIC && guest->self == guest &&
            !continuous_context_is_transition(context->uc_mcontext.pc) && hooks.is_guest_pc(context->uc_mcontext.pc))
            safe = capture_guest(guest, raw_context);
        if (safe) {
            uint32_t reasons = atomic_exchange_explicit(&slot->pending, 0, memory_order_acq_rel);
            atomic_fetch_or_explicit(&slot->delivered, reasons, memory_order_release);
            guest->event = NULL;
            context->uc_mcontext.pc = (uintptr_t)continuous_context_signal_return;
            context->uc_mcontext.regs[0] = (uintptr_t)guest;
            context->uc_mcontext.regs[18] = guest->host_x18;
            atomic_fetch_add_explicit(&counter_interrupted_guests, 1, memory_order_relaxed);
            return 0;
        }
#else
        (void)safe;
#endif
        atomic_fetch_add_explicit(&counter_deferred_interrupts, 1, memory_order_relaxed);
        return guest != NULL;
    }
    size_t selected = sizeof managed / sizeof managed[0];
    for (size_t i = 0; i < sizeof managed / sizeof managed[0]; ++i) if (managed[i] == signal_number) selected = i;
    if (selected == sizeof managed / sizeof managed[0]) fatal_default(signal_number);
    const struct sigaction *action = &production_actions[selected];
    if (action->sa_handler == SIG_DFL || action->sa_handler == SIG_IGN || !action->sa_handler)
        fatal_default(signal_number);
    atomic_fetch_add_explicit(&counter_chained_faults, 1, memory_order_relaxed);
#if defined(__aarch64__)
    ucontext_t *context = raw_context;
    uint64_t actual_pc = context->uc_mcontext.pc, original_pc = actual_pc;
    if (guest && guest->cpu) {
        (void)capture_guest(guest, raw_context);
        if (hooks.original_pc(actual_pc, &original_pc)) {
            guest->cpu->pc = original_pc;
            context->uc_mcontext.pc = original_pc;
        }
    }
#endif
    if (action->sa_flags & SA_SIGINFO) action->sa_sigaction(signal_number, raw_info, raw_context);
    else action->sa_handler(signal_number);
#if defined(__aarch64__)
    /* A dirty-page/watch handler returns to retry the actual faulting native
     * instruction. Diagnostic PC mapping must not change that continuation. */
    if (context->uc_mcontext.pc == original_pc) context->uc_mcontext.pc = actual_pc;
#endif
    atomic_fetch_add_explicit(&counter_resumed_faults, 1, memory_order_relaxed);
    return guest != NULL;
}
