/* Adapted from LAS Recomp Android, pinned cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

// One write per line: stderr is unbuffered (main.c / platform: crash logs aren't lost), and Windows' CRT then wrote a
// formatted line as dozens of separate WriteFile calls - the 2-second stats line cost ~40 ms, a hitch every 2 s.
static inline void __attribute__((format(printf, 1, 2))) rt_log(const char *fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof b) { fwrite(b, 1, (size_t)n, stderr); return; }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);  // (longer: as before)
    va_end(ap);
}
#define LOG(...) rt_log(__VA_ARGS__)
#define FATAL(...) (rt_fatal_msg(__VA_ARGS__), rt_fatal_trace())  // (runtime/crash_explain.c keeps the line)

// Immutable runtime-selected addresses. Never selected with a title compile flag.
#include "common_session.h"
static inline uint64_t shared_region_base(enum SessionRegionId id) {
    return shared_required_layout()->regions[id].lo;
}
static inline uint64_t shared_region_size(enum SessionRegionId id) {
    SessionRange region = shared_required_layout()->regions[id];
    return region.hi - region.lo;
}
static inline uint64_t shared_module_base(uint32_t id) {
    const SessionLayout *layout = shared_required_layout();
    for (uint32_t i = 0; i < layout->module_count; ++i)
        if (layout->modules[i].module_id == id) return layout->modules[i].image.lo;
    abort();
}
#define GUEST_LO (shared_required_layout()->window.lo)
#define GUEST_HI (shared_required_layout()->window.hi)
#define MAIN_BASE shared_module_base(MODULE_ABI_MAIN)
#define SDK_BASE shared_module_base(MODULE_ABI_SDK)
#define HLE_BASE shared_region_base(SESSION_HLE)
#define HLE_SIZE shared_region_size(SESSION_HLE)
#define TLS_BASE shared_region_base(SESSION_TLS)
#define TLS_SIZE shared_region_size(SESSION_TLS)
#define HLE_HEAP shared_region_base(SESSION_HOST_HEAP)
#define HLE_HEAP_SIZE shared_region_size(SESSION_HOST_HEAP)
#define ALIAS_BASE shared_region_base(SESSION_ALIAS)
#define ALIAS_SIZE shared_region_size(SESSION_ALIAS)
#define HEAP_BASE shared_region_base(SESSION_APP_HEAP)
#define HEAP_MAX shared_region_size(SESSION_APP_HEAP)

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[4];
extern int g_nmodules;
void loader_load(const char *exefs_dir);  // (again after a reboot: modules are reloaded into wiped memory)
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t hle_register(const char *name, HostFn fn);  // returns fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
// runtime/crash_explain.c: the fatal line / crash spot, and the player's explanation of it (the crash dialog)
void rt_fatal_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc);
void rt_crash_explain(char *out, size_t n, const char *log_path);
extern int rt_romfs_files;
void crash_handler_install(void);  // per thread (alternate signal stack)

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
void kernel_stop_threads(void (*idle)(void));  // reboot: unwind every guest thread and join it (idle: called while waiting)
void kernel_reset(void);                       // reboot: empty handle table, memory map, thread list
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio);
void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- in-place reboot (main.c): switch seeds or go back to the start screen without leaving the process
enum { RT_REBOOT_NONE, RT_REBOOT_GAME, RT_REBOOT_TITLE };  // GAME: boot again at once (new seed); TITLE: start screen
void rt_request_reboot(int kind);  // any thread; the window loop (ui_run) returns it and main() carries it out
int rt_reboot_requested(void);
extern volatile int g_rt_unwind;   // set while guest threads are being stopped (kernel.c)
void rt_unwind_point(void);        // guest threads: unwind to thread_main if a reboot is stopping them

// ---- ui (runtime/nvn/metal.m)
int ui_run(void);  // window loop; returns the reboot kind requested (the window stays open)

// ---- helpers
void guest_call(Cpu *c, uint64_t addr);   // call guest function at addr (saves/restores x30)
uint64_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
void *hle_alloc(uint64_t size);           // zeroed guest-visible memory (never freed)
extern const char *g_romfs_dir;
