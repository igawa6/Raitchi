// Adapted from LAS Recomp rqueue.c, Copyright (c) 2026 Peiikai.
// MIT: native/runtime/LICENSE-LASRecomp.
// The render thread: see rqueue.h.
//
// The game's GPU thread records command lists and submits them; translating them into Metal / Vulkan work took
// milliseconds of that thread's frame. Now nvnQueueSubmitCommands queues the list and returns, and this thread
// translates and submits it. Everything the translation reads (command memory, constant and vertex buffers,
// textures, descriptor pools) is memory the Switch GPU would read while executing the list, which the game keeps
// intact until the fence after it signals: a fence here means "translated and submitted" (rq_wait_item) and then
// "executed" (the backend's own wait on the submission that item ended with), so the game's memory discipline
// holds as it did. The present, the texture destruction (recorded lists hold host NvnTexture pointers) and movie
// frame uploads are queued too, so they keep their order against the lists.
//
// Back-pressure: a present waits until the previous present is done (the thread then always holds about a frame
// of work; latency stays one frame), and the queue has a fixed size. Waits by guest threads give up while the
// kernel stops them for a reboot (g_rt_unwind), as kernel waits do.
//
// Hang dump: part of runtime/kernel.c's "[hang]" watch (LARECOMP_HANG_DUMP=<seconds>, default 30, 0 = off; this side
// armed at the game's GPU device setup, render thread on or off): when no frame was presented twice that long after
// the device setup, presenting stops for that long, or a wait noted with rq_watch() lasts that long, the dump adds
// the queue (counts, the last items, the one running and for how long) and what each noted thread waits on to the
// guest threads' kernel waits - so a hang explains itself.
#include "rqueue.h"
#include "command_recording.h"

#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../platform.h"
#include "../rt.h"

__thread uint64_t g_rq_wait_ns;
__thread uint64_t g_rq_lock_ns;
__thread uint64_t g_present_ns, g_hooks_ns;  // (the backends' backend_present)
extern __thread uint64_t g_blocked_ns;  // kernel.c: idle time of the presenting thread (frame pacing)

enum { IT_EXEC, IT_PRESENT, IT_DESTROY, IT_UPLOAD, IT_FLUSH, IT_FREE, IT_CALL, IT_FENCE, IT_N };
static const char *const kind_name[IT_N] = {"exec", "present", "destroy", "upload", "flush", "free", "call", "fence"};
typedef struct {
    int kind;
    uint64_t handle;  // EXEC
    NvnTexture *tex;  // PRESENT / DESTROY / UPLOAD
    uint64_t frame;   // PRESENT
    bool advance;     // PRESENT: a new frame (false: the last one again, backend_represent)
    int up_kind;      // UPLOAD
    void *data;
    uint32_t w, h, stride, uv_off;
    void (*fn)(void *);  // CALL: fn(data); FREE: free(data)
    NvnClLease *lease; // optional coherent CPU reader; NULL for legacy/non-list items
} Item;

#define RQ_CAP 1024  // items queued at most (power of two)
#define RQ_HIST 4096
static Item items[RQ_CAP];
static uint64_t gpu_seq_of[RQ_HIST];  // per item number: the backend submission it ended with
static uint8_t kind_of[RQ_HIST];      // (hang dump) per item number: its kind
static uint64_t enq_ns[RQ_HIST], done_ns[RQ_HIST];  // (LARECOMP_LOG_PACE fence stats) per item: queued, translated
static uint64_t q_enq, q_done, q_cur;  // items queued / finished / in progress (0: none)
static int presents_pending;
static uint64_t last_present_q;  // (LARECOMP_LOG_PACE fence stats) the latest present's item
extern __thread uint64_t rq_fence_q;
uint64_t rq_trace_t0;  // (debug: LARECOMP_RQ_TRACE's time base)
static uint64_t exec_frame;  // (the render thread) the frame whose lists it runs now: see rq_frame
static _Atomic uint64_t presents_done, cur_since;  // (hang dump) presents the thread finished; when q_cur started
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static _Atomic bool started;
static _Atomic int on = -1;
static _Atomic bool stopping, parked;  // (quitting: rq_stop) the thread is to stop / has stopped between items

static void *run(void *arg);
static _Atomic uint64_t render_cpu_ns;
uint64_t rq_render_cpu_ns(void) { return atomic_load_explicit(&render_cpu_ns, memory_order_relaxed); }

// Started once, by whichever thread asks first: the game's threads get here concurrently at boot (the GPU thread's
// first submit or fence, a loader thread's texture finalize, the movie thread's frame), and two callers each starting
// a render thread had two threads running the same items at the same time.
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void start(void) {
    extern uint64_t kernel_current_thread_id(void);
    uint64_t asker = kernel_current_thread_id();
    if (getenv("LARECOMP_RQ_START_DELAY_MS")) usleep((unsigned)atoi(getenv("LARECOMP_RQ_START_DELAY_MS")) * 1000u);  // (test: the race)
    const char *e = getenv("LARECOMP_RENDER_THREAD");
    int want = !e || atoi(e) != 0;
    if (want) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 << 20);
        rq_watch("starting the render thread", 0);
        int r = pthread_create(&thread, &attr, run, NULL);
        rq_watch(NULL, 0);
        if (r != 0) {
            LOG("[rq] cannot start the render thread: running on the game thread\n");
            want = 0;
        } else {
            atomic_store(&started, true);
        }
        pthread_attr_destroy(&attr);
    }
    LOG("[rq] render thread %s (first asked by guest thread %llu)\n", want ? "on" : "off", (unsigned long long)asker);
    atomic_store(&on, want);
    rq_hang_watch_start();
}

extern uint64_t kernel_current_thread_id(void);
bool rq_on(void) {
    int v = atomic_load_explicit(&on, memory_order_acquire);
    if (__builtin_expect(v < 0, 0)) {
        static _Atomic int askers;
        int n = atomic_fetch_add(&askers, 1);
        pthread_once(&once, start);
        if (n == 1)
            LOG("[rq] guest thread %llu asked too while the render thread was being started (started once)\n",
                (unsigned long long)kernel_current_thread_id());
        v = atomic_load(&on);
    }
    return v > 0;
}

// ---- what threads wait on (the hang dump's view)
#define NWATCH 48
typedef struct {
    _Atomic(const char *) what;  // NULL: not waiting
    _Atomic uint64_t since, arg;
    char name[24];
} Watch;
static Watch watches[NWATCH];
static _Atomic int nwatch;
static __thread Watch *my_watch;
static __thread bool my_watch_full, is_render;

static Watch *watch_self(void) {
    if (my_watch || my_watch_full) return my_watch;
    int i = atomic_fetch_add(&nwatch, 1);
    if (i >= NWATCH) { my_watch_full = true; return NULL; }
    Watch *w = &watches[i];
    extern uint64_t kernel_current_thread_id(void);
    uint64_t kt = kernel_current_thread_id();
    if (is_render) snprintf(w->name, sizeof w->name, "render thread");
    else if (kt) snprintf(w->name, sizeof w->name, "guest thread %llu", (unsigned long long)kt);
    else snprintf(w->name, sizeof w->name, "host thread #%d", i);
    my_watch = w;
    return w;
}

void rq_watch(const char *what, uint64_t arg) {
    Watch *w = watch_self();
    if (!w) return;
    if (what) {
        atomic_store_explicit(&w->arg, arg, memory_order_relaxed);
        atomic_store_explicit(&w->since, plat_now_ns(), memory_order_relaxed);
    }
    atomic_store_explicit(&w->what, what, memory_order_release);
}

// ---- the queue

// Wait on cv while !ready(arg); guest: give up while the kernel stops the threads. Caller holds the lock. A wait of
// 2 s and more is logged with the queue's state (a stuck render thread shows up instead of a silent hang).
static void wait_until(pthread_cond_t *cv, bool (*ready)(uint64_t), uint64_t arg, bool guest, const char *what) {
    uint64_t t0 = 0;
    bool warned = false;
    bool noted = what && !ready(arg);
    if (noted) rq_watch(what, arg);
    while (!ready(arg)) {
        if (guest && g_rt_unwind) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(cv, &lock, &ts);
        if (!what) continue;
        uint64_t now = plat_now_ns();
        if (!t0) t0 = now;
        else if (!warned && now - t0 > 2000000000ull) {
            warned = true;
            static int n;
            if (n++ < 20)
                LOG("[rq] %s: waiting %llu ms (queued %llu, done %llu, running %llu, presents pending %d)\n", what,
                    (unsigned long long)((now - t0) / 1000000), (unsigned long long)q_enq, (unsigned long long)q_done,
                    (unsigned long long)q_cur, presents_pending);
        }
    }
    if (noted) rq_watch(NULL, 0);
}

static bool has_room(uint64_t a) { (void)a; return q_enq - q_done < RQ_CAP; }
static bool room_or_stopping(uint64_t a){return has_room(a)||atomic_load(&stopping);}
static bool no_present(uint64_t a) { (void)a; return presents_pending == 0; }
static bool no_present_or_stopping(uint64_t a){return no_present(a)||atomic_load(&stopping);}
static bool all_done(uint64_t a) { (void)a; return q_done == q_enq; }
// (each waiter its own item: with one shared target, a waiter for a later item let one for an earlier item go on
// before its list was translated, and the other way round)
static bool item_done(uint64_t q) { return q_done >= q; }
static bool has_work(uint64_t a) { (void)a; return q_done < q_enq || atomic_load(&stopping); }

static void lock_timed(void) {  // (LARECOMP_LOG_PACE: the game thread's time waiting for this lock)
    if (pthread_mutex_trylock(&lock) == 0) return;
    uint64_t t0 = plat_now_ns();
    pthread_mutex_lock(&lock);
    g_rq_lock_ns += plat_now_ns() - t0;
}

// Queue an item. A list's fences (prescan) get its number in the same step, under the lock: no other thread's item
// (a texture destruction, a movie frame, a present) can take the number they were given.
static uint64_t enqueue(const Item *it, const uint64_t *fences, int nfences) {
    lock_timed();
    uint64_t t0 = 0;
    bool managed=nvn_command_lifetime_enabled();
    if(managed&&atomic_load(&stopping)){pthread_mutex_unlock(&lock);return 0;}
    if (!has_room(0)) { t0 = plat_now_ns(); wait_until(&cv_done, managed?room_or_stopping:has_room, 0, false, "queue full"); }
    if(managed&&atomic_load(&stopping)){pthread_mutex_unlock(&lock);return 0;}
    if (it->kind == IT_PRESENT) {
        if (!t0) t0 = plat_now_ns();
        wait_until(&cv_done, managed?no_present_or_stopping:no_present, 0, true, "present (the previous one to finish)");
        if(managed&&atomic_load(&stopping)){pthread_mutex_unlock(&lock);return 0;}
        presents_pending++;
        last_present_q = q_enq + 1;
    }
    if (t0) g_blocked_ns += plat_now_ns() - t0;  // (the pacer: as nextDrawable's wait was)
    uint64_t q = ++q_enq;
    items[q % RQ_CAP] = *it;
    kind_of[q % RQ_HIST] = (uint8_t)it->kind;
    enq_ns[q % RQ_HIST] = plat_now_ns();
    if (nfences) {
        extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
        for (int i = 0; i < nfences; i++) nvn_sync_set_seq(fences[i], q);
    }
    pthread_cond_signal(&cv_work);
    pthread_mutex_unlock(&lock);
    return q;
}

_Atomic uint64_t g_render_frame_busy_ns;  // the render thread's busy time in its last frame (vulkan/exec.c vk_pace_adjust)

// Only the render thread runs this. Command lists already submit in
// backend_execute; an explicit marker also covers pending non-list tail work
// such as alias-preservation copies recorded while destroying a texture.
static uint64_t rq_item_completed_seq(const Item *it) {
    if (it->kind == IT_FENCE) {
        extern uint64_t renderer_fence_seq(void);
        extern void renderer_submit_upto(uint64_t seq);
        renderer_submit_upto(renderer_fence_seq());
    }
    return renderer_issued_seq();
}

static void cancel_pending_command_readers_locked(void){
    for(uint64_t q=q_done+1;q<=q_enq;++q){
        Item *it=&items[q%RQ_CAP];
        if(it->lease){
            NvnClLease *lease=it->lease;it->lease=NULL;
            if(nvn_cl_lease_release_completed(lease,true)!=NVN_CL_OK)FATAL("[rq] cancelled command lease release failed\n");
        }
    }
}
static void *run(void *arg) {
    (void)arg;
    plat_thread_name("render");
    plat_thread_guest(44);  // (the game's GPU thread's class: performance cores on a Mac)
    plat_thread_perf_core(PLAT_PERF_RENDER);
    plat_thread_core(PLAT_CORE_RENDER);  // (phones: the big cores, the hint session; elsewhere a no-op)
    crash_handler_install(); /* Retain the continuous backend's process-wide TLS-safe signal front. */        // (its own alternate signal stack: a crash in a driver call is reported)
    uint64_t busy = 0;  // (this frame's items, less this thread's waits: GPU fences, window images, the display)
    is_render = true;
    rq_watch(NULL, 0);  // (registers this thread for the hang dump)
    for (;;) {
        pthread_mutex_lock(&lock);
        wait_until(&cv_work, has_work, 0, false, NULL);
        if (atomic_load(&stopping)) {  // quitting (rq_stop): no more GPU work from here on, nothing queued runs
            if(nvn_command_lifetime_enabled())cancel_pending_command_readers_locked();
            atomic_store(&parked, true);
            pthread_cond_broadcast(&cv_done);
            for (;;) pthread_cond_wait(&cv_work, &lock);
        }
        uint64_t q = q_done + 1;
        Item it = items[q % RQ_CAP];
        q_cur = q;
        atomic_store_explicit(&cur_since, plat_now_ns(), memory_order_relaxed);
        pthread_mutex_unlock(&lock);
        static long stall_at = -1;  // (test) LARECOMP_RQ_STALL_AT=<item>: the thread stops there for 60 s (hang dump test)
        if (stall_at < 0) stall_at = getenv("LARECOMP_RQ_STALL_AT") ? atol(getenv("LARECOMP_RQ_STALL_AT")) : 0;
        if (stall_at && q == (uint64_t)stall_at) { rq_watch("LARECOMP_RQ_STALL_AT (test)", q); sleep(60); rq_watch(NULL, 0); }
        static int stats = -1;  // (LARECOMP_LOG_PACE) the thread's wall vs CPU time per item kind
        if (stats < 0) stats = getenv("LARECOMP_LOG_PACE") != NULL;
        uint64_t c0 = stats ? plat_thread_cpu_ns() : 0, w0 = stats ? plat_now_ns() : 0;
        uint64_t b0 = plat_now_ns(), bw0 = g_blocked_ns;
        void *pool = backend_pool_push();
        NvnClLease *previous=nvn_command_reader_enter(it.lease);
        switch (it.kind) {
        case IT_EXEC: backend_execute(it.handle); break;
        case IT_PRESENT: backend_present_now(it.tex, it.frame); break;
        case IT_DESTROY: { extern void nvn_texture_destroy(NvnTexture *t); nvn_texture_destroy(it.tex); break; }
        case IT_UPLOAD: backend_upload_now(it.tex, it.up_kind, it.data, it.w, it.h, it.stride, it.uv_off); break;
        case IT_FLUSH: { extern void renderer_flush(void); renderer_flush(); break; }
        case IT_FREE: free(it.data); break;
        case IT_CALL: it.fn(it.data); break;
        case IT_FENCE: break; // submission is finalized below, before publishing this item
        }
        backend_pool_pop(pool);
        // Complete the actual CPU consumer and leave TLS before dropping its
        // reference. GPU work already records its own resources; it never
        // dereferences CmdRec storage after backend_execute returns.
        nvn_command_reader_leave(previous);
        if(it.lease){
            if(nvn_cl_lease_release_completed(it.lease,true)!=NVN_CL_OK)FATAL("[rq] completed command lease release failed\n");
            // This slot is still owned by q until completion publication.
            items[q%RQ_CAP].lease=NULL;
        }
        busy += plat_now_ns() - b0 - (g_blocked_ns - bw0);
        if (it.kind == IT_PRESENT) {
            atomic_store(&g_render_frame_busy_ns, busy), busy = 0;
            static unsigned np;
            if (np++ % 120 == 0) plat_thread_core(PLAT_CORE_RENDER);  // (again now and then: Android resets it)
        }
        uint64_t gs = rq_item_completed_seq(&it), c1 = plat_thread_cpu_ns();  // (its last submission; renderer_wait_seq waits for it)
        atomic_store_explicit(&render_cpu_ns, c1, memory_order_relaxed);  // (LARECOMP_LOG_PACE: pace.c)
        if (stats) {
            static uint64_t n[IT_N], wall[IT_N], cpu[IT_N], items;
            int k = it.kind % IT_N;
            n[k]++, wall[k] += plat_now_ns() - w0, cpu[k] += c1 - c0;
            if (++items % 1200 == 0) {
                for (int i = 0; i < IT_N; i++)
                    if (n[i]) LOG("[rq] render thread %s items: %llu, avg %.3f ms wall, %.3f ms CPU\n", kind_name[i],
                                  (unsigned long long)n[i], wall[i] / 1e6 / n[i], cpu[i] / 1e6 / n[i]);
                memset(n, 0, sizeof n), memset(wall, 0, sizeof wall), memset(cpu, 0, sizeof cpu);
            }
        }
        {   // (debug) LARECOMP_RQ_TRACE=<frame>: that frame's and the next 2 frames' items, times in ms from its first
            static long tr_at = -1;
            static uint64_t tr_t0;
            if (tr_at < 0) tr_at = getenv("LARECOMP_RQ_TRACE") ? atol(getenv("LARECOMP_RQ_TRACE")) : 0;
            if (tr_at && exec_frame >= (uint64_t)tr_at && exec_frame < (uint64_t)tr_at + 3) {
                uint64_t now = plat_now_ns(), e = enq_ns[q % RQ_HIST];
                if (!tr_t0) tr_t0 = e / 1000000000ull * 1000000000ull;
                LOG("[rq-trace] frame %llu item %llu %s: queued %.3f started %.3f done %.3f (gpu submission %llu)\n",
                    (unsigned long long)exec_frame, (unsigned long long)q, kind_name[it.kind], (e - tr_t0) / 1e6,
                    (atomic_load(&cur_since) - tr_t0) / 1e6, (now - tr_t0) / 1e6, (unsigned long long)gs);
                rq_trace_t0 = tr_t0;
            }
        }
        pthread_mutex_lock(&lock);
        gpu_seq_of[q % RQ_HIST] = gs;
        done_ns[q % RQ_HIST] = plat_now_ns();
        if (it.kind == IT_PRESENT) {
            presents_pending--;
            atomic_fetch_add(&presents_done, 1);
            exec_frame = it.frame + (it.advance ? 1 : 0);
        }
        q_cur = 0;
        q_done = q;
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

// ---- items

// The sync objects a list's fences signal (CMD_FENCE_SYNC records, also in the lists it calls): numbered when it's
// queued, as the game may wait on one before the thread reaches the list.
typedef struct { uint64_t v[32], *p; int n, cap; } Fences;
static void fences_add(Fences *f, uint64_t guest) {
    if (f->n == f->cap) {
        int nc = f->cap * 2;
        uint64_t *np = malloc(sizeof *np * (size_t)nc);
        if (!np) FATAL("[rq] fence list allocation failed\n");
        memcpy(np, f->p, sizeof *np * (size_t)f->n);
        if (f->p != f->v) free(f->p);
        f->p = np, f->cap = nc;
    }
    f->p[f->n++] = guest;
}
static void prescan(uint64_t handle, Fences *f, int depth) {
    const CmdRec *first = (const CmdRec *)(uintptr_t)nvn_command_resolve(handle);
    if(nvn_command_lifetime_enabled()){
        if(!nvn_command_flags(handle).fences)return;
    }else if (first->op == CMD_BARRIER && first->x[6] != CMD_LIST_FENCES) return;  // (none: cmdbuf.c EndRecording marks them)
    uint64_t p = (uintptr_t)first;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) prescan(h[i], f, depth + 1);
            break;
        }
        case CMD_FENCE_SYNC:
            if (r->x[1]) fences_add(f, r->x[1]);
            break;
        default: break;
        }
        if (r->size < offsetof(CmdRec, extra)) return;  // (malformed: stop)
        p += r->size;
    }
}

uint64_t rq_submit(uint64_t handle) {
    if(nvn_command_lifetime_enabled())return rq_submit_leased(handle,nvn_command_reader_current());
    Fences f = {.n = 0, .cap = 32};
    f.p = f.v;
    prescan(handle, &f, 0);
    Item it = {.kind = IT_EXEC, .handle = handle};
    uint64_t q = enqueue(&it, f.p, f.n);
    if (f.p != f.v) free(f.p);
    return q;
}
uint64_t rq_submit_leased(uint64_t handle,NvnClLease *lease) {
    Fences f = {.n = 0, .cap = 32};
    f.p = f.v;
    if(!lease)FATAL("[rq] missing command lease\n");
    NvnClLease *previous=nvn_command_reader_enter(lease);
    prescan(handle,&f,0);
    nvn_command_reader_leave(previous);
    Item it = {.kind = IT_EXEC, .handle = handle, .lease=lease};
    if(nvn_cl_lease_retain(lease)!=NVN_CL_OK)FATAL("[rq] command item retain failed\n");
    uint64_t q = enqueue(&it, f.p, f.n);
    if (f.p != f.v) free(f.p);
    if(!q&&nvn_cl_lease_release_completed(lease,true)!=NVN_CL_OK)FATAL("[rq] rejected command item release failed\n");
    return q;
}

void rq_present(NvnTexture *t, uint64_t frame, bool advance) {
    Item it = {.kind = IT_PRESENT, .tex = t, .frame = frame, .advance = advance};
    enqueue(&it, NULL, 0);
}

void rq_destroy_texture(NvnTexture *t) {
    Item it = {.kind = IT_DESTROY, .tex = t};
    enqueue(&it, NULL, 0);
}

void rq_upload(NvnTexture *t, int kind, void *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    Item it = {.kind = IT_UPLOAD, .tex = t, .up_kind = kind, .data = data, .w = w, .h = h, .stride = stride, .uv_off = uv_off};
    enqueue(&it, NULL, 0);
}

// Host objects a queued list may still refer to (syncs, samplers, programs, pools the game finalized): freed in
// queue order, after those lists (as the phones' first render thread did; the memory is ours, not the game's).
void rq_free(void *p) {
    if (!p) return;
    if (!rq_on()) { free(p); return; }
    Item it = {.kind = IT_FREE, .data = p};
    enqueue(&it, NULL, 0);
}

// fn(arg) on the render thread after everything queued before it; returns when it has run (on the render thread
// itself or with the thread off: right away).
void rq_call(void (*fn)(void *), void *arg) {
    if (!rq_on() || rq_current_item()) { fn(arg); return; }
    Item it = {.kind = IT_CALL, .fn = fn, .data = arg};
    uint64_t q = enqueue(&it, NULL, 0);
    rq_wait_item(q);
}

// ---- waiting

uint64_t rq_wait_item(uint64_t q) {
    uint64_t t0 = plat_now_ns(), gs;
    lock_timed();
    if (q > q_enq) q = q_enq;
    wait_until(&cv_done, item_done, q, true, "fence (its list not translated yet)");
    gs = q_done >= q ? gpu_seq_of[q % RQ_HIST] : renderer_issued_seq();  // (gave up: whatever is queued)
    pthread_mutex_unlock(&lock);
    g_rq_wait_ns += plat_now_ns() - t0;
    return gs;
}

// (LARECOMP_LOG_PACE) a fence's item: how many items were queued after it, and whether it's in the frame being
// recorded (after the latest present) or an earlier one.
bool rq_fence_info(uint64_t q, uint64_t *behind, bool *untranslated) {
    lock_timed();
    rq_fence_q = q;
    *behind = q_enq > q ? q_enq - q : 0;
    *untranslated = q_done < q;
    bool cur = q > last_present_q;
    pthread_mutex_unlock(&lock);
    return cur;
}

// (LARECOMP_LOG_PACE) after rq_fence_info + the waits: the item's translation time (queued to done) and how long
// before the wait began it was done (its GPU head start; 0 when the wait had to wait for it)
__thread uint64_t rq_fence_q;
void rq_fence_times(uint64_t wait_start, uint64_t *translate_ns, uint64_t *head_ns) {
    uint64_t q = rq_fence_q;
    lock_timed();
    uint64_t e = enq_ns[q % RQ_HIST], d = done_ns[q % RQ_HIST];
    pthread_mutex_unlock(&lock);
    *translate_ns = d > e ? d - e : 0;
    *head_ns = wait_start > d ? wait_start - d : 0;
}

uint64_t rq_fence_seq(void) {
    // An explicit marker is ordered after all preceding queue work and submits
    // its pending CB before rq_wait_item can publish GPU completion metadata.
    Item it = {.kind = IT_FENCE};
    return enqueue(&it, NULL, 0);
}

uint64_t rq_frame(void) { return exec_frame; }

uint64_t rq_current_item(void) {
    if (!atomic_load(&started) || !pthread_equal(pthread_self(), thread)) return 0;
    return q_cur;
}

void rq_drain(void) {
    if (atomic_load(&on) <= 0 || !atomic_load(&started)) return;
    if (rq_current_item()) return;  // (the thread itself: a flush from inside an item)
    uint64_t t0 = plat_now_ns();
    lock_timed();
    wait_until(&cv_done, all_done, 0, false, "drain (every queued item)");
    pthread_mutex_unlock(&lock);
    g_rq_wait_ns += plat_now_ns() - t0;
}

// Quitting: the thread finishes the item it runs and stops there (what is still queued never runs), so nothing
// presents or submits while the process ends - exit()'s handlers and the libraries' destructors (Vulkan layers such
// as the Steam overlay, #40) tear down under it otherwise. Waits up to 3 s for a thread stuck in the driver.
bool rq_stopping(void) { return atomic_load_explicit(&stopping, memory_order_relaxed); }
void rq_stop(void) {
    atomic_store(&stopping, true);
    if (atomic_load(&on) <= 0 || !atomic_load(&started) || pthread_equal(pthread_self(), thread)) return;
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&cv_work);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    while (!atomic_load(&parked))
        if (pthread_cond_timedwait(&cv_done, &lock, &ts) != 0 && !atomic_load(&parked)) {
            LOG("[rq] quit: the render thread did not stop in 3 s (running item %llu)\n", (unsigned long long)q_cur);
            break;
        }
    pthread_mutex_unlock(&lock);
}

void rq_flush(void) {
    Item it = {.kind = IT_FLUSH};
    enqueue(&it, NULL, 0);
    rq_drain();
}

// ---- the hang dump's renderer side (runtime/kernel.c's hang watch: one "[hang]" facility, LARECOMP_HANG_DUMP)

static void hang_state(const char *pfx) {
    uint64_t now = plat_now_ns();
    extern uint64_t renderer_present_count(void);
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&lock) == 0); i++) usleep(1000);
    uint64_t enq = q_enq, done = q_done, cur = q_cur, cs = atomic_load(&cur_since);
    int pp = presents_pending;
    if (locked) pthread_mutex_unlock(&lock);
    LOG("%s render thread %s; queued %llu, done %llu, running %llu%s%s (for %llu ms), presents pending %d, "
        "presents done %llu, game frames %llu, backend submissions %llu%s\n", pfx,
        atomic_load(&on) > 0 ? "on" : "off", (unsigned long long)enq, (unsigned long long)done, (unsigned long long)cur,
        cur ? " " : "", cur ? kind_name[kind_of[cur % RQ_HIST] % IT_N] : "",
        cur && cs < now ? (unsigned long long)((now - cs) / 1000000) : 0ull, pp,
        (unsigned long long)atomic_load(&presents_done), (unsigned long long)renderer_present_count(),
        (unsigned long long)renderer_submit_seq(), locked ? "" : " (queue lock held 100+ ms: read without it)");
    if (enq) {
        char b[600];
        int n = 0;
        uint64_t from = enq > 24 ? enq - 23 : 1;
        for (uint64_t q = from; q <= enq && n < (int)sizeof b - 32; q++)
            n += snprintf(b + n, sizeof b - (size_t)n, " %llu:%s", (unsigned long long)q, kind_name[kind_of[q % RQ_HIST] % IT_N]);
        LOG("%s render queue, last items:%s\n", pfx, b);
    }
    int nw = atomic_load(&nwatch);
    if (nw > NWATCH) nw = NWATCH;
    for (int i = 0; i < nw; i++) {
        const char *w = atomic_load(&watches[i].what);
        uint64_t since = atomic_load(&watches[i].since);
        if (w) LOG("%s %s: waiting on %s (%llu) for %llu ms\n", pfx, watches[i].name, w,
                   (unsigned long long)atomic_load(&watches[i].arg),
                   (unsigned long long)(since < now ? (now - since) / 1000000 : 0));
        else LOG("%s %s: no noted wait\n", pfx, watches[i].name);
    }
}

static _Atomic uint64_t hang_t_start;  // the GPU device setup (rq_hang_watch_start)
static bool hang_check(uint64_t limit, char *why, size_t nwhy) {
    static uint64_t last_frames, frames_at;
    static bool reported_first, reported_stall;
    static uint64_t reported_since[NWATCH];
    uint64_t t_start = atomic_load(&hang_t_start);
    if (!t_start) return false;
    uint64_t now = plat_now_ns();
    if (!frames_at) frames_at = now;
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count() + atomic_load(&presents_done);
    if (f != last_frames || g_rt_unwind) last_frames = f, frames_at = now, reported_stall = false;
    bool hit = false;  // (one reason per check: the dump shows every wait anyway)
    if (!f && !reported_first && now - t_start > 2 * limit) {  // (the first frame: twice the time, loading)
        reported_first = hit = true;
        snprintf(why, nwhy, "no frame presented since the GPU device was set up");
    } else if (f && !reported_stall && now - frames_at > limit) {
        reported_stall = hit = true;
        snprintf(why, nwhy, "presenting stopped");
    }
    int nw = atomic_load(&nwatch);
    if (nw > NWATCH) nw = NWATCH;
    now = plat_now_ns();  // (after the loads below can't see a newer start: since <= now)
    for (int i = 0; i < nw; i++) {
        const char *w = atomic_load(&watches[i].what);
        uint64_t since = atomic_load(&watches[i].since);
        // (a wait that started after `now` was read, or a slot rewritten between the two loads: not stale)
        if (w && since < now && now - since > limit && reported_since[i] != since) {
            reported_since[i] = since;
            if (hit) continue;
            hit = true;
            snprintf(why, nwhy, "%s: one wait for %llu s (%s)", watches[i].name,
                     (unsigned long long)((now - since) / 1000000000ull), w);
        }
    }
    return hit;
}

static pthread_once_t wd_once = PTHREAD_ONCE_INIT;
static void watchdog_start_once(void) {
    atomic_store(&hang_t_start, plat_now_ns());
    extern void kernel_hang_hooks(bool (*)(uint64_t, char *, size_t), void (*)(const char *));
    kernel_hang_hooks(hang_check, hang_state);
}
void rq_hang_watch_start(void) { pthread_once(&wd_once, watchdog_start_once); }

/* Call only after guest entry is stopped. A stop timeout is not a reader
 * completion proof. The worker publishes parked after canceling pending pins. */
bool rq_command_readers_stopped(void){
    if(atomic_load(&on)<=0||!atomic_load(&started))return true;
    return atomic_load(&parked);
}
