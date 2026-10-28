/* Adapted from LAS Recomp dispatch.c, Copyright (c) 2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. Shared AOT owns execution/cache/hooks. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gamelib.h"
#include "hle.h"
#include "runtime/aot_dispatch.h"
#include "native_session.h"
#include "continuous_signals.h"
// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static _Atomic int hle_count;
static pthread_mutex_t hle_names_lock = PTHREAD_MUTEX_INITIALIZER;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("LARECOMP_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}


uint64_t hle_register(const char *name, HostFn fn) {
    if (!fn) FATAL("unimplemented strong guest import %s\n", name);
    pthread_mutex_lock(&hle_names_lock);
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) {
            pthread_mutex_unlock(&hle_names_lock);
            return HLE_BASE + 16ull * i;
        }
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    uint64_t address = HLE_BASE + 16ull * hle_count;
    if (aot_dispatch_add_hle(address, fn) != AOT_OK) FATAL("cannot bind HLE %s\n", name);
    atomic_fetch_add(&hle_count, 1);
    pthread_mutex_unlock(&hle_names_lock);
    return address;
}

// (diagnostics) the host function behind an HLE stub address, NULL if pc isn't one
const char *hle_name_at(uint64_t pc) {
    if (pc < HLE_BASE || pc >= HLE_BASE + HLE_SIZE) return NULL;
    int i = (int)((pc - HLE_BASE) / 16);
    return i < hle_count ? hle_slots[i].name : NULL;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (aot_dispatch_override(addr, fn) != AOT_OK) FATAL("invalid override %s at %llx\n", name, addr);
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

static uint64_t hle_next;
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (!hle_next) hle_next = HLE_HEAP;
    uint64_t a = hle_next;
    if (!size || size > UINT64_MAX - 63 || ((size + 63) & ~63ull) > HLE_HEAP + HLE_HEAP_SIZE - hle_next)
        FATAL("HLE heap exhausted\n");
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

static void hle_safe_point(Cpu *cpu) { (void)cpu; rt_unwind_point(); }

void dispatch_init(void) {
    if (aot_dispatch_set_hle_guard(hle_safe_point) != AOT_OK) FATAL("cannot configure HLE safe point\n");
    if (!aot_dispatch_is_bound()) FATAL("shared execution registry is unbound\n");
    shared_las_hooks_register();
    shared_profiler_initialize();
    extern const char *rt_host_fn_name(uint64_t, uint64_t *);
    (void)rt_host_fn_name(0, NULL); /* initialize immutable diagnostics before guest threads */
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        const ModuleAbiDescriptor *descriptor = shared_gamelib_descriptor();
        if (!descriptor) return NULL;
        uint64_t total = 0, k = 0;
        for (uint32_t t = 0; t < descriptor->module_count; ++t) total += descriptor->modules[t].address_count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (uint32_t t = 0; t < descriptor->module_count; ++t)
            for (uint64_t i = 0; i < descriptor->modules[t].address_count; ++i) {
                v[k][0] = (uint64_t)(uintptr_t)descriptor->modules[t].functions[i];
                v[k++][1] = descriptor->modules[t].addresses[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    // (game functions first: they live in lasgame.dll, which the OS lookup can only name as a whole)
    const char *s = rt_host_fn_name(pc, offset);
    return s ? s : plat_symbol(pc, offset);
}

void rt_fatal_trace(void) {
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    Cpu *c = kernel_current_cpu();
    if (c) {
        fprintf(stderr, "guest: sp=%llx lr=%llx\n", c->sp, c->x[30]);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoint: the page is protected; accesses in [lo, hi) are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the next re-arm.
uint64_t g_watch_lo, g_watch_hi;
static uint64_t watch_page;
static void *watch_thread(void *a) {
    (void)a;
    int r = plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE);
    fprintf(stderr, "[watch] page %llx protect -> %d\n", (unsigned long long)watch_page, r);
    for (;;) { plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE); usleep(100); }
    return NULL;
}
void watch_arm(void) {
    if (!g_watch_lo || watch_page) return;
    watch_page = g_watch_lo & ~(PLAT_PAGE - 1);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    if (!watch_page || fa < watch_page || fa >= watch_page + PLAT_PAGE) return false;
    if (fa >= g_watch_lo && fa < g_watch_hi) {
        static char seen[64][48];
        static int nseen;
        const char *nm = host_sym(pc, NULL);
        if (!nm) nm = "?";
        char key[48];  // (each function + offset once)
        snprintf(key, sizeof key, "%.36s+%llx", nm, (unsigned long long)(fa - g_watch_lo));
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= !strcmp(seen[i], key);
        if (!found && nseen < 64) {
            snprintf(seen[nseen++], 48, "%s", key);
            fprintf(stderr, "[watch] +%#llx by %s\n", fa - g_watch_lo, nm);
        }
    }
    plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    rt_crash_fault(what, fa, pc);
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) {
    if(native_session_continuous()) {
        if(continuous_signals_install_thread(plat_crash_handler_install))
            FATAL("cannot install continuous native thread signal front\n");
    } else plat_crash_handler_install();
}
