/* Actual host POSIX signal routing/altstack tests. The assembly front and
 * kernel_current_cpu are explicit stand-ins: no guest TLS/ARM execution claim. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "continuous_signals.h"
#include "continuous_context.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned assertions;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "continuous signals line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
static _Thread_local Cpu *bound_cpu;
static _Thread_local stack_t expected_stack;
static _Thread_local volatile sig_atomic_t handled, on_altstack, mask_ok;
static unsigned installer_calls;
Cpu *kernel_current_cpu(void) { return bound_cpu; }
void continuous_context_signal_front(int signal_number, void *info, void *context) {
    (void)continuous_signals_dispatch(signal_number, info, context, NULL);
}
void continuous_context_signal_return(CnceContext *context) { (void)context; abort(); }
int continuous_context_is_transition(uint64_t pc) { (void)pc; return 0; }
static int guest_pc(uint64_t pc) { (void)pc; return 1; }
static int original_pc(uint64_t pc, uint64_t *original) { *original = pc; return 0; }
static void production_handler(int signal_number, siginfo_t *info, void *context) {
    (void)signal_number; (void)info; (void)context;
    /* ASan may place local objects on a fake stack; inspect the actual frame. */
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    on_altstack = here >= (uintptr_t)expected_stack.ss_sp &&
        here < (uintptr_t)expected_stack.ss_sp + expected_stack.ss_size;
    sigset_t mask;
    sigprocmask(SIG_SETMASK, NULL, &mask);
    const int managed[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGURG};
    mask_ok = 1;
    for (unsigned i = 0; i < sizeof managed / sizeof managed[0]; ++i)
        if (sigismember(&mask, managed[i]) != 1) mask_ok = 0;
    ++handled;
}
static void production_install(void) {
    ++installer_calls;
    struct sigaction action = {0};
    action.sa_sigaction = production_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    const int production[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof production / sizeof production[0]; ++i)
        if (sigaction(production[i], &action, NULL)) abort();
}
static void *worker(void *argument) {
    bound_cpu = argument;
    if (continuous_signals_install_thread(production_install) || sigaltstack(NULL, &expected_stack)) return (void *)1;
    if (expected_stack.ss_size < 1024u * 1024u || (expected_stack.ss_flags & SS_DISABLE)) return (void *)2;
    raise(SIGSEGV);
    return handled == 1 && on_altstack && mask_ok ? NULL : (void *)3;
}
static void child_fault(int operation) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (operation == 0) raise(SIGTRAP); /* Captured default must terminate, not return. */
        else {
            signal(SIGSEGV, SIG_DFL);
            volatile unsigned char *where = (unsigned char *)expected_stack.ss_sp;
            if (operation == 1) --where;
            else where += expected_stack.ss_size;
            *where = 1;
        }
        _exit(77);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == (operation ? SIGSEGV : SIGTRAP));
}
int main(void) {
    Cpu *main_cpu = aligned_alloc(_Alignof(Cpu), sizeof(Cpu));
    Cpu *other_cpu = aligned_alloc(_Alignof(Cpu), sizeof(Cpu));
    CHECK(main_cpu && other_cpu);
    bound_cpu = main_cpu;
    CHECK(continuous_signals_configure(NULL) == EINVAL);
    ContinuousSignalHooks invalid = {.is_guest_pc = guest_pc};
    CHECK(continuous_signals_configure(&invalid) == EINVAL);
    ContinuousSignalHooks hooks = {.is_guest_pc = guest_pc, .original_pc = original_pc};
    CHECK(!continuous_signals_configure(&hooks));
    CHECK(continuous_signals_configure(&hooks) == EALREADY);
    CHECK(continuous_signals_install_thread(NULL) == EINVAL);
    CHECK(!continuous_signals_install_thread(production_install));
    CHECK(installer_calls == 1);
    CHECK(!sigaltstack(NULL, &expected_stack) && expected_stack.ss_size >= 1024u * 1024u);
    CHECK(!(expected_stack.ss_flags & SS_DISABLE));
    CHECK(!raise(SIGSEGV));
    CHECK(handled == 1 && on_altstack && mask_ok);
    CHECK(!continuous_signals_install_thread(production_install) && installer_calls == 1);
    const int managed[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGURG};
    for (unsigned i = 0; i < sizeof managed / sizeof managed[0]; ++i) {
        struct sigaction action;
        CHECK(!sigaction(managed[i], NULL, &action));
        CHECK((action.sa_flags & (SA_SIGINFO | SA_ONSTACK)) == (SA_SIGINFO | SA_ONSTACK));
        CHECK(!(action.sa_flags & SA_NODEFER));
        CHECK((void *)action.sa_sigaction == (void *)continuous_context_signal_front);
        for (unsigned j = 0; j < sizeof managed / sizeof managed[0]; ++j)
            CHECK(sigismember(&action.sa_mask, managed[j]) == 1);
    }
    CHECK(continuous_signals_interrupt_cpu(NULL, CONTINUOUS_SIGNAL_STOP) == EINVAL);
    CHECK(continuous_signals_interrupt_cpu(other_cpu, CONTINUOUS_SIGNAL_STOP) == ESRCH);
    CHECK(!continuous_signals_interrupt_cpu(main_cpu, CONTINUOUS_SIGNAL_STOP));
    CHECK(continuous_signals_pending(main_cpu) == CONTINUOUS_SIGNAL_STOP);
    CHECK(!continuous_signals_interrupt_cpu(main_cpu, CONTINUOUS_SIGNAL_PAUSE));
    CHECK(continuous_signals_take_reason(main_cpu) == (CONTINUOUS_SIGNAL_STOP | CONTINUOUS_SIGNAL_PAUSE));
    CHECK(!continuous_signals_pending(main_cpu));
    pthread_t thread; void *result;
    CHECK(!pthread_create(&thread, NULL, worker, other_cpu));
    CHECK(!pthread_join(thread, &result) && !result && installer_calls == 1);
    child_fault(0); child_fault(1); child_fault(2);
    ContinuousSignalStats stats;
    continuous_signals_stats(&stats);
    CHECK(stats.registered_threads == 2 && stats.chained_faults == 2 && stats.resumed_faults == 2);
    CHECK(stats.interrupt_requests == 2 && stats.deferred_interrupts == 2 && !stats.interrupted_guests);
    free(main_cpu); free(other_cpu);
    printf("continuous signal routing assertions=%u; POSIX host routing/guard tests, assembly/kernel stand-ins only\n", assertions);
    return 0;
}
