/* Adapted from LAS Recomp Android, pinned cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"
#include "kernel_thread_affinity.h"

/* SDK16.2 startup includes a genuine fini callback in guest x4. Preserve the
 * four-argument kernel_run_main ABI for existing LAS/Eevee callers. */
void kernel_run_main5(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2,
                      uint64_t x3, uint64_t x4);

#define STACK_BASE shared_region_base(SESSION_STACK)
#define STACK_SIZE shared_region_size(SESSION_STACK)

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
#ifdef _WIN32
    uintptr_t host;
#else
    pthread_t host;
#endif
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;
    bool cancel, in_sync_wait;
    bool is_main;  // the process's main thread (kernel_create_main_thread)
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished, joined;
    KThread *anext;        // every thread of this boot (g_klock)
    // (diagnostics: kernel_dump_threads) when the current kernel wait began; the handles of a WaitSynchronization
    uint64_t wait_since;
    uint64_t sync_handles;
    int32_t sync_count;
    // Logical guest scheduling metadata; append to preserve every prior offset.
    KernelThreadAffinity affinity;
};

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
void kernel_close_service_handle(uint32_t handle, KObject *expected);
KThread *kernel_current_thread(void);
/* Private embedding support; no public KThread layout is exported. */
bool kernel_threads_all_finished(void);
// (diagnostics) the "[hang]" dump (kernel.c): every guest thread and what it waits on, the kernel trace
void kernel_dump_threads(const char *prefix);
// The renderer's part of the hang dump (runtime/nvn/rqueue.c), called from the hang watch thread every second:
// check fills why and returns true when it sees a hang (limit_ns: LARECOMP_HANG_DUMP); state logs its side.
void kernel_hang_hooks(bool (*check)(uint64_t limit_ns, char *why, size_t n), void (*state)(const char *prefix));
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize

/* Configure validated NPDM process resources before guest SDK startup. */
int kernel_configure_system_resources(uint32_t size);
/* Validated NPDM default core; inherited emulated allowed mask remains0xf. */
int kernel_configure_thread_core(uint32_t default_core);

#define KERNEL_READABLE_COPY_MAX (UINT64_C(16) * 1024 * 1024)
/* Snapshot a nonempty readable guest span into a caller-owned writable buffer.
 * Mapping validation and copy share the memory lock. Failure changes no output;
 * free/reserved spans, holes, overflow, overlap and spans over the cap reject.
 * Caller guarantees destination capacity and immutable source contents during
 * copy; this lock protects mapping lifetime, not concurrent guest CPU stores. */
bool kernel_copy_readable(void *destination, uint64_t guest_address, uint64_t size);
