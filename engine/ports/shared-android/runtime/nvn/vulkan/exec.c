/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_iostream.h>
#include "nvn/shader_job_queue.h"
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../command_recording.h"
#include "record.h"
#include "../renderer.h"
#include "../pace.h"
#include "../recolour.h"
#include "../screen_filter.h"
#include "../shader_prep.h"
#include "../rqueue.h"
#include "../constant_buffer.h"
#include "../shader_snapshot.h"
#include "../shader_discovery.h"
#include "../shader_worker_policy.h"
#include "../shader_preparation.h"
#include "../maxwell/shader_compiler.h"
#include "../../settings.h"
#include "../../common_session.h"
#include "../../shared_gpu_cache.h"
#include "../../paths.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// (the guest heap for shaders in chunks: with the heap import below)
static uint32_t gc_shift;
static uint32_t gmem_chunk_shift(void);
static VkDescriptorSetLayout gmem_chunk_layout(void);
static void gc_scan_pointers(const uint32_t *w, uint64_t bytes);
static bool gc_lazy;
static int gc_imported;
static uint64_t gc_imported_bytes, stat_gc_misses;
static uint64_t gmem_heap_size(void);
static void gc_frame(void);
static uint64_t stat_upload_bytes;
static uint64_t prof_cb_reused, prof_cb_copied;  // (VK_PROF) lean constant blocks: copies reused, made (cbuf_lean)
static bool cbuf_lean(void);
static uint64_t prof_prog_memo, prof_pipe_memo, prof_key_draws;  // (VK_PROF) key memo answers (get_program), draws
static bool key_memo_on(void);
static void discovery_replay(void);
static void discovery_seed_profiles(void);
static void discovery_finish(void);
static void discovery_record(const NvnDiscoveryRecipe *recipe, uint64_t content);
static uint64_t prof_sets_reused, prof_sets_new;  // (VK_PROF) set reuse (allocations: record.c RecStats)
static bool set_reuse_on(void);
static uint64_t prof_slot_wait;  // (VK_PROF) sub(): ring slot waits (ns; GPU waits, inside "pre" when begin_pass starts a submission)
static uint64_t prof_hook_draws, prof_hook_known;  // (VK_PROF) draws through the texture hook pass, with a known texture
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;  // the GPU timer's timestamps (3 per ring slot: start, end, window image acquired)
static uint64_t prof_win, prof_span, prof_wframes;  // (profiling) the window image's submissions: GPU ns, frame spans, count
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
// The game's 16:9 UI box in a window of another shape: its draws go into the box of the window-shaped target, and a
// frame with nothing else (menus, the file select, movies: no scene composited over the whole target) leaves the rest
// of the target as it was - another frame's picture, a different one in each buffer: the bars flickered. Then the
// present covers them (device.c bars_backdrop). Render thread.
bool g_box_drawn, g_full_drawn;  // such draws since the last present (device.c resets them; render thread, in order)
void *g_box_rt;  // (the target they went into)
float g_box_frac[4];  // the box: x0, x1, y0, y1 as fractions of the target

static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);
void vk_mirror_writeback(void);
bool vk_gmem_mirror(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
// Every call on a command buffer, the descriptor pools and the queue goes through record.c (the recorder thread, or
// at once with it off): this file decides, the recorder calls. submit_seq counts what this thread handed to the
// recorder (what it will have submitted, in this order); rec_submitted() is what the driver has.
#define SUBS VK_SUBS  // (early submits: several a frame, a few frames in flight)
typedef struct {
    VkCommandBuffer main, upload;       // (allocated once, vk_exec_init: the pool is the recorder's from then on)
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (the GPU timer)
    bool ts_skip;                       // (waits on the window image: not counted, see vk_gpu_total_ns)
    bool ts_win;                        // the window image's submission: a 3rd timestamp after its acquire wait
    uint64_t seq;
    // (its descriptor pools: record.c, per slot)
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last handed to the recorder (= submitted, with the recorder off)
static uint32_t work_since_submit;  // draws (+ dispatches x4) recorded since (early_submit_check)
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
// A storing draw has prepared its mirror uploads/tables in this submission.
static bool mir_graphics_prepared, mir_graphics_preparing;
static uint64_t mir_graphics_seq;
static bool uniform_forward_graphics_prepared;
static uint64_t uniform_forward_write_epoch;
static int passes_in_sub;  // render passes in the submission being recorded (LARECOMP_CHUNK_PASSES, begin_pass)

// Completed submissions. Phones: Turnip on Android's kernel driver (KGSL) has no timeline semaphores - it emulates them
// in software behind one lock, which submissions and waits all go through (the S20 FE's render thread spent half its
// time blocked on it, ~25 ms a frame with the stable 24.1 driver). There each submission signals a fence of its own
// instead (a ring, by submission number), one watcher thread waits for them in turn and publishes how far the GPU got;
// everything else waits for that number. LARECOMP_VK_FENCES=0/1 (default on Android; elsewhere the timeline).
#define FENCE_N 64
static int g_fences = -1;
static VkFence fence_ring[FENCE_N];
static uint64_t fence_seq[FENCE_N];        // the submission each fence was last used for
static _Atomic uint64_t fence_sent[FENCE_N];  // ... and when it went out (the watcher's GPU busy time)
static _Atomic bool fence_present[FENCE_N];   // ... a presentation's (waits for a window image: not the game's GPU time)
_Atomic uint64_t g_gpu_busy_ns;            // GPU busy time so far (fences: summed by the watcher; vk_gpu_frame_busy)
static _Atomic uint64_t g_done_seq;        // fences: the last completed submission (the watcher's)
static _Atomic bool g_fence_lost;
static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;   // a submission went out (the watcher waits for one)
static pthread_cond_t done_adv = PTHREAD_COND_INITIALIZER;  // g_done_seq moved (vk_wait_seq waits for it)

static void *done_watcher(void *arg) {
    (void)arg;
    plat_thread_name("vk-done");
    for (;;) {
        uint64_t next = atomic_load(&g_done_seq) + 1;
        pthread_mutex_lock(&done_lock);
        for (;;) {
            pthread_mutex_lock(&seq_lock);
            uint64_t sub_ = submit_seq;
            pthread_mutex_unlock(&seq_lock);
            if (sub_ >= next) break;
            pthread_cond_wait(&done_cv, &done_lock);  // (nothing in flight: until the next submission)
        }
        pthread_mutex_unlock(&done_lock);
        rec_wait_submitted(next);  // (queued is not submitted with the recorder on: a fence is waited for once the driver has it)
        VkResult r = vkWaitForFences(g_vk.dev, 1, &fence_ring[next % FENCE_N], VK_TRUE, 1000000000ull);
        if (r == VK_TIMEOUT) continue;
        if (r == VK_SUCCESS) {
            // GPU busy: the queue runs submissions in order, so each one ran from when it went out or the one before
            // it was done, whichever came later, until now (seen done: the watcher wakes late by a little)
            static uint64_t last_done;
            uint64_t now = SDL_GetTicksNS(), from = atomic_load(&fence_sent[next % FENCE_N]);
            if (from < last_done) from = last_done;
            if (from && now > from && !atomic_load(&fence_present[next % FENCE_N])) atomic_fetch_add(&g_gpu_busy_ns, now - from);
            last_done = now;
        }
        pthread_mutex_lock(&done_lock);
        if (r == VK_SUCCESS) atomic_store(&g_done_seq, next);
        else atomic_store(&g_fence_lost, true);
        pthread_cond_broadcast(&done_adv);
        pthread_mutex_unlock(&done_lock);
        if (r != VK_SUCCESS) return NULL;  // (device lost: vk_wait_seq reports it)
    }
}

static bool fences_on(void) {
    if (g_fences < 0) {
        const char *e = getenv("LARECOMP_VK_FENCES");
#ifdef __ANDROID__
        g_fences = !e || atoi(e) != 0;
#else
        g_fences = e && atoi(e) != 0;
#endif
        if (g_fences) {
            VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (int i = 0; i < FENCE_N; i++) VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &fence_ring[i]));
            pthread_t t;
            pthread_create(&t, NULL, done_watcher, NULL);
            LOG("[vk] submissions signal fences (no timeline semaphore waits)\n");
        }
    }
    return g_fences;
}

// fences: until submission seq is done (false: device lost)
static bool fence_wait_done(uint64_t seq, uint64_t timeout_ns) {
    uint64_t end = SDL_GetTicksNS() + timeout_ns;
    pthread_mutex_lock(&done_lock);
    while (atomic_load(&g_done_seq) < seq && !atomic_load(&g_fence_lost)) {
        uint64_t now = SDL_GetTicksNS();
        if (now >= end) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t left = end - now > 100000000ull ? 100000000ull : end - now, ns = (uint64_t)ts.tv_nsec + left;
        ts.tv_sec += (time_t)(ns / 1000000000ull), ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&done_adv, &done_lock, &ts);
    }
    bool ok = atomic_load(&g_done_seq) >= seq;
    pthread_mutex_unlock(&done_lock);
    return ok;
}

// fences: the fence submission seq (about to go out) signals, free of older work
static VkFence fence_for(uint64_t seq) {
    int i = (int)(seq % FENCE_N);
    // (the watcher is past it: safe to reset - never while it may still be pending: a GPU 10 s late is a lost one)
    if (fence_seq[i] && !fence_wait_done(fence_seq[i], 10ull * 1000000000ull)) {
        vk_device_fault_report();
        FATAL("submission %llu not done after 10 s (device lost)\n", (unsigned long long)fence_seq[i]);
    }
    vkResetFences(g_vk.dev, 1, &fence_ring[i]);
    fence_seq[i] = seq;
    atomic_store(&fence_sent[i], SDL_GetTicksNS());  // (just before the submission: close enough)
    return fence_ring[i];
}

static void done_submitted(void) {  // (vk_submit_extra: a submission went out)
    if (g_fences <= 0) return;
    pthread_mutex_lock(&done_lock);
    pthread_cond_signal(&done_cv);
    pthread_mutex_unlock(&done_lock);
}

uint64_t vk_completed_seq(void) {
    if (fences_on()) return atomic_load(&g_done_seq);
    uint64_t v = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v));
    return v;
}

// (LARECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    if (seq > rec_submitted()) {  // not handed to the driver yet: the recorder first (a timeline value can't be waited for sooner)
        pthread_mutex_lock(&seq_lock);
        uint64_t issued = submit_seq;
        pthread_mutex_unlock(&seq_lock);
        if (seq > issued) {  // (not even queued: a caller's mistake - everything queued so far instead, logged)
            static int n;
            if (n++ < 5) LOG("[vk] wait for submission %llu, only %llu queued\n", (unsigned long long)seq, (unsigned long long)issued);
            seq = issued;
            if (!seq) { wait_why = NULL; return; }
        }
        rec_wait_submitted(seq);
    }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    extern void rq_watch(const char *what, uint64_t arg);
    rq_watch("vkWaitSemaphores (GPU submission)", seq);
    VkResult r;
    if (fences_on()) r = fence_wait_done(seq, 10ull * 1000000000ull) ? VK_SUCCESS : atomic_load(&g_fence_lost) ? VK_ERROR_DEVICE_LOST : VK_TIMEOUT;
    else r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    rq_watch(NULL, 0);
    uint64_t waited = SDL_GetTicksNS() - t0;  // (the game thread's fence waits and the render thread's: atomic sums)
    __atomic_fetch_add(&stat_wait_ns, waited, __ATOMIC_RELAXED);
    __atomic_fetch_add(&cum_wait, waited, __ATOMIC_RELAXED);
    wait_account(waited);
    g_blocked_ns += waited;  // (waiting for the GPU: idle for the frame pacer, as the game's own waits)
    // Callers recycle command buffers and allocations after this returns; a timeout is not completion.
    if (r == VK_TIMEOUT) FATAL("[vk] GPU did not finish submission %llu within 10 s; stopping before reusing in-flight resources\n",
                              (unsigned long long)seq);
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). LARECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX], *start[CQ_MAX]; } CounterQ;  // (start: the
// pair's start counter of an end one, wherever it was submitted - early submits put a pair in two submissions)
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("LARECOMP_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing (runtime/nvn/pace.c): the game's 30 fps mode when frames don't fit 60, through these counters.
// Its busy time gets the render thread's and the GPU's too (pace_busy_adjust): the game's thread waits for them in its
// present (blocked time), so a frame they can't keep up with looked like headroom - the game took its 60 fps mode and
// played in slow motion until it was back in 30. Phones: the frame's work goes to Android's performance hints.
static void vk_pace_adjust(double *busy, bool slow) {
    static uint64_t last_gpu;
    uint64_t gb = atomic_load(&g_gpu_busy_ns), gpu = last_gpu ? gb - last_gpu : 0;
    last_gpu = gb;
    extern _Atomic uint64_t g_render_frame_busy_ns;
    if (rq_on()) { double rb = atomic_load(&g_render_frame_busy_ns) / 1e6; if (rb > *busy) *busy = rb; }
    // (Android's hints get the CPU threads' work only: with the GPU's time in it a GPU-bound frame read as CPU work
    // over its target, and the CPU clocks went up for nothing - heat out of the budget the GPU shares on a phone. Seen
    // in the mamcjr Android port, which reports the two apart)
    plat_frame_work((uint64_t)(*busy * 1e6), slow ? 33333333u : 16666667u);
    if (gpu && gpu / 1e6 > *busy) *busy = gpu / 1e6;
}
static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // LARECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("LARECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            q->out[k][1] = pace_end_counter(q->start[k], q->out[k], gpu);
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done, oldest first (a pair's start first)
    pthread_mutex_lock(&cq_lock);
    for (;;) {
        int best = -1;
        for (int i = 0; i < SUBS; i++)
            if (cq[i].n && cq[i].seq <= done && (best < 0 || cq[i].seq < cq[best].seq)) best = i;
        if (best < 0) break;
        cq_resolve(best);
        cq[best].n = 0;
    }
    pthread_mutex_unlock(&cq_lock);
}

// ---- GPU timer (dynamic resolution, runtime/nvn/output_scale.c; and LARECOMP_VK_PROF's "gpu"): a timestamp at the
// start and the end of each submission's main command buffer, read when its ring slot comes round again (so a few
// submissions late; the controller averages half a second). Submissions overlap a little (the next one's commands
// may start before the previous one's end): their union is counted, the time the GPU was busy.
// The window image's submission (device.c: the game's image drawn into the window - FSR / scaling - and the menu) waits
// for the image, which can be a vsync's wait, so its start timestamp isn't its work's: it has a 3rd timestamp, written
// after the barrier that waits for the image (vk_gpu_timer_window_start), and its time is counted from there - in a
// total of its own (vk_gpu_split_ns): device.c adds it to what dynamic resolution sees unless
// LARECOMP_DYNRES_COUNT_PRESENT=0. Also per frame: the span from the GPU starting the frame's first game submission
// to the end of its window submission; span - game - window = the GPU idle / doing work no timestamp covers
// (uploads ahead of a submission's main command buffer, waiting for the window image).
static uint64_t gpu_total_ns, gpu_last_end;  // ns counted; (ticks) the latest submission end seen
static uint64_t gpu_win_ns, gpu_span_ns, gpu_frames, span_from;  // window submissions: ns, frame spans' ns, count; (ticks)
static bool gpu_timer_on;

static void gpu_timer_add(const uint64_t *t, bool skip, bool win) {
    double per = g_vk.props.limits.timestampPeriod;
    uint64_t t0 = t[0], t1 = t[1], tw = win ? t[2] : 0;
    if (g_vk.ts_bits < 64) { uint64_t m = (1ull << g_vk.ts_bits) - 1; t0 &= m, t1 &= m, tw &= m; }
    if (t1 < t0 || (t1 - t0) * per > 1e9) { span_from = 0; return; }  // (wrapped, or nonsense)
    if (gpu_last_end > t0 && (gpu_last_end - t0) * per > 1e9) gpu_last_end = 0;  // (the counter wrapped)
    if (span_from > t1 || (span_from && (t1 - span_from) * per > 1e9)) span_from = 0;
    uint64_t prev_end = gpu_last_end;
    uint64_t from = t0 > gpu_last_end ? t0 : gpu_last_end;
    if (t1 > gpu_last_end) gpu_last_end = t1;
    if (win) {  // from after the image was acquired (and the work before it ended), to the end
        if (tw >= t0 && tw <= t1) {
            uint64_t wf = tw > prev_end ? tw : prev_end;
            if (t1 > wf) {
                uint64_t ns = (uint64_t)((double)(t1 - wf) * per);
                __atomic_add_fetch(&gpu_win_ns, ns, __ATOMIC_RELAXED);
                if (prof) prof_win += ns;
            }
        }
        if (span_from && t1 > span_from) {
            uint64_t ns = (uint64_t)((double)(t1 - span_from) * per);
            __atomic_add_fetch(&gpu_span_ns, ns, __ATOMIC_RELAXED);
            __atomic_add_fetch(&gpu_frames, 1, __ATOMIC_RELAXED);
            if (prof) prof_span += ns, prof_wframes++;
        }
        span_from = 0;
        return;
    }
    if (!span_from && !skip) span_from = from;  // the frame's first game submission (from where the GPU got to it)
    if (skip || t1 <= from) return;
    uint64_t ns = (uint64_t)((double)(t1 - from) * per);
    __atomic_add_fetch(&gpu_total_ns, ns, __ATOMIC_RELAXED);
    if (prof) prof_gpu += ns;
}

// GPU time of all counted submissions so far (ns); -1: no GPU timer on this device
int64_t vk_gpu_total_ns(void) { return gpu_timer_on ? (int64_t)__atomic_load_n(&gpu_total_ns, __ATOMIC_RELAXED) : -1; }
// (cumulative) out: game submissions' GPU ns, window submissions' GPU ns, frame spans' ns, frames with a span.
// false: no GPU timer.
bool vk_gpu_split_ns(uint64_t out[4]) {
    if (!gpu_timer_on) return false;
    out[0] = __atomic_load_n(&gpu_total_ns, __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&gpu_win_ns, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&gpu_span_ns, __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&gpu_frames, __ATOMIC_RELAXED);
    return true;
}
// Frame pacing (pace.c): GPU time so far, the game's submissions and the window image's; -1: no GPU timer.
int64_t pace_gpu_ns(void) {
    uint64_t t[4];
    return vk_gpu_split_ns(t) ? (int64_t)(t[0] + t[1]) : -1;
}
uint64_t vk_cum_wait_ns(void) { return __atomic_load_n(&cum_wait, __ATOMIC_RELAXED); }  // (all vk_wait_seq waits so far)

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        uint64_t tw = prof ? SDL_GetTicksNS() : 0;
        vk_wait_seq(s->seq);
        if (prof) prof_slot_wait += SDL_GetTicksNS() - tw;
        cq_resolve_done(s->seq);
        if (s->ts) {  // GPU time of that submission
            uint64_t t[3];
            uint32_t nq = s->ts_win ? 3 : 2;
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 3, nq, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                gpu_timer_add(t, s->ts_skip, s->ts_win);
            s->ts = s->ts_skip = s->ts_win = false;
        }
        rec_reset_pools(cur_sub);  // (its sets went with the submission; the recorder resets in order)
        pthread_mutex_lock(&seq_lock);
        s->main_begun = s->upload_begun = false;
        s->seq = submit_seq + 1;
        pthread_mutex_unlock(&seq_lock);
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    rec_BeginCommandBuffer(*cb);
    pthread_mutex_lock(&seq_lock);
    *begun = true;
    pthread_mutex_unlock(&seq_lock);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) rec_CmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof_qp) {
            rec_CmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 3, 3);
            rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 3);
            s->ts = true;
        }
    }
    return s->main;
}

// (device.c vk_frame_begin) the window image's submission: its work starts here, after the barrier that waits for the
// image (ALL_COMMANDS: after it, so not while the GPU waits for the image)
void vk_gpu_timer_window_start(void) {
    Sub *s = sub();
    if (!s->ts || s->ts_win) return;
    rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 3 + 2);
    s->ts_win = true;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted; =2 each pass, =3 each draw in a submission of its own).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
// GPU breadcrumbs (LARECOMP_VK_BREADCRUMBS=0: off): after the full barrier that starts each pass and each dispatch, the
// GPU writes that operation's number into a host-visible word - so it has reached it, everything before is done. On a
// device loss (a hang) the word says which operation the GPU was in; its label (pass size, first shader, draws) is
// kept here. Costs one tiny fill per pass.
static VkBuffer bc_buf;
static VmaAllocation bc_mem;
static volatile uint32_t *bc_ptr;
static char bc_label[1024][128];
static uint32_t bc_next, bc_draws;
static int bc_on = -1;
static void bc_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (bc_on < 0) { const char *e = getenv("LARECOMP_VK_BREADCRUMBS"); bc_on = e ? atoi(e) : 1; }
    if (!bc_on) return;
    if (!bc_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &bc_buf, &bc_mem, &info) != VK_SUCCESS) { bc_on = 0; return; }
        bc_ptr = info.pMappedData;
    }
    uint32_t id = ++bc_next;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(bc_label[id & 1023], sizeof bc_label[0], fmt, ap);
    va_end(ap);
    bc_draws = 0;
    rec_CmdFillBuffer(cb, bc_buf, 0, 4, id);
}
void vk_bc_mark(VkCommandBuffer cb, const char *label) {  // (device.c: the window image's steps)
    vk_full_barrier(cb);
    bc_mark(cb, "%s", label);
}
static void bc_draw(uint64_t fs_hash) {  // (a draw in the current pass: its first shader and the count go in its label)
    if (bc_on <= 0) return;
    char *l = bc_label[bc_next & 1023];
    size_t n = strlen(l);
    if (!bc_draws++) snprintf(l + n, sizeof bc_label[0] - n, ", first fs %016llx", (unsigned long long)fs_hash);
}

void vk_devlost_dump(void) {
    if (bc_ptr) {
        vmaInvalidateAllocation(g_vk.vma, bc_mem, 0, 4);
        uint32_t at = bc_ptr[0];
        LOG("[vk] GPU breadcrumbs: reached operation %u of %u recorded\n", at, bc_next);
        for (uint32_t i = at > 4 ? at - 4 : 1; i <= at + 3 && i <= bc_next; i++)
            LOG("[vk]   %s%u: %s\n", i == at ? "=> " : "   ", i, bc_label[i & 1023]);
    }
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) rec_CmdResetQueryPool(cb, gp_qp, 0, 512);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        rec_EndCommandBuffer(s->upload);
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    s->ts_skip = wait != VK_NULL_HANDLE;
    if (s->ts_win && !s->ts_skip) s->ts_win = false;  // (not the window image's after all: counted as game work)
    if (s->ts) rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 3 + 1);
    rec_EndCommandBuffer(s->main);
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    bool fenced = fences_on();
    VkFence fence = fenced ? fence_for(submit_seq + 1) : VK_NULL_HANDLE;
    if (fenced) atomic_store(&fence_present[(submit_seq + 1) % FENCE_N], wait != VK_NULL_HANDLE);
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = fenced ? (signal ? 1 : 0) : (signal ? 2 : 1),
                        .pSignalSemaphoreInfos = fenced ? &sig[1] : sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        rec_QueueSubmit2(&si, fence, submit_seq + 1);  // (the recorder: in order with the commands; its vkQueueSubmit2 time is in RecStats)
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    cur_sub = (cur_sub + 1) % SUBS;
    pthread_mutex_unlock(&seq_lock);
    passes_in_sub = 0;
    stat_flushes++;
    work_since_submit = 0;
    done_submitted();
    vk_mirror_writeback();
    cum_submits++;
    ring_retire();
    vk_collect_dead();
    if (signal) gc_frame();  // (a present: once a frame)
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
    vk_mirror_writeback();  // (what the GPU wrote, in guest memory: a buffer copy reads it next)
}

// The latest submission the driver has (rqueue.h): with the recorder, behind renderer_issued_seq until it catches up.
uint64_t renderer_submit_seq(void) { return rec_submitted(); }

// The latest submission this backend queued (rqueue.c: an item's work ends with it; renderer_wait_seq waits for the
// recorder to hand it to the driver, then for the GPU)
uint64_t renderer_issued_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    return s;
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
    vk_mirror_writeback();
}

static void submit_upto(void *a) {  // (render thread) the work up to submission *a goes out, if it hasn't
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    if (*(uint64_t *)a > s) renderer_submit();
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    Sub *s = &subs[cur_sub];
    // A reused slot still has its old begun flags until sub() prepares it. An empty queue fence must refer to
    // the last submission, not a future value that submit's empty-work fast path will never signal.
    bool pending = s->seq == submit_seq + 1 && (s->main_begun || s->upload_begun);
    uint64_t q = submit_seq + (pending ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

// The work up to submission seq submitted (copies of guest data taken), without waiting for the GPU (objects.c)
void renderer_submit_upto(uint64_t seq) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    if (seq > s && rq_on()) rq_call(submit_upto, &seq);
    else if (seq > s) renderer_submit();
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    // With the render thread, a fence still in the submission being recorded: it goes out now, and the wait is the
    // real one - the game rewrites what it thinks the GPU is done with (dynamic vertex data, copied at submit time)
    if (seq > s && rq_on()) {
        rq_call(submit_upto, &seq);
        pthread_mutex_lock(&seq_lock);
        s = submit_seq;
        pthread_mutex_unlock(&seq_lock);
    }
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not queued yet (other thread): everything queued so far (vk_wait_seq: the recorder, then the GPU)
    vk_mirror_writeback();  // (what the GPU wrote, in guest memory before the game reads it)
}

// A descriptor set for the pending submission (record.c: the slot's pools, allocated in batches per layout). With
// the recorder on it is a stand-in, to be written and bound through rec_* only, and only for this submission.
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    sub();
    return rec_alloc_set(cur_sub, layout);
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed. Without resizable BAR the device-local host-visible memory is a
// small window (256 MB on a GTX 1060): once it's full, later chunks go to system memory (issue #45: it stopped the
// game with "out of device memory").
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    Chunk *grown = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    if (!grown) FATAL("[vk] transient chunk table allocation failed\n");
    chunks = grown;
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    static bool sysmem;  // (the device-local window ran out: system memory from then on)
    // Without resizable BAR the device-local host-visible window is ~256 MB, shared with other programs and the driver:
    // two 64 MB chunks took 128 of its 132 MB in use while its budget fell to 81 MB (RTX 3080 Ti), and a GTX 1060 ran
    // out (issue #45). A chunk goes there only while the window's use stays under half its budget; past that, system
    // memory (~1 ms more GPU time a frame on a 3080 Ti at 1x). The first chunks, which most frames use, stay fast.
    // LARECOMP_VK_RING_BAR=1: no budget check.
    bool spill = false;
    if (!sysmem && !getenv("LARECOMP_VK_RING_BAR")) {
        uint32_t type;
        if (vmaFindMemoryTypeIndexForBufferInfo(g_vk.vma, &bi, &ai, &type) == VK_SUCCESS) {
            const VkPhysicalDeviceMemoryProperties *mp;
            vmaGetMemoryProperties(g_vk.vma, &mp);
            uint32_t heap = mp->memoryTypes[type].heapIndex;
            VmaBudget bud[VK_MAX_MEMORY_HEAPS];
            vmaGetHeapBudgets(g_vk.vma, bud);
            spill = (mp->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    bud[heap].usage + size > bud[heap].budget / 2;
            static bool logged;
            if (spill && !logged) {
                logged = true;
                LOG("[vk] transient ring: device-local window %llu of %llu MB budget in use: new chunks in system memory\n",
                    (unsigned long long)(bud[heap].usage >> 20), (unsigned long long)(bud[heap].budget >> 20));
            }
        }
    }
    if (sysmem || spill || getenv("LARECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VkResult r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    bool moved = false;
    // (a driver may report the full window as out of host memory too; if memory is really gone, the retry fails as well)
    if ((r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) && ai.usage != VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
        LOG("[vk] transient ring: device-local memory full after %d chunks, system memory from now on\n", nchunks);
        vk_log_memory("ring chunk failed");
        sysmem = moved = true;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    }
    if (r != VK_SUCCESS) {  // (system memory too: vk_transient waits for a chunk to come free instead - issue #45)
        LOG("[vk] transient ring: no memory for chunk %d (%llu MB, error %d)\n", nchunks, (unsigned long long)(size >> 20), r);
        vk_log_memory("ring chunk failed");
        return -1;
    }
    c->ptr = info.pMappedData;
    c->size = size;
    static bool spilled;  // (the first chunk in system memory: logged too)
    if (!nchunks || moved || (spill && !spilled)) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        if (spill) spilled = true;
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    if (pick < 0) {  // no memory for another chunk (a GTX 1060 with 6 GB: issue #45): wait for the work already submitted,
        // which frees the chunks only it used, and take one of those (the recording in progress keeps its own)
        vk_wait_seq(submit_seq);
        done = vk_completed_seq();
        for (int i = 0; i < nchunks; i++)
            if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
        if (pick < 0) FATAL("the graphics card ran out of memory for uploads (%llu MB needed, %d chunks in use)\n",
                            (unsigned long long)(size >> 20), nchunks);
        static int nlog;
        if (nlog++ < 5) LOG("[vk] transient ring: reused chunk %d after waiting for the GPU\n", pick);
    }
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
#define GCOPY_PROBE_SMALL_MAX 8192u
static struct { uint64_t addr, size, seq; VkSpan span; uint64_t scanned; } gcopy[GCOPY_TAB];  // (scanned: heap size then, 0 not)
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
// Diagnostic only: never refresh the cached copy or change what a draw binds.
// The active guest reservation is readable throughout its lifetime (platform.h);
// dirty tracking removes WRITE, not READ. Reject outside/overflowing ranges.
static void gcopy_probe_hit(uint64_t addr, uint64_t size, VkSpan cached) {
    static int enabled = -1;
    static unsigned mismatches;
    static struct { uint64_t addr, size, seq, gen, first; } seen[64];
    if (enabled < 0) {
        const char *e = getenv("LARECOMP_GCOPY_PROBE");
        enabled = e && atoi(e) != 0;
    }
    if (!enabled || mismatches >= 64 || !cached.ptr || !size || size > GCOPY_PROBE_SMALL_MAX ||
        addr < GUEST_LO || addr >= GUEST_HI || size > GUEST_HI - addr) return;
    const uint8_t *before = cached.ptr, *after = (const uint8_t *)(uintptr_t)addr;
    if (!memcmp(before, after, (size_t)size)) return;
    uint64_t first = 0;
    while (first < size && before[first] == after[first]) first++;
    if (first == size) return; // the concurrently running guest may have changed it again
    uint64_t seq = vk_pending_seq(), gen = gcopy_gen;
    for (unsigned i = 0; i < mismatches; i++)
        if (seen[i].addr == addr && seen[i].size == size && seen[i].seq == seq &&
            seen[i].gen == gen && seen[i].first == first) return;
    unsigned n = size - first < 32 ? (unsigned)(size - first) : 32;
    char old_hex[65], new_hex[65];
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) {
        uint8_t a = before[first + i], b = after[first + i];
        old_hex[i * 2] = hex[a >> 4]; old_hex[i * 2 + 1] = hex[a & 15];
        new_hex[i * 2] = hex[b >> 4]; new_hex[i * 2 + 1] = hex[b & 15];
    }
    old_hex[n * 2] = new_hex[n * 2] = 0;
    seen[mismatches].addr = addr; seen[mismatches].size = size; seen[mismatches].seq = seq;
    seen[mismatches].gen = gen; seen[mismatches].first = first;
    mismatches++;
    LOG("[gcopy-probe] mismatch %u addr %llx size %llu seq %llu gen %llu first %llu bytes %u cached %s guest %s\n",
        mismatches, (unsigned long long)addr, (unsigned long long)size,
        (unsigned long long)seq, (unsigned long long)gen,
        (unsigned long long)first, n, old_hex, new_hex);
}
// Controlled diagnostic only: isolate camera/model UBO sharing without copying
// every large shader table per draw. Normal sessions retain the original cache.
static bool gcopy_bypass_small(uint64_t size) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("LARECOMP_GCOPY_BYPASS_SMALL");
        enabled = e && atoi(e) != 0;
    }
    return enabled && size && size <= GCOPY_PROBE_SMALL_MAX;
}
// scan: (lazy heap chunks, a uniform buffer of a shader with global memory) the range is also scanned for heap
// pointers (gc_scan_pointers), once per shared copy: its contents can't change while the copy is shared (again if the
// heap grew since: pointers past its end were passed over).
static bool gc_scan_needed(void);
static uint64_t prof_scan_calls, prof_scan_bytes, prof_scan_skipped;  // (VK_PROF, lazy chunks) pointer scans
static VkSpan guest_copy_ex(uint64_t addr, uint64_t size, bool scan) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    bool hit = !gcopy_bypass_small(size) && gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size;
    uint64_t heap = 0;
    if (scan) {
        heap = gmem_heap_size();
        if ((!hit || gcopy[slot].scanned != heap) && gc_scan_needed()) gc_scan_pointers((const uint32_t *)(uintptr_t)addr, size);
        else prof_scan_skipped++;
    }
    if (hit) {
        gcopy_probe_hit(addr, size, gcopy[slot].span);
        if (scan) gcopy[slot].scanned = heap;
        return gcopy[slot].span;
    }
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].scanned = heap;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) { return guest_copy_ex(addr, size, false); }

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; graphics draws wait for readiness.
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
static JobQueue g_jobs = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .once = PTHREAD_ONCE_INIT, .bg_max = 2};
static JobQueue g_tjobs = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .once = PTHREAD_ONCE_INIT, .bg_max = 1};
static int shader_workers = 1; // guarded by g_jobs.lock
static bool shader_preparing; // loading may use the whole bounded compiler pool

void vk_shader_preparation_mode(bool preparing) {
    pthread_mutex_lock(&g_jobs.lock);
    shader_preparing = preparing;
    g_jobs.bg_max = shader_background_capacity(shader_workers, preparing);
    pthread_cond_broadcast(&g_jobs.cond);
    pthread_mutex_unlock(&g_jobs.lock);
}

static void queue_push(JobQueue *q, void (*fn)(void *), void *arg, bool bg) {
    if (!shader_job_queue_push(q, fn, arg, bg))
        FATAL("[vk] shader job allocation failed\n");
}

static void *worker(void *a) {
    plat_thread_name("vk-pipeline");
    (void)a;
    shader_job_queue_run(&g_jobs);
    return NULL;
}

// Keep compilation on the worker queues, but do not present a frame with
// required geometry/composition missing just because its first build is pending.
static int graphics_wait_ready(_Atomic int *state) {
    int status = atomic_load_explicit(state, memory_order_acquire);
    if (status != 0) return status;
    uint64_t start = SDL_GetTicksNS();
    do {
        SDL_Delay(1); // yield the CPU to the translator/compiler, never spin
        status = atomic_load_explicit(state, memory_order_acquire);
    } while (status == 0);
    uint64_t waited = SDL_GetTicksNS() - start;
    __atomic_fetch_add(&stat_wait_ns, waited, __ATOMIC_RELAXED);
    g_blocked_ns += waited;
    return status;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static void *translator(void *a) {
    plat_thread_name("vk-translate");
    (void)a;
    shader_job_queue_run(&g_tjobs);
    return NULL;
}
static void start_translator(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, translator, NULL))
        FATAL("[vk] shader translator unavailable\n");
    pthread_detach(t);
}
static void run_translate_q(void (*fn)(void *), void *arg, bool bg) {
    if (sync_shaders() && !bg) { fn(arg); return; }
    pthread_once(&g_tjobs.once, start_translator);
    queue_push(&g_tjobs, fn, arg, bg);
}
static void run_translate(void (*fn)(void *), void *arg) { run_translate_q(fn, arg, false); }

static void start_workers(void) {
    bool android = false;
#ifdef __ANDROID__
    android = true;
#endif
    int n = shader_worker_count(plat_cpu_count(), SDL_GetNumLogicalCPUCores(), android,
                                getenv("LARECOMP_VK_JOBS"));
    int started = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (!pthread_create(&t, NULL, worker, NULL)) { pthread_detach(t); started++; }
    }
    if (!started) FATAL("[vk] shader compiler workers unavailable\n");
    pthread_mutex_lock(&g_jobs.lock);
    shader_workers = started;
    g_jobs.bg_max = shader_background_capacity(started, shader_preparing);
    pthread_cond_broadcast(&g_jobs.cond);
    pthread_mutex_unlock(&g_jobs.lock);
    LOG("[vk] shader compiler: %d workers, one translator, CPU-aware capacity\n", started);
}
static void run_job_q(void (*fn)(void *), void *arg, bool bg) {
    if (sync_shaders() && !bg) { fn(arg); return; }
    pthread_once(&g_jobs.once, start_workers);
    queue_push(&g_jobs, fn, arg, bg);
}
static void run_job(void (*fn)(void *), void *arg) { run_job_q(fn, arg, false); }

// The driver's pipeline cache (cache/vk_pipelines.bin) saved once new pipelines have settled: a few seconds after the
// last one built, on a worker. Phones end apps without exit() (swiped away, killed in the background), so the exit
// handler's save never came and every start built every pipeline again: pop-in and CPU in every area. Per frame.
static _Atomic uint64_t g_pipes_built;  // pipelines created (build_pipeline, build_compute)
static void cache_save_job(void *arg) { (void)arg; extern void vk_pipeline_cache_save(void); vk_pipeline_cache_save(); }
// Also every 30 s while new ones keep coming: the head start's hundreds of pipelines build for minutes after a shader
// change (a couple a second on a phone), never a 4 s pause, and a session ended before the 3-minute saver lost them all.
void vk_pipeline_cache_autosave(void) {
    static uint64_t last_n, saved_n, changed_at, saved_at;
    uint64_t n = atomic_load(&g_pipes_built), now = SDL_GetTicksNS();
    if (!saved_at) saved_at = now;
    bool due = n != saved_n && now - saved_at > 30000000000ull;
    if (n != last_n) { last_n = n; changed_at = now; if (!due) return; }
    if (n != saved_n && (due || now - changed_at > 4000000000ull)) { saved_n = n; saved_at = now; run_job(cache_save_job, NULL); }
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

// BindProgram(program, stages): the stages in the mask come from `program` (NULL: unbound), the others stay. A bind
// that leaves one program's stages and nothing else uses that program itself (as before); a mix of stages from
// several programs (Let's Go: vertex and fragment bound separately) uses a combined program. The shader caches are
// keyed by code address, so the combination needs no identity of its own.
static NvnProgram g_comb_prog;
static void bind_program_stages(NvnProgram *p, uint32_t mask) {
    if (!mask) mask = p ? p->stage_mask : 0x3F;  // (no mask recorded: the whole program)
    NvnProgram *cur = st.program;
    NvnProgram next = cur ? *cur : (NvnProgram){0};
    next.backend = NULL;
    for (int i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (p && (p->stage_mask & bit)) next.stages[i] = p->stages[i], next.stage_mask |= bit;
        else memset(&next.stages[i], 0, sizeof next.stages[i]), next.stage_mask &= ~bit;
    }
    if (!next.stage_mask) { st.program = NULL; return; }
    if (p && next.stage_mask == p->stage_mask &&
        !memcmp(next.stages, p->stages, sizeof next.stages)) { st.program = p; return; }
    g_comb_prog = next;
    st.program = &g_comb_prog;
}


uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms (LARECOMP_HITCH_MS) are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    static uint64_t limit;  // LARECOMP_HITCH_MS=<ms>: the threshold (default 50)
    if (!limit) limit = (getenv("LARECOMP_HITCH_MS") ? (uint64_t)atoi(getenv("LARECOMP_HITCH_MS")) : 50) * 1000000ull;
    if (last_t && now - last_t > limit) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static uint64_t stat_set_hits;  // (stage_set: sets reused)
// (the slow-frame log, slowframe.h; the game thread) running totals this file keeps anyway
void renderer_slow_counters(uint64_t *submits, uint64_t *gpu_wait_ns, uint64_t *imports) {
    *submits = __atomic_load_n(&cum_submits, __ATOMIC_RELAXED);
    *gpu_wait_ns = __atomic_load_n(&cum_wait, __ATOMIC_RELAXED);
    *imports = (uint64_t)__atomic_load_n(&gc_imported, __ATOMIC_RELAXED);
}

// LARECOMP_LOG_SKIPS=1: the skipped draws and dispatches of each stats interval by reason, and the top offenders
// (shader code addresses and content hashes, as in "[shader] slow build" / shader_variants.txt, and how long they
// have been skipped without a break). "building" for tens of seconds is a build that never finishes.
enum { SK_NO_PROGRAM, SK_NO_STAGES, SK_PROG_BUILDING, SK_PROG_FAILED, SK_PIPE_BUILDING, SK_PIPE_FAILED, SK_CS_BUILDING,
       SK_CS_FAILED, SK_TOPOLOGY, SK_DEBUG_SKIP, SK_HIDDEN, SK_N };
static const char *const skip_names[SK_N] = {
    "no program", "program missing a stage", "program building", "program failed", "pipeline building", "pipeline failed",
    "compute building", "compute failed", "topology", "debug skip list", "hidden on purpose"};
static int log_skips = -1;  // (-1: not read yet: the first skip reads it)
static uint64_t skip_n[SK_N];
#define SKIP_TOP 32
static struct { int why; uint64_t a, b, ha, hb, n, first_ns; } skip_top[SKIP_TOP];
static int skip_ntop;
static void skip_note(int why, bool compute) {
    if (log_skips < 0) log_skips = getenv("LARECOMP_LOG_SKIPS") && atoi(getenv("LARECOMP_LOG_SKIPS")) != 0;
    if (!log_skips) return;
    skip_n[why]++;
    const NvnProgram *p = st.program;
    uint64_t a = p ? p->stages[compute ? 5 : 0].code_addr : 0, b = p && !compute ? p->stages[1].code_addr : 0;
    int j = 0;
    while (j < skip_ntop && !(skip_top[j].why == why && skip_top[j].a == a && skip_top[j].b == b)) j++;
    if (j == skip_ntop) {
        if (skip_ntop < SKIP_TOP) skip_ntop++;
        else {  // (full: the entry skipped least this interval goes)
            j = 0;
            for (int i = 1; i < SKIP_TOP; i++) if (skip_top[i].n < skip_top[j].n) j = i;
        }
        skip_top[j].why = why; skip_top[j].a = a; skip_top[j].b = b; skip_top[j].n = 0;
        skip_top[j].ha = p ? shader_stage_hash(&p->stages[compute ? 5 : 0]) : 0;
        skip_top[j].hb = p && !compute ? shader_stage_hash(&p->stages[1]) : 0;
        skip_top[j].first_ns = SDL_GetTicksNS();
    }
    skip_top[j].n++;
}
#define SKIP(why) do { stat_skipped++; if (log_skips) skip_note(why, false); } while (0)
#define SKIP_CS(why) do { stat_skipped++; if (log_skips) skip_note(why, true); } while (0)
static void skip_log(void) {
    if (log_skips <= 0) return;
    uint64_t total = 0;
    for (int i = 0; i < SK_N; i++) total += skip_n[i];
    if (total) {
        LOG("[skips] %llu:", (unsigned long long)total);
        for (int i = 0; i < SK_N; i++) if (skip_n[i]) LOG(" %s %llu", skip_names[i], (unsigned long long)skip_n[i]);
        uint64_t now = SDL_GetTicksNS();
        bool shown[SKIP_TOP] = {0};
        for (int t = 0; t < 4; t++) {  // the top offenders of this interval
            int best = -1;
            for (int i = 0; i < skip_ntop; i++)
                if (skip_top[i].n && !shown[i] && (best < 0 || skip_top[i].n > skip_top[best].n)) best = i;
            if (best < 0) break;
            shown[best] = true;
            bool cs = skip_top[best].why == SK_CS_BUILDING || skip_top[best].why == SK_CS_FAILED;
            LOG("%s %s: %s %llx (%016llx)", t ? "," : " | top", skip_names[skip_top[best].why], cs ? "cs" : "vs",
                (unsigned long long)skip_top[best].a, (unsigned long long)skip_top[best].ha);
            if (!cs) LOG(" fs %llx (%016llx)", (unsigned long long)skip_top[best].b, (unsigned long long)skip_top[best].hb);
            LOG(" x%llu, for %llu s", (unsigned long long)skip_top[best].n,
                (unsigned long long)((now - skip_top[best].first_ns) / 1000000000ull));
        }
        LOG("\n");
    }
    memset(skip_n, 0, sizeof skip_n);
    int k = 0;  // (an offender not skipped this interval is dropped: its "for N s" starts again if it comes back)
    for (int i = 0; i < skip_ntop; i++)
        if (skip_top[i].n) { skip_top[k] = skip_top[i]; skip_top[k++].n = 0; }
    skip_ntop = k;
}

// Builds in flight (programs, graphics / compute pipelines): one still running after 10 s is logged once, with its
// shaders' code addresses. "[shader] slow build" only comes when a build finishes, so one that never does (its draws
// or dispatches skipped for good, compile 0 in the stats) left no trace.
#define BUILDS_MAX 64
static pthread_mutex_t builds_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { const char *what; uint64_t c0, c1, t0; bool logged; } builds[BUILDS_MAX];
static int build_begin(const char *what, uint64_t c0, uint64_t c1) {
    pthread_mutex_lock(&builds_lock);
    int s = 0;
    while (s < BUILDS_MAX && builds[s].what) s++;
    if (s < BUILDS_MAX) { builds[s].what = what; builds[s].c0 = c0; builds[s].c1 = c1; builds[s].t0 = SDL_GetTicksNS(); builds[s].logged = false; }
    pthread_mutex_unlock(&builds_lock);
    return s < BUILDS_MAX ? s : -1;
}
static void build_end(int s) {
    if (s < 0) return;
    pthread_mutex_lock(&builds_lock);
    builds[s].what = NULL;
    pthread_mutex_unlock(&builds_lock);
}
static void build_diag_begin_program(uint64_t key, uint64_t vs_hash, uint64_t fs_hash) {
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] begin modules vs/fs key %016llx vs_hash %016llx fs_hash %016llx\n",
            (unsigned long long)key, (unsigned long long)vs_hash, (unsigned long long)fs_hash);
}
static void build_diag_begin_compute(uint64_t stage_hash, uint64_t code_or_prepared, const uint32_t local[3]) {
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] begin pipeline cs stage_hash %016llx code_or_prepared %016llx local %u,%u,%u\n",
            (unsigned long long)stage_hash, (unsigned long long)code_or_prepared, local[0], local[1], local[2]);
}
static void build_diag_done(const char *what, uint64_t id, uint64_t t0, int state) {
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] done %s id %016llx state %d took %llu ms\n", what, (unsigned long long)id,
            state, (unsigned long long)((SDL_GetTicksNS() - t0) / 1000000));
}
static void builds_check(void) {
    uint64_t now = SDL_GetTicksNS();
    pthread_mutex_lock(&builds_lock);
    for (int s = 0; s < BUILDS_MAX; s++) {
        if (!builds[s].what || builds[s].logged || now - builds[s].t0 < 10000000000ull) continue;
        builds[s].logged = true;
        LOG("[shader] still building after %llu s: %s %llx %llx\n", (unsigned long long)((now - builds[s].t0) / 1000000000ull),
            builds[s].what, (unsigned long long)builds[s].c0, (unsigned long long)builds[s].c1);
    }
    pthread_mutex_unlock(&builds_lock);
}

const char *renderer_stats(void) {
    skip_log();
    builds_check();
    static uint64_t logged_compiler_requests;
    ShaderCompilerStats compiler;
    shader_compiler_stats(&compiler);
    uint64_t compiler_requests = compiler.stage[STAGE_VERTEX].requests + compiler.stage[STAGE_FRAGMENT].requests;
    if (compiler_requests != logged_compiler_requests) {
        logged_compiler_requests = compiler_requests;
        LOG("[shader-compiler] direct VS %llu FS %llu; GLSL fallback VS %llu FS %llu; attempts %llu\n",
            (unsigned long long)compiler.stage[STAGE_VERTEX].direct,
            (unsigned long long)compiler.stage[STAGE_FRAGMENT].direct,
            (unsigned long long)compiler.stage[STAGE_VERTEX].fallback,
            (unsigned long long)compiler.stage[STAGE_FRAGMENT].fallback,
            (unsigned long long)(compiler.stage[STAGE_VERTEX].direct_attempts + compiler.stage[STAGE_FRAGMENT].direct_attempts));
    }
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu, upload %.1fMB, sets reused %llu",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0,
             (unsigned long long)stat_set_hits);
    stat_feedback_copies = stat_upload_bytes = stat_set_hits = 0;
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        RecStats rs;  // (the recorder's counters for this interval: record.c)
        rec_stats_take(&rs);
        prof_submit = rs.submit_ns;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        if (gc_shift) {
            snprintf(stats_text + n, sizeof stats_text - n, " | heap chunks %d imported (%llu MB), %llu misses", gc_imported,
                     (unsigned long long)gc_imported_bytes >> 20, (unsigned long long)stat_gc_misses);
            n = strlen(stats_text);
            if (gc_lazy) {
                snprintf(stats_text + n, sizeof stats_text - n, ", pointer scans %llu (%.2f MB), %llu skipped",
                         (unsigned long long)prof_scan_calls, prof_scan_bytes / 1048576.0, (unsigned long long)prof_scan_skipped);
                n = strlen(stats_text);
            }
            prof_scan_calls = prof_scan_bytes = prof_scan_skipped = 0;
        }
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_is_slow() ? "30" : "60", (unsigned long long)pace_frames_over());
        stat_counters_gpu = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu", (unsigned long long)prof_tex_watch_skips);
            prof_tex_watch_skips = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | const blocks %s: %llu reused %llu copied", cbuf_lean() ? "lean" : "full",
                     (unsigned long long)prof_cb_reused, (unsigned long long)prof_cb_copied);
            prof_cb_reused = prof_cb_copied = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | hook pass %llu draws, %llu with known textures",
                     (unsigned long long)prof_hook_draws, (unsigned long long)prof_hook_known);
            prof_hook_draws = prof_hook_known = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | key memo %s: program %llu pipeline %llu of %llu", key_memo_on() ? "on" : "off",
                     (unsigned long long)prof_prog_memo, (unsigned long long)prof_pipe_memo, (unsigned long long)prof_key_draws);
            prof_prog_memo = prof_pipe_memo = prof_key_draws = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | sets (reuse %s, batch %s, recycle %s): %llu reused %llu written, %llu allocated"
                     " (%llu new from the pools in %llu calls), pool resets %llu %.2f ms%s",
                     set_reuse_on() ? "on" : "off", rec_set_batch() ? "on" : "off", rec_set_recycle() ? "on" : "off",
                     (unsigned long long)prof_sets_reused, (unsigned long long)prof_sets_new, (unsigned long long)rs.set_allocs,
                     (unsigned long long)rs.sets_fresh, (unsigned long long)rs.set_calls, (unsigned long long)rs.pool_resets,
                     rs.pool_reset_ns / 1e6, rec_on() ? " (on the recorder)" : "");
            prof_sets_reused = prof_sets_new = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | slot wait %.1f ms", prof_slot_wait / 1e6);
            prof_slot_wait = 0;
            n = strlen(stats_text);
            // the recorder thread (record.c): packets queued, the translating thread's time queuing them, the recorder's
            // time making the calls, and the waits on it (drain: ImGui / idle waits; seq: GPU waits that had to wait for a
            // submission to be handed to the driver first; present: the next frame's acquire waiting for the last present;
            // full: the ring full)
            snprintf(stats_text + n, sizeof stats_text - n, " | recorder %s: %llu packets (%.1f MB) enqueue %.1f ms, busy %.1f ms (%llu wakes), waits:"
                     " drain %llux %.1f ms, seq %llux %.1f ms, present %llux %.1f ms, full %llux %.1f ms, pipes %llux %.1f ms, dropped %llu",
                     rec_on() ? "on" : "off", (unsigned long long)rs.packets, rs.bytes / 1048576.0, rs.enqueue_ns / 1e6, rs.busy_ns / 1e6,
                     (unsigned long long)rs.wakeups, (unsigned long long)rs.drains, rs.drain_ns / 1e6, (unsigned long long)rs.seq_waits,
                     rs.seq_wait_ns / 1e6, (unsigned long long)rs.present_waits, rs.present_wait_ns / 1e6, (unsigned long long)rs.full_waits,
                     rs.full_ns / 1e6, (unsigned long long)rs.pipe_waits, rs.pipe_wait_ns / 1e6, (unsigned long long)rs.draws_dropped);
        }
        {   // GPU time per frame by part (see gpu_timer_add), CPU waits per frame by place (device.c g_vk_frame_waits)
            static uint64_t lw[4];
            uint64_t dw[4];
            for (int i = 0; i < 4; i++) {  // ([2], [3]: the recorder's)
                uint64_t v = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);
                dw[i] = v - lw[i], lw[i] = v;
            }
            double nf = prof_wframes ? (double)prof_wframes : 1, np = dw[3] ? (double)dw[3] : 1;
            double gap = ((double)prof_span - (double)prof_gpu - (double)prof_win) / nf / 1e6;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | gpu/frame game %.1f window %.1f span %.1f gap %.1f ms (%llu frames)"
                     " | cpu wait/frame slot %.1f acquire %.1f present %.1f ms",
                     prof_gpu / nf / 1e6, prof_win / nf / 1e6, prof_span / nf / 1e6, prof_wframes ? gap : 0.0,
                     (unsigned long long)prof_wframes, dw[0] / np / 1e6, dw[1] / np / 1e6, dw[2] / np / 1e6);
            prof_win = prof_span = prof_wframes = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

static VkPipeline pipe_resolve(void *obj, bool wait);
void vk_exec_init(void) {
    pace_busy_adjust = vk_pace_adjust;
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) prof_up_ns = 0;
    // the GPU timer (LARECOMP_VK_GPU_TIMER=0: off - dynamic resolution then goes by the frame times)
    const char *gt = getenv("LARECOMP_VK_GPU_TIMER");
    if (g_vk.ts_bits && g_vk.props.limits.timestampComputeAndGraphics && g_vk.props.limits.timestampPeriod > 0 && !(gt && !atoi(gt))) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 3};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    gpu_timer_on = prof_qp != VK_NULL_HANDLE;
    LOG("[vk] GPU timer %s (dynamic resolution goes by %s)\n", gpu_timer_on ? "on" : "off", gpu_timer_on ? "GPU time" : "frame times");
    st_defaults();
    // The heap's chunk / lazy decision now, on this thread, before the render thread and the build workers exist:
    // they read gc_shift / gc_lazy directly (a buffer copy can come before any shader was translated)
    gmem_chunk_shift();
    // The command buffers of every slot, from the pool, before the recorder thread exists: from then on the pool and
    // the buffers are the recorder's (begun, recorded, ended and submitted in packet order)
    for (int i = 0; i < SUBS; i++) {
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cbs));
        subs[i].main = cbs[0];
        subs[i].upload = cbs[1];
    }
    rec_set_pipe_resolver(pipe_resolve);
    rec_init(prof != 0);
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);  // (textures.c: the part the contents occupy)
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static float tex_fr[2][64][2];  // per bound texture slot: the part of the texture the contents occupy (Dynamic)
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static VkImageView pass_depth_attachment_view;
static int debug_pass_no;

// A draw's vertex layout. Without dynamic vertex input it is copied into the PipeKey (each layout its own pipeline);
// with it (g_vk.vids) the key leaves it out and draw_ sets it (vkCmdSetVertexInputEXT) when it changes.
typedef struct {
    uint32_t attr_mask, used_streams;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t stride[17], divisor[17];  // divisor 0: per vertex; else per instance
} VtxLayout;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets: an identical one written earlier in the submission is reused (stage_set) and not bound again when the
// dynamic offsets are the same too (with dynamic uniform buffers most sets repeat their program's previous one, which
// stage_set keeps: "set reuse"). LARECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16, SS_DS = 1 << 17, SS_VI = 1 << 18 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    const void *pipe;   // the Pipe bound (a building one has no handle yet: WAIT_PIPES)
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
    VkDescriptorSet ds[2];
    uint32_t ndyn, dyn[2 * 96];  // (the sets' dynamic offsets, as bound: 2 * MAX_BIND)
    VtxLayout vi;  // (dynamic vertex input)
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("LARECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

// ---- GPU time per pass (LARECOMP_VK_PASS_GPU=<minimum render frame>).
// One bounded capture; no shader payloads. Readback never waits for a submission.
#define PG_MAX 1024
#define PG_READBACK_NS 5000000000ull
#define PG_READBACK_FRAMES 120
static VkQueryPool pg_qp;
static int pg_n, pg_state;  // 0 armed/disabled, 1 recording, 2 awaiting results, 3 done
static uint64_t pg_target, pg_frame, pg_draws0, pg_verts, pg_seq, pg_poll_ns;
static bool pg_full, pg_missed, pg_pending;
static char pg_what[PG_MAX][112];
static uint64_t pg_vc[PG_MAX];
uint64_t renderer_present_count(void);
static bool pg_enabled(void) {
    static bool initialized;
    if (!initialized) {
        initialized = true;
        const char *e = getenv("LARECOMP_VK_PASS_GPU");
        if (!e) return false;
        const char *p = e;
        while (*p >= '0' && *p <= '9' && pg_target <= (UINT64_MAX - (unsigned)(*p - '0')) / 10)
            pg_target = pg_target * 10 + (unsigned)(*p++ - '0');
        if (p == e || *p) {
            LOG("[passgpu] invalid target; expected a positive decimal render frame\n");
            pg_target = 0, pg_state = 3;
        } else if (pg_target) {
            LOG("[passgpu] armed minimum render frame %llu (1024 marks, nonblocking readback)\n", (unsigned long long)pg_target);
        }
    }
    return pg_target && pg_state != 3;
}
static void pg_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!pg_enabled()) return;
    uint64_t frame = vk_frame_counter();
    if (pg_state == 0) {
        if (frame < pg_target) return;
        if (!g_vk.ts_bits || g_vk.ts_bits > 64 || !g_vk.props.limits.timestampComputeAndGraphics ||
            !(g_vk.props.limits.timestampPeriod > 0) || !isfinite(g_vk.props.limits.timestampPeriod)) {
            LOG("[passgpu] capture failed: unsupported timestamp properties\n");
            pg_state = 3;
            return;
        }
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PG_MAX};
        VkResult result = vkCreateQueryPool(g_vk.dev, &qi, NULL, &pg_qp);
        if (result != VK_SUCCESS) {
            LOG("[passgpu] capture failed: create query pool result %d\n", (int)result);
            pg_state = 3;
            return;
        }
        rec_CmdResetQueryPool(cb, pg_qp, 0, PG_MAX);
        pg_state = 1, pg_frame = frame, pg_n = 0;
        LOG("[passgpu] capture render frame %llu, requested %llu, target %s\n", (unsigned long long)pg_frame,
            (unsigned long long)pg_target, frame == pg_target ? "matched" : "missed; capturing next work");
    }
    if (pg_state != 1) return;
    if (frame != pg_frame) { pg_state = 2; return; }
    if (pg_n >= PG_MAX) {
        // Stop recording state too: callers must not attach later draw counts to the final mark.
        pg_full = true, pg_state = 2;
        LOG("[passgpu] capture truncated at %d marks in render frame %llu\n", PG_MAX, (unsigned long long)pg_frame);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pg_what[pg_n], sizeof pg_what[0], fmt, ap);
    va_end(ap);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pg_qp, (uint32_t)pg_n++);
    pg_seq = vk_pending_seq();
}
// Each draw mark closes the segment since the previous mark; groups identify FS/VS pairs.
static bool pg_draws_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_VK_PASS_GPU_DRAWS") != NULL;
    return on;
}
static uint64_t pg_delta(uint64_t end, uint64_t start) {
    uint64_t delta = end - start;
    return g_vk.ts_bits < 64 ? delta & ((1ull << g_vk.ts_bits) - 1) : delta;
}
static void pg_report(void) {  // Called before the current present ends/submits its pass.
    if (!pg_enabled()) return;
    uint64_t frame = vk_frame_counter();
    if (pg_state == 0) {
        if (frame >= pg_target && !pg_missed) {
            pg_missed = true;
            LOG("[passgpu] target %llu reached without marks at render present %llu; armed for next work\n",
                (unsigned long long)pg_target, (unsigned long long)frame);
        }
        return;
    }
    if (frame <= pg_frame) return;  // Include the selected present's final end-pass mark; never wait on its unsent commands.
    if (pg_state == 1) pg_state = 2;
    if (pg_state != 2) return;
    if (pg_n < 2) {
        LOG("[passgpu] capture failed: render frame %llu has only %d mark(s)\n", (unsigned long long)pg_frame, pg_n);
        pg_state = 3;
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    if (!pg_poll_ns) pg_poll_ns = now;
    static uint64_t t[PG_MAX];
    VkResult result = vkGetQueryPoolResults(g_vk.dev, pg_qp, 0, (uint32_t)pg_n, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        if (!pg_pending) {
            pg_pending = true;
            LOG("[passgpu] readback pending: render frame %llu last-seq %llu submitted %llu\n",
                (unsigned long long)pg_frame, (unsigned long long)pg_seq, (unsigned long long)rec_submitted());
        }
        if (now - pg_poll_ns >= PG_READBACK_NS || frame - pg_frame >= PG_READBACK_FRAMES) {
            LOG("[passgpu] readback failed: timeout render frame %llu marks %d last-seq %llu submitted %llu; no query WAIT\n",
                (unsigned long long)pg_frame, pg_n, (unsigned long long)pg_seq, (unsigned long long)rec_submitted());
            pg_state = 3;  // Keep the pool alive: queued reset/write commands may still reference it.
        }
        return;
    }
    pg_state = 3;
    if (result != VK_SUCCESS) {
        LOG("[passgpu] readback failed: result %d render frame %llu marks %d\n", (int)result, (unsigned long long)pg_frame, pg_n);
        return;
    }
    double per = g_vk.props.limits.timestampPeriod / 1e6, total = 0;
    double dt[PG_MAX];
    int order[PG_MAX];
    for (int i = 0; i < pg_n - 1; i++) { order[i] = i; dt[i] = pg_delta(t[i + 1], t[i]) * per; total += dt[i]; }
    for (int i = 0; i < pg_n - 1; i++)
        for (int j = i + 1; j < pg_n - 1; j++)
            if (dt[order[j]] > dt[order[i]]) { int x = order[i]; order[i] = order[j]; order[j] = x; }
    LOG("[passgpu] render frame %llu requested %llu: %d marks, %.2f ms first to last, %s\n", (unsigned long long)pg_frame,
        (unsigned long long)pg_target, pg_n, total, pg_full || pg_n == PG_MAX ? "partial: mark capacity reached" : "complete marked range");
    for (int i = 0; i < pg_n - 1 && i < 40; i++) {
        int k = order[i];
        LOG("[passgpu] %6.3f ms #%d->#%d %s -> %s\n", dt[k], k, k + 1, pg_what[k], pg_what[k + 1]);
    }
    if (getenv("LARECOMP_VK_PASS_GPU_ALL"))
        for (int i = 1; i < pg_n; i++) LOG("[passall] %6.3f ms #%d %s\n", dt[i - 1], i, pg_what[i]);
    // Draw timestamps subdivide passes. Sum them back into attachment-labelled pass ranges.
    struct { int first, last; double ms; bool partial; } passes[PG_MAX];
    int np = 0, active = -1;
    for (int i = 0; i < pg_n; i++) {
        if (!strncmp(pg_what[i], "pass ", 5)) {
            if (active >= 0) passes[np++] = (__typeof__(passes[0])){active, i, 0, true};
            active = i;
        } else if (active >= 0 && !strncmp(pg_what[i], "(after pass,", 12)) {
            passes[np++] = (__typeof__(passes[0])){active, i, 0, false}; active = -1;
        }
    }
    if (active >= 0) passes[np++] = (__typeof__(passes[0])){active, pg_n - 1, 0, true};
    for (int i = 0; i < np; i++) for (int j = passes[i].first; j < passes[i].last; j++) passes[i].ms += dt[j];
    for (int i = 0; i < np && i < 25; i++) {
        int b = i;
        for (int j = i + 1; j < np; j++) if (passes[j].ms > passes[b].ms) b = j;
        if (b != i) { __typeof__(passes[0]) x = passes[i]; passes[i] = passes[b]; passes[b] = x; }
        LOG("[passgpu-pass] %6.3f ms #%d->#%d %s%s\n", passes[i].ms, passes[i].first, passes[i].last,
            pg_what[passes[i].first], passes[i].partial ? " (partial pass)" : "");
    }
    if (pg_draws_on()) {
        LOG("[drawgpu] vertices in captured marks: %llu\n", (unsigned long long)pg_verts);
        static struct { char what[112]; double ms; int n; uint64_t v; } sum[256];
        int ns = 0, omitted = 0;
        for (int i = 1; i < pg_n; i++) {
            if (strncmp(pg_what[i], "draw ", 5)) continue;
            int k = 0;
            while (k < ns && strcmp(sum[k].what, pg_what[i])) k++;
            if (k == ns && ns < 256) snprintf(sum[ns++].what, sizeof sum[0].what, "%s", pg_what[i]);
            if (k < 256) sum[k].ms += dt[i - 1], sum[k].n++, sum[k].v += pg_vc[i];
            else omitted++;
        }
        for (int i = 0; i < ns && i < 25; i++) {
            int b = i;
            for (int j = i + 1; j < ns; j++) if (sum[j].ms > sum[b].ms) b = j;
            if (b != i) { __typeof__(sum[0]) x = sum[i]; sum[i] = sum[b]; sum[b] = x; }
            LOG("[drawgpu] %6.3f ms %3d draws %8llu vertices %s\n", sum[i].ms, sum[i].n, (unsigned long long)sum[i].v, sum[i].what);
        }
        if (omitted) LOG("[drawgpu] partial grouping: %d draw marks exceed 256 shader-pair groups\n", omitted);
    }
}

void vk_pass_gpu_report(void) { pg_report(); }  // device.c, each present on the render thread

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    rec_CmdEndRendering(vk_cmd());
    in_pass = false;
    if (pg_state == 1) pg_mark(vk_cmd(), "(after pass, %llu draws in it; ended at line %d)", (unsigned long long)(stat_draws - pg_draws0), line);
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
// The part of a level extent `e` a scalable image's contents occupy (Dynamic resolution: drawn at scale `sc` into an
// image allocated at scale `al`, the top-left part); the whole extent otherwise.
static int content_extent(int e, bool scalable, float sc, float al) {
    if (!scalable || sc == al || al <= 0) return e;
    int c = (int)lroundf(e * sc / al);
    return c < 1 ? 1 : c > e ? e : c;
}

static bool heap_whole_userptr(void);
static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2 && !mir_graphics_prepared && !uniform_forward_graphics_prepared) renderer_flush();  // no submit between mirror preparation and its draw
    else {
        // Submit the frame in chunks (as Metal, renderer.m begin_pass): the game puts a whole frame in one command
        // list and waits for its fence early in the next frame, so with one submission per list the GPU sat idle
        // while the list was translated, and the game's wait covered translation + GPU time - with the render thread
        // the translation left the game thread but came back as that wait. LARECOMP_CHUNK_PASSES=<n>: a submission
        // every n render passes (0 = one per list). Default 8 with the render thread (RTX 3080 Ti + i9, Field_11C:
        // gpu wait 0.6 -> 0.25 ms, game thread busy 3.4-3.5 ms vs 4.9 without the render thread); without it,
        // off (chunks cost that thread 0.1-0.3 ms there). Also off by default while the whole heap is one userptr
        // import (heap_whole_userptr): amdgpu checks all of its pages again on every submission, ~5 ms each for the
        // 3.2 GB heap, so ten chunks a frame cost ~50 ms (issue #92: RX 6800 XT, 12 fps at the title screen, where
        // 0c26936 - one submission per list - got 29-41).
        static int chunk = -1;
        static bool chunk_env;
        if (chunk < 0) {
            const char *e = getenv("LARECOMP_CHUNK_PASSES");
            chunk_env = e != NULL;
#ifdef __ANDROID__
            chunk = e ? atoi(e) : 0;  // (phones: one submission per list, as before; their drivers' submissions cost more)
#else
            chunk = e ? atoi(e) : rq_on() ? 8 : 0;
#endif
        }
        int c = chunk;
        if (c > 0 && !chunk_env && heap_whole_userptr()) {  // (decided at boot: asked here, not cached before it)
            static bool logged;
            if (!logged) { logged = true; LOG("[vk] submission chunks off: the guest heap is one userptr import (AMD)\n"); }
            c = 0;
        }
        if (c > 0 && ++passes_in_sub >= c && !mir_graphics_prepared && !uniform_forward_graphics_prepared) renderer_submit();
    }
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        bc_mark(cb, "pass %d color %ux%u fmt %#x depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        // (color: the game's size for the target; drawn: what is rendered - the render scale applied, as the pass's area)
        const TexBackend *tb = t0 ? t0->backend : NULL;
        float dsx = tb && tb->scalable ? tb->sx : 1, dsy = tb && tb->scalable ? tb->sy : 1;
        gp_mark(cb, "pass: %d color %ux%u (drawn %.0fx%.0f) fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0,
                t0 ? t0->height : 0, t0 ? t0->width * dsx : 0, t0 ? t0->height * dsy : 0, t0 ? t0->format : 0,
                st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        renderer_mark_gpu_owned(t); // vk_view consumed any initial CPU writes first
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        pass_color[i] = vk_view_format(b, pass_color[i]);  // (g_vk.lean_usage: the format the view really has)
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // Bound color slots without a texture / view stay as VK_NULL_HANDLE attachments (legal), but Intel's Windows
    // driver for UHD 630 (Vulkan 1.3.215) crashes in vkCmdBeginRendering reading near a null pointer on the title
    // screen's first clear (issues #8, #46). Empty slots at the end are left out (pipelines and clears follow
    // pass_ncolor); one in the middle, or a pass with no attachment at all, is logged once for the next report.
    while (pass_ncolor && !ca[pass_ncolor - 1].imageView) pass_ncolor--;
    static bool logged_hole;
    for (uint32_t i = 0; i < pass_ncolor && !logged_hole; i++)
        if (!ca[i].imageView) {
            logged_hole = true;
            LOG("[vk] pass with an empty color slot %u of %u (texture %s)\n", i, pass_ncolor, st.rt.color[i] ? "without a view" : "none");
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        renderer_mark_gpu_owned(st.rt.depth); // preserve CPU initialization before ownership
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) {
        pass_sx = pass_sy = 1; minw = minh = 16;
        static bool logged_none;
        if (!logged_none) logged_none = true, LOG("[vk] pass with no attachments (%d color slots bound)\n", st.rt.ncolor);
    }
    // A pass with depth / stencil but no color attachment gets a throwaway one on Intel (LARECOMP_VK_PLACEHOLDER_COLOR=0 /
    // 1 overrides): the UHD 630's Windows driver (Vulkan 1.3.215) crashes in the vkCmdBeginRendering after such a pass,
    // reading near a null pointer (issues #8, #46). Pipelines and clears follow pass_ncolor / pass_color as usual.
    static int placeholder = -1;
    if (placeholder < 0) {
        const char *e = getenv("LARECOMP_VK_PLACEHOLDER_COLOR");
        placeholder = e ? atoi(e) != 0 : g_vk.props.vendorID == 0x8086;
    }
    bool placed = false;
    if (placeholder && !pass_ncolor && (has_d || has_s)) {
        VkImageView v = vk_dummy_color_view(minw, minh, pass_samples);
        if (v) {
            ca[0] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE};
            pass_color[0] = VK_FORMAT_R8_UNORM;
            pass_ncolor = 1;
            placed = true;
        }
    }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = pass_ncolor ? ca : NULL,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    // LARECOMP_VK_LOG_PASSES=<n> (default 300 with =1): each of the first n passes before it begins - an Intel crash
    // inside vkCmdBeginRendering (issues #8, #46) ends the log on the pass the driver choked on
    static long log_passes = -1;
    if (log_passes < 0) {
        log_passes = getenv("LARECOMP_VK_LOG_PASSES") ? atol(getenv("LARECOMP_VK_LOG_PASSES")) : 0;
        if (log_passes == 1) log_passes = 300;
    }
    if (log_passes > 0 || g_debug_frame) {  // (and every pass of LARECOMP_DEBUG_FRAME's frame)
        if (log_passes > 0) log_passes--;
        LOG("[vk] pass %ux%u: %u color", minw, minh, pass_ncolor);
        for (uint32_t i = 0; i < pass_ncolor; i++) {
            TexBackend *b = !placed && st.rt.color[i] ? st.rt.color[i]->backend : NULL;
            LOG(" [%u: fmt %d view %s %s", i, pass_color[i], ca[i].imageView ? "yes" : "none", placed ? "placeholder"
                : ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "load");
            // (the image under it: its own format, size, levels / layers, samples; the view's first level and layer)
            if (b) LOG(", image fmt %d %ux%u lv %u ly %u s %u, view lv %u ly %u", b->format, b->width, b->height, b->levels,
                       b->layers, b->samples, st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0,
                       st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) ? st.rt.cview[i].min_layer : 0);
            LOG("]");
        }
        if (has_d || has_s)
            LOG(", depth %s fmt %d, stencil %s fmt %d (same view %s, layout %d, load op %d / %d)", has_d ? "yes" : "no",
                pass_depth, has_s ? "yes" : "no", pass_stencil, has_d && has_s && da.imageView == sa.imageView ? "yes" : "no",
                has_d ? da.imageLayout : sa.imageLayout, has_d ? da.loadOp : -1, has_s ? sa.loadOp : -1);
        else LOG(", no depth / stencil");
        LOG(", samples %u\n", pass_samples);
    }
    pg_mark(cb, "pass %ux%u, %u color (fmt %d), depth fmt %d, load %s", minw, minh, pass_ncolor, pass_ncolor ? pass_color[0] : 0,
            has_d ? pass_depth : 0, pass_ncolor && ca[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "load");
    pg_draws0 = stat_draws;
    pass_depth_attachment_view = has_d ? da.imageView : VK_NULL_HANDLE;
    rec_CmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int64_t x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if ((int64_t)x + sw > w) sw = w - x;
    if ((int64_t)y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    rec_CmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    rec_CmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    rec_CmdSetDepthBias(cb, 0, 0, 0);
    rec_CmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    rec_CmdSetBlendConstants(cb, bc);
    rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetCullMode(cb, VK_CULL_MODE_NONE);
    rec_CmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    rec_CmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    rec_CmdSetDepthTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthWriteEnable(cb, VK_FALSE);
    rec_CmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetStencilTestEnable(cb, VK_FALSE);
    rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    rec_CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

// Explicit research-only experiment, never a title/default renderer policy.
static int clear_scope_test_enabled = -1;
static bool clear_scope_full_test(void) {
    if (clear_scope_test_enabled < 0) {
        const char *e = getenv("LARECOMP_CLEAR_SCOPE_TEST");
        clear_scope_test_enabled = e && !strcmp(e, "full");
    }
    return clear_scope_test_enabled != 0;
}
static VkRect2D clear_scope_rect(VkRect2D normal, uint32_t width, uint32_t height, bool explicit_texture) {
    return !explicit_texture && clear_scope_full_test() ? (VkRect2D){{0, 0}, {width, height}} : normal;
}
static bool clear_scope_covers(uint32_t width, uint32_t height, const int32_t scissor[4]) {
    return clear_scope_full_test() || (scissor[0] <= 0 && scissor[1] <= 0 &&
           (int64_t)scissor[0] + scissor[2] >= width && (int64_t)scissor[1] + scissor[3] >= height);
}
static bool clear_scope_attachment_covers(uint32_t width, uint32_t height, unsigned level, uint32_t target_w, uint32_t target_h) {
    return level < 32 && (width >> level) >= target_w && (height >> level) >= target_h;
}
static void material_probe_poll(void);
static void material_probe_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval,
                                 bool stencil, uint32_t sval, uint32_t smask, bool explicit_texture, VkRect2D normal,
                                 VkRect2D resolved, const char *path);
static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask, bool explicit_texture) {
    material_probe_poll(); // Leading clears can precede the first draw of a requested frame.
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D normal = scissor_rect();
    VkRect2D rect = clear_scope_rect(normal, pass_w, pass_h, explicit_texture);
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    bool invalid = index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED);
    material_probe_clear(index, kind, color, mask, depth, dval, stencil, sval, smask, explicit_texture, normal, rect,
                         !rect.extent.width || !rect.extent.height ? "empty" : invalid ? "invalid-attachment" :
                         simple ? "clear-attachments" : "masked-draw");
    if (!rect.extent.width || !rect.extent.height || invalid) return;
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) rec_CmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { rec_CmdSetDepthTestEnable(cb, VK_TRUE); rec_CmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        rec_CmdSetStencilTestEnable(cb, VK_TRUE);
        rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    rec_CmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2], csize[2];  // instruction/immutable compiler-constant spans; translation inputs (copied: translated on the translator thread)
    const uint8_t *read_code[2]; // immutable stage bytes; code[] retains diagnostic guest addresses
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkShaderModule fs_late;  // (lazy chunks) fs without early fragment tests, for alpha to coverage pipelines; or NULL
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[3];  // [2] (compute: [1]): the heap chunk set (gmem_chunk_layout)
    VkPipelineLayout layout;
    bool vs_uses_vid;
    bool fs_hashed;     // fs_hash computed (draw_: the hash of its fragment code, fixed as the code is - see prog_tab)
    uint64_t fs_hash;
    uint64_t chash;     // its code's and keys' content hash (the shader head start's key)
    bool headstart;     // made from cache/headstart.bin (its code copied: not recorded again)
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t prep_vh, prep_fh;  // (prepared programs: their code hashes, for LARECOMP_PREP_CHECK)
    ShaderFp fp[2];             // (stale-program detector, shader_prep.h; compute: [0]; touched by the replay only)
    struct PipeMemo *pmemo;     // (render thread) draw_'s pipeline key memo for this program
    bool reuse;                 // set reuse (stage_set): some uniform buffers are dynamic, sets are kept while their contents repeat
    struct SetCache *scache[2]; // (render thread) stage_set's last set per set index
} Prog;
static const uint8_t *prog_code(const Prog *p, int stage) {
    return p->read_code[stage] ? p->read_code[stage] : (const uint8_t *)(uintptr_t)p->code[stage];
}
static void hs_record_program(const Prog *p);

// ---- descriptor set reuse (graphics programs; port of vk-draw-cpu-2 d50bbce)
// Every draw allocated and wrote two descriptor sets (~20 descriptors) although most repeat the previous draw's of
// the same program: the textures and samplers are the same, and the uniform buffers are copies in the same transient
// ring buffer at new offsets. With the uniform buffers that move per draw declared dynamic (their offsets go to
// vkCmdBindDescriptorSets), the set's contents (buffers, ranges, views, samplers) repeat, so stage_set keeps the last
// set of each program and set index and binds it again while what it would write is identical. A kept set belongs to
// the pending submission (its pools are reset with the slot) and to the current view generation; buffers bound in
// sets are destroyed deferred (after the submissions using them), or at a reboot (ds_gen).
// LARECOMP_VK_NO_SET_REUSE=1: no dynamic buffers, a new set per draw, as before.
static int set_reuse = -1;
static uint32_t ds_gen;  // bumped when a buffer bound in sets may be destroyed at once (reboot)
static bool set_reuse_on(void) {
    if (set_reuse < 0) set_reuse = getenv("LARECOMP_VK_NO_SET_REUSE") == NULL;
    return set_reuse;
}
static bool is_ubo(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; }
// Up to the device's limit for the whole pipeline layout (8 or more; NVIDIA 15, AMD 16), the uniform buffers whose
// copies move most: the game's constant buffers (bindings 2-16), the scale table (18), the driver constants (0), the
// tint (19), then the compiler constants (1: one copy per submission). (Build workers: before the program is ready.)
static void mark_dynamic(Prog *p) {
    if (!set_reuse_on()) return;
    uint32_t lim = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic, n = 0;
    if (lim > 32) lim = 32;
    static const int8_t tier_of[20] = {2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1, 3};
    for (int tier = 0; tier <= 4; tier++)
        for (int i = 0; i < p->nbinds && n < lim; i++) {
            VkBindingInfo *b = &p->binds[i];
            if (b->set > 1 || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->binding >= 20 || tier_of[b->binding] != tier) continue;
            b->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            n++;
        }
    p->reuse = true;
    static _Atomic int logged, logged_left;  // (the limit once; programs left with static buffers: the first 4)
    int left = 0;
    for (int i = 0; i < p->nbinds; i++) left += p->binds[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && p->binds[i].set <= 1;
    if (!atomic_exchange(&logged, 1) || (left && atomic_fetch_add(&logged_left, 1) < 4))
        LOG("[vk] set reuse: %u dynamic uniform buffers allowed; a program with %u dynamic, %d left static\n", lim, n, left);
}

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    bool gm = gc_shift && ((p->vi && p->vi->uses_gmem) || (p->fi && p->fi->uses_gmem));
    if (gm) p->set_layout[nsets++] = gmem_chunk_layout();
    // (lazy chunks, vertex / fragment: the storage buffers' chunks as push constants, shader.h GMEM_DIRECT_MAX)
    VkPushConstantRange gd = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * GMEM_DIRECT_MAX};
    bool direct = gm && gc_lazy && s1 == VK_SHADER_STAGE_FRAGMENT_BIT;
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout, .pushConstantRangeCount = direct ? 1 : 0,
                                     .pPushConstantRanges = direct ? &gd : NULL};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    char dir[512] = "build";
#ifdef __ANDROID__
    if (SDL_GetAndroidExternalStoragePath()) snprintf(dir, sizeof dir, "%s/build", SDL_GetAndroidExternalStoragePath());
#endif
    char path[600];
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/glsl", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/glsl/%llx_%s.glsl", dir, (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

// LARECOMP_SAVE_BLOB=1 (with LARECOMP_SAVE_MSL): the shader's code too, build/glsl/<addr>_<stage>.bin, for tools/flow_driver
static void save_blob(const uint8_t *code, size_t max, uint64_t addr, const char *suffix) {
    if (!code || !max || !getenv("LARECOMP_SAVE_MSL") || !getenv("LARECOMP_SAVE_BLOB")) return;
    char dir[512] = "build";
#ifdef __ANDROID__
    if (SDL_GetAndroidExternalStoragePath()) snprintf(dir, sizeof dir, "%s/build", SDL_GetAndroidExternalStoragePath());
#endif
    char path[600];
    snprintf(path, sizeof path, "%s/glsl/%llx_%s.bin", dir, (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(code, 1, (max < 262144 ? max : 262144) + 4096, f); fclose(f); }  // (+ the constants' first 4 KB: brx_table reads them)
}
void slow_note_build(const char *what, uint64_t ns);  // (slowframe.h: the slow-frame log's counts; every build,
void slow_note_cache_hit(void);                       // and the pipelines the driver's cache had)
ShaderInfo *slow_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
#define maxwell_translate slow_translate  // (the translations below: counted and timed for the slow-frame log)
// (with a record per stage: the spec allows none, but Turnip 24.3 writes the first one of a compute pipeline's without
// looking - a null write in the pipeline worker on Adreno phones)
#define SLOW_FEEDBACK(info, nstages) VkPipelineCreationFeedback fb_ = {0}, fbs_[2] = {{0}}; VkPipelineCreationFeedbackCreateInfo fbi_ = \
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = (info).pNext, .pPipelineCreationFeedback = &fb_, \
     .pipelineStageCreationFeedbackCount = (nstages), .pPipelineStageCreationFeedbacks = fbs_}; \
    (info).pNext = &fbi_
#define SLOW_FEEDBACK_HIT() do { if (fb_.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) slow_note_cache_hit(); } while (0)

// A build the driver takes over a second for is logged with its shaders: draws and dispatches are skipped until it
// is done, which looked like a rendering bug (the chain chomps' compute with the heap in chunks: never ready).
static void slow_build(uint64_t t0, const char *what, uint64_t code0, uint64_t code1) {
    slow_note_build(what, SDL_GetTicksNS() - t0);
    uint64_t ms = (SDL_GetTicksNS() - t0) / 1000000;
    if (ms < 1000) return;
    static int n;
    if (n++ < 40) LOG("[shader] slow build: %s %llx %llx took %llu ms\n", what, (unsigned long long)code0, (unsigned long long)code1, (unsigned long long)ms);
}

// A fragment shader the translator gave early fragment tests (lazy chunks, translate.c glsl_early_tests): removes the
// OpExecutionMode EarlyFragmentTests (in place; true if there was one). Alpha to coverage drops samples after the
// shader, and with early tests their depth / stencil would already be written: those pipelines take this module.
static bool spirv_strip_early_tests(uint32_t *w, size_t *nwords) {
    for (size_t i = 5; i < *nwords;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!n || i + n > *nwords) return false;
        if (op == 54) return false;  // OpFunction: past the execution modes
        if (op == 16 && n == 3 && w[i + 2] == 9) {  // OpExecutionMode %main EarlyFragmentTests
            memmove(w + i, w + i + n, (*nwords - i - n) * sizeof *w);
            *nwords -= n;
            return true;
        }
        i += n;
    }
    return false;
}

/* Direct words move exactly once to the module builder; metadata remains alive
 * for binding/layout construction. Unsupported stages retain source compilation. */
static uint32_t *program_spirv(ShaderInfo *info, int stage, const char *name, size_t *words) {
    *words = 0;
    if (!info) return NULL;
    if (info->spirv) {
        uint32_t *out = info->spirv;
        *words = info->spirv_words;
        info->spirv = NULL;
        info->spirv_words = 0;
        return out;
    }
    return info->msl ? vk_glsl_to_spirv(info->msl, stage, name, words) : NULL;
}

// A direct module may be rejected by a driver even after host SPIR-V validation.
// Retry only while this program is unpublished, using its exact immutable bytes.
// Pipeline creation happens after publication and cannot safely mutate this data.
static bool program_legacy_module_retry(Prog *p, int stage, ShaderInfo **selected,
                                         uint32_t **words, size_t *word_count, VkShaderModule *module) {
    int slot = stage == STAGE_VERTEX ? 0 : 1;
    const char *name = slot ? "fs" : "vs";
    if (!p->read_code[slot] || !p->cmax[slot] || p->cmax[slot] > SIZE_MAX ||
        p->csize[slot] > SIZE_MAX || p->csize[slot] > SIZE_MAX - p->cmax[slot]) {
        LOG("[shader] direct SPIR-V %s module rejected; immutable retry input unavailable\n", name);
        return false;
    }
    ShaderKey key = slot ? p->fkey : p->vkey;
    if (slot) key.vs_out_mask = p->vi->out_mask;
    pthread_mutex_lock(&translate_lock);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    const uint8_t *previous_debug = maxwell_dbg_code;
    // A stage admitted by the direct backend had no selected debug override.
    // Another queued translation must not change that choice for this retry.
    maxwell_dbg_code = NULL;
    ShaderInfo *candidate = maxwell_translate_sized(p->read_code[slot], (size_t)p->cmax[slot],
        (size_t)p->csize[slot], stage, &key);
    maxwell_dbg_code = previous_debug;
    pthread_mutex_unlock(&translate_lock);
    if (!candidate || !candidate->msl || (!slot && candidate->out_mask != (*selected)->out_mask)) {
        LOG("[shader] direct SPIR-V %s module rejected; legacy translation unavailable or interface changed\n", name);
        maxwell_free(candidate);
        return false;
    }
    size_t count = 0;
    uint32_t *replacement = vk_glsl_to_spirv(candidate->msl, stage, name, &count);
    VkShaderModule retry = replacement ? vk_module(replacement, count) : VK_NULL_HANDLE;
    if (!retry) {
        LOG("[shader] direct SPIR-V %s module rejected; legacy module retry failed\n", name);
        free(replacement);
        maxwell_free(candidate);
        return false;
    }
    maxwell_free(*selected);
    free(*words);
    *selected = candidate;
    *words = replacement;
    *word_count = count;
    *module = retry;
    if (!slot) p->vs_uses_vid = candidate->uses_vertex_id || strstr(candidate->msl, "gl_VertexIndex") != NULL;
    LOG("[shader] direct SPIR-V %s module rejected; legacy module retry succeeded\n", name);
    return true;
}

static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t id = p->chash;
    uint64_t vs_hash = p->fp[0].hash ? p->fp[0].hash : p->prep_vh;
    uint64_t fs_hash = p->fp[1].hash ? p->fp[1].hash : p->prep_fh;
    int bs = build_begin("modules vs/fs", p->code[0] ? p->code[0] : p->prep_vh, p->code[1] ? p->code[1] : p->prep_fh);
    build_diag_begin_program(id, vs_hash, fs_hash);
    bool direct_vs = p->vi && p->vi->spirv, direct_fs = p->fi && p->fi->spirv;
    size_t nv = 0, nf = 0;
    uint32_t *sv = program_spirv(p->vi, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? program_spirv(p->fi, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (!sv || !sf) {
        free(sv); free(sf); atomic_store(&p->state, 2); build_end(bs);
        build_diag_done("modules vs/fs", id, t0, 2); return;
    }
    p->vs = vk_module(sv, nv);
    if (!p->vs && direct_vs) program_legacy_module_retry(p, STAGE_VERTEX, &p->vi, &sv, &nv, &p->vs);
    p->fs = vk_module(sf, nf);
    if (!p->fs && direct_fs) program_legacy_module_retry(p, STAGE_FRAGMENT, &p->fi, &sf, &nf, &p->fs);
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    if (spirv_strip_early_tests(sf, &nf)) p->fs_late = vk_module(sf, nf);
    free(sv);
    free(sf);
    mark_dynamic(p);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1);
    if (!p->vs || !p->fs) LOG("[shader] vs/fs %llx %llx: shader module creation failed\n", (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
    build_end(bs);
    build_diag_done("modules vs/fs", id, t0, atomic_load(&p->state));
    slow_build(t0, "modules vs/fs", p->code[0], p->code[1]);
}

// Translates both stages (false: failed, logged). Holds the translator for the whole program.
static bool translate_program_now(Prog *p) {
    pthread_mutex_lock(&translate_lock);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    {   // LARECOMP_DBG_FS=<fs code addr hex>:<pc hex>:<reg> (as renderer.m): that register at that pc as the colour
        static int parsed;
        static unsigned long long dbg_addr;
        static unsigned dbg_pc;
        static int dbg_reg;
        if (!parsed) { parsed = 1; const char *e = getenv("LARECOMP_DBG_FS"); if (e) sscanf(e, "%llx:%x:%d", &dbg_addr, &dbg_pc, &dbg_reg); }
        // The user's selector remains a guest code address; the translator
        // compares its immutable input pointer while this program is active.
        maxwell_dbg_code = dbg_addr && p->code[1] == dbg_addr ? prog_code(p, 1) : NULL;
        maxwell_dbg_pc = dbg_pc;
        maxwell_dbg_reg = dbg_reg;
    }
    p->vi = maxwell_compile_sized(prog_code(p, 0), p->cmax[0],
                                   p->csize[0] ? p->csize[0] : (p->headstart || p->read_code[0] ? 0 : SIZE_MAX), STAGE_VERTEX, &p->vkey);
    if (shader_info_succeeded(p->vi) && !p->code[1]) {
        // NVN permits depth-only draws with no fragment stage.
        p->fi = calloc(1, sizeof *p->fi);
        p->fi->stage = STAGE_FRAGMENT;
        p->fi->msl = strdup("#version 460\nvoid main() {}\n");
    } else if (shader_info_succeeded(p->vi)) {
        ShaderKey fkey = p->fkey;
        fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_compile_sized(prog_code(p, 1), p->cmax[1],
                                       p->csize[1] ? p->csize[1] : (p->headstart || p->read_code[1] ? 0 : SIZE_MAX), STAGE_FRAGMENT, &fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    if (!shader_info_succeeded(p->vi)) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi ? p->vi->error : "allocation failed", (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return false;
    }
    if (!shader_info_succeeded(p->fi)) {
        LOG("[shader] FS translate failed: %s\n", p->fi ? p->fi->error : "allocation failed");
        atomic_store(&p->state, 2);
        return false;
    }
    p->vs_uses_vid = p->vi->uses_vertex_id || (p->vi->msl && strstr(p->vi->msl, "gl_VertexIndex") != NULL);
    hs_record_program(p);
    return true;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    if (!translate_program_now(p)) return;
    if (p->vi->msl) save_glsl(p->vi->msl, p->code[0], "vs");
    if (p->fi->msl) save_glsl(p->fi->msl, p->code[1], "fs");
    save_blob(prog_code(p, 0), p->cmax[0], p->code[0], "vs");
    save_blob(prog_code(p, 1), p->cmax[1], p->code[1], "fs");
    run_job_q(compile_program, p, p->headstart);
}

// ================================================================== prepared objects (shader_prep.h)
// Built before the game from the shipped variant list; found by content (code hashes + keys) at first use, as code
// addresses differ from run to run. Kept for the whole process (reboots included). prep_lock guards the tables.
static pthread_mutex_t prep_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t key; void *obj; } PrepEnt;
static PrepEnt *prep_progs, *prep_pipes, *prep_cs;  // program / pipeline / compute content key -> Prog / Pipe / CsProg
static size_t prep_progs_cap, prep_pipes_cap, prep_cs_cap, prep_progs_n, prep_pipes_n, prep_cs_n;
static _Atomic int prep_pending;

static void *prep_get(PrepEnt *t, size_t cap, uint64_t key) {
    if (!cap) return NULL;
    if (!key) key = 1;
    for (size_t j = key & (cap - 1); t[j].key; j = (j + 1) & (cap - 1))
        if (t[j].key == key) return t[j].obj;
    return NULL;
}
static void prep_put(PrepEnt **t, size_t *cap, size_t *n, uint64_t key, void *obj) {
    if (!key) key = 1;
    if (*n * 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        PrepEnt *nt = calloc(nc, sizeof *nt);
        for (size_t i = 0; i < *cap; i++)
            if ((*t)[i].key) {
                size_t j = (*t)[i].key & (nc - 1);
                while (nt[j].key) j = (j + 1) & (nc - 1);
                nt[j] = (*t)[i];
            }
        free(*t);
        *t = nt, *cap = nc;
    }
    size_t j = key & (*cap - 1);
    while ((*t)[j].key && (*t)[j].key != key) j = (j + 1) & (*cap - 1);
    if (!(*t)[j].key) (*n)++;
    (*t)[j] = (PrepEnt){key, obj};
}
#define PREP_GET(tab, key) ({ pthread_mutex_lock(&prep_lock); void *o_ = prep_get(tab, tab##_cap, key); pthread_mutex_unlock(&prep_lock); o_; })
#define PREP_PUT(tab, key, obj) do { pthread_mutex_lock(&prep_lock); prep_put(&tab, &tab##_cap, &tab##_n, key, obj); pthread_mutex_unlock(&prep_lock); } while (0)

static uint64_t prog_content_key(uint64_t vh, uint64_t fh, const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t c[2] = {vh, fh};
    const uint8_t *b = (const uint8_t *)c;
    for (size_t i = 0; i < sizeof c; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    b = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    return h;
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// ---- shader head start (cache/headstart.bin): every program translated in play is recorded (its code and keys),
// and so is every pipeline (its key, the program by content hash). The next start translates, compiles and builds
// all of them in the background, at the lowest priority, from the moment the renderer starts (the start screen, the
// game's boot and intro): an area seen before no longer stops to translate and build on entry - no hitches, no
// models popping in. As the WWHD port's shader head start; LARECOMP_HEADSTART=0 turns it off.
#define HS_MAGIC 0x3753484cu  // "LHS7": adds bounded compute head-start records
#define HS_PROG 1
#define HS_PIPE 2
#define HS_CS 3
// BEGIN headstart cache identity (also exercised by the asset-free host checker).
typedef struct { uint32_t type, size; } HsRec;
typedef struct { uint64_t chash, cmax[2], csize[2]; ShaderKey vkey, fkey; } HsProgHeader;
typedef struct { uint64_t stage_hash; uint32_t local[3], code_bytes, constant_bytes; ShaderKey key; } HsCsHeader;
#define HS_CS_MAX_BYTES KERNEL_READABLE_COPY_MAX
#define HS_CACHE_MAX_FILE_BYTES (UINT64_C(256) * 1024 * 1024)
#define HS_CACHE_MAX_RECORDS UINT32_C(65536)
#define HS_CS_TAB 256
#define HS_CS_DEDUP_TAB 131072
#define HS_CS_PENDING_BYTES (UINT64_C(32) * 1024 * 1024)
typedef struct CsProg CsProg;
static uint64_t hash_words(uint64_t h, const void *p, size_t n);
static uint64_t hs_cache_file_bytes;
static uint32_t hs_cache_record_count;
// Subtractive checks: adding two untrusted 64-bit lengths can wrap to record size.
static bool hs_program_size_valid(uint32_t size, const HsProgHeader *h) {
    if (size < sizeof *h || !h->cmax[0]) return false;
    uint64_t payload = size - sizeof *h;
    for (int i = 0; i < 2; i++) {
        if ((!h->cmax[i] && h->csize[i]) || h->cmax[i] > payload) return false;
        payload -= h->cmax[i];
        if (h->csize[i] > payload) return false;
        payload -= h->csize[i];
    }
    return payload == 0;
}
static bool hs_cs_size_valid(uint32_t size, const HsCsHeader *h, const uint8_t *payload) {
    if (size < sizeof *h || !h->stage_hash || !h->code_bytes ||
        !h->local[0] || !h->local[1] || !h->local[2]) return false;
    if (h->code_bytes > HS_CS_MAX_BYTES || h->constant_bytes > HS_CS_MAX_BYTES - h->code_bytes) return false;
    uint32_t bytes = h->code_bytes + h->constant_bytes;
    if (!payload || size - sizeof *h != bytes || shader_code_hash(payload, bytes) != h->stage_hash) return false;
    ShaderKey expected = {0}; expected.glsl = 1;
    return !memcmp(&h->key, &expected, sizeof expected);
}
static uint64_t hs_cs_content_key(const HsCsHeader *h) {
    uint64_t content[6] = {h->stage_hash, h->code_bytes, h->constant_bytes, h->local[0], h->local[1], h->local[2]};
    uint64_t key = hash_words(0xcbf29ce484222325ull, content, sizeof content);
    key = hash_words(key, &h->key, sizeof h->key);
    return key ? key : 1;
}
static uint64_t hs_cs_recorded[HS_CS_DEDUP_TAB];
static size_t hs_cs_recorded_n;
static uint64_t hs_cs_queued_keys[HS_CS_TAB];
static size_t hs_cs_queued_n;
static void hs_cs_recorded_clear_locked(void) { memset(hs_cs_recorded, 0, sizeof hs_cs_recorded); hs_cs_recorded_n = 0; }
static bool hs_cs_recorded_find_locked(uint64_t key) {
    uint32_t at = (uint32_t)(key ^ (key >> 31)) & (HS_CS_DEDUP_TAB - 1);
    for (uint32_t n = 0; n < HS_CS_DEDUP_TAB && hs_cs_recorded[at]; n++, at = (at + 1) & (HS_CS_DEDUP_TAB - 1))
        if (hs_cs_recorded[at] == key) return true;
    return false;
}
static bool hs_cs_recorded_add_locked(uint64_t key) {
    uint32_t at = (uint32_t)(key ^ (key >> 31)) & (HS_CS_DEDUP_TAB - 1);
    for (uint32_t n = 0; n < HS_CS_DEDUP_TAB && hs_cs_recorded[at]; n++, at = (at + 1) & (HS_CS_DEDUP_TAB - 1))
        if (hs_cs_recorded[at] == key) return true;
    if (hs_cs_recorded_n * 2 >= HS_CS_DEDUP_TAB) return false;
    hs_cs_recorded[at] = key; hs_cs_recorded_n++;
    return true;
}
static bool hs_cs_queued_find_locked(uint64_t key) {
    for (size_t i = 0; i < hs_cs_queued_n; i++) if (hs_cs_queued_keys[i] == key) return true;
    return false;
}
static void hs_cs_queued_remove_locked(uint64_t key) {
    for (size_t i = 0; i < hs_cs_queued_n; i++) if (hs_cs_queued_keys[i] == key) {
        hs_cs_queued_keys[i] = hs_cs_queued_keys[--hs_cs_queued_n]; return;
    }
}
typedef struct {
    uint32_t magic, header_size, pipe_size, shader_key_size;
    uint8_t identity[32];
} HsHeader;
static uint64_t hash_words(uint64_t h, const void *p, size_t n);
static uint32_t pipekey_size(void);

// Cache records contain process-local key encodings, never a portable pipeline ABI.
// Bind replay to the compiled renderer dependency closure, not unrelated CPU/UI code.
static bool hs_header_make(HsHeader *out) {
    struct {
        uint8_t runtime[32], environment[32], game[32], pipeline_uuid[VK_UUID_SIZE];
        uint64_t heap_base, heap_size;
        uint32_t vendor, device, driver, api, modes[32];
    } in;
    memset(&in, 0, sizeof in);
    if (!shared_gpu_build_identity(false, in.runtime) ||
        !shared_gpu_environment(in.environment) || !shared_gpu_game_identity(in.game)) return false;
    memcpy(in.pipeline_uuid, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    in.vendor = g_vk.props.vendorID; in.device = g_vk.props.deviceID;
    in.driver = g_vk.props.driverVersion; in.api = g_vk.api;
    in.heap_base = HEAP_BASE; in.heap_size = HEAP_MAX;
    // vids omits the static vertex arrays from PipeKey; divisor changes their values.
    // Include all resolved global translator modes and heap layout choices as well.
    uint32_t modes[] = {
        g_vk.vids, g_vk.divisor, g_vk.host_import, g_vk.ssbo_chunks, g_vk.bda,
        maxwell_shader_int64, maxwell_gmem_readonly, maxwell_frag_subgroups,
        maxwell_h2f_bits, maxwell_gmem_mirror, maxwell_gmem_zero, maxwell_gfind_cache,
        maxwell_half_native, maxwell_precise_pos, maxwell_scalar_cbuf,
        maxwell_ieee_minmax, maxwell_robust_cbuf, maxwell_static_stacks,
        maxwell_brx_table, gmem_chunk_shift(), gc_lazy, maxwell_gmem_fs_miss,
        g_vk.gpl_probe, g_vk.gpl, g_vk.custom_border, g_vk.lean_usage,
        g_vk.props.apiVersion, g_vk.props.limits.maxUniformBufferRange,
        g_vk.props.limits.maxStorageBufferRange,
        maxwell_vote_stages, maxwell_storage_image_write, maxwell_clip_distance,
    };
    _Static_assert(sizeof modes == sizeof in.modes, "headstart modes");
    memcpy(in.modes, modes, sizeof modes);
    memset(out, 0, sizeof *out);
    out->magic = HS_MAGIC; out->header_size = sizeof *out;
    out->pipe_size = pipekey_size(); out->shader_key_size = sizeof(ShaderKey);
    return shared_sha256_bytes(&in, sizeof in, out->identity);
}

// Only complete records are appended. A legacy/mismatched header or torn tail is
// discarded before any record can be replayed or appended. This touches no save files.
static FILE *hs_cache_open(const char *path, const HsHeader *expected) {
    hs_cs_recorded_clear_locked();
    FILE *f = fopen(path, "r+b");
    bool valid = false;
    if (f) {
        HsHeader got;
        if (!fseek(f, 0, SEEK_END)) {
            long end = ftell(f);
            rewind(f);
            valid = end >= (long)sizeof got && (uint64_t)end <= HS_CACHE_MAX_FILE_BYTES && fread(&got, sizeof got, 1, f) == 1 &&
                    !memcmp(&got, expected, sizeof got);
            long at = sizeof got;
            uint32_t records = 0;
            while (valid && at < end) {
                if (++records > HS_CACHE_MAX_RECORDS) { valid = false; break; }
                HsRec r;
                if (end - at < (long)sizeof r || fread(&r, sizeof r, 1, f) != 1) { valid = false; break; }
                at += sizeof r;
                if (r.size > (uint64_t)(end - at) ||
                    (r.type != HS_PROG && r.type != HS_PIPE && r.type != HS_CS) ||
                    (r.type == HS_PIPE && r.size != expected->pipe_size)) { valid = false; break; }
                uint32_t skip = r.size;
                if (r.type == HS_PROG) {
                    HsProgHeader h;
                    if (r.size < sizeof h || fread(&h, sizeof h, 1, f) != 1 ||
                        !hs_program_size_valid(r.size, &h)) { valid = false; break; }
                    skip -= sizeof h;
                } else if (r.type == HS_CS) {
                    HsCsHeader h;
                    if (r.size < sizeof h || fread(&h, sizeof h, 1, f) != 1 ||
                        h.code_bytes > HS_CS_MAX_BYTES || h.constant_bytes > HS_CS_MAX_BYTES - h.code_bytes) { valid = false; break; }
                    uint32_t payload_size = h.code_bytes + h.constant_bytes;
                    uint8_t *payload = malloc(payload_size ? payload_size : 1);
                    bool accepted = payload && fread(payload, 1, payload_size, f) == payload_size &&
                                    hs_cs_size_valid(r.size, &h, payload);
                    free(payload);
                    if (!accepted || !hs_cs_recorded_add_locked(hs_cs_content_key(&h))) { valid = false; break; }
                    skip = 0;  // header and exact bounded payload were consumed for content-hash validation
                }
                if (fseek(f, (long)skip, SEEK_CUR)) { valid = false; break; }
                at += r.size;
            }
            if (valid && !ferror(f) && !fseek(f, 0, SEEK_END)) {
                hs_cache_file_bytes = (uint64_t)end;
                hs_cache_record_count = records;
                return f;
            }
        }
        fclose(f);
        hs_cs_recorded_clear_locked();
        LOG("[vk] shader head start: discarded incompatible or incomplete cache\n");
    }
    f = fopen(path, "w+b");
    if (f && (fwrite(expected, sizeof *expected, 1, f) != 1 || fflush(f))) { fclose(f); return NULL; }
    hs_cache_file_bytes = f ? sizeof *expected : 0;
    hs_cache_record_count = 0;
    return f;
}
// END headstart cache identity
static pthread_mutex_t hs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t key; Prog *prog; bool taken; } hs_tab[PROG_TAB];  // by content hash
static FILE *hs_out;
static HsHeader hs_header;
static bool hs_prepared;
static struct { uint64_t key, stage_hash; uint32_t code_bytes, constant_bytes, local[3]; ShaderKey shader_key; CsProg *cp; } hs_cs_tab[HS_CS_TAB];
static size_t hs_cs_n;
static uint64_t hs_cs_pending_bytes;
static CsProg *hs_cs_replay(uint32_t record_size, const HsCsHeader *h, const uint8_t *payload, bool *fresh);
static CsProg *hs_cs_take(uint64_t stage_hash, uint32_t code_bytes, uint32_t constant_bytes, const uint32_t local[3], const ShaderKey *key);
static int hs_cs_state(CsProg *cp);
// Called with hs_lock held: the file hash and cache validation happen once/process,
// before either the writer or delayed reader can use the persistent file.
static bool hs_prepare_locked(void) {
    if (!hs_prepared) {
        hs_prepared = true;
        if (!hs_header_make(&hs_header)) {
            LOG("[vk] shader head start: implementation identity unavailable; disk replay/recording disabled\n");
            return false;
        }
        mkdir("cache", 0755);
        hs_out = hs_cache_open("cache/headstart.bin", &hs_header);
    }
    return hs_out != NULL;
}
static _Atomic int hs_total, hs_done;  // finished builds (including failures), not queued jobs
static _Atomic bool hs_started, hs_finished, hs_requested;

static uint64_t prog_content_hash(const uint8_t *vc, uint64_t vn, uint64_t vs,
                                  const uint8_t *fc, uint64_t fn, uint64_t fs,
                                  const ShaderKey *vkey, const ShaderKey *fkey) {
    const uint64_t spans[] = {vn, vs, fn, fs};
    uint64_t h = hash_words(0x6c61732d68730002ull, spans, sizeof spans);
    h = hash_words(h, vc, vn + vs);
    h = hash_words(h, fc, fn + fs);
    h = hash_words(h, vkey, sizeof *vkey);
    h = hash_words(h, fkey->rt_kind, sizeof fkey->rt_kind);
    h = hash_words(h, &fkey->tex_uint, sizeof fkey->tex_uint);
    return h ? h : 1;
}

static bool headstart_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_HEADSTART") || atoi(getenv("LARECOMP_HEADSTART"));
    return on;
}

// Return the unique published object and tell the caller whether it owns the
// translation/compile job. Lookup and insertion must be one locked operation:
// replay and foreground can both have observed an earlier cache miss.
static Prog *hs_add_locked(Prog *p, bool taken, bool *fresh) {
    uint32_t slot = (uint32_t)(p->chash ^ (p->chash >> 31)) & (PROG_TAB - 1);
    int n = 0;
    for (; n < PROG_TAB && hs_tab[slot].key && hs_tab[slot].key != p->chash; n++) slot = (slot + 1) & (PROG_TAB - 1);
    if (n < PROG_TAB && hs_tab[slot].key) {
        hs_tab[slot].taken |= taken;
        *fresh = false;
        return hs_tab[slot].prog;
    }
    // Preserve the old full-table foreground behavior: an unindexed object can
    // still build. At normal capacity every key has exactly one owner.
    *fresh = true;
    if (n < PROG_TAB) hs_tab[slot].prog = p, hs_tab[slot].taken = taken, hs_tab[slot].key = p->chash;
    return p;
}
static Prog *hs_add(Prog *p, bool taken, bool *fresh) {
    pthread_mutex_lock(&hs_lock);
    Prog *winner = hs_add_locked(p, taken, fresh);
    pthread_mutex_unlock(&hs_lock);
    return winner;
}

// the program with this content, built ahead (or by another code address of the same shader)
static Prog *hs_take(uint64_t ch) {
    pthread_mutex_lock(&hs_lock);
    uint32_t slot = (uint32_t)(ch ^ (ch >> 31)) & (PROG_TAB - 1);
    for (int n = 0; n < PROG_TAB && hs_tab[slot].key && hs_tab[slot].key != ch; n++) slot = (slot + 1) & (PROG_TAB - 1);
    Prog *p = hs_tab[slot].key == ch ? hs_tab[slot].prog : NULL;
    if (p) hs_tab[slot].taken = true;
    pthread_mutex_unlock(&hs_lock);
    return p;
}

static void hs_write(uint32_t type, const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc) {
    if (!headstart_on() || na > UINT32_MAX || nb > UINT32_MAX - na || nc > UINT32_MAX - na - nb) return;
    pthread_mutex_lock(&hs_lock);
    hs_prepare_locked();
    if (hs_out && hs_cache_record_count < HS_CACHE_MAX_RECORDS) {
        uint64_t total = sizeof(HsRec) + na + nb + nc;
        if (total <= HS_CACHE_MAX_FILE_BYTES - hs_cache_file_bytes) {
            HsRec r = {type, (uint32_t)(na + nb + nc)};
            bool ok = fwrite(&r, sizeof r, 1, hs_out) == 1 && fwrite(a, 1, na, hs_out) == na &&
                      (!nb || fwrite(b, 1, nb, hs_out) == nb) && (!nc || fwrite(c, 1, nc, hs_out) == nc) && !fflush(hs_out);
            if (ok) hs_cache_file_bytes += total, hs_cache_record_count++;
            else { fclose(hs_out); hs_out = NULL; }  // torn append: never append behind it this process
        }
    }
    pthread_mutex_unlock(&hs_lock);
}

// a program translated in play (translator thread): its code and keys, once
// BEGIN headstart external-constant policy (asset-free regression checker).
// BRX may inspect compiler constants after the instructions. Only admit it when
// NVN supplied a verified constant span; maxwell_translate_sized bounds every
// table read to that span. Missing metadata retains original-memory fallback.
// Header/scheduling words are data, not executable BRX instructions.
static bool hs_stage_replayable(const uint8_t *code, uint64_t bytes, uint64_t constants) {
    if (!bytes) return true;  // absent fragment stage
    if (!code || bytes < 0x80 || bytes > SIZE_MAX) return false;
    for (uint64_t off = 0x80, instruction = 0; off <= bytes - 8; off += 8, instruction++) {
        if (instruction % 4 == 0) continue;
        uint64_t word;
        memcpy(&word, code + (size_t)off, sizeof word);
        if ((word & 0xfff0000000000000ull) == 0xe250000000000000ull && !constants) return false;
    }
    return true;
}
static bool hs_program_replayable(const uint8_t *vs, uint64_t vs_bytes, uint64_t vs_constants,
                                   const uint8_t *fs, uint64_t fs_bytes, uint64_t fs_constants) {
    return vs_bytes && hs_stage_replayable(vs, vs_bytes, vs_constants) && hs_stage_replayable(fs, fs_bytes, fs_constants);
}
static bool hs_constants_valid(uint64_t address, uint64_t instructions, uint64_t constants,
                               uint64_t heap_base, uint64_t heap_size) {
    if (address < heap_base || address - heap_base >= heap_size || !instructions) return false;
    uint64_t available = heap_size - (address - heap_base);
    return instructions <= available && constants <= available - instructions;
}
// END headstart external-constant policy.
static pthread_once_t hs_snapshot_once = PTHREAD_ONCE_INIT;
static NvnShaderSnapshotStore *hs_snapshot_store;
static void hs_snapshot_init(void) {
    // Retained bytes shared by every output/layout variant of a stage. Capture
    // serializes its temporary too: 32 MiB retained + at most 16 MiB temporary.
    hs_snapshot_store = nvn_shader_snapshots_create(UINT64_C(32)*1024*1024, KERNEL_READABLE_COPY_MAX);
}
static const uint8_t *hs_capture_stage(const NvnShaderStage *stage, uint64_t instructions,
    uint64_t constants, enum NvnShaderSnapshotResult *result) {
    *result = NVN_SNAPSHOT_ARGUMENT;
    // Outer arithmetic/window check only; committed pages and read permission
    // are proved by kernel_copy_readable while it copies the exact closure.
    if (!hs_constants_valid(stage->code_addr, instructions, constants, GUEST_LO, GUEST_HI-GUEST_LO)) return NULL;
    pthread_once(&hs_snapshot_once, hs_snapshot_init);
    const NvnShaderSnapshot *snapshot = nvn_shader_snapshot_capture(hs_snapshot_store,
        stage->code_addr, instructions, constants, kernel_copy_readable, result);
    return nvn_shader_snapshot_data(snapshot);
}
static uint64_t hs_snapshot_stage_hash(const NvnShaderStage *stage, const uint8_t *snapshot) {
    if (!snapshot || !stage->code_addr || !stage->const_offset) return 0;
    // Same bytes/key as shader_stage_hash, from the proved immutable closure.
    return shader_code_hash(snapshot, (size_t)stage->const_offset+stage->const_size);
}
// A current successful portable rebuild has the same proved immutable closure
// as a foreground program. Persist it without changing headstart's bounded
// translation-input semantics or letting an ordinary strict replay append again.
static void hs_record_program_closure(const Prog *p) {
    if (!p->chash) return;
    if (!hs_program_replayable(prog_code(p, 0), p->cmax[0], p->csize[0],
                              prog_code(p, 1), p->cmax[1], p->csize[1])) {
        static _Atomic unsigned misses;
        if (atomic_fetch_add(&misses, 1) < 8)
            LOG("[vk] shader head start: external-constant program cache miss %016llx\n", (unsigned long long)p->chash);
        return;
    }
    HsProgHeader h = {p->chash, {p->cmax[0], p->cmax[1]}, {p->csize[0], p->csize[1]}, p->vkey, p->fkey};
    hs_write(HS_PROG, &h, sizeof h, prog_code(p, 0), p->cmax[0] + p->csize[0],
             prog_code(p, 1), p->cmax[1] + p->csize[1]);
}
static void hs_record_program(const Prog *p) {
    if (!p->headstart) hs_record_program_closure(p);
}
// Append only a caller-verified immutable snapshot; this does not mutate renderer or program state.
static void hs_record_compute(const HsCsHeader *h, const uint8_t *snapshot) {
    if (!headstart_on() || !h || h->code_bytes > HS_CS_MAX_BYTES ||
        h->constant_bytes > HS_CS_MAX_BYTES - h->code_bytes) return;
    uint32_t bytes = h->code_bytes + h->constant_bytes;
    if (sizeof *h > UINT32_MAX - bytes || !hs_cs_size_valid((uint32_t)(sizeof *h + bytes), h, snapshot)) return;
    hs_write(HS_CS, h, sizeof *h, snapshot, bytes, NULL, 0);
}
typedef struct { HsCsHeader h; uint8_t *snapshot; uint32_t bytes; } HsCsPersist;
static void hs_record_compute_once(const HsCsHeader *h, const uint8_t *snapshot) {
    uint64_t key = hs_cs_content_key(h);
    pthread_mutex_lock(&hs_lock);
    bool admitted = hs_prepare_locked();  // file scan and disk setup stay on this worker
    bool duplicate = !admitted || hs_cs_recorded_find_locked(key);
    if (!duplicate && !hs_cs_recorded_add_locked(key)) duplicate = true;
    pthread_mutex_unlock(&hs_lock);
    if (!duplicate) hs_record_compute(h, snapshot);
}
static void hs_cs_persist_job(void *arg) {
    HsCsPersist *j = arg;
    uint64_t key = hs_cs_content_key(&j->h);
    hs_record_compute_once(&j->h, j->snapshot);
    free(j->snapshot);
    pthread_mutex_lock(&hs_lock);
    hs_cs_queued_remove_locked(key);
    hs_cs_pending_bytes -= j->bytes;
    pthread_mutex_unlock(&hs_lock);
    free(j);
}
static void hs_cs_record_queue(const HsCsHeader *h, const uint8_t *snapshot) {
    if (!headstart_on() || !h || h->code_bytes > HS_CS_MAX_BYTES ||
        h->constant_bytes > HS_CS_MAX_BYTES - h->code_bytes) return;
    uint32_t bytes = h->code_bytes + h->constant_bytes;
    if (!hs_cs_size_valid((uint32_t)(sizeof *h + bytes), h, snapshot)) return;
    uint64_t key = hs_cs_content_key(h);
    pthread_mutex_lock(&hs_lock);
    bool seen = hs_cs_recorded_find_locked(key) || hs_cs_queued_find_locked(key);
    pthread_mutex_unlock(&hs_lock);
    if (seen) return;
    HsCsPersist *job = malloc(sizeof *job);
    uint8_t *copy = malloc(bytes);
    if (!job || !copy) { free(job); free(copy); return; }
    memcpy(copy, snapshot, bytes); job->h = *h; job->snapshot = copy; job->bytes = bytes;
    pthread_mutex_lock(&hs_lock);
    if (hs_cs_recorded_find_locked(key) || hs_cs_queued_find_locked(key) || hs_cs_queued_n == HS_CS_TAB ||
        bytes > HS_CS_PENDING_BYTES - hs_cs_pending_bytes) {
        pthread_mutex_unlock(&hs_lock); free(copy); free(job); return;
    }
    hs_cs_queued_keys[hs_cs_queued_n++] = key;
    hs_cs_pending_bytes += bytes;
    pthread_mutex_unlock(&hs_lock);
    run_job_q(hs_cs_persist_job, job, true);
}

static void prog_fp_check(Prog *p, NvnProgram *np) {  // (the lookups below are by code address: shader_prep.h)
    shader_fp_check(&p->fp[0], &np->stages[0], "vs");
    shader_fp_check(&p->fp[1], &np->stages[1], "fs");
}
static void prog_fp_snapshot_init(Prog *p, const NvnProgram *np, const uint8_t *const snapshot[2]) {
    for (int i = 0; i < 2; ++i) {
        if (p->fp[i].state) continue;
        if (!snapshot[i]) { p->fp[i].state = 2; continue; }
        NvnShaderStage readable = np->stages[i];
        readable.code_addr = (uintptr_t)snapshot[i];
        shader_fp_check(&p->fp[i], &readable, i ? "fs" : "vs");
    }
}

// Key memo: draws mostly repeat the program and pipeline state of a recent draw. draw_ compares the inputs of the
// program lookup with the previous lookup's, and those of the pipeline lookup with the last ones of that program
// (plain compares of everything the keys are made from), and reuses the answer instead of hashing the program key
// byte by byte and filling + hashing the ~700-byte PipeKey. The answers are the objects the tables give. key_gen:
// bumped by vk_exec_reset (tables cleared). LARECOMP_VK_NO_KEY_MEMO=1: off.
static int key_memo = -1;
static uint32_t key_gen = 1;
static bool key_memo_on(void) {
    if (key_memo < 0) key_memo = getenv("LARECOMP_VK_NO_KEY_MEMO") == NULL;
    return key_memo;
}

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey);
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    static struct { uint64_t code[2]; ShaderKey vkey; uint8_t rt[8]; uint32_t f_uint, gen; Prog *prog; } m;
    bool on = key_memo_on();
    if (on && m.prog && m.gen == key_gen && (np->stage_mask & 3) == 3 && m.code[0] == np->stages[0].code_addr &&
        m.code[1] == np->stages[1].code_addr && !memcmp(&m.vkey, vkey, sizeof *vkey) && !memcmp(m.rt, fkey->rt_kind, 8) &&
        m.f_uint == fkey->tex_uint) {
        prof_prog_memo++;
        prog_fp_check(m.prog, np);  // (as a table hit)
        return m.prog;
    }
    Prog *p = get_program_(np, vkey, fkey);
    if (on && p) {
        m.code[0] = np->stages[0].code_addr; m.code[1] = np->stages[1].code_addr;
        m.vkey = *vkey; memcpy(m.rt, fkey->rt_kind, 8); m.f_uint = fkey->tex_uint; m.gen = key_gen; m.prog = p;
    }
    return p;
}

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 4; i++) { h ^= (fkey->tex_uint >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) {
        prog_fp_check(prog_tab[slot].prog, np);
        return prog_tab[slot].prog;
    }
    uint64_t cmax[2], csize[2] = {0};
    const uint8_t *snapshot[2] = {0};
    enum NvnShaderSnapshotResult capture[2] = {NVN_SNAPSHOT_ARGUMENT, NVN_SNAPSHOT_ARGUMENT};
    bool captured = true;
    for (int i = 0; i < 2; i++) {
        const NvnShaderStage *stage = &np->stages[i];
        cmax[i] = stage->code_addr ? (stage->const_offset ? stage->const_offset : 0x10000) : 0;
        // NVN's control section describes immutable compiler constants (c1), not
        // per-draw uniform buffers. Capture actual readable mappings, including
        // committed alias memory; a heap-only bound rejected valid VAMM shaders.
        if (!cmax[i]) continue;
        uint64_t declared = stage->const_offset ? stage->const_size : 0;
        snapshot[i] = hs_capture_stage(stage, cmax[i], declared, &capture[i]);
        if (snapshot[i]) csize[i] = declared;
        else captured = false;
    }
    bool replayable = captured && hs_program_replayable(snapshot[0], cmax[0], csize[0],
                                                        snapshot[1], cmax[1], csize[1]);
    // Failed proof/capture must not turn a content hash into a read of an
    // unproved range. The original-memory foreground translator remains the
    // fallback; it cannot be reused or recorded as a cache replay recipe.
    uint64_t ch = captured ? prog_content_hash(snapshot[0], cmax[0], csize[0],
                                              snapshot[1], cmax[1], csize[1], vkey, fkey) : 0;
    if (!replayable) {
        static _Atomic unsigned reasons;
        unsigned reason = atomic_load(&reasons);
        while (reason < 8 && !atomic_compare_exchange_weak(&reasons, &reason, reason+1)) {}
        if (reason < 8)
            LOG("[vk] shader head start: unavailable constant snapshot %016llx "
                "vs code %llx instructions %llu declared %u admitted %llu capture %d; "
                "fs code %llx instructions %llu declared %u admitted %llu capture %d; heap %llx +%llx\n",
                (unsigned long long)ch, (unsigned long long)np->stages[0].code_addr,
                (unsigned long long)cmax[0], np->stages[0].const_size, (unsigned long long)csize[0], capture[0],
                (unsigned long long)np->stages[1].code_addr, (unsigned long long)cmax[1],
                np->stages[1].const_size, (unsigned long long)csize[1], capture[1],
                (unsigned long long)HEAP_BASE, (unsigned long long)kernel_heap_size());
    }
    // Content identity includes the exact declared compiler constants and span boundaries.
    {   // prepared before the game (the same code and keys), hash only proved bytes
        uint64_t vh = hs_snapshot_stage_hash(&np->stages[0], snapshot[0]);
        uint64_t fh = hs_snapshot_stage_hash(&np->stages[1], snapshot[1]);
        uint64_t ck = prog_content_key(vh, fh, vkey, fkey);
        Prog *pp = vh && fh && prep_progs_n ? PREP_GET(prep_progs, ck) : NULL;
        if (pp) {
            prog_tab[slot].key = h;
            prog_tab[slot].prog = pp;
            prog_fp_snapshot_init(pp, np, snapshot);
            prog_fp_check(pp, np);
            return pp;
        }
        shader_first_visit("program", vh, fh, ck);
    }
    Prog *p = replayable ? hs_take(ch) : NULL;
    if (p) {
        // A skipped preparation gate can leave its worker reading this object.
        // Reuse the published immutable metadata without rewriting code[];
        // guest bindings and their addresses remain in st.program/np.
        prog_tab[slot].prog = p;
        prog_tab[slot].key = h;
        prog_fp_snapshot_init(p, np, snapshot);
        prog_fp_check(p, np);
        return p;
    }
    p = calloc(1, sizeof *p);
    prog_fp_snapshot_init(p, np, snapshot);
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = cmax[i];
        p->csize[i] = csize[i];
        // Missing BRX constant metadata still requires original-memory fallback;
        // never let its unbounded resolver read beyond a bounded snapshot.
        if (snapshot[i] && hs_stage_replayable(snapshot[i], cmax[i], csize[i])) p->read_code[i] = snapshot[i];
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->chash = replayable ? ch : 0;
    bool fresh = true;
    Prog *candidate = p;
    if (replayable) p = hs_add(candidate, true, &fresh);
    if (p != candidate) free(candidate);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    prog_fp_snapshot_init(p, np, snapshot);
    prog_fp_check(p, np);
    if (fresh) run_translate(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;
static uint64_t pipe_key_hash(const PipeKey *key);

typedef struct { PipeKey k; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
// (record.c, at the bind) the pipeline of a Pipe: NULL while it builds (wait: sleeps until it is done) or failed
static VkPipeline pipe_resolve(void *obj, bool wait) {
    Pipe *pp = obj;
    int st_;
    while ((st_ = atomic_load(&pp->state)) == 0) {
        if (!wait) return VK_NULL_HANDLE;
        struct timespec ts = {0, 200000};
        nanosleep(&ts, NULL);
    }
    return st_ == 1 ? pp->pipe : VK_NULL_HANDLE;
}
// (key memo, see get_program) everything pipe_key_fill reads besides the program: the pass's formats and the PipeIn.
// One per program (draws often alternate between a few programs): the last pipeline that program drew with.
struct PipeMemo {
    uint32_t gen, ncolor, samples, channel_mask;
    int nattrib, nstream;
    VkFormat color[8], depth, stencil;
    uint32_t attrib[16], stream[16][2], rt_fmt[8];
    uint8_t blend[8][8], blend_on, a2c, cls, poly_fill;
    Pipe *pp;
    VtxLayout vl;  // the layout pipe_key_fill made from these inputs (dynamic vertex input leaves it out of pp's key)
};
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// What a pipeline is built from, besides its program and the pass's attachment formats: the draw's state (draw_) or a
// variant record (renderer_prepare)
typedef struct {
    const uint32_t *attrib; int nattrib;
    const uint32_t (*stream)[2]; int nstream;
    const uint32_t *rt_fmt;         // color target texture formats (0: none)
    uint32_t channel_mask;
    uint8_t blend_on;               // color state blend enables
    const uint8_t (*blend)[8];
    uint8_t a2c, topo_class, poly_fill;
} PipeIn;

// vl (optional): the vertex layout, also when the key leaves it out (dynamic vertex input)
static void pipe_key_fill(PipeKey *k, Prog *pr, const PipeIn *in, const VkFormat color[8], uint32_t ncolor, VkFormat depth,
                          VkFormat stencil, uint32_t samples, VtxLayout *vl) {
    memset(k, 0, sizeof *k);
    k->prog = pr;
    k->ncolor = ncolor;
    // (only the pass's own slots: begin_pass leaves the slots past them as the last pass with more targets set them -
    // a depth-only pass's key carried the previous pass's color formats, so prepared pipelines never matched it)
    for (uint32_t i = 0; i < ncolor && i < 8; i++) k->color[i] = color[i];
    k->depth = depth;
    k->stencil = stencil;
    k->samples = samples;
    VtxLayout l_, *l = vl ? vl : &l_;
    memset(l, 0, sizeof *l);
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        l->attr_mask |= 1u << i;
        uint32_t a = i < in->nattrib ? in->attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= in->nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            l->attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            l->attr_bind[i] = ZERO_BINDING;
            l->used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        l->attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        l->attr_off[i] = off;
        l->attr_bind[i] = stream;
        l->used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(l->used_streams & (1u << s))) continue;
        l->stride[s] = s < in->nstream ? in->stream[s][0] : 0;
        l->divisor[s] = s < in->nstream && l->stride[s] ? in->stream[s][1] : 0;
        if (l->divisor[s] > 1 && !g_vk.divisor) l->divisor[s] = 1;
    }
    k->attr_mask = l->attr_mask;  // (the program's: the same with either path)
    if (!g_vk.vids) {             // the layout is part of the pipeline
        memcpy(k->attr_fmt, l->attr_fmt, sizeof k->attr_fmt);
        memcpy(k->attr_off, l->attr_off, sizeof k->attr_off);
        memcpy(k->attr_bind, l->attr_bind, sizeof k->attr_bind);
        memcpy(k->stride, l->stride, sizeof k->stride);
        memcpy(k->divisor, l->divisor, sizeof k->divisor);
        k->used_streams = l->used_streams;
    }
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        if (!in->rt_fmt[i] || k->color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k->write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (in->channel_mask >> (i * 4)) & 0xF : 0;
        if (((in->blend_on >> i) & 1) && !rt_kind(in->rt_fmt[i])) {
            k->blend_on |= (uint8_t)(1u << i);
            memcpy(k->blend[i], in->blend[i], 8);
        }
    }
    k->a2c = in->a2c;
    k->topo_class = in->topo_class;
    k->poly_fill = in->poly_fill;
}

// A pipeline's create info from its key (the structures point into each other and into the key: filled in place)
typedef struct {
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vs;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[NDYN + 1];  // (+ the vertex layout with dynamic vertex input: draw_ sets it)
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo pi;
} PipeCI;

static void pipe_ci_fill(PipeCI *c, const PipeKey *k) {
    memset(c, 0, sizeof *c);
    c->stages[0] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"};
    c->stages[1] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->a2c && k->prog->fs_late ? k->prog->fs_late : k->prog->fs, .pName = "main"};
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) c->va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        c->vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) c->vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    c->dvi = (VkPipelineVertexInputDivisorStateCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                              .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = c->vd};
    c->vi = (VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &c->dvi : NULL,
                                                   .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = c->vb,
                                                   .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = c->va};
    c->ia = (VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = topo_class_base(k->topo_class)};
    c->vs = (VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    c->rs = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    c->ms = (VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    c->ds = (VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        c->cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            c->cba[i].blendEnable = VK_TRUE;
            c->cba[i].srcColorBlendFactor = blend_factor(b[1]);
            c->cba[i].dstColorBlendFactor = blend_factor(b[2]);
            c->cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            c->cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            c->cba[i].colorBlendOp = blend_op(b[5]);
            c->cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    c->cb = (VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = k->ncolor, .pAttachments = c->cba};
    memcpy(c->dyn, DYN, sizeof DYN);
    if (g_vk.vids) c->dyn[NDYN] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    c->dy = (VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = NDYN + g_vk.vids, .pDynamicStates = c->dyn};
    c->ri = (VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                            .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                            .stencilAttachmentFormat = k->stencil};
    c->pi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &c->ri, .stageCount = 2,
                                           .pStages = c->stages, .pVertexInputState = g_vk.vids ? NULL : &c->vi,
                                           .pInputAssemblyState = &c->ia, .pViewportState = &c->vs, .pRasterizationState = &c->rs,
                                           .pMultisampleState = &c->ms, .pDepthStencilState = &c->ds, .pColorBlendState = &c->cb,
                                           .pDynamicState = &c->dy, .layout = k->prog->layout};
}

// ================================================================== reusable graphics pipeline libraries
typedef struct {
    unsigned hits, misses, library_driver_hits;
    uint64_t library_ns, wait_ns, link_ns;
    bool attempted, fallback, link_driver_hit;
} GplBuildStats;
#ifdef VK_EXT_graphics_pipeline_library
// Cache immutable driver objects, not pointers into a temporary PipeCI. Shader modules/layouts and old
// Prog/Pipe objects already live until process exit (including vk_exec_reset); these libraries have the same
// lifetime. Never evict a library while a linked executable or concurrent build could still refer to it.
// Capacity exhaustion and remembered failures take the ordinary full-pipeline path, never skip a draw.
#define GPL_BUCKETS 1024
#define GPL_LIMIT 8192
static const VkGraphicsPipelineLibraryFlagsEXT gpl_parts[4] = {
    VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
typedef struct {
    VkShaderModule module;
    VkPipelineLayout layout;
    uint32_t part, vids, divisor, topo_class, poly_fill, samples, a2c;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, attr_mask, attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t stride[17], divisors[17], used_streams, write_mask[8];
    uint8_t blend[8][8], blend_on;
} GplKey;
static void gpl_key_fill(GplKey *out, const PipeKey *k, const PipeCI *c, unsigned part) {
    memset(out, 0, sizeof *out);  // byte equality includes initialized padding, never transient pointers
    out->part = part;
    switch (part) {
    case 0:
        out->vids = g_vk.vids;
        out->topo_class = k->topo_class;
        if (!g_vk.vids) {
            out->divisor = g_vk.divisor;
            out->attr_mask = k->attr_mask; out->used_streams = k->used_streams;
            memcpy(out->attr_fmt, k->attr_fmt, sizeof out->attr_fmt);
            memcpy(out->attr_off, k->attr_off, sizeof out->attr_off);
            memcpy(out->attr_bind, k->attr_bind, sizeof out->attr_bind);
            memcpy(out->stride, k->stride, sizeof out->stride);
            memcpy(out->divisors, k->divisor, sizeof out->divisors);
        }
        break;
    case 1:
        out->module = c->stages[0].module; out->layout = c->pi.layout;
        out->poly_fill = k->poly_fill;
        break;
    case 2:
        out->module = c->stages[1].module; out->layout = c->pi.layout;
        // Includes the selected early/late fragment module. Conservatively retain all rendering formats.
        // Multisample state occurs in both fragment subsets and must agree at link time.
        out->samples = k->samples; out->a2c = k->a2c;
        out->ncolor = k->ncolor; out->depth = k->depth; out->stencil = k->stencil;
        memcpy(out->color, k->color, sizeof out->color);
        break;
    case 3:
        out->samples = k->samples; out->a2c = k->a2c;
        out->ncolor = k->ncolor; out->depth = k->depth; out->stencil = k->stencil;
        memcpy(out->color, k->color, sizeof out->color);
        out->blend_on = k->blend_on;
        memcpy(out->blend, k->blend, sizeof out->blend);
        memcpy(out->write_mask, k->write_mask, sizeof out->write_mask);
        break;
    }
}
// Only advertise dynamic state owned by a subset. The executable obtains exactly the full pipeline's state
// from the union; shader modules are reused unchanged (including position invariance), without link-time opt.
static unsigned gpl_dynamic_part(VkDynamicState state) {
    switch (state) {
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: return 0;
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_SCISSOR: case VK_DYNAMIC_STATE_DEPTH_BIAS:
    case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE: case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: return 1;
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: return 3;
    default: return 2;
    }
}
typedef struct {
    VkGraphicsPipelineLibraryCreateInfoEXT library;
    VkPipelineRenderingCreateInfo rendering;
    VkDynamicState dynamic[NDYN + 1];
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo pi;
} GplCI;
static void gpl_ci_fill(GplCI *out, const PipeCI *c, unsigned part) {
    memset(out, 0, sizeof *out);
    out->rendering = c->ri;
    if (part == 1) {  // Pre-rasterization only consumes viewMask; attachment formats are not its state.
        out->rendering.colorAttachmentCount = 0; out->rendering.pColorAttachmentFormats = NULL;
        out->rendering.depthAttachmentFormat = out->rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    }
    out->library = (VkGraphicsPipelineLibraryCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = part ? &out->rendering : NULL, .flags = gpl_parts[part]};
    for (uint32_t i = 0; i < c->dy.dynamicStateCount; i++)
        if (gpl_dynamic_part(c->dy.pDynamicStates[i]) == part)
            out->dynamic[out->dy.dynamicStateCount++] = c->dy.pDynamicStates[i];
    out->dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    out->dy.pDynamicStates = out->dynamic;
    out->pi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &out->library, .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .pDynamicState = &out->dy};
    switch (part) {
    case 0: out->pi.pVertexInputState = c->pi.pVertexInputState; out->pi.pInputAssemblyState = &c->ia; break;
    case 1:
        out->pi.stageCount = 1; out->pi.pStages = &c->stages[0]; out->pi.layout = c->pi.layout;
        out->pi.pViewportState = &c->vs; out->pi.pRasterizationState = &c->rs; break;
    case 2:
        out->pi.stageCount = 1; out->pi.pStages = &c->stages[1]; out->pi.layout = c->pi.layout;
        out->pi.pDepthStencilState = &c->ds; out->pi.pMultisampleState = &c->ms; break;
    case 3: out->pi.pColorBlendState = &c->cb; out->pi.pMultisampleState = &c->ms; break;
    }
}
typedef struct GplEntry {
    struct GplEntry *next;
    GplKey key;
    VkPipeline pipeline;
    VkResult result;
    bool ready;
} GplEntry;
static GplEntry *gpl_buckets[GPL_BUCKETS];
static unsigned gpl_count;
static pthread_mutex_t gpl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpl_ready = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned gpl_hits, gpl_builds, gpl_links, gpl_fallbacks;
// Keep real driver-cache feedback separate from our in-process library memo hits. Some mobile drivers write
// one stage-feedback record unconditionally, so provide storage for both stages even for a zero-stage link.
static VkResult gpl_driver_create(VkGraphicsPipelineCreateInfo *info, VkPipeline *out, bool *cache_hit) {
    VkPipelineCreationFeedback feedback = {0}, stages[2] = {{0}};
    VkPipelineCreationFeedbackCreateInfo feedback_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = info->pNext,
        .pPipelineCreationFeedback = &feedback, .pipelineStageCreationFeedbackCount = info->stageCount,
        .pPipelineStageCreationFeedbacks = stages};
    info->pNext = &feedback_info;
    VkResult result = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, info, NULL, out);
    info->pNext = feedback_info.pNext;
    *cache_hit = result == VK_SUCCESS &&
        (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
        (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
    return result;
}
static VkResult gpl_library_get(const PipeKey *k, const PipeCI *c, unsigned part, VkPipeline *out, GplBuildStats *stats) {
    *out = VK_NULL_HANDLE;
    GplKey key;
    gpl_key_fill(&key, k, c, part);
    unsigned bucket = hash_words(0xcbf29ce484222325ull, &key, sizeof key) & (GPL_BUCKETS - 1);
    pthread_mutex_lock(&gpl_lock);
    GplEntry *entry;
    for (entry = gpl_buckets[bucket]; entry; entry = entry->next) {
        if (memcmp(&entry->key, &key, sizeof key)) continue;
        uint64_t wait_start = SDL_GetTicksNS();
        while (!entry->ready) pthread_cond_wait(&gpl_ready, &gpl_lock);
        stats->wait_ns += SDL_GetTicksNS() - wait_start; stats->hits++;
        *out = entry->pipeline;
        VkResult r = entry->result;
        atomic_fetch_add(&gpl_hits, 1);
        pthread_mutex_unlock(&gpl_lock);
        return r;
    }
    if (gpl_count == GPL_LIMIT || !(entry = calloc(1, sizeof *entry))) {
        pthread_mutex_unlock(&gpl_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(&entry->key, &key, sizeof key);
    entry->next = gpl_buckets[bucket]; gpl_buckets[bucket] = entry; gpl_count++;
    pthread_mutex_unlock(&gpl_lock);
    // Exactly one owner builds each immutable key. No cache lock or worker dependency queue across driver work.
    GplCI info;
    gpl_ci_fill(&info, c, part);
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t start = SDL_GetTicksNS();
    bool driver_hit;
    VkResult result = gpl_driver_create(&info.pi, &pipeline, &driver_hit);
    stats->library_driver_hits += driver_hit;
    stats->library_ns += SDL_GetTicksNS() - start; stats->misses++;
    if (result != VK_SUCCESS && pipeline) { vkDestroyPipeline(g_vk.dev, pipeline, NULL); pipeline = VK_NULL_HANDLE; }
    pthread_mutex_lock(&gpl_lock);
    entry->pipeline = pipeline; entry->result = result; entry->ready = true;
    *out = pipeline;
    atomic_fetch_add(&gpl_builds, 1);
    pthread_cond_broadcast(&gpl_ready);
    pthread_mutex_unlock(&gpl_lock);
    return result;
}
static VkResult pipe_gpl_build(const PipeKey *k, const PipeCI *c, VkPipeline *out, GplBuildStats *stats) {
    VkPipeline libraries[4];
    for (unsigned part = 0; part < 4; part++) {
        VkResult r = gpl_library_get(k, c, part, &libraries[part], stats);
        if (r != VK_SUCCESS) return r;
    }
    VkPipelineLibraryCreateInfoKHR link = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .libraryCount = 4, .pLibraries = libraries};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &link, .layout = c->pi.layout};
    atomic_fetch_add(&gpl_links, 1);
    uint64_t start = SDL_GetTicksNS();
    VkResult result = gpl_driver_create(&info, out, &stats->link_driver_hit);
    stats->link_ns += SDL_GetTicksNS() - start;
    return result;
}
#endif
static VkResult pipe_create_graphics(const PipeKey *k, PipeCI *c, VkPipeline *out, GplBuildStats *stats) {
    memset(stats, 0, sizeof *stats);
    *out = VK_NULL_HANDLE;
#ifdef VK_EXT_graphics_pipeline_library
    if (g_vk.gpl && !c->pi.flags) {  // Preserve executable-statistics/IR capture via the existing monolithic path.
        stats->attempted = true;
        VkResult r = pipe_gpl_build(k, c, out, stats);
        if (r == VK_SUCCESS) return r;
        stats->fallback = true;
        if (*out) { vkDestroyPipeline(g_vk.dev, *out, NULL); *out = VK_NULL_HANDLE; }
        if (atomic_fetch_add(&gpl_fallbacks, 1) < 8)
            LOG("[gpl] library/link failed (%d); full pipeline fallback (built %u, reused %u, linked %u)\n", r,
                atomic_load(&gpl_builds), atomic_load(&gpl_hits), atomic_load(&gpl_links));
    }
#else
    (void)k;
#endif
    return vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &c->pi, NULL, out);
}
// ================================================================== end reusable graphics pipeline libraries

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    PipeCI c;
    pipe_ci_fill(&c, k);
    extern bool g_vk_shader_stats;
    static uint64_t ir_fs = 1;  // LARECOMP_VK_SHADER_IR=<fs code addr hex>: that shader's compiled forms -> build/ir_<addr>.txt
    if (ir_fs == 1) ir_fs = getenv("LARECOMP_VK_SHADER_IR") ? strtoull(getenv("LARECOMP_VK_SHADER_IR"), NULL, 16) : 0;
    bool want_ir = g_vk_shader_stats && ir_fs && k->prog->code[1] == ir_fs;
    if (g_vk_shader_stats) c.pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (want_ir) c.pi.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    uint64_t t0 = SDL_GetTicksNS();
    int bs = build_begin("pipeline vs/fs", k->prog->code[0] ? k->prog->code[0] : k->prog->prep_vh,
                         k->prog->code[1] ? k->prog->code[1] : k->prog->prep_fh);
    SLOW_FEEDBACK(c.pi, c.pi.stageCount);
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] begin pipeline key %016llx prog %p vs %llx fs %llx ncolor %u c0 %d samples %u attrs %08x streams %08x blend %02x\n",
            (unsigned long long)pipe_key_hash(k), (void *)k->prog,
            (unsigned long long)k->prog->code[0], (unsigned long long)k->prog->code[1],
            k->ncolor, k->color[0], k->samples, k->attr_mask, k->used_streams, k->blend_on);
    GplBuildStats gpl_stats;
    VkResult r = pipe_create_graphics(k, &c, &pp->pipe, &gpl_stats);
    build_diag_done("pipeline vs/fs", pipe_key_hash(k), t0, r == VK_SUCCESS ? 1 : 2);
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] graphics driver id %016llx path %s result %d library_hits %u library_misses %u library_driver_hits %u link_driver_hit %d library_us %llu wait_us %llu link_us %llu\n",
            (unsigned long long)pipe_key_hash(k), !gpl_stats.attempted ? "full" : gpl_stats.fallback ? "gpl-fallback" : "gpl",
            r, gpl_stats.hits, gpl_stats.misses, gpl_stats.library_driver_hits, gpl_stats.link_driver_hit,
            (unsigned long long)(gpl_stats.library_ns / 1000),
            (unsigned long long)(gpl_stats.wait_ns / 1000), (unsigned long long)(gpl_stats.link_ns / 1000));
    SLOW_FEEDBACK_HIT();
    if (!gpl_stats.fallback && gpl_stats.link_driver_hit) slow_note_cache_hit();
    build_end(bs);
    atomic_fetch_add(&g_pipes_built, 1);
    if (g_vk_shader_stats && r == VK_SUCCESS) {  // (LARECOMP_VK_SHADER_STATS: the driver's view of each stage)
        VkPipelineInfoKHR pinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pp->pipe};
        uint32_t ne = 0;
        VkPipelineExecutablePropertiesKHR props[4];
        for (int i = 0; i < 4; i++) props[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
        vkGetPipelineExecutablePropertiesKHR(g_vk.dev, &pinfo, &ne, NULL);
        if (ne > 4) ne = 4;
        vkGetPipelineExecutablePropertiesKHR(g_vk.dev, &pinfo, &ne, props);
        for (uint32_t e = 0; e < ne; e++) {
            VkPipelineExecutableInfoKHR ei = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pp->pipe, .executableIndex = e};
            VkPipelineExecutableStatisticKHR stv[32];
            uint32_t ns = 32;
            for (int i = 0; i < 32; i++) stv[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
            if (vkGetPipelineExecutableStatisticsKHR(g_vk.dev, &ei, &ns, stv) < 0) continue;
            char line[1024];
            int o = snprintf(line, sizeof line, "[stats] %s %llx:", props[e].name,
                             (unsigned long long)(props[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT ? k->prog->code[1] : k->prog->code[0]));
            for (uint32_t i = 0; i < ns && o < (int)sizeof line - 64; i++) {
                if (stv[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR)
                    o += snprintf(line + o, sizeof line - o, " %s=%llu", stv[i].name, (unsigned long long)stv[i].value.u64);
            }
            LOG("%s\n", line);
            if (want_ir && (props[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT)) {
                VkPipelineExecutableInternalRepresentationKHR irs[8];
                uint32_t nir = 8;
                for (int i = 0; i < 8; i++) irs[i] = (VkPipelineExecutableInternalRepresentationKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR};
                vkGetPipelineExecutableInternalRepresentationsKHR(g_vk.dev, &ei, &nir, irs);
                for (uint32_t i = 0; i < nir; i++) LOG("[stats] representation %s: %zu bytes\n", irs[i].name, irs[i].dataSize);
                char path[700];
                const char *dir = SDL_GetAndroidExternalStoragePath();
                snprintf(path, sizeof path, "%s/build/ir_%llx.txt", dir ? dir : ".", (unsigned long long)ir_fs);
                FILE *f = fopen(path, "w");
                for (uint32_t i = 0; f && i < nir; i++) {
                    irs[i].pData = malloc(irs[i].dataSize + 1);
                }
                if (f && vkGetPipelineExecutableInternalRepresentationsKHR(g_vk.dev, &ei, &nir, irs) >= 0)
                    for (uint32_t i = 0; i < nir; i++) fprintf(f, "==== %s\n%.*s\n", irs[i].name, (int)irs[i].dataSize, (char *)irs[i].pData);
                for (uint32_t i = 0; i < nir; i++) free(irs[i].pData);
                if (f) fclose(f), LOG("[stats] internal representations -> %s\n", path);
            }
        }
    }
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    if (getenv("LARECOMP_LOG_PIPE_BUILD"))
        LOG("[build] pipeline prog %p hash %016llx ncolor %u c0 %d depth %d blend %x topo %u attrs %x\n", (void *)pp->k.prog,
            (unsigned long long)pipe_key_hash(k), pp->k.ncolor, pp->k.color[0], pp->k.depth, pp->k.blend_on, pp->k.topo_class, pp->k.attr_mask);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
    slow_build(t0, "pipeline vs/fs", k->prog->code[0], k->prog->code[1]);
}

static uint32_t pipekey_size(void) { return (uint32_t)sizeof(PipeKey); }

// ---- shader head start: pipelines (see hs_tab above). Built ahead into their own table; draw_ takes one over on
// its first use (pipe_tab belongs to the recording thread).
static struct { uint64_t key; Pipe *pipe; } hs_pipes[PIPE_TAB];
static uint64_t pipe_key_hash(const PipeKey *k) {
    uint64_t ph = hash_words(0xcbf29ce484222325ull, k, sizeof *k);
    return ph ? ph : 1;
}
// The largest index of a draw (what part of its vertex streams it reads). Without primitive restart a plain maximum,
// which the compiler vectorizes; with it the restart value is skipped (the mamcjr Android port's 327ce07: the per-index
// test was ~9% of its render thread)
static uint32_t index_max(const void *src, uint32_t isz, uint32_t count, bool restart_on, uint32_t restart, uint32_t m) {
    if (isz == 2) {
        const uint16_t *s = src;
        uint16_t r = (uint16_t)restart, mx = 0;
        if (restart_on) { for (uint32_t i = 0; i < count; i++) if (s[i] > mx && s[i] != r) mx = s[i]; }
        else for (uint32_t i = 0; i < count; i++) mx = s[i] > mx ? s[i] : mx;
        return mx > m ? mx : m;
    }
    const uint32_t *s = src;
    uint32_t mx = 0;
    if (restart_on) { for (uint32_t i = 0; i < count; i++) if (s[i] > mx && s[i] != restart) mx = s[i]; }
    else for (uint32_t i = 0; i < count; i++) mx = s[i] > mx ? s[i] : mx;
    return mx > m ? mx : m;
}
static Pipe *hs_pipe_take(uint64_t ph) {
    pthread_mutex_lock(&hs_lock);
    uint32_t slot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    for (int n = 0; n < PIPE_TAB && hs_pipes[slot].key && hs_pipes[slot].key != ph; n++) slot = (slot + 1) & (PIPE_TAB - 1);
    Pipe *pp = hs_pipes[slot].key == ph ? hs_pipes[slot].pipe : NULL;
    pthread_mutex_unlock(&hs_lock);
    return pp;
}
static Pipe *hs_pipe_put(uint64_t ph, Pipe *pp, bool *fresh) {
    pthread_mutex_lock(&hs_lock);
    uint32_t slot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    int n = 0;
    for (; n < PIPE_TAB && hs_pipes[slot].key && hs_pipes[slot].key != ph; n++) slot = (slot + 1) & (PIPE_TAB - 1);
    *fresh = n < PIPE_TAB && !hs_pipes[slot].key;
    Pipe *winner = n < PIPE_TAB ? (*fresh ? pp : hs_pipes[slot].pipe) : NULL;
    if (*fresh) hs_pipes[slot].pipe = pp, hs_pipes[slot].key = ph;
    pthread_mutex_unlock(&hs_lock);
    return winner;
}
// a pipeline made in play: its key, the program by content hash
static void hs_record_pipe(const PipeKey *k) {
    if (!k->prog->chash || !hs_program_replayable(prog_code(k->prog, 0), k->prog->cmax[0], k->prog->csize[0],
                                                 prog_code(k->prog, 1), k->prog->cmax[1], k->prog->csize[1])) return;
    PipeKey r = *k;
    uint64_t ch = k->prog->chash;
    memcpy(&r.prog, &ch, sizeof ch);
    hs_write(HS_PIPE, &r, sizeof r, NULL, 0, NULL, 0);
}

static void *headstart_thread(void *arg) {
    (void)arg;
    plat_thread_name("vk-headstart");
    // not while the game boots: its threads starved of CPU there (a cold start compiling everything) left it waiting
    // for itself, on a black screen. From its first couple of seconds of frames on (the title, the intro).
    extern uint64_t renderer_present_count(void);
    while (!atomic_load(&hs_requested) && renderer_present_count() < 120) SDL_Delay(50);
    double t0 = SDL_GetTicksNS() / 1e6;
    discovery_seed_profiles();
    pthread_mutex_lock(&hs_lock);
    FILE *f = hs_prepare_locked() ? fopen("cache/headstart.bin", "rb") : NULL;
    pthread_mutex_unlock(&hs_lock);
    if (!f) { discovery_replay(); atomic_store(&hs_finished, true); return NULL; }
    HsHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || memcmp(&hdr, &hs_header, sizeof hdr)) {
        fclose(f);
        LOG("[vk] shader head start: header changed after validation; replay skipped\n");
        discovery_replay();
        atomic_store(&hs_finished, true);
        return NULL;
    }
    PipeKey *pk = NULL;
    size_t npk = 0, cap = 0;
    int nprog = 0, ncs = 0;
    size_t prog_cap = 0, cs_cap = 0;
    Prog **programs = NULL;
    CsProg **computes = NULL;
    // The file mapped, not read into memory: the programs' code stays in it for their translations (tens of MB that
    // were the app's own memory - phones closed the game for it - are file pages the system can drop and read again).
    // Records appended from now on are for the next start. (A failed map: read into memory, as before.)
    struct stat fst;
    size_t fsize = fstat(fileno(f), &fst) == 0 ? (size_t)fst.st_size : 0;
    uint8_t *map = fsize ? mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fileno(f), 0) : MAP_FAILED;
    if (map == MAP_FAILED) map = NULL;
    size_t at = sizeof hdr;
    HsRec r;
    for (;;) {
        uint8_t *buf;
        if (map) {
            if (at + sizeof r > fsize) break;
            memcpy(&r, map + at, sizeof r);
            at += sizeof r;
            if (r.size > fsize - at) break;
            buf = map + at;
            at += r.size;
        } else {
            if (fread(&r, sizeof r, 1, f) != 1) break;
            buf = malloc(r.size);
            if (!buf || fread(buf, 1, r.size, f) != r.size) { free(buf); break; }
        }
#define HS_DROP() do { if (!map) free(buf); } while (0)
        if (r.type == HS_PIPE && r.size == sizeof(PipeKey)) {
            if (npk == cap) {
                cap = cap ? cap * 2 : 1024;
                PipeKey *grown_pk = realloc(pk, cap * sizeof *pk);
                if (!grown_pk) FATAL("[vk] shader head start: pipeline key table allocation failed\n");
                pk = grown_pk;
            }
            memcpy(&pk[npk++], buf, sizeof(PipeKey));
            HS_DROP();
            continue;
        }
        if (r.type == HS_CS && r.size >= sizeof(HsCsHeader)) {
            HsCsHeader ch;
            memcpy(&ch, buf, sizeof ch);
            bool fresh = false;
            CsProg *cp = hs_cs_replay(r.size, &ch, buf + sizeof ch, &fresh);
            if (fresh && cp) {
                if ((size_t)ncs == cs_cap) {
                    cs_cap = cs_cap ? cs_cap * 2 : 32;
                    if (cs_cap > HS_CS_TAB) cs_cap = HS_CS_TAB;
                    CsProg **grown_cs = realloc(computes, cs_cap * sizeof *computes);
                    if (!grown_cs) FATAL("[vk] shader head start: compute progress allocation failed\n");
                    computes = grown_cs;
                }
                computes[ncs++] = cp;
            }
            HS_DROP();
            continue;
        }
        HsProgHeader h;
        if (r.type != HS_PROG || r.size < sizeof h) { HS_DROP(); continue; }
        memcpy(&h, buf, sizeof h);
        if (!hs_program_size_valid(r.size, &h) || hs_take(h.chash)) { HS_DROP(); continue; }
        if (!hs_program_replayable(buf + sizeof h, h.cmax[0], h.csize[0],
                                   buf + sizeof h + h.cmax[0] + h.csize[0], h.cmax[1], h.csize[1]) ||
            h.chash != prog_content_hash(buf + sizeof h, h.cmax[0], h.csize[0],
                                        buf + sizeof h + h.cmax[0] + h.csize[0], h.cmax[1], h.csize[1], &h.vkey, &h.fkey)) {
            LOG("[vk] shader head start: external-constant replay cache miss %016llx\n", (unsigned long long)h.chash);
            HS_DROP(); continue;
        }
#undef HS_DROP
        Prog *p = calloc(1, sizeof *p);
        p->code[0] = (uint64_t)(uintptr_t)(buf + sizeof h);  // (the record's copy: kept, the translation reads it)
        p->code[1] = h.cmax[1] ? p->code[0] + h.cmax[0] + h.csize[0] : 0;
        p->read_code[0] = (const uint8_t *)(uintptr_t)p->code[0];
        p->read_code[1] = (const uint8_t *)(uintptr_t)p->code[1];
        p->cmax[0] = h.cmax[0], p->cmax[1] = h.cmax[1];
        p->csize[0] = h.csize[0], p->csize[1] = h.csize[1];
        p->vkey = h.vkey, p->fkey = h.fkey, p->chash = h.chash, p->headstart = true;
        bool fresh;
        Prog *winner = hs_add(p, false, &fresh);
        if (winner != p) {
            free(p);
            if (!map) free(buf);
            continue;
        }
        if (fresh) run_translate_q(translate_program, p, true);
        if ((size_t)nprog == prog_cap) {
            prog_cap = prog_cap ? prog_cap * 2 : 256;
            Prog **grown = realloc(programs, prog_cap * sizeof *programs);
            if (!grown) FATAL("[vk] shader head start: program progress allocation failed\n");
            programs = grown;
        }
        programs[nprog++] = p;
    }
    fclose(f);
    atomic_store(&hs_total, nprog + (int)npk + ncs);
    // pipelines, as their programs become ready
    int built = 0, dropped = 0, left = (int)npk;
    uint8_t *done = calloc(npk ? npk : 1, 1);
    Pipe **pipelines = calloc(npk ? npk : 1, sizeof *pipelines);
    if (!done || !pipelines) FATAL("[vk] shader head start: pipeline progress allocation failed\n");
    while (left > 0) {
        int progress = 0;
        for (size_t i = 0; i < npk; i++) {
            if (done[i]) continue;
            uint64_t ch;
            memcpy(&ch, &pk[i].prog, sizeof ch);
            Prog *p = hs_take(ch);
            int state = p ? atomic_load(&p->state) : 2;
            if (state == 0) continue;
            if (state == 1) {
                Pipe *pp = calloc(1, sizeof *pp);
                pp->k = pk[i];
                pp->k.prog = p;
                bool fresh;
                Pipe *winner = hs_pipe_put(pipe_key_hash(&pp->k), pp, &fresh);
                if (fresh) run_job_q(build_pipeline, pp, true), built++;
                else free(pp);
                pipelines[i] = winner;
            } else {
                dropped++;
            }
            done[i] = 1;
            left--, progress++;
        }
        int complete = 0;
        for (int i = 0; i < nprog; i++) complete += atomic_load(&programs[i]->state) != 0;
        for (size_t i = 0; i < npk; i++) complete += done[i] && (!pipelines[i] || atomic_load(&pipelines[i]->state) != 0);
        for (int i = 0; i < ncs; i++) complete += hs_cs_state(computes[i]) != 0;
        atomic_store(&hs_done, complete);
        if (!progress) SDL_Delay(5);
    }
    // Queueing a pipeline is not readiness. Keep the explicit boot gate open
    // until shader modules and driver pipelines actually finish (success/failure).
    for (;;) {
        int complete = 0;
        for (int i = 0; i < nprog; i++) complete += atomic_load(&programs[i]->state) != 0;
        for (size_t i = 0; i < npk; i++) complete += !pipelines[i] || atomic_load(&pipelines[i]->state) != 0;
        for (int i = 0; i < ncs; i++) complete += hs_cs_state(computes[i]) != 0;
        atomic_store(&hs_done, complete);
        if (complete == atomic_load(&hs_total)) break;
        SDL_Delay(10);
    }
    free(programs);
    free(computes);
    free(pipelines);
    free(pk);
    free(done);
    LOG("[vk] shader head start: %d programs, %d pipelines, %d compute queued (%d unresolved) in %.0f ms\n", nprog, built, ncs, dropped,
        SDL_GetTicksNS() / 1e6 - t0);
    discovery_replay();
    atomic_store(&hs_finished, true);
    return NULL;
}

void vk_headstart_begin(void) {
    if (!headstart_on() || sync_shaders() || atomic_exchange(&hs_started, true)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, headstart_thread, NULL)) {
        LOG("[vk] shader head start: worker unavailable; foreground compilation retained\n");
        atomic_store(&hs_finished, true);
        return;
    }
    pthread_detach(t);
}

// Optional caller-thread boot gate: begin after device setup, before guest draws.
// A 0 total means cache inspection is in progress; cancellation releases the gate
// while the background worker retains ownership of any jobs already submitted.
void vk_headstart_prepare(void (*progress)(int done, int total), bool (*cancelled)(void)) {
    if (!headstart_on() || sync_shaders()) return;
    atomic_store(&hs_requested, true);
    vk_headstart_begin();
    do {
        bool finished = atomic_load(&hs_finished);
        int done = atomic_load(&hs_done), total = atomic_load(&hs_total);
        // Discovery may still be admitting additional work after a completed
        // strict-cache phase. Equality alone must not display final completion.
        if (progress) {
            if (!finished && done == total) progress(0, 0);
            else progress(done, total);
        }
        if (cancelled && cancelled()) break;
        if (finished) break;
        if (atomic_load(&hs_finished)) {
            // Cover completion racing the callback without losing its final
            // actual counts. Cancellation above retains the partial report.
            if (progress) progress(atomic_load(&hs_done), atomic_load(&hs_total));
            break;
        }
        SDL_Delay(25);
    } while (true);
}

// Normal startup-window close must finish submitted work before backend_quit
// saves the driver cache. This is for the main/UI thread, never a build worker.
void vk_headstart_finish(void) {
    if (atomic_load(&hs_started)) {
        atomic_store(&hs_requested, true);
        while (!atomic_load(&hs_finished)) SDL_Delay(10);
    }
    discovery_finish();
}

// Dynamic vertex input (g_vk.vids): the draw's layout, as build_pipeline would bake it in
static void set_vertex_input(VkCommandBuffer cb, const VtxLayout *l) {
    VkVertexInputAttributeDescription2EXT va[32];
    VkVertexInputBindingDescription2EXT vb[17];
    uint32_t na = 0, nb = 0;
    for (int i = 0; i < 32; i++)
        if (l->attr_mask & (1u << i))
            va[na++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = (uint32_t)i, .binding = l->attr_bind[i], .format = l->attr_fmt[i], .offset = l->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(l->used_streams & (1u << s))) continue;
        bool inst = l->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = s, .stride = l->stride[s], .inputRate = inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = inst ? l->divisor[s] : 1};
    }
    rec_CmdSetVertexInputEXT(cb, nb, vb, na, va);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static TexBackend *feedback_image[4]; // the same copied image, for a legal one-layer array view
static int nfeedback;

static VkImageView feedback_sample_view(int slot, int textype, bool stencil) {
    if (slot < 0 || slot >= nfeedback) return VK_NULL_HANDLE;
    if (textype == TEXTYPE_2D && !stencil) return feedback_view[slot];
    if (textype != TEXTYPE_2D_ARRAY && textype != TEXTYPE_2D) return VK_NULL_HANDLE;
    TexBackend *b = feedback_image[slot];
    // snapshot_image copies one mip/layer into an ordinary 2D image. Viewing
    // that same single layer as a 2D array is legal; fabricating more layers or
    // treating a multisample snapshot as sampler2DArray is not.
    if (!b || !b->image || b->base_type != VK_IMAGE_VIEW_TYPE_2D || b->samples != 1 ||
        b->depth != 1 || b->layers != 1 || b->levels != 1) return VK_NULL_HANDLE;
    if (stencil && !(b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) return VK_NULL_HANDLE;
    VkImageAspectFlags aspect = stencil ? VK_IMAGE_ASPECT_STENCIL_BIT :
                               b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, textype == TEXTYPE_2D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                         b->format, 0, 1, 0, 1, id, aspect);
}
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

// Maxwell TEX.B consumes a packed 32-bit texture/sampler handle. The
// translator admits only a proven constant-buffer location, never an arbitrary
// register or GPU pointer. Bound buffers retain the existing NVN lifetime.
static bool sampled_texture_handle(int stage, const ShaderTexture *texture, uint64_t *handle) {
    if (!handle) return false;
    *handle = 0;
    if (!texture || stage < 0 || stage >= 6 || texture->is_image) return false;
    if (!texture->bindless) {
        if (texture->handle_offset < 0x20 || texture->handle_offset >= 0x20 + 32 * 8 ||
            (texture->handle_offset & 3)) return false;
        int slot = ((int)texture->handle_offset - 0x20) / 8;
        if (slot >= 0 && slot < 32) *handle = st.tex[stage][slot];
        return true;
    }
    uint32_t k = texture->bindless_cbuf;
    uint64_t offset = texture->bindless_offset;
    if (k == 2) {
        if (offset < 0x20 || (offset - 0x20) % 8 || (offset - 0x20) / 8 >= 32) return false;
        *handle = (uint32_t)st.tex[stage][(offset - 0x20) / 8];
        return true;
    }
    if (k < 3 || k - 3 >= 18) return false;
    Range range = st.ubuf[stage][k - 3];
    if (!range.addr) return true; // an unbound uniform buffer is the zero binding
    if (offset > range.size || range.size - offset < sizeof(uint32_t) ||
        range.addr > UINT64_MAX - offset || range.addr + offset > UINTPTR_MAX - sizeof(uint32_t)) return false;
    uint32_t word;
    memcpy(&word, (const void *)(uintptr_t)(range.addr + offset), sizeof word);
    *handle = word;
    return true;
}

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    uint64_t h;
    if (!sampled_texture_handle(stage, st_, &h)) return NULL;
    uint32_t tic = h & 0xFFFFF;
    if ((!h && !st_->bindless) || !st.tpool || !st.tpool->entries ||
        st.tpool->count <= 0 || tic >= (uint32_t)st.tpool->count) return NULL;
    NvnTexEntry *entry = &((NvnTexEntry *)st.tpool->entries)[tic];
    if (entry->tex && st_->bindless && !vk_sampled_float_compatible(entry->tex, &entry->view, entry->has_view))
        FATAL("[vk] bindless sampled texture requires a supported float/depth view\n");
    return entry->tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D; d->usage = ci.usage;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    rec_CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// Request-only asynchronous stencil evidence. It never submits or waits while
// a graphics mirror is prepared; readback buffers retire after their own GPU seq.
#define STENCIL_PROBE_PLANES 32
#define STENCIL_PROBE_BYTES (64ull << 20)
#define PROJECTION_PROBE_BYTES (24ull << 20)
#define UNIFORM_PROBE_BYTES 1024ull
#define PROBE_PLANES (STENCIL_PROBE_PLANES + 7)
typedef struct {
    VkBuffer buffer;
    VmaAllocation mem;
    const uint8_t *bytes;
    uint64_t n, seq, frame, source, snapshot, guest, fs_hash, image;
    int stage;
    uint32_t handle_offset;
    uint32_t width, height;
    VkFormat format;
    bool reused, color, depth, uniform;
    uint64_t owner_lo, owner_hi, owner_off, owner_gpu_seq, owner_last_use, source_buffer_offset, observer_last_use;
    char path[160], role[12];
} StencilProbePlane;
static StencilProbePlane stencil_probe_planes[PROBE_PLANES];
static uint64_t stencil_probe_bytes, stencil_probe_captured, stencil_probe_frame;
static uint64_t stencil_probe_polled = ~0ull;
static unsigned stencil_probe_pairs;
static bool stencil_probe_active;
static bool projection_probe_pre, projection_probe_post, projection_probe_mirror_done;
static void projection_probe_mirror_cb4(void);
static TexBackend *projection_probe_rt[2];
static uint64_t projection_probe_guest[2], projection_probe_color_bytes;
static unsigned stencil_probe_sampled;
static struct { unsigned uint_candidates, shape_rejected, queued, alloc_failed; } stencil_probe_stats;
static struct { uint64_t image, fs; uint32_t offset; int stage; } stencil_probe_seen[16];

static bool debug_stencil_shape(const TexBackend *b) {
    return b && b->image && (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
           (b->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && b->samples == 1 &&
           b->levels == 1 && b->layers == 1 && b->depth == 1 && b->width && b->height &&
           (uint64_t)b->width * b->height <= (16ull << 20);
}

static void stencil_probe_write(const StencilProbePlane *p) {
    if (vmaInvalidateAllocation(g_vk.vma, p->mem, 0, p->n) != VK_SUCCESS || !p->bytes) return;
    uint64_t hist[256] = {0}, bits[8] = {0};
    for (uint64_t i = 0; !p->color && i < p->n; i++) {
        hist[p->bytes[i]]++;
        for (unsigned bit = 0; bit < 8; bit++) bits[bit] += (p->bytes[i] >> bit) & 1;
    }
    FILE *f = fopen(p->path, "wb");
    bool written = false;
    if (f) { if (!p->color) fprintf(f, "P5 %u %u 255\n", p->width, p->height); written = fwrite(p->bytes, 1, (size_t)p->n, f) == p->n; fclose(f); }
    char metadata[192];
    snprintf(metadata, sizeof metadata, "%s.json", p->path);
    f = fopen(metadata, "w");
    if (f) {
        fprintf(f, "{\"schema\":2,\"frame\":%llu,\"seq\":%llu,\"role\":\"%s\",\"reused\":%s,\"pgm_written\":%s,"
                   "\"image\":\"%llx\",\"source_image\":\"%llx\",\"snapshot_image\":\"%llx\","
                   "\"texture_guest\":\"%llx\",\"fs_hash\":\"%016llx\",\"format\":%d,"
                   "\"encoding\":\"%s\",\"width\":%u,\"height\":%u,\"mip\":0,\"layer\":0,\"bytes\":%llu,\"sampled_stage\":%d,\"handle_offset\":%u,\"histogram\":[",
                (unsigned long long)p->frame, (unsigned long long)p->seq, p->role, p->reused ? "true" : "false", written ? "true" : "false",
                (unsigned long long)p->image,
                (unsigned long long)p->source, (unsigned long long)p->snapshot, (unsigned long long)p->guest,
                (unsigned long long)p->fs_hash, p->format, p->uniform ? "raw-uniform" : p->depth ? "raw-depth32" : p->color ? "raw-color32" : "pgm-stencil8", p->width, p->height, (unsigned long long)p->n, p->stage, p->handle_offset);
        for (unsigned i = 0; i < 256; i++) fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)hist[i]);
        fprintf(f, "],\"bit_counts\":[");
        for (unsigned i = 0; i < 8; i++) fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)bits[i]);
        fprintf(f, "]");
        if (p->uniform) fprintf(f, ",\"owner_lo\":\"%llx\",\"owner_hi\":\"%llx\",\"owner_off\":%llu,\"owner_gpu_seq\":%llu,\"owner_last_use\":%llu,\"source_buffer_offset\":%llu,\"source_cbuf_addr\":\"%llx\",\"observer_last_use\":%llu",
            (unsigned long long)p->owner_lo, (unsigned long long)p->owner_hi, (unsigned long long)p->owner_off,
            (unsigned long long)p->owner_gpu_seq, (unsigned long long)p->owner_last_use,
            (unsigned long long)p->source_buffer_offset, (unsigned long long)p->guest, (unsigned long long)p->observer_last_use);
        fprintf(f, "}\n"); fclose(f);
    }
    LOG("[vk] stencil capture frame %llu seq %llu %s %s src %llx snap %llx guest %llx %ux%u reused %d nonzero %llu written %d\n",
        (unsigned long long)p->frame, (unsigned long long)p->seq, p->role, p->path,
        (unsigned long long)p->source, (unsigned long long)p->snapshot, (unsigned long long)p->guest,
        p->width, p->height, p->reused, (unsigned long long)(p->n - hist[0]), written);
}

static void stencil_probe_retire(void) {
    if (!stencil_probe_bytes) return;
    uint64_t done = vk_completed_seq();
    for (unsigned i = 0; i < PROBE_PLANES; i++) {
        StencilProbePlane *p = &stencil_probe_planes[i];
        if (!p->buffer || p->seq > done) continue;
        stencil_probe_write(p);
        vmaDestroyBuffer(g_vk.vma, p->buffer, p->mem);
        stencil_probe_bytes -= p->n;
        memset(p, 0, sizeof *p);
    }
}

static void stencil_probe_poll(void) {
    stencil_probe_retire();
    uint64_t frame = vk_frame_counter();
    if (stencil_probe_active && frame != stencil_probe_frame) {
        LOG("[vk] stencil probe frame %llu ended: uint_candidates %u shape_rejected %u queued %u alloc_failed %u pairs %u sampled %u captured_bytes %llu pending_bytes %llu pending_seq %llu completed_seq %llu\n",
            (unsigned long long)stencil_probe_frame, stencil_probe_stats.uint_candidates, stencil_probe_stats.shape_rejected,
            stencil_probe_stats.queued, stencil_probe_stats.alloc_failed, stencil_probe_pairs, stencil_probe_sampled,
            (unsigned long long)stencil_probe_captured, (unsigned long long)stencil_probe_bytes,
            (unsigned long long)vk_pending_seq(), (unsigned long long)vk_completed_seq());
        stencil_probe_active = false;
    }
    if (frame == stencil_probe_polled) return;
    stencil_probe_polled = frame;
    // A legacy F9 pass PNG can itself submit work: keep this probe independent.
    if (g_debug_frame || stencil_probe_active || stencil_probe_bytes) return;
    struct stat sb;
    if (stat("stencil-probe.request", &sb) || !S_ISREG(sb.st_mode) || unlink("stencil-probe.request")) return;
    stencil_probe_active = true; stencil_probe_frame = frame;
    stencil_probe_pairs = 0; stencil_probe_sampled = 0; stencil_probe_captured = 0;
    projection_probe_pre = projection_probe_post = projection_probe_mirror_done = false; projection_probe_color_bytes = 0;
    memset(&stencil_probe_stats, 0, sizeof stencil_probe_stats);
    LOG("[vk] stencil probe frame %llu armed (8 pairs + 16 sampled planes, 64MiB, async)\n", (unsigned long long)frame);
}

static StencilProbePlane *debug_save_stencil(TexBackend *b, const char *path, uint64_t frame,
                               uint64_t source_image, uint64_t snapshot_image,
                               uint64_t guest_addr, const char *role, bool reused) {
    if (!debug_stencil_shape(b)) return NULL;
    uint64_t n = (uint64_t)b->width * b->height;
    if (n > STENCIL_PROBE_BYTES - stencil_probe_bytes || n > STENCIL_PROBE_BYTES - PROJECTION_PROBE_BYTES - UNIFORM_PROBE_BYTES - stencil_probe_captured) return NULL;
    StencilProbePlane *p = NULL;
    for (unsigned i = 0; i < STENCIL_PROBE_PLANES; i++) if (!stencil_probe_planes[i].buffer) { p = &stencil_probe_planes[i]; break; }
    if (!p) return NULL;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VmaAllocationInfo info;
    VkBuffer buffer;
    VmaAllocation mem;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &mem, &info) != VK_SUCCESS) { stencil_probe_stats.alloc_failed++; return NULL; }
    if (!info.pMappedData) { vmaDestroyBuffer(g_vk.vma, buffer, mem); return NULL; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    rec_CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    *p = (StencilProbePlane){.buffer = buffer, .mem = mem, .bytes = info.pMappedData, .n = n,
        .seq = vk_pending_seq(), .frame = frame, .source = source_image, .snapshot = snapshot_image,
        .guest = guest_addr, .fs_hash = g_cur_fs_hash, .width = b->width, .height = b->height,
        .format = b->format, .reused = reused, .image = (uint64_t)(uintptr_t)b->image, .stage = -1};
    snprintf(p->path, sizeof p->path, "%s", path); snprintf(p->role, sizeof p->role, "%s", role);
    stencil_probe_bytes += n; stencil_probe_captured += n; stencil_probe_stats.queued++;
    return p;
}

static void debug_stencil_pair(NvnTexture *t, TexBackend *src, TexBackend *dst, bool reused) {
    if (!stencil_probe_active || g_debug_frame || vk_frame_counter() != stencil_probe_frame || stencil_probe_pairs == 8) return;
    if (!debug_stencil_shape(src) || !debug_stencil_shape(dst)) { stencil_probe_stats.shape_rejected++; return; }
    uint64_t n = (uint64_t)src->width * src->height + (uint64_t)dst->width * dst->height;
    if (n > STENCIL_PROBE_BYTES - stencil_probe_bytes || n > STENCIL_PROBE_BYTES - stencil_probe_captured) return;
    unsigned free_slots = 0;
    for (unsigned i = 0; i < STENCIL_PROBE_PLANES; i++) free_slots += !stencil_probe_planes[i].buffer;
    if (free_slots < 18 || stencil_probe_captured + n > (32ull << 20)) return;
    unsigned pair = stencil_probe_pairs++;
    vk_end_pass();
    st.rt_dirty = true;
    char path[160];
    uint64_t source = (uint64_t)(uintptr_t)src->image, snapshot = (uint64_t)(uintptr_t)dst->image;
    snprintf(path, sizeof path, "build/stencil_%llu_%02u_src.pgm", (unsigned long long)stencil_probe_frame, pair);
    debug_save_stencil(src, path, stencil_probe_frame, source, snapshot, t->addr, "source", reused);
    snprintf(path, sizeof path, "build/stencil_%llu_%02u_snap.pgm", (unsigned long long)stencil_probe_frame, pair);
    debug_save_stencil(dst, path, stencil_probe_frame, source, snapshot, t->addr, "snapshot", reused);
}

static bool projection_color_shape(const TexBackend *b) {
    return b && b->image && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        (b->format == VK_FORMAT_R8G8B8A8_UNORM || b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
        (b->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && b->samples == 1 && b->levels == 1 &&
        b->layers == 1 && b->depth == 1 && b->width && b->height &&
        (uint64_t)b->width * b->height * 4 <= PROJECTION_PROBE_BYTES;
}

static bool projection_depth_shape(const TexBackend *b) {
    return b && b->image && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
        (b->format == VK_FORMAT_D32_SFLOAT_S8_UINT || b->format == VK_FORMAT_D24_UNORM_S8_UINT ||
         b->format == VK_FORMAT_D32_SFLOAT || b->format == VK_FORMAT_X8_D24_UNORM_PACK32) &&
        (b->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && b->samples == 1 && b->levels == 1 &&
        b->layers == 1 && b->depth == 1 && b->width && b->height &&
        (uint64_t)b->width * b->height * 4 <= PROJECTION_PROBE_BYTES;
}

static bool projection_plane_queue(TexBackend *b, uint64_t guest, const char *role, bool depth) {
    if (!(depth ? projection_depth_shape(b) : projection_color_shape(b))) return false;
    uint64_t n = (uint64_t)b->width * b->height * 4;
    if (n > PROJECTION_PROBE_BYTES - projection_probe_color_bytes || n > STENCIL_PROBE_BYTES - stencil_probe_bytes) return false;
    StencilProbePlane *p = NULL;
    for (unsigned i = STENCIL_PROBE_PLANES; i < PROBE_PLANES; i++)
        if (!stencil_probe_planes[i].buffer) { p = &stencil_probe_planes[i]; break; }
    if (!p) return false;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VkBuffer buffer; VmaAllocation mem; VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &mem, &info) != VK_SUCCESS) return false;
    if (!info.pMappedData) { vmaDestroyBuffer(g_vk.vma, buffer, mem); return false; }
    VkCommandBuffer cb = vk_cmd(); vk_full_barrier(cb);
    VkBufferImageCopy c = {.imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {b->width, b->height, 1}};
    rec_CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    *p = (StencilProbePlane){.buffer = buffer, .mem = mem, .bytes = info.pMappedData, .n = n,
        .seq = vk_pending_seq(), .frame = stencil_probe_frame, .source = (uint64_t)(uintptr_t)b->image,
        .image = (uint64_t)(uintptr_t)b->image, .guest = guest, .fs_hash = g_cur_fs_hash,
        .width = b->width, .height = b->height, .format = b->format, .stage = -1, .color = true, .depth = depth};
    snprintf(p->role, sizeof p->role, "%s", role);
    snprintf(p->path, sizeof p->path, "build/projection_%llu_%s.bin", (unsigned long long)p->frame, role);
    stencil_probe_bytes += n; projection_probe_color_bytes += n; stencil_probe_stats.queued++;
    return true;
}

static bool projection_color_queue(TexBackend *b, uint64_t guest, const char *role) {
    return projection_plane_queue(b, guest, role, false);
}

// (debug) LARECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("LARECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// LARECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; bool read; } CopyAlias;
// LARECOMP_VK_COPY_LAZY=1 (default): colour copies may alias too (same
// format, or exact R11G11B10F -> RGBA16F conversion). They remain deferred only
// while the source is unchanged; all bytes are preserved before either side is
// overwritten. An unread copy has the same lifetime contract as a read one.
static int copy_lazy(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_COPY_LAZY"); on = e && *e ? atoi(e) : 1; }
    return on;
}
static CopyAlias stale[8];
static int nstale;
static struct { NvnTexture *t; VkImage img; int unread; } alias_hist[16];  // per destination: aliases dropped unread in a row
static int nhist;
static uint64_t stat_alias_skipped, stat_alias_late;

extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;
static int hist_find(NvnTexture *t) {
    VkImage img = t->backend ? ((TexBackend *)t->backend)->image : VK_NULL_HANDLE;
    for (int i = 0; i < nhist; i++)
        if (alias_hist[i].t == t) {
            if (alias_hist[i].img != img) { alias_hist[i].img = img; alias_hist[i].unread = 0; }
            return i;
        }
    int i = nhist < 16 ? nhist++ : (int)(stat_alias_made & 15);
    alias_hist[i].t = t; alias_hist[i].img = img; alias_hist[i].unread = 0;
    return i;
}
static bool is_ds(const TexBackend *b) { return b && (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)); }
static bool fmt_exact(VkFormat s, VkFormat d) {  // every value of s representable in d
    return s == d || (s == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && d == VK_FORMAT_R16G16B16A16_SFLOAT);
}

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    if (!s || !d || !s->image || !d->image) return;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    int w = (int)d->width, h = (int)d->height;
    // (plain transfer commands only: this runs from texture teardown and from draw setup, where a draw_copy's render
    // state switch is not safe; a 1:1 nearest blit converts R11G11B10F -> RGBA16F exactly)
    if (s->format == d->format) rec_CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    else {
        VkImageBlit bl = {.srcSubresource = {aspects, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                          .dstSubresource = {aspects, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {w, h, 1}}};
        rec_CmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    }
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if ((!naliases && !nstale) || !t) return;
    for (int i = nstale - 1; i >= 0; i--) {  // a stale destination written: whole - forgotten; partly - catches up first
        CopyAlias *a = &stale[i];
        if (a->dst != t) continue;
        if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) && alias_valid(a)) { alias_copy(a, ((TexBackend *)t->backend)->aspect, why); stat_alias_late++; }
        stale[i] = stale[--nstale];
    }
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (LARECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy (depth only)
    bool ds = is_ds(t->backend);
    if (ds && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        if (!alias_valid(a)) { alias_drop(i); continue; }  // (either image gone: nothing to hand over)
        // A guest copy names bytes at copy time. An unread destination can
        // become an input later, so preserve its original contents before the
        // source changes; read history cannot justify dropping this copy.
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) { aliases[i].read = true; return aliases[i].src; }
            alias_drop(i);
            return t;
        }
    return t;
}

// a stale destination about to be read: its copy is made now (callers: where ending the pass is safe - before a draw's
// pass begins, a copy or a readback; not inside a draw's texture binding)
static void alias_catch_up(NvnTexture *t) {
    for (int i = 0; i < nstale; i++)
        if (stale[i].dst == t) {
            CopyAlias a = stale[i];
            stale[i] = stale[--nstale];
            alias_hist[hist_find(t)].unread = 0;
            if (alias_valid(&a)) { alias_copy(&a, ((TexBackend *)t->backend)->aspect, "late read"); stat_alias_late++; }
            return;
        }
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    bool ds = is_ds(s);
    if (!ds && (!copy_lazy() || !fmt_exact(s->format, d->format))) return false;
    if ((ds && s->format != d->format) || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image, false};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    for (int i = nstale - 1; i >= 0; i--)  // a stale destination goes: nothing to do; its source goes: it catches up now
        if (stale[i].dst == t) stale[i] = stale[--nstale];
        else if (stale[i].src == t) { if (alias_valid(&stale[i])) { alias_copy(&stale[i], ((TexBackend *)stale[i].dst->backend)->aspect, "src freed"); stat_alias_late++; } stale[i] = stale[--nstale]; }
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = nstale = nhist = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later, %llu skipped, %llu late (live %d, stale %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, (unsigned long long)stat_alias_skipped, (unsigned long long)stat_alias_late, naliases, nstale);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = stat_alias_skipped = stat_alias_late = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(Prog *pr) {
    if (!naliases && !nstale) return;
    if (nstale && pr) {  // sampled stale destinations catch up first
        ShaderInfo *infos[2] = {pr->vi, pr->fi};
        for (int sg = 0; sg < 2 && nstale; sg++)
            for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0) && nstale; i++) {
                NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
                if (t) alias_catch_up(t);
            }
    }
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

// A snapshot contains both planes of a depth/stencil image. Stencil-only
// producers must invalidate it even when depth writes are disabled.
static bool snapshot_draw_writes_depth_stencil(void) {
    return (st.ds[0] && st.ds[1]) ||
           (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
}

static bool snapshot_self_write_reuse(Prog *pr, NvnTexture *t) {
    if (t != st.rt.depth || !st.ds[0] || !st.ds[1]) return false;
    // The legacy floating-point depth feedback path intentionally reuses the
    // pre-write image. Raw stencil consumers require the current producer bits.
    // Inspect every binding before deduplication: depth and stencil can name the
    // same combined image in two different sampled slots.
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; infos[sg] && i < infos[sg]->ntex; i++)
            if (infos[sg]->tex[i].is_uint && shader_texture_object(sg, &infos[sg]->tex[i]) == t) return false;
    return true;
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = snapshot_self_write_reuse(pr, t);
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                debug_stencil_pair(t, src, &existing->copy, true);
                feedback_tex[nfeedback] = t;
                feedback_image[nfeedback] = &existing->copy;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            rec_CmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            debug_stencil_pair(t, src, dst, false);
            cb = vk_cmd();
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_image[nfeedback] = dst;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// Capture what a raw UINT consumer actually samples, including nonattachment
// mask images. Run before mirror_prepare_graphics: this only records transfers.
static void stencil_probe_consumers(Prog *pr) {
    if (!stencil_probe_active || g_debug_frame || vk_frame_counter() != stencil_probe_frame) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; infos[sg] && i < infos[sg]->ntex && stencil_probe_sampled < 16; i++) {
            ShaderTexture *tex = &infos[sg]->tex[i];
            if (!tex->is_uint) continue;
            stencil_probe_stats.uint_candidates++;
            if (tex->bindless || tex->is_image) continue;
            NvnTexture *t = shader_texture_object(sg, tex);
            if (!t) continue;
            NvnTexture *actual = naliases || nstale ? alias_read(t) : t;
            TexBackend *source = vk_tex(actual, false), *bound = source;
            bool snapshot = false;
            for (int k = 0; k < nfeedback; k++) if (feedback_tex[k] == t) {
                bound = feedback_image[k]; snapshot = true; break;
            }
            if (!debug_stencil_shape(bound)) { stencil_probe_stats.shape_rejected++; continue; }
            uint64_t image = (uint64_t)(uintptr_t)bound->image;
            bool seen = false;
            for (unsigned k = 0; k < stencil_probe_sampled; k++)
                if (stencil_probe_seen[k].image == image && stencil_probe_seen[k].fs == g_cur_fs_hash &&
                    stencil_probe_seen[k].stage == sg && stencil_probe_seen[k].offset == tex->handle_offset) seen = true;
            if (seen) continue;
            vk_end_pass(); st.rt_dirty = true;
            char path[160];
            snprintf(path, sizeof path, "build/stencil_%llu_sampled_%02u.pgm",
                     (unsigned long long)stencil_probe_frame, stencil_probe_sampled);
            StencilProbePlane *p = debug_save_stencil(bound, path, stencil_probe_frame,
                (uint64_t)(uintptr_t)source->image, snapshot ? image : 0, t->addr, "sampled", snapshot);
            if (!p) continue;
            p->stage = sg; p->handle_offset = tex->handle_offset;
            stencil_probe_seen[stencil_probe_sampled].image = image;
            stencil_probe_seen[stencil_probe_sampled].fs = g_cur_fs_hash;
            stencil_probe_seen[stencil_probe_sampled].stage = sg;
            stencil_probe_seen[stencil_probe_sampled++].offset = tex->handle_offset;
        }
}

static TexBackend *projection_sampled_image(ShaderTexture *tex, NvnTexture **original) {
    if (tex->bindless || tex->is_image) return NULL;
    NvnTexture *t = shader_texture_object(1, tex);
    if (!t) return NULL;
    *original = t;
    NvnTexture *actual = naliases || nstale ? alias_read(t) : t;
    TexBackend *bound = vk_tex(actual, false);
    for (int k = 0; k < nfeedback; k++) if (feedback_tex[k] == t) { bound = feedback_image[k]; break; }
    return bound;
}

static void projection_probe_before_draw(Prog *pr) {
    if (!stencil_probe_active || g_debug_frame || vk_frame_counter() != stencil_probe_frame ||
        projection_probe_pre || g_cur_fs_hash != 0x44b584f0d922d49dull) return;
    if (!pr->fi || pr->fi->ntex <= 3 || st.rt.ncolor < 2) return;
    NvnTexture *t = NULL;
    TexBackend *input = projection_sampled_image(&pr->fi->tex[3], &t);
    if (!projection_color_shape(input)) return;
    vk_end_pass(); st.rt_dirty = true;
    if (!projection_color_queue(input, t->addr, "input-t3")) return;
    projection_probe_pre = true;
    projection_probe_mirror_cb4();
    for (int i = 0; i < 2; i++) {
        NvnTexture *sampled = NULL;
        TexBackend *depth = projection_sampled_image(&pr->fi->tex[i ? 2 : 0], &sampled);
        if (depth && sampled) projection_plane_queue(depth, sampled->addr, i ? "depth-t2" : "depth-t0", true);
    }
    if (st.rt.depth) projection_plane_queue(vk_tex(st.rt.depth, true), st.rt.depth->addr, "depth-rt", true);
    for (int i = 0; i < 2; i++) {
        NvnTexture *rt = st.rt.color[i];
        projection_probe_rt[i] = rt ? vk_tex(rt, true) : NULL;
        projection_probe_guest[i] = rt ? rt->addr : 0;
    }
}

static void projection_probe_after_draw(void) {
    if (!stencil_probe_active || g_debug_frame || vk_frame_counter() != stencil_probe_frame ||
        !projection_probe_pre || projection_probe_post || g_cur_fs_hash != 0x44b584f0d922d49dull) return;
    projection_probe_post = true;
    vk_end_pass(); st.rt_dirty = true;
    projection_color_queue(projection_probe_rt[0], projection_probe_guest[0], "rt0-after");
    projection_color_queue(projection_probe_rt[1], projection_probe_guest[1], "rt1-after");
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Zeros for what isn't bound (constant buffers, vertex streams) and the
// (0, 0, 0, 1) of unbound attributes: one buffer, never written again - fresh zero copies each draw cost a memcpy and,
// at a new offset every time, a vertex buffer bind each.
#define ZBUF_SIZE 0x10000ull  // [0, ZBUF_SIZE): zeros; then (0, 0, 0, 1) as floats
static VkBuffer zbuf;
static VmaAllocation zbuf_mem;
static VkBuffer zero_buffer(void) {
    if (zbuf) return zbuf;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZBUF_SIZE + 256,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &zbuf, &zbuf_mem, &info));
    memset(info.pMappedData, 0, ZBUF_SIZE + 256);
    ((float *)((uint8_t *)info.pMappedData + ZBUF_SIZE))[3] = 1.0f;
    vmaFlushAllocation(g_vk.vma, zbuf_mem, 0, VK_WHOLE_SIZE);
    return zbuf;
}

// Lean constant blocks: the scale table (66 rows, 1 KB) and the driver constants (up to 2 KB) were zero-filled in
// full and compared in full per stage and draw, although only a few rows / entries carry anything. These build and
// compare only those (the scale table's rows 0..ntex and 65; the driver constants' storage buffer entries below the
// size the shader reads) and write the same bytes as before. LARECOMP_VK_NO_CBUF_LEAN=1: the full blocks, as before.
static int cbuf_lean_v = -1;
static bool cbuf_lean(void) {
    if (cbuf_lean_v < 0) cbuf_lean_v = getenv("LARECOMP_VK_NO_CBUF_LEAN") == NULL;
    return cbuf_lean_v;
}
// The scale table of a graphics stage: rows 0..nt and 65 of rs are set, the rows between are zero (not read from rs).
static VkSpan rscale_dedupe(int sg, const float rs[66][4], int nt) {
    static struct { uint64_t seq; int nt; float r[66][4]; VkSpan span; } last[2];
    uint64_t seq = vk_pending_seq();
    size_t used = (size_t)(nt + 1) * 16;
    if (last[sg].seq == seq && last[sg].nt == nt && !memcmp(last[sg].r, rs, used) && !memcmp(last[sg].r[65], rs[65], 16)) {
        prof_cb_reused++;
        return last[sg].span;
    }
    VkSpan s = vk_transient(66 * 16, 256);
    memcpy(s.ptr, rs, used);
    if (nt < 64) memset((uint8_t *)s.ptr + used, 0, (size_t)(64 - nt) * 16);
    memcpy((uint8_t *)s.ptr + 65 * 16, rs[65], 16);
    last[sg].seq = seq; last[sg].nt = nt; memcpy(last[sg].r, rs, used); memcpy(last[sg].r[65], rs[65], 16); last[sg].span = s;
    prof_cb_copied++;
    return s;
}
// The driver constants of a graphics stage (cbuf 0, size bytes): zero but for the stage's storage buffer table at
// base (16 bytes an entry: address low, high, size). Compared by the entries within size.
static VkSpan drv_dedupe(int stage, uint64_t size) {
    static struct { uint64_t seq, size; int stage; Range sb[16]; VkSpan span; } last[2];
    int slot = stage == 1;
    uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
    int ne = size > base ? (int)((size - base + 15) / 16) : 0;
    if (ne > 16) ne = 16;
    uint64_t seq = vk_pending_seq();
    if (last[slot].seq == seq && last[slot].size == size && last[slot].stage == stage &&
        !memcmp(last[slot].sb, st.sbuf[stage], (size_t)ne * sizeof(Range))) {
        prof_cb_reused++;
        return last[slot].span;
    }
    uint32_t drv[0x800 / 4];
    memset(drv, 0, size);
    for (int i = 0; i < ne; i++) {  // (an entry may run past size: drv is 0x800 bytes, only size of them are copied)
        uint32_t *e = &drv[(base + 16 * i) / 4];
        e[0] = (uint32_t)st.sbuf[stage][i].addr;
        e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
        e[2] = (uint32_t)st.sbuf[stage][i].size;
    }
    VkSpan s = vk_transient_copy(drv, size, 256);
    last[slot].seq = seq; last[slot].size = size; last[slot].stage = stage; last[slot].span = s;
    memcpy(last[slot].sb, st.sbuf[stage], (size_t)ne * sizeof(Range));
    prof_cb_copied++;
    return s;
}

// Exact CPU bytes of the already selected staging span, not a second guest-memory read.
// Existing material request gates this bounded diagnostic; descriptor/copy policy is untouched.
static bool material_probe_bound_enabled(void);
#define MATERIAL_BOUND_MAX_BYTES 8192u
static void material_bound_projection(int stage, int k, VkSpan span, uint64_t size,
                                      uint64_t guest, bool unbound_zero) {
    if (g_cur_fs_hash != UINT64_C(0x44b584f0d922d49d) || !material_probe_bound_enabled()) return;
    unsigned slot;
    if (stage == 1 && k == 4) slot = 0;
    else if (stage == 1 && k == 7) slot = 1;
    else if (stage == 0 && k == 12) slot = 2;
    else return;
    static uint64_t frame_saved = UINT64_MAX;
    static unsigned saved;
    uint64_t frame = vk_frame_counter();
    if (frame_saved != frame) { frame_saved = frame; saved = 0; }
    if (saved & (1u << slot)) return;
    saved |= 1u << slot;
    uint64_t limit = g_vk.props.limits.maxUniformBufferRange;
    uint64_t range = size > limit ? limit : size;
    size_t bytes = range > MATERIAL_BOUND_MAX_BYTES ? MATERIAL_BOUND_MAX_BYTES : (size_t)range;
    const char *status = "captured";
    static const uint8_t zeros[MATERIAL_BOUND_MAX_BYTES];
    const void *source = unbound_zero ? (const void *)zeros : span.ptr;
    if (!range || !span.buf || (!unbound_zero && (!source ||
        (uintptr_t)source > UINTPTR_MAX - bytes))) status = "invalid-or-unmapped-span";
    char path[128];
    snprintf(path, sizeof path, "bound-projection-%llu-%d-%d.bin", (unsigned long long)frame, stage, k);
    size_t written = 0;
    if (!strcmp(status, "captured")) {
        FILE *data = fopen(path, "wb");
        if (!data) status = "payload-open-failed";
        else {
            written = fwrite(source, 1, bytes, data);
            if (fclose(data) || written != bytes) status = "payload-write-failed";
        }
    }
    snprintf(path, sizeof path, "bound-projection-%llu-%d-%d.bin.json", (unsigned long long)frame, stage, k);
    FILE *meta = fopen(path, "w");
    if (!meta) return;
    fprintf(meta, "{\"schema\":1,\"source\":\"exact-bound-staging\",\"frame\":%llu,"
                  "\"stage\":%d,\"cbuf\":%d,\"buffer\":\"%llx\",\"offset\":%llu,\"range\":%llu,"
                  "\"guest\":\"%llx\",\"unbound_zero\":%s,\"captured_bytes\":%zu,"
                  "\"truncated\":%s,\"status\":\"%s\"}\n",
            (unsigned long long)frame, stage, k, (unsigned long long)(uintptr_t)span.buf,
            (unsigned long long)span.off, (unsigned long long)range, (unsigned long long)guest,
            unbound_zero ? "true" : "false", written, range > bytes ? "true" : "false", status);
    fclose(meta);
}

static bool mirror_uniform_info(int stage, int k, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);
static uint64_t cbuf_bound_size(const ShaderInfo *si, int k, Range range) {
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if ((si->cbuf_dynamic & (1u << k)) && range.size) size = range.size > 0x10000 ? 0x10000 : range.size;
    if (size > range.size && range.size) size = range.size;
    return size < 16 ? 16 : size;
}

// Shader reflection rounds C1 declarations to binding granularity. That
// rounded extent does not make adjacent guest pages part of the constants.
static VkDescriptorBufferInfo compiler_constants_span(const NvnShaderStage *shader, uint64_t extent) {
    NvnConstantBufferPlan plan;
    if (!nvn_constant_buffer_plan(shader->code_addr, shader->const_offset, shader->const_size,
            extent, g_vk.props.limits.maxUniformBufferRange, &plan))
        FATAL("[vk] invalid compiler constant buffer span\n");
    static struct {
        bool valid;
        uint64_t seq, generation;
        NvnConstantBufferPlan plan;
        VkSpan span;
    } cache[64];
    uint64_t h = plan.source_address * UINT64_C(0x9e3779b97f4a7c15) ^
        plan.source_size * UINT64_C(0xc2b2ae3d27d4eb4f) ^ plan.staging_size;
    unsigned slot = (unsigned)(h ^ (h >> 32)) & 63;
    uint64_t seq = vk_pending_seq();
    if (cache[slot].valid && cache[slot].seq == seq && cache[slot].generation == gcopy_gen &&
        cache[slot].plan.source_address == plan.source_address &&
        cache[slot].plan.source_size == plan.source_size && cache[slot].plan.staging_size == plan.staging_size)
        return ubo_span(cache[slot].span, plan.staging_size);
    VkSpan span = vk_transient(plan.staging_size, 256);
    if (!nvn_constant_buffer_copy(&plan, span.ptr, plan.staging_size))
        FATAL("[vk] invalid compiler constant buffer copy\n");
    cache[slot].valid = true;
    cache[slot].seq = seq;
    cache[slot].generation = gcopy_gen;
    cache[slot].plan = plan;
    cache[slot].span = span;
    static unsigned logged;
    if (logged < 8 && getenv("LARECOMP_LOG_CBUF")) {
        ++logged;
        LOG("[vk-cbuf] compiler constants source %#llx bytes %llu, binding bytes %llu\n",
            (unsigned long long)plan.source_address, (unsigned long long)plan.source_size,
            (unsigned long long)plan.staging_size);
    }
    return ubo_span(span, plan.staging_size);
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (k == 0) {
        if (stage != 5 && cbuf_lean()) {  // (the size as below)
            uint64_t size = si->cbuf_size[0] > 0x800 ? 0x800 : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
            return ubo_span(drv_dedupe(stage, size), size);
        }
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        return compiler_constants_span(ss, si->cbuf_size[1]);
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) {
        VkSpan span = {zero_buffer(), 0, NULL};
        uint64_t zero_size = size > ZBUF_SIZE ? ZBUF_SIZE : size;
        material_bound_projection(stage, k, span, zero_size, 0, true);
        return ubo_span(span, zero_size);
    }
    size = cbuf_bound_size(si, k, r);
    VkDescriptorBufferInfo forwarded;
    // GPU-written spans cannot be host-read by the CPU staging diagnostic.
    if (mirror_uniform_info(stage, k, r.addr, size, &forwarded)) return forwarded;
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    // (lazy heap chunks) pointers in it import their chunks: scanned once per shared copy (LARECOMP_VK_NO_SCAN_ONCE=1:
    // at every draw, as before)
    static int scan_once = -1;
    if (scan_once < 0) scan_once = getenv("LARECOMP_VK_NO_SCAN_ONCE") == NULL;
    VkSpan span;
    if (gc_lazy && si->uses_gmem && scan_once) span = guest_copy_ex(r.addr, size, true);
    else {
        if (gc_lazy && si->uses_gmem) gc_scan_pointers((const uint32_t *)(uintptr_t)r.addr, size);
        span = guest_copy(r.addr, size);
    }
    material_bound_projection(stage, k, span, size, r.addr, false);
    return ubo_span(span, size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

// Host memory [host, host + size) as a buffer (VK_EXT_external_memory_host; page-aligned). The allocation's result:
// out of memory tells whole_import_probe_oom to go lazy.
static VkResult host_import_ptr(void *host, uint64_t size, VkBuffer *out_buf, VkDeviceMemory *out_mem, bool bda) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &mp) != VK_SUCCESS || !mp.memoryTypeBits)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
    VkMemoryAllocateFlagsInfo fl = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = bda ? (void *)&fl : (void *)&imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) return r;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer buf;
    r = vkCreateBuffer(g_vk.dev, &bi, NULL, &buf);
    if (r != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return r; }
    r = vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (r != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, buf, NULL); vkFreeMemory(g_vk.dev, mem, NULL); return r; }
    *out_buf = buf;
    *out_mem = mem;
    return VK_SUCCESS;
}
// Heap bytes [off, off + size) as a buffer (a page-aligned range of the mapped heap).
static bool host_import(uint64_t off, uint64_t size, VkBuffer *out_buf, VkDeviceMemory *out_mem, bool bda) {
    return host_import_ptr((void *)(uintptr_t)(HEAP_BASE + off), size, out_buf, out_mem, bda) == VK_SUCCESS;
}

// One import of the whole heap, tried once at the start where it should fit but not by much (device.c
// whole_import_fits: g_vk.heap_probe): HEAP_MAX of scratch host memory imported and used by one submission, then
// freed. True: it ran out of memory (the import or the submission: issue #205's driver took the import and failed
// the first submission), and the heap is imported in lazy chunks instead - decided here, at device creation (vk_exec_init),
// before any shader is translated or the game starts. Costs: the import pins HEAP_MAX of RAM for a moment (well under a
// second). Any other failure: logged, and the heap is imported as one buffer as before.
// LARECOMP_VK_SIM_HEAP_OOM=1: the probe runs, then reports out of memory (tests the fallback on any GPU).
static bool whole_import_probe_oom(void) {
    const uint64_t size = HEAP_MAX, align = 64 << 10;  // (minImportedHostPointerAlignment: 4 KB here, 64 KB at most)
    const char *sim = getenv("LARECOMP_VK_SIM_HEAP_OOM");
    rec_drain();  // (the queue below is the recorder's: idle and empty while this runs)
    uint64_t t0 = plat_now_ns();
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    const char *step = "host memory";
    void *raw = malloc(size + align);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    if (raw) {
        step = "import";
        r = host_import_ptr((void *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1)), size, &buf, &mem, false);
    }
    if (r == VK_SUCCESS) {  // a submission that uses it: where the driver makes it resident
        step = "submission";
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkCommandBuffer cb = VK_NULL_HANDLE;
        r = vkCreateCommandPool(g_vk.dev, &pi, NULL, &pool);
        if (r == VK_SUCCESS) r = vkCreateFence(g_vk.dev, &fi, NULL, &fence);
        if (r == VK_SUCCESS) {
            VkCommandBufferAllocateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            r = vkAllocateCommandBuffers(g_vk.dev, &ci, &cb);
        }
        if (r == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkBeginCommandBuffer(cb, &bi);
            vkCmdFillBuffer(cb, buf, 0, 256, 0);         // (zeros into zeroed memory: the start and the end of it)
            vkCmdFillBuffer(cb, buf, size - 256, 256, 0);
            r = vkEndCommandBuffer(cb);
        }
        if (r == VK_SUCCESS) {
            VkCommandBufferSubmitInfo cbs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
            VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbs};
            r = vkQueueSubmit2(g_vk.queue, 1, &si, fence);
            if (r == VK_SUCCESS) r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 10000000000ull);
        }
    }
    if (r == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (pool) vkDestroyCommandPool(g_vk.dev, pool, NULL);
    if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
    if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
    free(raw);
    bool oom = r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY;
    if (r == VK_SUCCESS && sim && atoi(sim)) { oom = true; r = VK_ERROR_OUT_OF_DEVICE_MEMORY; step = "simulated (LARECOMP_VK_SIM_HEAP_OOM)"; }
    unsigned ms = (unsigned)((plat_now_ns() - t0) / 1000000);
    if (oom)
        LOG("[vk] guest heap: one import of %llu MB ran out of memory (%s, error %d, %u ms): falling back to chunks "
            "imported as the game binds them\n", (unsigned long long)(size >> 20), step, r, ms);
    else if (r != VK_SUCCESS)
        LOG("[vk] guest heap: one import of %llu MB failed to test (%s, error %d, %u ms): one buffer as before\n",
            (unsigned long long)(size >> 20), step, r, ms);
    else
        LOG("[vk] guest heap: one import of %llu MB works here (%u ms)\n", (unsigned long long)(size >> 20), ms);
    return oom;
}

static bool gmem_import(uint64_t size) {
    VkBuffer buf;
    VkDeviceMemory mem;
    if (!host_import(0, size, &buf, &mem, false)) return false;
    if (gmem_buf) vk_defer_buffer(gmem_buf, gmem_mem);  // the heap grew: the pending submission may still use the old import
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

// Linux, AMD (RADV): the import above is a userptr buffer, and amdgpu checks every page of a userptr buffer again on
// each submission - 3.2 GB of heap, ~45 ms of driver time a frame on a ROG Ally X, Steam Deck, Radeon laptops (issues
// #40, #51). Here the heap is instead allocated by the driver (cached system memory, exported as a dma-buf) and that
// memory is mapped at HEAP_BASE before the game starts, so the GPU sees an ordinary buffer. Called from boot (main.c)
// on the main thread before any guest thread exists; a reboot frees it with gmem_buf (vk_exec_reset) and the guest
// window's reset unmaps it, so the next boot gets fresh zeroed memory. Costs: the whole HEAP_MAX is committed once the
// GPU first uses it, and Linux write tracking can't watch this mapping (the heap is then re-checked like on Beta 4).
// Nothing writes the heap before boot (the start screen doesn't use guest memory): the mapping replaces it unseen.
// The mapping: where the driver has VK_EXT_map_memory_placed (RADV, Mesa 24.1+), it maps the memory at HEAP_BASE
// itself, through its own DRM file as for any vkMapMemory. Else the memory is exported as a dma-buf and that is
// mmapped there - which some systems refuse (issue #92: RX 6800 XT, Mesa 25.0.7, "Permission denied"), and then
// the whole heap was a userptr import again.
// Default on for RADV; LARECOMP_VK_HEAP_DMABUF=0 off, =1 on any Linux driver with dma-buf export or placed maps.
// LARECOMP_VK_HEAP_PLACED=0: the dma-buf mapping even where placed maps exist (tests).
#if defined(__linux__) && !defined(__ANDROID__)  // (phones: the guest heap stays their own memory, android.c)
static bool gmem_dmabuf;  // the heap is the driver's memory mapped at HEAP_BASE
static bool gmem_placed;  // ... by the driver (vkMapMemory2KHR, placed): unmapped through it at a reboot

// The driver maps `mem` (HEAP_MAX, one allocation) at HEAP_BASE. NULL, or why not; HEAP_BASE is ordinary guest
// memory again after a failure.
static const char *heap_map_placed(VkDeviceMemory mem) {
#ifndef VK_EXT_map_memory_placed
    (void)mem;
    return "built without VK_EXT_map_memory_placed";  // (g_vk.map_placed is never set then)
#else
    PFN_vkMapMemory2KHR map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkMapMemory2KHR");
    PFN_vkUnmapMemory2KHR unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkUnmapMemory2KHR");
    if (!map2 || !unmap2) return "no vkMapMemory2KHR";
    // First mapped anywhere, to know the driver can map it at all: a placed map replaces what's at HEAP_BASE before
    // the driver's own mmap can fail
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem};
    void *p = NULL;
    if (map2(g_vk.dev, &mi, &p) != VK_SUCCESS || !p) return "the driver can't map it";
    unmap2(g_vk.dev, &ui);
    VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)(uintptr_t)HEAP_BASE};
    mi.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
    mi.pNext = &pl;
    p = NULL;
    VkResult r = map2(g_vk.dev, &mi, &p);
    if (r == VK_SUCCESS && p == (void *)(uintptr_t)HEAP_BASE) return NULL;
    if (r == VK_SUCCESS) unmap2(g_vk.dev, &ui);
    // (the old mapping may be gone: plain zeroed memory again, registered for write tracking like a reboot's)
    if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + HEAP_MAX)) FATAL("cannot restore the guest heap mapping\n");
    return "the driver's placed mapping failed";
#endif
}

// The driver memory heap at a reboot (vk_exec_reset, GPU idle, guest threads stopped): the driver's mapping is left
// as a reserved range (or unmapped), then HEAP_BASE gets fresh zeroed guest memory at once, so no hole is left in the
// guest window until the window's reset.
static void heap_unmap_placed(void) {
    if (!gmem_placed) return;
    PFN_vkUnmapMemory2KHR unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkUnmapMemory2KHR");
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = gmem_mem,
                               .flags = 0};
#ifdef VK_EXT_map_memory_placed
    if (g_vk.unmap_reserve) ui.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
#endif
    if (unmap2) unmap2(g_vk.dev, &ui);
    gmem_placed = false;
}

static bool guest_heap_dmabuf(void) {
    if (gmem_buf || !g_vk.dev || !(g_vk.dmabuf || g_vk.map_placed) || getenv("LARECOMP_VK_NO_GMEM")) return false;
    const char *e = getenv("LARECOMP_VK_HEAP_DMABUF");
    if (e ? !atoi(e) : !strstr(g_vk.props.deviceName, "RADV")) return false;
    const uint64_t size = HEAP_MAX;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;  // (cached: the game's CPU work runs in it)
    int type = -1;
    uint64_t largest = 0;  // (the largest heap with a cached host-visible type, for the log when none is big enough)
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++) {
        if ((mp.memoryTypes[i].propertyFlags & want) != want) continue;
        uint64_t hs = mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size;
        if (hs >= size) type = (int)i;
        else if (hs > largest) largest = hs;
    }
    if (type < 0) {
        if (largest)  // (Steam Deck: a 3072 MB system heap)
            LOG("[vk] heap as driver memory: the driver's cached system memory heap (%llu MB) is smaller than the guest heap "
                "(%llu MB)\n", (unsigned long long)(largest >> 20), (unsigned long long)(size >> 20));
        else LOG("[vk] heap as driver memory: no cached host-visible memory type\n");
        return false;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    const char *pe = getenv("LARECOMP_VK_HEAP_PLACED");
    bool placed = g_vk.map_placed && (!pe || atoi(pe));
    if (!placed && !g_vk.dmabuf) { LOG("[vk] heap as driver memory: no way to map it here\n"); return false; }
    bi.pNext = placed ? NULL : &eb;  // (an exportable buffer only for the dma-buf mapping)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { LOG("[vk] heap as driver memory: buffer failed\n"); return false; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << type))) {
        LOG("[vk] heap as driver memory: memory type %d not allowed for the buffer\n", type);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return false;
    }
    if (req.size != size) {  // (the mapping must cover exactly the heap: guest memory follows it)
        LOG("[vk] heap as driver memory: the allocation would be %llu bytes, not %llu\n", (unsigned long long)req.size,
            (unsigned long long)size);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return false;
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf};
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = placed ? (void *)&ded : (void *)&ex,
                               .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) { LOG("[vk] heap as driver memory: allocation failed (%d)\n", r); vkDestroyBuffer(g_vk.dev, buf, NULL); return false; }
    const char *why = NULL;
    if (placed) {
        why = vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS ? "binding failed" : heap_map_placed(mem);
        if (why) {
            LOG("[vk] heap as driver memory: placed mapping at %llx failed (%s): the heap is shared as before\n",
                (unsigned long long)HEAP_BASE, why);
            vkDestroyBuffer(g_vk.dev, buf, NULL);
            vkFreeMemory(g_vk.dev, mem, NULL);
            return false;
        }
        gmem_placed = true;
        goto mapped;
    }
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if (!get_fd || get_fd(g_vk.dev, &fi, &fd) != VK_SUCCESS || fd < 0) why = "no dma-buf for it";
    // First mapped anywhere, to know the driver lets it be mapped at all: MAP_FIXED unmaps what's at HEAP_BASE before
    // the driver is asked, so a refusal there would leave the heap unmapped (review of 2c27f3d)
    if (!why) {
        void *probe = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (probe == MAP_FAILED) why = strerror(errno);
        else munmap(probe, size);
        if (probe == MAP_FAILED) LOG("[vk] heap as driver memory: the dma-buf can't be mapped (%s)\n", why);
    }
    if (!why && mmap((void *)(uintptr_t)HEAP_BASE, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) !=
                    (void *)(uintptr_t)HEAP_BASE) {
        why = strerror(errno);
        // (the old mapping is gone: plain zeroed memory again, registered for write tracking like a reboot's)
        if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + size)) FATAL("cannot restore the guest heap mapping\n");
    }
    if (fd >= 0) close(fd);  // (the mapping keeps the dma-buf)
    VkResult br = why ? VK_SUCCESS : vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (!why && br != VK_SUCCESS) {
        why = "binding failed";
        if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + size)) FATAL("cannot restore the guest heap mapping\n");
    }
    if (why) {
        LOG("[vk] heap as driver memory: mapping at %llx failed (%s): the heap is shared as before\n",
            (unsigned long long)HEAP_BASE, why);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        vkFreeMemory(g_vk.dev, mem, NULL);
        return false;
    }
mapped:;
    // the game expects a zeroed heap; the kernel hands out zeroed pages, but 8 spread pages are checked to be sure
    static const uint8_t zero_page[4096];
    for (int i = 0; i < 8; i++)
        if (memcmp((const void *)(uintptr_t)(HEAP_BASE + size / 8 * i), zero_page, sizeof zero_page)) {
            LOG("[vk] heap as driver memory: not zeroed, clearing it\n");
            memset((void *)(uintptr_t)HEAP_BASE, 0, size);
            break;
        }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    gmem_dmabuf = true;
    LOG("[vk] guest heap as driver memory (%s, memory type %d): %llu MB at %llx - all of it is held in system memory "
        "once the GPU first uses it\n", gmem_placed ? "placed map" : "dma-buf", type, (unsigned long long)(size >> 20), (unsigned long long)HEAP_BASE);
    return true;
}
#endif

// The guest heap's GPU memory on Linux: the driver's own (above) where it can be, else host-pointer imports - the
// whole heap (gmem_import) or 16 MB chunks as draws first bind them (gc_lazy: RADV without the dma-buf heap, Steam
// Deck; GPUs whose storage buffers can't span the heap). On Mesa's drivers every such import is a userptr buffer, and
// page write tracking (platform/linux.c: userfaultfd write protection, PAGEMAP_SCAN) changes the protection of pages
// inside them: each tracking query over vertex or texture memory is an MMU-notifier invalidation of the imports it
// overlaps, so the kernel driver waits for the GPU to finish with them and checks their pages again at the next
// submission (amdgpu; the CPU and GPU stop overlapping - Steam Deck, issue #181: 30 fps on Beta 4, 20 with tracking
// over the whole-heap import). Chunks only make the re-check smaller, not the wait. So with any userptr import,
// tracking is off, as on Beta 4 (the vertex cache and the texture checks go without it). NVIDIA's own driver pins
// the pages instead and keeps it. Decided here, at boot, before any query protected a page. LARECOMP_WRITE_TRACKING=1:
// on anyway.
static bool gmem_userptr;  // the heap is imported by host pointer: userptr (vertex cache default, vcache_lookup)
// The whole heap as one userptr import (not chunks) on AMD: each submission has amdgpu check all of its pages
static bool heap_whole_userptr(void) { return gmem_userptr && !gc_lazy && g_vk.props.vendorID == 0x1002; }

void backend_guest_heap_prepare(void) {
    gmem_userptr = false;
#if defined(__linux__) && !defined(__ANDROID__)
    guest_heap_dmabuf();
#endif
    if (!g_vk.dev) return;
    gmem_chunk_shift();  // (the chunk / lazy decision, before the first draw binds anything)
#if defined(__linux__) && !defined(__ANDROID__)
    // driver memory: write tracking can't watch that mapping (its queries over the heap would fail; PAGEMAP_SCAN
    // refuses a range without userfaultfd write protection), so it's off there too
    if (gmem_dmabuf) { plat_write_tracking_off("the guest heap is driver memory"); return; }
    if (!g_vk.host_import || getenv("LARECOMP_VK_NO_GMEM") || g_vk.props.vendorID == 0x10DE) return;
    gmem_userptr = true;
    plat_write_tracking_off(gc_lazy ? "the guest heap is imported in userptr chunks" : "the guest heap is a userptr import");
#endif
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import || gc_lazy;  // (lazy: chunks instead)
    uint64_t want = gmem_heap_size();
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];  // (fresh each time, not zero_buffer: a shader may store to it)
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Read-only equivalent of LAS Android's mirror_table: pack the bound ranges, not the entire
// heap, into the existing submission-owned ring. No GPU writes/readbacks are implemented here.
static void readonly_mirror_flush_span(VkSpan span, uint64_t bytes) {
    for (int i = 0; i < nchunks; ++i)
        if (chunks[i].buf == span.buf) {
            VK_CHECK(vmaFlushAllocation(g_vk.vma, chunks[i].mem, span.off, bytes));
            return;
        }
    FATAL("read-only shader mirror: snapshot is not in the submission ring\n");
}

static VkDescriptorBufferInfo readonly_mirror_info(int stage, uint32_t table[GMAP_MAX + 1][4]) {
    Range ranges[GMAP_MAX];
    int n = 0;
    memset(table, 0, sizeof(uint32_t) * (GMAP_MAX + 1) * 4);
    for (int i = 0; i < (stage == STAGE_COMPUTE ? 32 : 16); ++i) {
        Range r = i < 16 ? st.sbuf[stage][i] : st.ubuf[stage][i - 16];
        if (!r.addr || !r.size) continue;
        if (r.addr < HEAP_BASE || r.addr - HEAP_BASE >= HEAP_MAX || r.size > HEAP_MAX - (r.addr - HEAP_BASE))
            FATAL("read-only shader mirror: stage %d range %llx+%llx outside guest heap\n", stage,
                  (unsigned long long)r.addr, (unsigned long long)r.size);
        uint64_t end = (r.addr + r.size + 3) & ~3ull;
        r.addr &= ~3ull;
        r.size = end - r.addr;
        int at = n++;
        while (at && ranges[at - 1].addr > r.addr) { ranges[at] = ranges[at - 1]; --at; }
        ranges[at] = r;
    }
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (count && ranges[i].addr <= ranges[count - 1].addr + ranges[count - 1].size) {
            uint64_t end = ranges[i].addr + ranges[i].size;
            if (end > ranges[count - 1].addr + ranges[count - 1].size)
                ranges[count - 1].size = end - ranges[count - 1].addr;
        } else ranges[count++] = ranges[i];
    }
    uint64_t bytes = 0, limit = g_vk.props.limits.maxStorageBufferRange;
    if (limit > (16ull << 20)) limit = 16ull << 20;
    for (int i = 0; i < count; ++i) {
        if (ranges[i].size > limit || bytes > limit - ranges[i].size)
            FATAL("read-only shader mirror exceeds the bounded storage snapshot (%llu bytes)\n", (unsigned long long)limit);
        bytes += ranges[i].size;
    }
    if (limit < 16) FATAL("read-only shader mirror: storage buffer range limit is too small\n");
    uint64_t align = g_vk.props.limits.minStorageBufferOffsetAlignment;
    VkSpan span = vk_transient(bytes ? bytes : 16, align > 16 ? align : 16);
    uint64_t offset = 0;
    for (int i = 0; i < count; ++i) {
        memcpy((uint8_t *)span.ptr + offset, (const void *)(uintptr_t)ranges[i].addr, ranges[i].size);
        table[i][0] = (uint32_t)(ranges[i].addr - HEAP_BASE);
        table[i][1] = (uint32_t)ranges[i].size;
        table[i][2] = (uint32_t)(offset >> 2);
        offset += ranges[i].size;
    }
    if (!bytes) memset(span.ptr, 0, 16);
    table[GMAP_MAX][0] = (uint32_t)count;
    // Unlike the desktop heap import, this is a host-written ring allocation; flush if noncoherent.
    readonly_mirror_flush_span(span, bytes ? bytes : 16);
    static int logged[6];
    if (logged[stage]++ < 4 || g_debug_frame)
        LOG("[vk] read-only shader mirror stage %d: %d ranges, %llu bytes, storage alignment %llu\n",
            stage, count, (unsigned long long)bytes, (unsigned long long)align);
    return (VkDescriptorBufferInfo){span.buf, span.off, bytes ? bytes : 16};
}

// ---- the guest heap for shaders in chunks (shader.h maxwell_gmem_chunk_shift)
// Chunks for two reasons:
// - a GPU whose storage buffers can't span HEAP_MAX (maxStorageBufferRange: 1 GB on Intel): binding one range past
//   that limit is out of spec, and the UHD 630's driver then read garbage beyond 1 GB: dark lighting, black objects
//   (issue #8). Lazy chunks as below (device.c lazy_heap_predicted); where they aren't possible (or with
//   LARECOMP_VK_GMEM_LAZY=0), windows of the one imported heap, one binding each;
// - lazy import (gmem_lazy): on Linux/AMD the import is a userptr buffer and amdgpu checks every page of such a buffer
//   again on each submission - 3.2 GB of heap, ~45 ms of driver time a frame (issues #40, #51). The dma-buf heap
//   (guest_heap_dmabuf) avoids that, but needs a cached host-visible memory type that can hold HEAP_MAX, and APUs
//   have none (Steam Deck: a 3072 MB system heap; Ryzen 3200G: 3172 MB, so the 3220 MB import is also over its GTT
//   budget: issues #40, #92). The GPU itself reads only a little of the heap in place: 73 MB over a four-area tour
//   (vertex and index data 67 MB, storage buffers 9 MB), 15-28 MB a frame, scattered over the whole 3.25 GB (measured
//   on the Metal backend, which binds the same ranges). So each 16 MB chunk is imported on its own when a draw or
//   dispatch first binds a range in it (23 chunks, 368 MB, over that tour), and only the imported chunks exist for the
//   driver to check: a fraction of the pages, under the GTT budget, and still the game's own memory (nothing to copy
//   back). Vertex and index data bind their chunk directly; shaders (LDG/STG) take the chunk's device address and
//   size from a table (GChunks, rewritten when a chunk is imported) and go through a buffer reference after a bounds
//   check: a chunk not imported has size 0, the access reads 0 / drops the store, and the shader counts it in GMiss
//   with the lowest word index hit: such a chunk (a pointer the game keeps in a uniform buffer, which no bind told us
//   about) is imported for the next frame, and the count is in the stats (after a chunk's first use it should stay 0).
//   Buffer device addresses are enabled only where this mode will be used (device.c lazy_heap_predicted): on RADV
//   they put every allocation on the driver's global BO list.
//   Default on RADV when the dma-buf heap doesn't apply, wherever one storage buffer can't span the heap and where the
//   system memory heap (or its budget) can't hold one import of it (8 GB laptops: issue #205), and where one import
//   was tried at the start and ran out of memory (whole_import_probe_oom; LARECOMP_VK_SIM_HEAP_OOM=1 simulates it);
//   LARECOMP_VK_GMEM_LAZY=1 anywhere (tests), =0 off.
// LARECOMP_VK_GMEM_CHUNK_MB=<n> forces chunks of n MB (a power of two, 1 GB at most), =0 one buffer.
#define GC_MAX GMEM_CHUNKS_MAX
static struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t size, addr;  // (addr: the chunk's device address, lazy mode)
} gc[GC_MAX];
static int gc_n;             // chunks over HEAP_MAX
static uint32_t gc_shift;    // 0: one buffer
static bool gc_lazy;         // imports per chunk (else windows of gmem_buf)
static VkBuffer gc_dummy, gc_miss_buf;
static VmaAllocation gc_dummy_mem, gc_miss_mem;
static uint32_t *gc_miss;    // GMiss, host-visible: per chunk, accesses that found no chunk, then the lowest word index
static VkDescriptorSetLayout gc_layout;
static VkDescriptorSet gc_set;
static uint64_t gc_set_seq;
static uint32_t gc_gen, gc_set_gen;  // (gen: bumped when a chunk is imported; the set is rebuilt per submission and gen)
static int gc_scanned;                // chunks imported for a pointer found in a uniform buffer
static uint8_t gc_remembered[GC_MAX]; // chunks shaders reached without a bind (a miss, a pointer): imported again after a reboot
                                      // and, through cache/gmem_chunks.txt, at the next start (a machine misses a chunk once)
static bool gc_file_loaded;
static uint64_t gc_remembered_heap;   // the heap size the remembered chunks were imported at this boot (0: not yet)

// cache/gmem_chunks.txt: one heap byte offset (hex) per line of a chunk shaders reached without a bind. Offsets, not
// chunk numbers: the chunk size depends on the GPU. Loaded once, into gc_remembered; written whenever one is added.
static void gc_remembered_load(void) {
    if (gc_file_loaded) return;
    gc_file_loaded = true;
    FILE *f = fopen("cache/gmem_chunks.txt", "r");
    if (!f) return;
    char line[64];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long off = strtoull(line, NULL, 16);
        int k = (int)(off >> gc_shift);
        if (k >= 0 && k < gc_n && !gc_remembered[k]) { gc_remembered[k] = 1; n++; }
    }
    fclose(f);
    if (n) LOG("[vk] guest heap: %d chunks shaders reached without a bind last time (cache/gmem_chunks.txt): imported at the start\n", n);
}
static void gc_remembered_save(void) {
    mkdir("cache", 0755);
    FILE *f = fopen("cache/gmem_chunks.txt.tmp", "w");
    if (!f) return;
    for (int k = 0; k < gc_n; k++)
        if (gc_remembered[k]) fprintf(f, "%llx\n", (unsigned long long)((uint64_t)k << gc_shift));
    fclose(f);
    remove("cache/gmem_chunks.txt");
    rename("cache/gmem_chunks.txt.tmp", "cache/gmem_chunks.txt");
}
static void gc_remember(int k) {
    if (gc_remembered[k]) return;
    gc_remembered[k] = 1;
    gc_remembered_save();
}
static int gc_imported;
static uint64_t gc_imported_bytes, stat_gc_misses;

static bool gmem_lazy(void) {
    static int lazy = -1;
    if (lazy >= 0) return lazy;
    lazy = g_vk.lazy_heap;
    if (!lazy && g_vk.heap_probe && !getenv("LARECOMP_VK_NO_GMEM")) lazy = whole_import_probe_oom();
    if (lazy && (!g_vk.host_import || !g_vk.bda || !g_vk.ssbo_chunks)) {
        LOG("[vk] lazy heap import: not possible here (%s)\n", !g_vk.host_import ? "no host memory import" : !g_vk.bda ? "no buffer device addresses" : "no stores from fragment / vertex shaders");
        lazy = 0;
    }
    return lazy;
}

// Decided once (pthread_once): the render thread's draws, the translation and pipeline workers and the boot
// (backend_guest_heap_prepare) all ask, and a second caller must not see a half-made decision (shift is 0 until the
// fit loop below ends - a shader translated then would use one buffer while draws bind chunks).
static int gc_shift_decided = -1;
static void gmem_chunk_decide(void);
static pthread_once_t gc_decide_once = PTHREAD_ONCE_INIT;
static uint32_t gmem_chunk_shift(void) {
    pthread_once(&gc_decide_once, gmem_chunk_decide);
    return (uint32_t)gc_shift_decided;
}
static void gmem_chunk_decide(void) {
    if (maxwell_gmem_readonly) {
        gc_shift = gc_n = 0;
        gc_lazy = false;
        maxwell_gmem_lazy = maxwell_gmem_fs_miss = false;
        gc_shift_decided = 0;
        return; // No imported heap windows or buffer-device-address path.
    }
    int shift;
    const char *e = getenv("LARECOMP_VK_GMEM_CHUNK_MB");
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    gc_lazy = gmem_lazy();
    shift = 0;
    if (e) { uint64_t b = strtoull(e, NULL, 10) << 20; if (b) shift = 63 - __builtin_clzll(b); }
    else if (gc_lazy) shift = 24;
    // (no heap import - phones: shaders reach the heap through the address table and the mirror, mirror_table; there
    // is no heap buffer to cut into windows)
    else if (HEAP_MAX > lim && lim && !vk_gmem_mirror()) shift = 63 - __builtin_clzll(lim);
    if (shift > 30) shift = 30;  // (1 GB at most: NVIDIA computes a block's length in signed 32 bits, a 2 GB range reads as negative)
    // The table holds GC_MAX chunks at most, and a stage's storage buffers (the windows, gfast, binding 17) its limit
    // (Intel: 200): a chunk size that doesn't fit grows until it does (4 MB asked for -> 16 MB). One buffer only where
    // one buffer is within maxStorageBufferRange.
    int asked = shift, n = 0;
    uint32_t per_stage = g_vk.props.limits.maxPerStageDescriptorStorageBuffers, per_set = g_vk.props.limits.maxDescriptorSetStorageBuffers;
    if (shift && gc_lazy && !g_vk.ssbo_chunks) {
        LOG("[vk] guest heap chunks: not possible here (no stores from fragment / vertex shaders): one buffer\n");
        shift = 0;
    }
    while (shift) {
        n = (int)((HEAP_MAX + (1ull << shift) - 1) >> shift);
        bool fits = n <= GC_MAX && (gc_lazy || ((uint32_t)n + 8 <= per_stage && (uint32_t)n + 1 <= per_set));
        if (fits) break;
        if ((1ull << shift) >= HEAP_MAX) { shift = 0; break; }  // (not even one chunk fits: one buffer)
        shift++;
    }
    if (shift != asked && asked)
        LOG("[vk] guest heap chunks of %llu MB don't fit here (%d chunks at most, %u storage buffers a stage, %u a set): %s\n",
            (unsigned long long)(1ull << asked) >> 20, GC_MAX, per_stage, per_set, shift ? "larger chunks" : "one buffer");
    if (!shift && HEAP_MAX > lim && lim && !vk_gmem_mirror()) LOG("[vk] warning: one storage buffer over the heap exceeds this GPU's %llu MB range\n", (unsigned long long)lim >> 20);
    if (!shift) gc_lazy = false;
    gc_shift = (uint32_t)shift;
    gc_n = shift ? n : 0;
    maxwell_gmem_lazy = gc_lazy;
    maxwell_gmem_fs_miss = gc_lazy && getenv("LARECOMP_VK_GMEM_FS_MISS") && atoi(getenv("LARECOMP_VK_GMEM_FS_MISS")) != 0;
    if (maxwell_gmem_fs_miss) LOG("[vk] lazy chunks: every fragment shader counts misses, no early tests (LARECOMP_VK_GMEM_FS_MISS)\n");
    if (shift) LOG("[vk] guest heap bound to shaders in %llu MB %s (storage buffers up to %llu MB here)\n",
                   (unsigned long long)(1ull << shift) >> 20, gc_lazy ? "chunks, imported as the game binds them" : "windows", (unsigned long long)lim >> 20);
    gc_shift_decided = shift;
}

// The heap mapped so far, as the imports see it (SetHeapSize; pages beyond it don't exist yet).
static uint64_t gmem_heap_size(void) {
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    return want > HEAP_MAX ? HEAP_MAX : want;
}

static bool host_import(uint64_t off, uint64_t size, VkBuffer *buf, VkDeviceMemory *mem, bool bda);

// Lazy mode: chunk k imported (again, if the heap grew into it since). False: not mapped yet / the import failed.
static bool gc_import(int k) {
    uint64_t off = (uint64_t)k << gc_shift, heap = gmem_heap_size();
    if (k < 0 || k >= gc_n || off >= heap) return false;
    uint64_t size = heap - off < (1ull << gc_shift) ? heap - off : 1ull << gc_shift;
    if (gc[k].buf && gc[k].size >= size) return true;
    VkBuffer buf;
    VkDeviceMemory mem;
    if (!host_import(off, size, &buf, &mem, true)) {
        static int warned;
        if (warned++ < 8) LOG("[vk] guest heap chunk %d (%llu MB at +%llu MB): import failed\n", k, (unsigned long long)size >> 20, (unsigned long long)off >> 20);
        return false;
    }
    if (gc[k].buf) {  // (the heap grew into the chunk: the pending submission may still use the old import)
        vk_defer_buffer(gc[k].buf, gc[k].mem);
        gc_imported_bytes -= gc[k].size;
    } else {
        gc_imported++;
    }
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    gc[k].buf = buf; gc[k].mem = mem; gc[k].size = size;
    gc[k].addr = vkGetBufferDeviceAddress(g_vk.dev, &ai);
    gc_imported_bytes += size;
    gc_gen++;
    return true;
}

// Pointers the game keeps in its uniform buffers (the lighting pass's light list, the grass's instance tables): the
// chunk a shader reaches that way has no storage buffer bound over it, so it is imported from the uniform buffer's
// contents - which the per-draw copy reads anyway - before the draw. Without this the first frame in such a chunk
// read 0 (chunk 47 at +752 MB at every scene entry, chunk 50 at the first grass cut: 576 misses over a tour). An
// 8-byte-aligned word pair that happens to look like a heap address imports a chunk for nothing: bounded, and logged.
#define GC_SCAN_MAX 48
static int gc_scan_warned;
static void gc_scan_pointers(const uint32_t *w, uint64_t bytes) {
    uint64_t heap = gmem_heap_size();
    prof_scan_calls++;
    prof_scan_bytes += bytes;
    for (uint64_t i = 0; i + 1 < bytes / 4; i += 2) {
        uint64_t v = (uint64_t)w[i] | (uint64_t)w[i + 1] << 32;
        if (v < HEAP_BASE || v - HEAP_BASE >= heap) continue;
        int k = (int)((v - HEAP_BASE) >> gc_shift);
        if (k >= gc_n || gc[k].buf) continue;
        if (gc_scanned >= GC_SCAN_MAX) {
            if (!gc_scan_warned++) LOG("[vk] guest heap: %d chunks imported for pointers in uniform buffers: no more\n", gc_scanned);
            return;
        }
        if (gc_import(k)) {
            gc_scanned++;
            gc_remember(k);
            LOG("[vk] guest heap chunk %d (+%llu MB): a pointer in a uniform buffer (%llx): imported\n", k,
                (unsigned long long)((uint64_t)k << gc_shift) >> 20, (unsigned long long)v);
        }
    }
}

// Whether a pointer scan can still import anything: some chunk of the heap mapped so far isn't imported (and the
// scan's import limit isn't used up). Once every chunk is in, the scans are skipped (guest_copy_ex).
static bool gc_scan_needed(void) {
    static uint64_t c_heap;
    static uint32_t c_gen;
    static int c_scanned = -1, c_warned;
    static bool c_need = true;
    uint64_t heap = gmem_heap_size();
    if (heap != c_heap || gc_gen != c_gen || gc_scanned != c_scanned || gc_scan_warned != c_warned) {
        c_heap = heap; c_gen = gc_gen; c_scanned = gc_scanned; c_warned = gc_scan_warned;
        c_need = false;
        if (gc_scanned < GC_SCAN_MAX || !gc_scan_warned) {
            int n = (int)((heap + (1ull << gc_shift) - 1) >> gc_shift);
            if (n > gc_n) n = gc_n;
            for (int k = 0; k < n && !c_need; k++) c_need = !gc[k].buf;
        }
    }
    return c_need;
}

// A heap range a draw or dispatch binds: its chunks get imported (lazy mode), and the chunk after it as well - the
// game's storage buffers are bound with sizes the shaders read past (the lighting pass's tile list: the misses sat
// 1-9 MB into the chunk after the bound range), and a size of 0 means "the rest of the buffer".
static void gc_touch(uint64_t off, uint64_t size) {
    if (!gc_lazy || off >= HEAP_MAX) return;
    int k0 = (int)(off >> gc_shift), k1 = (int)((off + (size ? size : 1) - 1) >> gc_shift) + 1;
    if (k1 >= gc_n) k1 = gc_n - 1;
    for (int k = k0; k <= k1; k++) if (!gc[k].buf) gc_import(k);
}

// The chunk set's layout: lazy mode the table (GChunks), GMiss and the draw's chunks (GDir<k>); windows one storage
// binding each (shader.h).
// (once: pipeline layouts are made on the build workers, sets on the render thread)
static pthread_once_t gc_layout_once = PTHREAD_ONCE_INIT;
static void gmem_chunk_layout_make(void);
static VkDescriptorSetLayout gmem_chunk_layout(void) {
    pthread_once(&gc_layout_once, gmem_chunk_layout_make);
    return gc_layout;
}
static void gmem_chunk_layout_make(void) {
    gmem_chunk_shift();  // (gc_lazy / gc_n decided)
    VkShaderStageFlags st_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    static VkDescriptorSetLayoutBinding lb[GC_MAX];
    uint32_t nb = 0;
    if (gc_lazy) {
        lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_CHUNK_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st_, NULL};
        lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_MISS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
        for (int k = 0; k < GMEM_DIRECT_MAX; k++)
            lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_DIRECT_BINDING + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
    } else {
        for (int k = 0; k < gc_n; k++) lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_CHUNK_BINDING + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
    }
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = lb};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &gc_layout));
}

// The chunk set for the pending submission: the table of imported chunks, GMiss and the draw's chunks dk[k] (-1: none)
// as GDir<k> (lazy), or the windows. Lazy sets are kept per submission and chunk generation, keyed by dk: a frame has a
// handful of storage buffer chunk combinations, so a draw binds a set it already has (no allocation, no update).
#define GC_SET_CACHE 32
static uint64_t gc_direct_bytes(int k) {  // (GDir<k>'s range: the chunk, within the storage buffer range limit)
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    return lim && gc[k].size > lim ? lim & ~3ull : gc[k].size;
}
static VkDescriptorSet gmem_chunk_set(const int dk[GMEM_DIRECT_MAX]) {
    if (!gc_dummy) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gc_dummy, &gc_dummy_mem, &info));
        memset(info.pMappedData, 0, 4096);
        bi.size = sizeof(uint32_t) * 2 * GC_MAX;  // n[GC_MAX] accesses, w[GC_MAX] lowest word index hit
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gc_miss_buf, &gc_miss_mem, &info));
        gc_miss = info.pMappedData;
        memset(gc_miss, 0, sizeof(uint32_t) * GC_MAX);
        memset(gc_miss + GC_MAX, 0xFF, sizeof(uint32_t) * GC_MAX);
    }
    uint64_t seq = vk_pending_seq();
    // A boot, or a reboot: the chunks shaders reached without a bind last time (this process, or cache/gmem_chunks.txt),
    // before they miss again. Once a boot per heap size (a chunk beyond the heap mapped so far comes when it grows) -
    // not "while nothing is imported": copies and storage buffers import chunks before the first set is built.
    if (gc_lazy && gc_remembered_heap != gmem_heap_size()) {
        gc_remembered_heap = gmem_heap_size();
        gc_remembered_load();
        for (int k = 0; k < gc_n; k++) if (gc_remembered[k] && !gc[k].buf) gc_import(k);
    }
    static struct { VkDescriptorSet set; int dk[GMEM_DIRECT_MAX]; } cache[GC_SET_CACHE];
    static int ncache, next;
    static VkSpan tab_span;  // (the table for this submission and generation, shared by its sets)
    // (windows: also when the heap was imported again since - guest_direct / gfast_prepare re-import as it grows
    // mid-submission, and a set still on the old import read zeros past its end for the rest of the submission)
    static VkBuffer gc_set_heap;
    if (gc_set_seq != seq || gc_set_gen != gc_gen || (!gc_lazy && gc_set_heap != gmem_buf)) { ncache = 0; next = 0; gc_set = VK_NULL_HANDLE; }
    if (!gc_lazy) {
        if (gc_set) return gc_set;
    } else {
        for (int j = 0; j < ncache; j++)
            if (!memcmp(cache[j].dk, dk, sizeof cache[j].dk)) return cache[j].set;
    }
    static VkDescriptorBufferInfo bi[GC_MAX + 2 + GMEM_DIRECT_MAX];
    static VkWriteDescriptorSet w[GC_MAX + 2 + GMEM_DIRECT_MAX];
    VkDescriptorSet set = vk_alloc_set(gmem_chunk_layout());
    uint32_t nw = 0;
    if (gc_lazy) {  // GChunks: uvec4 per chunk = device address lo, hi, words (0: not imported), 0
        if (!ncache) {
            static uint32_t tab[GC_MAX * 4];
            for (int k = 0; k < gc_n; k++) {
                tab[k * 4 + 0] = (uint32_t)gc[k].addr; tab[k * 4 + 1] = (uint32_t)(gc[k].addr >> 32);
                tab[k * 4 + 2] = gc[k].buf ? (uint32_t)(gc[k].size >> 2) : 0; tab[k * 4 + 3] = 0;
            }
            tab_span = vk_transient_copy(tab, sizeof tab, 256);
        }
        bi[0] = (VkDescriptorBufferInfo){tab_span.buf, tab_span.off, sizeof(uint32_t) * GC_MAX * 4};
        bi[1] = (VkDescriptorBufferInfo){gc_miss_buf, 0, sizeof(uint32_t) * 2 * GC_MAX};
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_CHUNK_BINDING,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0]};
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_MISS_BINDING,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]};
        for (int j = 0; j < GMEM_DIRECT_MAX; j++) {  // (GDir<j>: the chunk's own import, whole; unused: the zero buffer)
            int k = dk[j];
            bi[2 + j] = k >= 0 && k < gc_n && gc[k].buf ? (VkDescriptorBufferInfo){gc[k].buf, 0, gc_direct_bytes(k)}
                                                       : (VkDescriptorBufferInfo){gc_dummy, 0, 4096};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_DIRECT_BINDING + j,
                                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[2 + j]};
        }
        int slot = ncache < GC_SET_CACHE ? ncache++ : (next++ % GC_SET_CACHE);
        cache[slot].set = set;
        memcpy(cache[slot].dk, dk, sizeof cache[slot].dk);
    } else {  // windows of the one import; one past it is the dummy (reads 0, as the zero buffer before the import)
        VkDescriptorBufferInfo all = gmem_info();
        for (int k = 0; k < gc_n; k++) {
            uint64_t off = (uint64_t)k << gc_shift;
            if (all.buffer == gmem_buf && gmem_buf && off < gmem_size)
                bi[k] = (VkDescriptorBufferInfo){gmem_buf, off, gmem_size - off < (1ull << gc_shift) ? gmem_size - off : 1ull << gc_shift};
            else bi[k] = (VkDescriptorBufferInfo){gc_dummy, 0, 4096};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_CHUNK_BINDING + k,
                                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[k]};
        }
    }
    rec_UpdateDescriptorSets(nw, w);
    gc_set = set; gc_set_seq = seq; gc_set_gen = gc_gen; gc_set_heap = gmem_buf;
    return set;
}

// Lazy mode, a draw: the imported chunks under its vertex / fragment storage buffers, in slot order (shader.h
// GMEM_DIRECT_MAX): gd[] for the shaders' GDirect push constants (the range each GDir<k> covers), dk[] the chunks for
// the set's GDir<k> bindings. A range over two chunks gives both. Accesses there are storage buffer loads, not the table.
// LARECOMP_VK_GMEM_DIRECT=0: none (every access through the table, as before; no shader change).
static void gmem_direct_pick(uint32_t stages, uint32_t gd[GMEM_DIRECT_MAX * 4], int dk[GMEM_DIRECT_MAX]) {
    static int on = -1;
    if (on < 0) {
        on = !getenv("LARECOMP_VK_GMEM_DIRECT") || atoi(getenv("LARECOMP_VK_GMEM_DIRECT")) != 0;
        if (!on) LOG("[vk] lazy chunks: storage buffer chunks through the table too (LARECOMP_VK_GMEM_DIRECT=0)\n");
    }
    memset(gd, 0, sizeof(uint32_t) * GMEM_DIRECT_MAX * 4);
    for (int j = 0; j < GMEM_DIRECT_MAX; j++) dk[j] = -1;
    int n = 0;
    for (int stage = 0; on && stage < 2; stage++) {
        if (!(stages & (1u << stage))) continue;
        for (int i = 0; i < 16 && n < GMEM_DIRECT_MAX; i++) {
            uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
            if (a < HEAP_BASE || a - HEAP_BASE >= HEAP_MAX) continue;
            uint64_t off = a - HEAP_BASE;
            int k0 = (int)(off >> gc_shift), k1 = (int)((off + (sz ? sz : 1) - 1) >> gc_shift);
            for (int k = k0; k <= k1 && k < gc_n && n < GMEM_DIRECT_MAX; k++) {
                if (!gc[k].buf) continue;
                int j = 0;
                while (j < n && dk[j] != k) j++;
                if (j < n) continue;
                dk[n] = k;
                gd[n * 4 + 0] = (uint32_t)(((uint64_t)k << gc_shift) >> 2);
                gd[n * 4 + 1] = (uint32_t)(gc_direct_bytes(k) >> 2);
                gd[n * 4 + 2] = (uint32_t)gc[k].addr;
                gd[n * 4 + 3] = (uint32_t)(gc[k].addr >> 32);
                n++;
            }
        }
    }
}

// Binds the chunk set for a draw / dispatch whose shaders (stages: a mask of 1 << stage) use global memory: their storage
// buffers' chunks first.
static void gmem_chunk_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, uint32_t stages) {
    static int log_sb = -1;  // LARECOMP_LOG_GMEM_SBUF=1: each distinct storage buffer bound to a gmem shader, once
    if (log_sb < 0) log_sb = getenv("LARECOMP_LOG_GMEM_SBUF") != NULL;
    for (int stage = 0; stage < 6; stage++) {
        if (!(stages & (1u << stage))) continue;
        for (int i = 0; i < 16; i++) {
            uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
            if (a < HEAP_BASE || a - HEAP_BASE >= HEAP_MAX) continue;
            if (log_sb) {
                static uint64_t seen[64]; static int nseen;
                uint64_t key = a ^ (sz << 40) ^ ((uint64_t)(stage * 16 + i) << 56);
                int j = 0; while (j < nseen && seen[j] != key) j++;
                if (j == nseen && nseen < 64) { seen[nseen++] = key; LOG("[vk] gmem sbuf stage %d slot %d: %llx + %llx (chunk %d)\n", stage, i, (unsigned long long)a, (unsigned long long)sz, (int)((a - HEAP_BASE) >> gc_shift)); }
            }
            gc_touch(a - HEAP_BASE, sz);
        }
    }
    uint32_t gd[GMEM_DIRECT_MAX * 4];
    int dk[GMEM_DIRECT_MAX] = {-1, -1, -1, -1};
    bool direct = gc_lazy && bp == VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (direct) gmem_direct_pick(stages, gd, dk);
    VkDescriptorSet set = gmem_chunk_set(dk);
    rec_CmdBindDescriptorSets(cb, bp, layout, set_index, 1, &set, 0, NULL);
    if (direct) rec_CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof gd, gd);
}

// Once a frame (the present's submission): a chunk shaders missed is imported for the next frame; the lowest address
// that missed is logged (a pointer the game keeps in a uniform buffer, which no bind told us about).
static void gc_frame(void) {
    if (!gc_shift || !gc_miss || !gc_lazy) return;
    for (int k = 0; k < gc_n; k++) {
        uint32_t m = gc_miss[k], w = gc_miss[GC_MAX + k];
        if (!m) continue;
        gc_miss[k] = 0;
        gc_miss[GC_MAX + k] = 0xFFFFFFFFu;
        stat_gc_misses += m;
        static int nlog;
        if (nlog++ < 40) LOG("[vk] guest heap chunk %d (+%llu MB): %u shader accesses before its import, lowest %llx: imported for the next frame\n", k,
                             (unsigned long long)((uint64_t)k << gc_shift) >> 20, m, (unsigned long long)HEAP_BASE + ((uint64_t)w << 2));
        gc_touch((uint64_t)k << gc_shift, 1);
        gc_remember(k);
    }
}

// In lazy mode the heap-side offsets of a copy are in chunks: one vkCmdCopyBuffer per chunk piece. other = the GPU side.
static void gmem_copy(VkCommandBuffer cb, VkBuffer other, const VkBufferCopy *cp, int n, bool to_heap) {
    if (!gc_lazy) {
        if (to_heap) rec_CmdCopyBuffer(cb, other, gmem_buf, (uint32_t)n, cp);
        else rec_CmdCopyBuffer(cb, gmem_buf, other, (uint32_t)n, cp);
        return;
    }
    for (int i = 0; i < n; i++) {
        uint64_t h = to_heap ? cp[i].dstOffset : cp[i].srcOffset, o = to_heap ? cp[i].srcOffset : cp[i].dstOffset, left = cp[i].size;
        while (left) {
            int k = (int)(h >> gc_shift);
            uint64_t in = h - ((uint64_t)k << gc_shift), piece = (1ull << gc_shift) - in < left ? (1ull << gc_shift) - in : left;
            if (k < gc_n && (gc[k].buf || gc_import(k)) && in + piece <= gc[k].size) {
                VkBufferCopy c = to_heap ? (VkBufferCopy){o, in, piece} : (VkBufferCopy){in, o, piece};
                if (to_heap) rec_CmdCopyBuffer(cb, other, gc[k].buf, 1, &c);
                else rec_CmdCopyBuffer(cb, gc[k].buf, other, 1, &c);
            }
            h += piece; o += piece; left -= piece;
        }
    }
}

// The heap is there for the GPU over this range (one import, or lazy chunks: imported on demand).
static bool gmem_covers(uint64_t off, uint64_t size) {
    if (gc_lazy) {
        if (off + size > gmem_heap_size()) return false;
        gc_touch(off, size);
        for (int k = (int)(off >> gc_shift); k <= (int)((off + size - 1) >> gc_shift); k++) if (!gc[k].buf) return false;
        return true;
    }
    return gmem_buf && off + size <= gmem_size;
}


// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. LARECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;

static VkDescriptorBufferInfo gfast_info(void) {
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_GMEM_FAST"); on = !e || atoi(e) != 0; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    if (!gc_lazy) gmem_info();  // (the heap import, made on first use)
    gfast_info();
    if (!on || (!gmem_buf && !gc_lazy)) return;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || !gmem_covers(a - HEAP_BASE, sz)) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull, top = gc_lazy ? gmem_heap_size() : gmem_size;
        if (hi > top) hi = top;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (getenv("LARECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("LARECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    gmem_copy(cb, gfast_buf, cp, m, false);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    gmem_copy(cb, gfast_buf, back, gfast_n, true);
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- global memory without the heap import (phones: Turnip has no VK_EXT_external_memory_host, and the 3 GB heap
// can't be driver memory there). Shaders reach their storage buffers through the address table in every stage
// (translate.c maxwell_gmem_mirror), into one persistent host-visible buffer - phone GPUs share the memory, so the
// CPU fills it directly:
//  - ranges only read: copied from guest memory once per submission (snapshot ring, recycled by submission);
//  - ranges a compute dispatch stores to: a slot, the GPU's own copy, which draws, dispatches and vertex fetches use
//    from then on. CPU and GPU both write such buffers (the game resets a list the GPU fills, every frame), so the
//    two copies are merged by word against a base (guest memory as of the last merge): what the CPU changed goes to
//    the slot in GPU order before its next use, and after each storing dispatch the slot is copied to a readback
//    area whose words the GPU changed go to guest memory once that submission is done (mirror_merge).
#define MIR_SIZE (64ull << 20)
#define MIR_SNAP (48ull << 20)  // [0, MIR_SNAP): snapshot ring; the rest: slots
#define MIR_SLOTS 128
#define MIR_FIFO 4096
static VkBuffer mir_buf;
static VmaAllocation mir_mem;
static uint8_t *mir_ptr;
static pthread_mutex_t mir_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t off, end, seq; } mir_fifo[MIR_FIFO];  // snapshot allocations in ring order
static int mir_fh, mir_ft;
static uint64_t mir_head;
#define MIR_RB 4  // readbacks of a slot in flight
typedef struct {
    uint64_t lo, hi, off, gpu_seq, last_use;  // guest range, its copy (offset), last storing / any use (submissions)
    uint64_t checked_seq;                     // submission whose CPU changes went in (mirror_cpu_changes: once each)
    uint8_t *base;                            // guest memory as of the last merge
    uint64_t rb_off[MIR_RB], rb_seq[MIR_RB];  // readback areas: the slot after a storing dispatch, until merged
    uint8_t *rb_base[MIR_RB];                 // ... and the base it started from
    bool used;
} MirSlot;
static MirSlot *mir_stored[32];  // union of slots written by one dispatch or VS+FS draw
static uint32_t mir_graphics_tables[2][(GMAP_MAX + 1) * 4];
typedef struct { uint64_t lo, hi; bool ro; } MirRange;
static int mir_nstored;
static MirSlot mir_slot[MIR_SLOTS];
static struct { uint64_t lo, hi, seq, off; } mir_snap_memo[64];

// Read the GPU bytes of CB4's existing writable-mirror owner, without changing
// forwarding, merges or submission state. The owner lifetime is extended to
// cover this copy; the queue orders it
// before the ProjectionMask draw and after earlier GPU producers.
static void projection_probe_mirror_cb4(void) {
    if (!stencil_probe_active || g_debug_frame || projection_probe_mirror_done ||
        vk_frame_counter() != stencil_probe_frame || g_cur_fs_hash != 0x44b584f0d922d49dull) return;
    projection_probe_mirror_done = true;
    uint64_t addr = st.ubuf[1][1].addr, n = st.ubuf[1][1].size;
    if (n > 768) n = 768;
    n &= ~3ull;
    MirSlot owner = {0};
    uint64_t pending = vk_pending_seq(), protected_last_use = 0;
    bool found = false;
    if (mir_buf && addr && !(addr & 3) && n && addr <= UINT64_MAX - n) {
        pthread_mutex_lock(&mir_lock);
        for (int i = 0; i < MIR_SLOTS; i++)
            if (mir_slot[i].used && mir_slot[i].lo <= addr && addr < mir_slot[i].hi && n <= mir_slot[i].hi - addr) {
                owner = mir_slot[i];
                if (mir_slot[i].last_use < pending) mir_slot[i].last_use = pending;
                protected_last_use = mir_slot[i].last_use;
                found = true; break;
            }
        pthread_mutex_unlock(&mir_lock);
    }
    if (!found) {
        LOG("[vk] projection CB4 mirror unowned frame %llu guest %llx bytes %llu\n",
            (unsigned long long)stencil_probe_frame, (unsigned long long)addr, (unsigned long long)n);
        return;
    }
    uint64_t delta = addr - owner.lo;
    if (owner.off > MIR_SIZE || delta > MIR_SIZE - owner.off || n > MIR_SIZE - owner.off - delta ||
        n > UNIFORM_PROBE_BYTES || n > STENCIL_PROBE_BYTES - stencil_probe_bytes) return;
    StencilProbePlane *p = &stencil_probe_planes[STENCIL_PROBE_PLANES + 6];
    if (p->buffer) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VkBuffer buffer; VmaAllocation mem; VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &mem, &info) != VK_SUCCESS) return;
    if (!info.pMappedData) { vmaDestroyBuffer(g_vk.vma, buffer, mem); return; }
    VkCommandBuffer cb = vk_cmd(); vk_full_barrier(cb);
    VkBufferCopy copy = {.srcOffset = owner.off + delta, .size = n};
    rec_CmdCopyBuffer(cb, mir_buf, buffer, 1, &copy);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    *p = (StencilProbePlane){.buffer = buffer, .mem = mem, .bytes = info.pMappedData, .n = n,
        .seq = pending, .frame = stencil_probe_frame, .source = (uint64_t)(uintptr_t)mir_buf,
        .guest = addr, .fs_hash = g_cur_fs_hash, .stage = 1, .handle_offset = 4, .color = true, .uniform = true,
        .owner_lo = owner.lo, .owner_hi = owner.hi, .owner_off = owner.off, .owner_gpu_seq = owner.gpu_seq,
        .owner_last_use = owner.last_use, .source_buffer_offset = copy.srcOffset, .observer_last_use = protected_last_use};
    snprintf(p->role, sizeof p->role, "gpu-cb4");
    snprintf(p->path, sizeof p->path, "build/projection_%llu_gpu-cb4.bin", (unsigned long long)p->frame);
    stencil_probe_bytes += n; stencil_probe_stats.queued++;
    LOG("[vk] projection CB4 mirror queued frame %llu guest %llx owner %llx..%llx off %llu gpu_seq %llu last_use %llu seq %llu\n",
        (unsigned long long)p->frame, (unsigned long long)addr, (unsigned long long)owner.lo, (unsigned long long)owner.hi,
        (unsigned long long)copy.srcOffset, (unsigned long long)owner.gpu_seq, (unsigned long long)owner.last_use, (unsigned long long)p->seq);
}

bool vk_gmem_mirror(void) { return !g_vk.host_import && !maxwell_gmem_readonly; }

static bool uniform_forward_enabled(void);
static void mirror_init(void) {
    if (mir_buf) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MIR_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      (uniform_forward_enabled() ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0) | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &mir_buf, &mir_mem, &info));
    mir_ptr = info.pMappedData;
    LOG("[vk] shader global memory: storage buffers mirrored (no heap import), %llu MB\n", (unsigned long long)(MIR_SIZE >> 20));
}

// size bytes of the snapshot ring for the pending submission (waits for older submissions still reading that part)
static uint64_t mirror_snap_alloc(uint64_t size) {
    if (!size || size > MIR_SNAP / 4) return ~0ull;
    size = (size + 255) & ~255ull;
    uint64_t pending = vk_pending_seq(), done = vk_completed_seq();
    while (mir_ft != mir_fh && mir_fifo[mir_ft].seq <= done) mir_ft = (mir_ft + 1) % MIR_FIFO;
    if ((mir_fh + 1) % MIR_FIFO == mir_ft) return ~0ull;
    if (mir_head + size > MIR_SNAP) mir_head = 0;  // (wrap)
    uint64_t off = mir_head, end = off + size, need = 0;
    for (int i = mir_ft; i != mir_fh; i = (i + 1) % MIR_FIFO) {
        if (!(mir_fifo[i].off < end && off < mir_fifo[i].end) || mir_fifo[i].seq <= done) continue;
        if (mir_fifo[i].seq >= pending) return ~0ull;  // (this submission's own: the ring is too small for it)
        if (mir_fifo[i].seq > need) need = mir_fifo[i].seq;
    }
    if (need) {
        pthread_mutex_unlock(&mir_lock);
        wait_why = "gmem mirror ring";
        vk_wait_seq(need);
        pthread_mutex_lock(&mir_lock);
    }
    mir_fifo[mir_fh].off = off, mir_fifo[mir_fh].end = end, mir_fifo[mir_fh].seq = pending;
    mir_fh = (mir_fh + 1) % MIR_FIFO;
    mir_head = end;
    return off;
}

static uint64_t mir_slot_end(const MirSlot *m) { return m->off + (uint64_t)(MIR_RB + 1) * ((m->hi - m->lo + 255) & ~255ull); }

// GPU writes come back: a slot's readbacks whose submission is done, oldest first, merged into guest memory (mir_lock)
static void mirror_merge_locked(void) {
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < MIR_SLOTS; i++) {
        MirSlot *m = &mir_slot[i];
        if (!m->used) continue;
        for (;;) {
            int k = -1;
            for (int j = 0; j < MIR_RB; j++)
                if (m->rb_seq[j] && m->rb_seq[j] <= done && (k < 0 || m->rb_seq[j] < m->rb_seq[k])) k = j;
            if (k < 0) break;
            uint64_t n = (m->hi - m->lo) / 4;
            vmaInvalidateAllocation(g_vk.vma, mir_mem, m->rb_off[k], n * 4);
            const uint32_t *gpu = (const uint32_t *)(mir_ptr + m->rb_off[k]), *was = (const uint32_t *)m->rb_base[k];
            uint32_t *guest = (uint32_t *)(uintptr_t)m->lo, *base = (uint32_t *)m->base;
            // the newer readbacks of this slot started from an older base: what this one brings in is theirs too -
            // otherwise they'd see these words as GPU changes again, undoing what the CPU wrote in between (the game
            // resetting a list the GPU fills)
            uint32_t *newer[MIR_RB];
            int nn = 0;
            for (int j = 0; j < MIR_RB; j++)
                if (j != k && m->rb_seq[j] > m->rb_seq[k]) newer[nn++] = (uint32_t *)m->rb_base[j];
            for (uint64_t w = 0; w < n; w++)
                if (gpu[w] != was[w]) {  // (a word the GPU changed)
                    // This readback's baseline identifies the event it may
                    // replace. A newer CPU upload or edit is not an old GPU
                    // completion's property to overwrite.
                    uint32_t baseline = was[w];
                    if (guest[w] == baseline) guest[w] = gpu[w];
                    if (base[w] == baseline) base[w] = gpu[w];
                    for (int j = 0; j < nn; j++)
                        if (newer[j][w] == baseline) newer[j][w] = gpu[w];
                }
            m->rb_seq[k] = 0;
        }
    }
}

void vk_mirror_writeback(void) {
    if (!mir_buf) return;
    pthread_mutex_lock(&mir_lock);
    mirror_merge_locked();
    pthread_mutex_unlock(&mir_lock);
}

// Slots unused since before submission `before` (or the least recently used idle one, lru) are dropped
static bool mirror_slot_idle(const MirSlot *m, uint64_t done) {
    bool busy = m->last_use > done;
    for (int j = 0; j < MIR_RB; j++) busy |= m->rb_seq[j] != 0;
    return m->used && !busy;
}
static void mirror_slot_drop(MirSlot *m) {
    free(m->base);
    for (int j = 0; j < MIR_RB; j++) free(m->rb_base[j]);
    memset(m, 0, sizeof *m);
}
static bool mirror_slot_reclaim(uint64_t before, bool lru) {
    uint64_t done = vk_completed_seq();
    MirSlot *oldest = NULL;
    bool any = false;
    for (int i = 0; i < MIR_SLOTS; i++) {
        MirSlot *m = &mir_slot[i];
        if (!mirror_slot_idle(m, done)) continue;
        if (m->last_use < before) mirror_slot_drop(m), any = true;
        else if (!oldest || m->last_use < oldest->last_use) oldest = m;
    }
    if (!any && lru && oldest) mirror_slot_drop(oldest), any = true;
    return any;
}

// A slot region (its copy + MIR_RB readback areas): first fit among slots
static uint64_t mirror_slot_alloc(uint64_t size) {
    // Reject before rounding/multiplication, including sizes whose arithmetic
    // would wrap into a tiny allocation. A slot needs all its readback copies.
    if (!size || size > UINT64_MAX - 255) return ~0ull;
    uint64_t rounded = (size + 255) & ~255ull;
    if (rounded > (MIR_SIZE - MIR_SNAP) / (MIR_RB + 1)) return ~0ull;
    size = (uint64_t)(MIR_RB + 1) * rounded;
    for (uint64_t at = MIR_SNAP; at + size <= MIR_SIZE;) {
        bool clash = false;
        for (int i = 0; i < MIR_SLOTS && !clash; i++) {
            MirSlot *m = &mir_slot[i];
            if (m->used && m->off < at + size && at < mir_slot_end(m)) at = mir_slot_end(m), clash = true;
        }
        if (!clash) return at;
    }
    return ~0ull;
}

// mir_lock held. Retire only already submitted work; the pending command
// buffer's slots and readbacks must never be recycled while it references them.
static uint64_t mirror_slot_retirement(uint64_t pending) {
    uint64_t done = vk_completed_seq(), oldest = 0;
    for (int i = 0; i < MIR_SLOTS; i++) {
        const MirSlot *m = &mir_slot[i];
        if (!m->used || m->last_use >= pending) continue;
        uint64_t need = m->last_use;
        for (int j = 0; j < MIR_RB; j++) if (m->rb_seq[j] > need) need = m->rb_seq[j];
        if (need > done && need < pending && (!oldest || need < oldest)) oldest = need;
    }
    return oldest;
}
static bool mirror_slot_reserve(uint64_t size, int *index, uint64_t *offset) {
    if (!size || size > UINT64_MAX - 255 ||
        ((size + 255) & ~255ull) > (MIR_SIZE - MIR_SNAP) / (MIR_RB + 1)) return false;
    uint64_t pending = vk_pending_seq();
    mirror_merge_locked();
    if (pending > 600) mirror_slot_reclaim(pending - 600, false);
    unsigned reclaimed = 0, waited = 0;
    // Each reclaim removes a slot; each wait makes at least one older slot
    // eligible. The old eight-victim limit could fail with ample idle storage.
    for (unsigned progress = 0; progress <= 2 * MIR_SLOTS; progress++) {
        int k;
        for (k = 0; k < MIR_SLOTS && mir_slot[k].used; k++) {}
        uint64_t at = k < MIR_SLOTS ? mirror_slot_alloc(size) : ~0ull;
        if (at != ~0ull) {
            *index = k; *offset = at;
            static unsigned pressure_logs;
            if ((reclaimed > 8 || waited) && pressure_logs++ < 12)
                LOG("[vk] writable mirror pressure: reclaimed %u idle slots, retired %u submissions for %llu bytes\n",
                    reclaimed, waited, (unsigned long long)size);
            return true;
        }
        if (mirror_slot_reclaim(0, true)) { reclaimed++; continue; }
        uint64_t need = mirror_slot_retirement(pending);
        if (!need) break;
        pthread_mutex_unlock(&mir_lock);
        wait_why = "gmem writable mirror retirement";
        vk_wait_seq(need);
        pthread_mutex_lock(&mir_lock);
        mirror_merge_locked();
        waited++;
    }
    return false;
}

// What the CPU changed in a slot's range since the last merge: into the slot, recorded on cb (mir_lock)
static void mirror_cpu_changes(MirSlot *m, VkCommandBuffer cb) {
    uint64_t n = (m->hi - m->lo) / 4, pending = vk_pending_seq();
    const uint32_t *guest = (const uint32_t *)(uintptr_t)m->lo;
    uint32_t *base = (uint32_t *)m->base;
    // once a submission (a list's draws see guest memory as of its translation, like every other copy), and the
    // common case - nothing changed - by memcmp (a whole frame's draws bind the light lists: 110 word scans of 32 KB)
    if (m->checked_seq == pending) return;
    m->checked_seq = pending;
    if (!memcmp(guest, base, n * 4)) return;
    int ncp = 0;
    for (uint64_t w = 0; w < n;) {
        if (guest[w] == base[w]) { w++; continue; }
        uint64_t e = w;
        // Unchanged CPU words may contain newer GPU-only values. Never
        // coalesce a CPU upload across those words, even for a short gap.
        while (e < n && guest[e] != base[e]) e++;
        VkSpan sp = vk_transient_copy(guest + w, (e - w) * 4, 16);
        VkBufferCopy one = {sp.off, m->off + w * 4, (e - w) * 4};
        rec_CmdCopyBuffer(cb, sp.buf, mir_buf, 1, &one);
        memcpy(base + w, guest + w, (e - w) * 4);
        stat_upload_bytes += (e - w) * 4;
        ncp++;
        w = e;
    }
    if (ncp) { uniform_forward_write_epoch++; vk_full_barrier(cb); }
}

// Validate before rounding or copying. Subtractive bounds cannot wrap a+size.
static bool mirror_guest_range(uint64_t addr, uint64_t size, uint64_t *lo, uint64_t *hi) {
    if (!size || addr < HEAP_BASE) return false;
    uint64_t offset = addr - HEAP_BASE;
    if (offset >= HEAP_MAX || size > HEAP_MAX - offset || size > UINT64_MAX - addr) return false;
    uint64_t end = addr + size;
    if (end > UINT64_MAX - 3) return false;
    *lo = addr & ~3ull;
    *hi = (end + 3) & ~3ull;
    return true;
}

// Shared range-to-slot machinery. A NULL table prepares an entire graphics
// union (up to32 ranges), independently of per-stage descriptor tables.
static void mirror_table_ranges(int stage, bool store, VkCommandBuffer cb,
                                const MirRange *r, int m, uint32_t *tab) {
    uint64_t pending = vk_pending_seq();
    pthread_mutex_lock(&mir_lock);
    int out = 0;
    for (int i = 0; i < m && (!tab || out < GMAP_MAX); i++) {
        uint64_t lo = r[i].lo, hi = r[i].hi, off = ~0ull;
        MirSlot *slot = NULL;
        for (int k = 0; k < MIR_SLOTS && !slot; k++)
            if (mir_slot[k].used && mir_slot[k].lo <= lo && hi <= mir_slot[k].hi) slot = &mir_slot[k];
        // Existing persistent copies cannot be split or expanded implicitly:
        // that would create two GPU owners for overlapping guest words.
        if (mir_graphics_preparing) for (int k = 0; k < MIR_SLOTS; ++k) {
            MirSlot *other = &mir_slot[k];
            if (other->used && other != slot && other->lo < hi && lo < other->hi) {
                pthread_mutex_unlock(&mir_lock);
                FATAL("[vk] graphics mirror partial persistent-slot overlap is unsupported\n");
            }
        }
        if (!slot && store && !r[i].ro) {  // a new GPU copy, starting from the CPU's
            if (uniform_forward_enabled()) for (int j = 0; j < MIR_SLOTS; ++j)
                if (mir_slot[j].used && mir_slot[j].lo < hi && lo < mir_slot[j].hi)
                    FATAL("[vk] missing mirror overlap admission preflight for %llx..%llx\n",
                          (unsigned long long)lo, (unsigned long long)hi);
            int k = 0;
            uint64_t at = ~0ull;
            if (mirror_slot_reserve(hi - lo, &k, &at)) {
                slot = &mir_slot[k];
                *slot = (MirSlot){.lo = lo, .hi = hi, .off = at, .used = true, .base = malloc(hi - lo)};
                uint64_t stride = (hi - lo + 255) & ~255ull;
                if (!slot->base) FATAL("[vk] writable mirror base allocation failed\n");
                for (int j = 0; j < MIR_RB; j++) {
                    slot->rb_off[j] = at + (uint64_t)(j + 1) * stride;
                    slot->rb_base[j] = malloc(hi - lo);
                    if (!slot->rb_base[j]) FATAL("[vk] writable mirror readback-base allocation failed\n");
                }
                memcpy(slot->base, (const void *)(uintptr_t)lo, hi - lo);
                memcpy(mir_ptr + at, slot->base, hi - lo);
                vmaFlushAllocation(g_vk.vma, mir_mem, at, hi - lo);
                uniform_forward_write_epoch++;
                stat_upload_bytes += hi - lo;
            } else {
                int used = 0, pending_slots = 0;
                uint64_t bytes = 0, done = vk_completed_seq();
                for (int j = 0; j < MIR_SLOTS; j++) if (mir_slot[j].used) {
                    used++; bytes += mir_slot_end(&mir_slot[j]) - mir_slot[j].off;
                    pending_slots += mir_slot[j].last_use >= pending;
                }
                pthread_mutex_unlock(&mir_lock);
                FATAL("[vk] writable mirror capacity exhausted for %llx+%llx (%d of %d slots, %llu/%llu bytes, %d pending slots, done %llu pending %llu); refusing to discard shader stores\n",
                      (unsigned long long)lo, (unsigned long long)(hi - lo), used, MIR_SLOTS,
                      (unsigned long long)bytes, (unsigned long long)(MIR_SIZE - MIR_SNAP), pending_slots,
                      (unsigned long long)done, (unsigned long long)pending);
            }
        }
        if (slot) {
            if (cb) mirror_cpu_changes(slot, cb);
            else if (slot->gpu_seq < pending) mirror_cpu_changes(slot, vk_upload_cmd());
            off = slot->off + (lo - slot->lo);
            slot->last_use = pending;
            if (store && !r[i].ro) {
                bool duplicate = false;
                for (int j = 0; j < mir_nstored; ++j) duplicate |= mir_stored[j] == slot;
                if (!duplicate) {
                    if (mir_nstored >= (int)(sizeof mir_stored / sizeof mir_stored[0]))
                        FATAL("[vk] writable mirror operation exceeds its stored-range capacity\n");
                    mir_stored[mir_nstored++] = slot;
                }
            }
        } else {
            for (int k = 0; k < 64 && off == ~0ull; k++)  // (the same range again in this submission: its copy)
                if (mir_snap_memo[k].seq == pending && mir_snap_memo[k].lo == lo && mir_snap_memo[k].hi == hi) off = mir_snap_memo[k].off;
            if (off == ~0ull) {
                off = mirror_snap_alloc(hi - lo);
                if (off == ~0ull) {  // preserving the old read-only fallback outside storing graphics operations
                    if (mir_graphics_preparing) {
                        pthread_mutex_unlock(&mir_lock);
                        FATAL("[vk] graphics mirror read input exceeds snapshot capacity\n");
                    }
                    static int nlog;
                    if (nlog++ < 20) LOG("[vk] gmem mirror: stage %d range %llx+%llx doesn't fit the snapshot ring: reads zero\n",
                                         stage, (unsigned long long)lo, (unsigned long long)(hi - lo));
                    continue;
                }
                memcpy(mir_ptr + off, (const void *)(uintptr_t)lo, hi - lo);
                vmaFlushAllocation(g_vk.vma, mir_mem, off, hi - lo);
                stat_upload_bytes += hi - lo;
                static int memo_at;
                mir_snap_memo[memo_at].lo = lo, mir_snap_memo[memo_at].hi = hi, mir_snap_memo[memo_at].seq = pending;
                mir_snap_memo[memo_at].off = off;
                memo_at = (memo_at + 1) % 64;
            }
        }
        if (tab) {
            tab[out * 4 + 0] = (uint32_t)(lo - HEAP_BASE);
            tab[out * 4 + 1] = (uint32_t)(hi - lo);
            tab[out * 4 + 2] = (uint32_t)(off >> 2);
            out++;
        }
    }
    pthread_mutex_unlock(&mir_lock);
    if (tab) tab[GMAP_MAX * 4] = (uint32_t)out;
}

// The stage table is built on each descriptor request, before either descriptor
// cache can return a reused set. Graphics store operations precompute both tables
// outside rendering and stage_set then uses those immutable tables.
static void mirror_table(int stage, bool store, VkCommandBuffer cb, uint32_t *tab) {
    mirror_init();
    memset(tab, 0, sizeof gmap_tab);
    // the stage's storage buffers, and a compute dispatch's uniform buffers too: the driver constants hand it their
    // addresses (cbuf_info), and shaders read them as global memory - the light culling reads its parameters that way,
    // and without them it made no lights at all: a dark, flat world (point lights, the lamps' glow)
    MirRange r[32];
    int n = 0;
    for (int i = 0; i < (stage == 5 ? 32 : 16); i++) {
        bool ubo = i >= 16;
        uint64_t a = ubo ? st.ubuf[stage][i - 16].addr : st.sbuf[stage][i].addr, sz = ubo ? st.ubuf[stage][i - 16].size : st.sbuf[stage][i].size;
        if (!a || !sz) continue;
        uint64_t lo, hi;
        if (!mirror_guest_range(a, sz, &lo, &hi)) {
            if (ubo) continue;
            if (store) FATAL("[vk] writable mirror stage %d buffer %d %llx+%llx is outside the guest heap\n",
                             stage, i, (unsigned long long)a, (unsigned long long)sz);
            static int nlog;
            if (nlog++ < 20) LOG("[vk] gmem mirror: stage %d buffer %d %llx+%llx outside the heap: reads zero\n", stage, i,
                                 (unsigned long long)a, (unsigned long long)sz);
            continue;
        }
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
        r[k].lo = lo; r[k].hi = hi; r[k].ro = ubo;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && r[i].lo < r[m - 1].hi) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; r[m - 1].ro &= r[i].ro; }
        else r[m++] = r[i];
    }
    mirror_table_ranges(stage, store, cb, r, m, tab);
}

// After a storing dispatch: its slots copied to readback areas (merged into guest memory once done)
static void mirror_after_dispatch(VkCommandBuffer cb) {
    if (!mir_nstored) return;
    uniform_forward_write_epoch++;
    uint64_t pending = vk_pending_seq();
    vk_full_barrier(cb);
    pthread_mutex_lock(&mir_lock);
    for (int i = 0; i < mir_nstored; i++) {
        MirSlot *m = mir_stored[i];
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= mir_stored[j] == m;
        if (dup) continue;
        int k = -1;
        for (int j = 0; j < MIR_RB; j++) if (m->rb_seq[j] == pending) k = j;  // (again this submission: the same area)
        while (k < 0) {
            for (int j = 0; j < MIR_RB && k < 0; j++) if (!m->rb_seq[j]) k = j;
            if (k >= 0) break;
            uint64_t oldest = ~0ull;  // (all in flight: the oldest must be done and merged first)
            for (int j = 0; j < MIR_RB; j++) if (m->rb_seq[j] < oldest) oldest = m->rb_seq[j];
            if (oldest >= pending) break;
            pthread_mutex_unlock(&mir_lock);
            wait_why = "gmem mirror readback";
            vk_wait_seq(oldest);
            pthread_mutex_lock(&mir_lock);
            mirror_merge_locked();
        }
        if (k < 0) {
            if (mir_graphics_prepared) {
                pthread_mutex_unlock(&mir_lock);
                FATAL("graphics mirror readback capacity exhausted");
            }
            continue;
        }
        // The latest snapshot in this submission follows any ordered CPU
        // uploads too. Rebase even when reusing the same readback area.
        memcpy(m->rb_base[k], m->base, m->hi - m->lo);
        VkBufferCopy c = {m->off, m->rb_off[k], m->hi - m->lo};
        rec_CmdCopyBuffer(cb, mir_buf, mir_buf, 1, &c);
        m->rb_seq[k] = pending;
        m->gpu_seq = pending;
    }
    mir_nstored = 0;
    pthread_mutex_unlock(&mir_lock);
    vk_full_barrier(cb);
}

// VS and FS share one logical guest address space, including read/store aliases.
// Merge ALL participating storage ranges first, then allocate one persistent
// owner for each component touched by a writer. Read-only aliases map to it too.
static void mirror_prepare_graphics(const ShaderInfo *vs, const ShaderInfo *fs) {
    if (!vk_gmem_mirror() || !(vs->uses_gmem_store || fs->uses_gmem_store)) return;
    if ((vs->uses_gmem_store && (!vs->uses_gmem_map || vs->uses_gmem_readonly)) ||
        (fs->uses_gmem_store && (!fs->uses_gmem_map || fs->uses_gmem_readonly)))
        FATAL("[vk] graphics shader stores require a writable address-table mirror\n");
    MirRange ranges[32]; int n = 0;
    const ShaderInfo *stages[2] = {vs, fs};
    for (int stage = 0; stage < 2; ++stage) {
        if (!stages[stage]->uses_gmem_map) continue;
        for (int i = 0; i < 16; ++i) {
            uint64_t addr = st.sbuf[stage][i].addr, size = st.sbuf[stage][i].size, lo, hi;
            if (!addr || !size) continue;
            if (!mirror_guest_range(addr, size, &lo, &hi))
                FATAL("[vk] storing graphics operation has an invalid input range\n");
            int k = n++;
            while (k && ranges[k-1].lo > lo) { ranges[k] = ranges[k-1]; --k; }
            ranges[k] = (MirRange){lo, hi, !stages[stage]->uses_gmem_store};
        }
    }
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (m && ranges[i].lo < ranges[m-1].hi) {
            if (ranges[i].hi > ranges[m-1].hi) ranges[m-1].hi = ranges[i].hi;
            ranges[m-1].ro &= ranges[i].ro;
        } else ranges[m++] = ranges[i];
    }
    int writers = 0;
    for (int i = 0; i < m; ++i) if (!ranges[i].ro) ranges[writers++] = ranges[i];
    if (!writers) FATAL("[vk] storing graphics shader has no mapped writable storage range\n");
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb); // earlier shader reads/writes complete before slot uploads
    mirror_init();
    mir_nstored = 0;
    mir_graphics_preparing = true;
    mirror_table_ranges(-1, true, cb, ranges, writers, NULL);
    for (int stage = 0; stage < 2; ++stage) {
        memset(mir_graphics_tables[stage], 0, sizeof mir_graphics_tables[stage]);
        if (stages[stage]->uses_gmem_map)
            mirror_table(stage, stages[stage]->uses_gmem_store, cb, mir_graphics_tables[stage]);
    }
    mir_graphics_preparing = false;
    vk_full_barrier(cb); // host/transfer uploads visible to both shader stages
    mir_graphics_seq = vk_pending_seq();
    mir_graphics_prepared = true;
    // Caller begins rendering only after uploads and tables are complete.
}
static void mirror_finish_graphics(VkCommandBuffer cb) {
    if (!mir_graphics_prepared) return;
    vk_end_pass();
    // A diagnostic end-pass screenshot may submit the recording command
    // buffer. Never append a readback to that stale recording or lose writes.
    if (mir_graphics_seq != vk_pending_seq())
        FATAL("graphics mirror submission changed before readback");
    mirror_after_dispatch(cb); // full shader->transfer->later-use ordering
    mir_graphics_prepared = false;
}

// (guest_direct) a range the GPU holds the current copy of: that copy
static bool mirror_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (!mir_buf) return false;
    bool hit = false;
    // (no lock: a slot's range and use change only on this thread - the one recording - and the merges elsewhere
    // touch its readbacks and base alone)
    for (int k = 0; k < MIR_SLOTS && !hit; k++) {
        MirSlot *m = &mir_slot[k];
        if (m->used && m->lo <= addr && addr + size <= m->hi) {
            *buf = mir_buf, *off = m->off + (addr - m->lo);
            uint64_t pending = vk_pending_seq();
            m->last_use = pending;
            // what the CPU wrote there too, as for a shader's reads (mirror_table): a vertex buffer the game fills
            // partly on the CPU, partly by compute (Madam MeowMeow's little chain chomps were torn into white wedges)
            if (m->gpu_seq < pending) {
                pthread_mutex_lock(&mir_lock);  // (base: merges elsewhere)
                mirror_cpu_changes(m, vk_upload_cmd());
                pthread_mutex_unlock(&mir_lock);
            }
            hit = true;
        }
    }
    return hit;
}

#include "uniform_forward.h"
#include "mirror_normalize.h"


// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. LARECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, -1: unknown (no tracking, outside the heap, or pages the GPU writes).
// *now: the generation to pass next time.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) return -1;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) vc_page_gpu[p] = 1;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz);
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("LARECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        // integrated GPU with a userptr heap (only with LARECOMP_WRITE_TRACKING=1: tracking is off there otherwise): the
        // heap already is the GPU's memory, no bus to save - the copies would only cost. LARECOMP_VK_VCACHE=1: on.
        if (vc_on && !e && gmem_userptr && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            LOG("[vk] vertex cache off: integrated GPU\n");
            vc_on = 0;
        }
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    // (without the heap import - phones - the cache is filled from guest memory by the CPU: vc_flush)
    if (!vc_on || (!gmem_buf && !gc_lazy && !vk_gmem_mirror()) || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (gmem_buf || gc_lazy ? !gmem_covers(lo, hi - lo) : hi > HEAP_MAX) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc && !gmem_buf && !gc_lazy) {  // (no heap import: the written pages through the transient ring)
            for (int k = 0; k < nc; k++) {
                VkSpan sp = vk_transient_copy((const void *)(uintptr_t)(HEAP_BASE + cp[k].srcOffset), cp[k].size, 16);
                cp[k].srcOffset = sp.off;
                rec_CmdCopyBuffer(cb, sp.buf, e->buf, 1, &cp[k]);
                prof_vc_copied += cp[k].size;
            }
        } else if (nc) {
            gmem_copy(cb, e->buf, cp, nc, false);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1
// forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (gc_lazy) {
        uint64_t o = addr - HEAP_BASE;
        if (!size || o + size > HEAP_MAX) return false;
        int k = (int)(o >> gc_shift);
        if ((int)((o + size - 1) >> gc_shift) != k || !gmem_covers(o, size)) return false;  // (crossing chunks: copied)
        if (vcache_lookup(addr, size, buf, off)) return true;
        *buf = gc[k].buf;
        *off = o - ((uint64_t)k << gc_shift);
        return true;
    }
    if (g_vk.host_import && addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf) return mirror_direct(addr, size, buf, off) || vcache_lookup(addr, size, buf, off);
    if (addr + size > HEAP_BASE + gmem_size) return false;
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    key_gen++;  // (the key memo: the tables' answers are gone)
    ds_gen++;   // (set reuse: the heap import below is destroyed at once)
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
#if defined(__linux__) && !defined(__ANDROID__)
        bool placed = gmem_placed;
        heap_unmap_placed();
#endif
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
#if defined(__linux__) && !defined(__ANDROID__)
        if (placed && !plat_reset_guest(HEAP_BASE, HEAP_BASE + HEAP_MAX)) FATAL("cannot restore the guest heap mapping\n");
        gmem_dmabuf = false;  // (the next boot decides again)
#endif
    }
    gmem_size = gmem_tried = 0;
    for (int k = 0; k < gc_n; k++) {  // (lazy chunks: their pages are replaced too)
        if (gc[k].buf) { vkDestroyBuffer(g_vk.dev, gc[k].buf, NULL); vkFreeMemory(g_vk.dev, gc[k].mem, NULL); }
        memset(&gc[k], 0, sizeof gc[k]);
    }
    gc_imported = 0; gc_scanned = 0; gc_imported_bytes = 0; gc_gen++;  // (gc_remembered stays: the same heap layout next boot)
    gc_remembered_heap = 0;  // (imported again at the next boot's first chunk set)
    if (gc_miss) { memset(gc_miss, 0, sizeof(uint32_t) * GC_MAX); memset(gc_miss + GC_MAX, 0xFF, sizeof(uint32_t) * GC_MAX); }
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
    if (mir_buf) {  // (the mirror: the old boot's slots and readbacks, not merged into the fresh heap; the GPU is idle)
        pthread_mutex_lock(&mir_lock);
        for (int i = 0; i < MIR_SLOTS; i++) {
            free(mir_slot[i].base);
            for (int j = 0; j < MIR_RB; j++) free(mir_slot[i].rb_base[j]);
        }
        memset(mir_slot, 0, sizeof mir_slot);
        memset(mir_snap_memo, 0, sizeof mir_snap_memo);
        mir_fh = mir_ft = 0, mir_head = 0, mir_nstored = 0;
        mir_graphics_prepared = mir_graphics_preparing = false; mir_graphics_seq = 0;
        pthread_mutex_unlock(&mir_lock);
    }
    st_defaults();
}

// Shadow (sampler*Shadow) slots need a depth view whose format can be compared (VUID-vkCmdDrawIndexed-None-06479):
// the colour dummy or a colour texture there is replaced by a 1x1 depth dummy at 1.0 (lit, textures.c). Each cause is
// logged once (LARECOMP_SHADOW_SLOT_LOG=1: every change of cause / handle, up to 200 lines), with the texture's
// address, size and format, so a run shows why the slot had nothing usable.
static void shadow_slot_check(int stage, const ShaderTexture *st_, int i, uint64_t h, uint32_t tic, const NvnTexEntry *e,
                              NvnTexture *t, VkImageView *view) {
    VkImageView dummy = vk_dummy_view(st_->type);
    if (*view && *view != dummy && t && vk_tex_depth_compare(t)) return;
    enum { HANDLE0, NO_ENTRY, FINALIZED, UNREGISTERED, MULTISAMPLE, FEEDBACK_ARRAY, VIEW_FAILED, TYPE_MISMATCH,
           DEPTH_NO_COMPARE, COLOUR, NCAUSE };
    static const char *names[NCAUSE] = {"handle 0 (nothing bound)", "handle past the texture pool", "texture finalized",
                                        "pool slot not registered", "multisample texture", "feedback snapshot array view unavailable",
                                        "view creation failed", "type mismatch", "depth format without compare support",
                                        "colour format"};
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    int why;
    if (!h) why = HANDLE0;
    else if (!e) why = NO_ENTRY;
    else if (!t) why = e->dead_format ? FINALIZED : UNREGISTERED;
    else if (*view == dummy || !*view) {
        bool fb = false;
        for (int k = 0; k < nfeedback; k++) fb |= feedback_tex[k] == e->tex;
        why = b && b->samples > 1 ? MULTISAMPLE : fb && st_->type == TEXTYPE_2D_ARRAY ? FEEDBACK_ARRAY : !*view ? VIEW_FAILED : TYPE_MISMATCH;
    } else why = b && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? DEPTH_NO_COMPARE : COLOUR;
    static int verbose = -1, nverbose;
    static bool logged[NCAUSE];
    static int last_why = -1;
    static uint64_t last_h;
    if (verbose < 0) verbose = getenv("LARECOMP_SHADOW_SLOT_LOG") != NULL;
    bool log = !logged[why] || (verbose && nverbose < 200 && (why != last_why || h != last_h));
    if (log) {
        logged[why] = true;
        if (verbose) nverbose++;
        last_why = why; last_h = h;
        if (t)
            LOG("[vk] shadow slot t%d (stage %d, type %d) f%llu fs %016llx: %s - handle %#llx tic %u, texture %#llx %ux%ux%u "
                "target %u nvn fmt %#x vk fmt %d aspect %#x samples %u: 1x1 depth dummy instead%s\n",
                i, stage, st_->type, (unsigned long long)vk_frame_counter(), (unsigned long long)g_cur_fs_hash, names[why],
                (unsigned long long)h, tic, (unsigned long long)t->addr, t->width, t->height, t->depth, t->target, t->format,
                b ? b->format : 0, b ? b->aspect : 0, b ? b->samples : 0, verbose ? "" : " (once per cause)");
        else
            LOG("[vk] shadow slot t%d (stage %d, type %d) f%llu fs %016llx: %s - handle %#llx tic %u (finalized nvn fmt %#x): "
                "1x1 depth dummy instead%s\n", i, stage, st_->type, (unsigned long long)vk_frame_counter(),
                (unsigned long long)g_cur_fs_hash, names[why], (unsigned long long)h, tic, e && e->dead_format ? e->dead_format - 1 : 0,
                verbose ? "" : " (once per cause)");
    }
    *view = vk_dummy_depth_view(st_->type);
}

// Shader numeric type follows the selected view, not the combined D/S storage
// format. No Vulkan objects or uploads are created while constructing keys.
static uint32_t sampled_uint_mask(int stage) {
    uint32_t mask = 0;
    if (!st.tpool) return mask;
    for (unsigned slot = 0; slot < 32; slot++) {
        uint64_t h = st.tex[stage][slot];
        uint32_t tic = h & 0xFFFFF;
        if (!h || tic >= (uint32_t)st.tpool->count) continue;
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex && vk_sampled_stencil(e->tex, &e->view, e->has_view)) mask |= 1u << slot;
    }
    return mask;
}

// Preserve the pre-stencil-fix comparison path only on an actual combined
// image. This is compatibility behavior, not a claim about NVN integer+Dref.
static bool legacy_stencil_depth_view(const NvnTexEntry *e, NvnTextureView *view) {
    if (!e || !e->tex || (e->tex->format != 0x35 && e->tex->format != 0x36) ||
        !vk_sampled_stencil(e->tex, &e->view, e->has_view)) return false;
    *view = e->has_view ? e->view : (NvnTextureView){0};
    view->setmask |= 16;
    view->ds_mode = 0;
    return true;
}

// Render-thread diagnostic only. Cache the opt-in flag once and retain only
// the first64 distinct bindings; disabled sessions inspect no extra metadata.
static int sampling_trace_enabled = -1;
static struct { int stage; uint64_t shader, handle; uint8_t type; } sampling_trace_seen[64];
static unsigned sampling_trace_count;
static uint64_t sampling_trace_polled = UINT64_MAX;
static bool sampling_trace_on(void) {
    if (sampling_trace_enabled < 0) {
        const char *v = getenv("LARECOMP_SAMPLING_TRACE");
        sampling_trace_enabled = v && !strcmp(v, "1");
    }
    return sampling_trace_enabled != 0;
}
// Request-only rearm for a later scene. Poll once per presented frame at draw
// entry, never from individual sampler lookups. Consume only regular files.
static void sampling_trace_poll(void) {
    if (!sampling_trace_on()) return;
    uint64_t frame = vk_frame_counter();
    if (frame == sampling_trace_polled) return;
    sampling_trace_polled = frame;
    struct stat sb;
    if (stat("sampling-trace.request", &sb) || !S_ISREG(sb.st_mode)) return;
    if (unlink("sampling-trace.request")) return;
    sampling_trace_count = 0; // old entries are outside the active prefix
    LOG("[sampling] armed frame=%llu limit=64\n", (unsigned long long)frame);
}
static bool sampling_trace_admit(int stage, uint64_t shader, uint64_t handle, uint8_t type) {
    if (!sampling_trace_on() || sampling_trace_count == 64) return false;
    for (unsigned i = 0; i < sampling_trace_count; i++)
        if (sampling_trace_seen[i].stage == stage && sampling_trace_seen[i].shader == shader &&
            sampling_trace_seen[i].handle == handle && sampling_trace_seen[i].type == type) return false;
    sampling_trace_seen[sampling_trace_count].stage = stage;
    sampling_trace_seen[sampling_trace_count].shader = shader;
    sampling_trace_seen[sampling_trace_count].handle = handle;
    sampling_trace_seen[sampling_trace_count].type = type;
    sampling_trace_count++;
    return true;
}
static void sampling_trace_binding(int stage, uint64_t shader, uint64_t handle, const ShaderTexture *image,
                                   const NvnTexEntry *entry, const NvnSamplerDesc *sampler,
                                   bool sampler_default, bool null_view_fallback) {
    if (!sampling_trace_admit(stage, shader, handle, image->type)) return;
    const NvnTexture *t = entry ? entry->tex : NULL;
    const TexBackend *b = t ? t->backend : NULL;
    const NvnTextureView *v = entry && entry->has_view ? &entry->view : NULL;
    const char *sampler_source = sampler_default ? (sampler ? "pool-rejected-default" : "renderer-default") : "pool";
    NvnSamplerDesc fallback = {.min_filter = !image->is_uint, .mag_filter = !image->is_uint,
        .wrap = {7, 7, 7}, .lod_max = 0.25f, .max_aniso = 1}; // vk_default_sampler's actual policy
    if (sampler_default) sampler = &fallback;
    LOG("[sampling] stage=%d shader=%016llx handle=%llx tic=%u tsc=%u type=%u shadow=%u bindless=%u cb=%u off=%x"
        " texture_id=%llu extent=%ux%ux%u levels=%u nvn_fmt=%x backend_fmt=%d format_fallback=%u null_view_fallback=%u"
        " view_mask=%x view_fmt=%x mip=%u+%u layer=%u+%u ds_mode=%u sampler_source=%s sampler_default=%u"
        " min=%u mag=%u wrap=%u,%u,%u lod=%.9g..%.9g bias=%.9g compare=%u/%u aniso=%.9g reduction=%u\n",
        stage, (unsigned long long)shader, (unsigned long long)handle, (uint32_t)handle & 0xfffffu,
        (uint32_t)(handle >> 20) & 0xfffu, image->type, (unsigned)image->shadow, (unsigned)image->bindless,
        image->bindless_cbuf, image->bindless_offset, (unsigned long long)(t ? t->debug_id : 0),
        t ? t->width : 0, t ? t->height : 0, t ? t->depth : 0, t ? t->levels : 0, t ? t->format : 0,
        b ? b->format : VK_FORMAT_UNDEFINED, (unsigned)(b && t && b->format != vk_format(t->format)),
        (unsigned)null_view_fallback, v ? v->setmask : 0, v ? v->format : 0, v ? v->base_level : 0,
        v ? v->num_levels : 0, v ? v->min_layer : 0, v ? v->num_layers : 0,
        v && (v->setmask & 16) ? v->ds_mode : t ? t->ds_mode : 0, sampler_source,
        (unsigned)sampler_default, sampler ? sampler->min_filter : 0, sampler ? sampler->mag_filter : 0,
        sampler ? sampler->wrap[0] : 0, sampler ? sampler->wrap[1] : 0, sampler ? sampler->wrap[2] : 0,
        sampler ? (double)sampler->lod_min : 0, sampler ? (double)sampler->lod_max : 0,
        sampler ? (double)sampler->lod_bias : 0, sampler ? sampler->compare_mode : 0, sampler ? sampler->compare_func : 0,
        sampler ? (double)sampler->max_aniso : 0, sampler ? sampler->reduction : 0);
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2], float fr[2], uint64_t shader_hash) {
    ShaderTexture *st_ = &si->tex[i];
    uint64_t h;
    if (!sampled_texture_handle(stage, st_, &h)) {
        if (st_->bindless) FATAL("[vk] sampled texture handle outside its bound constant buffer\n");
        h = 0; // preserve the existing dummy binding for an unbound fixed slot
    }
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    fr[0] = fr[1] = 1;
    NvnTexEntry *ent = NULL;  // (shadow slots: what the view came from)
    const NvnSamplerDesc *trace_sampler = NULL;
    if ((h || st_->bindless) && st.tpool && st.tpool->entries &&
        st.tpool->count > 0 && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = ent = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            if (st_->bindless && !vk_sampled_float_compatible(e->tex, &e->view, e->has_view))
                FATAL("[vk] bindless sampled texture requires a supported float/depth view\n");
            NvnTextureView effective_view = e->view;
            bool effective_has_view = e->has_view;
            if (st_->legacy_stencil_compare) {
                if (!st_->shadow || st_->is_uint || !legacy_stencil_depth_view(e, &effective_view))
                    FATAL("[vk] legacy stencil/comparison requires a real combined depth/stencil image\n");
                effective_has_view = true;
            } else if (st_->is_uint != vk_sampled_stencil(e->tex, &e->view, e->has_view))
                FATAL("[vk] sampled texture numeric type changed without shader specialization\n");
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = effective_has_view ? hash_words(0, &effective_view, sizeof effective_view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == effective_has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases || nstale ? alias_read(e->tex) : e->tex, &effective_view, effective_has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = effective_has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            renderer_texture_fraction(e->tex, &fr[0], &fr[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_sample_view(k, st_->type, st_->is_uint);
                }
        } else if (e->dead_format) {
            nvn_dead_slot_sampled(e, tic);
        }
    }
    if ((h || st_->bindless) && st.spool && st.spool->entries &&
        st.spool->count > 0 && tsc < (uint32_t)st.spool->count) {
        NvnSamplerDesc *d = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        trace_sampler = d;
        smp = st_->is_uint ? vk_sampler_uint(d) : vk_sampler(d);
        if (!smp && st_->is_uint)
            FATAL("[vk] integer sampled view has unsupported sampler state\n");
    }
    if (getenv("LARECOMP_LOG_NOFILTER") && smp && h && st.tpool && tic < (uint32_t)st.tpool->count && st.spool && tsc < (uint32_t)st.spool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        NvnSamplerDesc *d = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        TexBackend *tb = e->tex ? vk_tex(e->tex, false) : NULL;
        if (tb && d->mag_filter == 1) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, tb->format, &fp);
            static VkFormat seen[32];
            static int ns;
            bool known = false;
            for (int k = 0; k < ns; k++) known |= seen[k] == tb->format;
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && !known && ns < 32) {
                seen[ns++] = tb->format;
                LOG("[nofilter] format %d %ux%u sampled linear (no linear filtering here): fs %llx stage %d\n", tb->format,
                    tb->width, tb->height, (unsigned long long)st.program->stages[1].code_addr, stage);
            }
        }
    }
    if (st_->legacy_stencil_compare) {
        NvnTexture *t = ent && ent->tex ? (naliases || nstale ? alias_read(ent->tex) : ent->tex) : NULL;
        const TexBackend *b = t ? t->backend : NULL;
        VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!view || view == vk_dummy_view(st_->type) || !b || (b->aspect & both) != both ||
            b->samples != 1 || !vk_tex_depth_compare(t))
            FATAL("[vk] legacy stencil/comparison real depth view unavailable; refusing dummy\n");
        static bool legacy_logged;
        if (!legacy_logged) {
            legacy_logged = true;
            LOG("[vk] legacy stencil/comparison compatibility, accuracy unqualified\n");
        }
    } else if (st_->shadow)
        shadow_slot_check(stage, st_, i, h, tic, ent, ent && ent->tex ? (naliases ? alias_read(ent->tex) : ent->tex) : NULL, &view);
    if (!view && st_->is_uint) FATAL("[vk] integer sampled view unavailable; refusing float dummy\n");
    bool null_view_fallback = !view, sampler_default = !smp;
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(!st_->is_uint);
    sampling_trace_binding(stage, shader_hash, h, st_, ent, trace_sampler, sampler_default, null_view_fallback);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Storage-image writes use real descriptor-pool textures, never sampled-image
// dummies. Track ownership per operation before descriptor-set cache lookup.
static NvnTexture *storage_written[64];
static int storage_nwritten;
static NvnTexEntry *storage_image_entry(int stage, const ShaderTexture *image) {
    if (stage != 5 || !image->is_image)
        FATAL("storage image: unsupported shader stage or resource kind");
    uint32_t handle;
    if (image->bindless) {
        // Maxwell cbuf 2 contains the driver's image handle table at 0x120,
        // eight bytes per image. Other bound cbufs match cbuf_info's k-3 map.
        uint32_t k = image->bindless_cbuf;
        uint64_t off = image->bindless_offset;
        if (k == 2) {
            if (off < 0x120 || (off - 0x120) % 8 || (off - 0x120) / 8 >= 8)
                FATAL("storage image: unsupported driver image-table offset");
            handle = (uint32_t)st.img[stage][(off - 0x120) / 8];
        } else {
            if (k < 3 || k - 3 >= 18)
                FATAL("storage image: unsupported bindless constant buffer %u", k);
            Range r = st.ubuf[stage][k - 3];
            if (!r.addr || off > r.size || r.size - off < sizeof handle ||
                r.addr > UINT64_MAX - off || r.addr + off > UINTPTR_MAX - sizeof handle)
                FATAL("storage image: bindless handle outside bound constant buffer");
            memcpy(&handle, (const void *)(uintptr_t)(r.addr + off), sizeof handle);
        }
    } else {
        uint32_t off = image->handle_offset;
        if (off % 4 || off / 4 >= 8)
            FATAL("storage image: bound image slot out of range");
        handle = (uint32_t)st.img[stage][off / 4];
    }
    uint32_t tic = handle & 0xfffffu;
    // GetImageHandle(0) legitimately addresses a registered pool entry zero.
    if (!st.tpool || !st.tpool->entries ||
        st.tpool->count <= 0 || tic >= (uint32_t)st.tpool->count)
        FATAL("storage image: missing or out-of-range texture handle");
    NvnTexEntry *entry = &((NvnTexEntry *)st.tpool->entries)[tic];
    if (!entry->tex) FATAL("storage image: texture is unregistered or finalized");
    return entry;
}
static VkDescriptorImageInfo storage_image_info(int stage, ShaderInfo *si, uint32_t i) {
    if (i >= (uint32_t)si->ntex || i >= 64)
        FATAL("storage image: reflected binding has no shader resource");
    ShaderTexture *image = &si->tex[i];
    NvnTexEntry *entry = storage_image_entry(stage, image);
    if (in_pass) FATAL("storage image: preparation inside rendering is unsupported");
    alias_before_write(entry->tex, 0, "storage image");
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageView view = vk_storage_view(entry->tex, &entry->view, entry->has_view, image->type, &format);
    if (!view || vk_storage_format_kind(format) != 0)
        FATAL("storage image: unsupported view or non-floating-point image format %d", format);
    int j = 0;
    while (j < storage_nwritten && storage_written[j] != entry->tex) j++;
    if (j == storage_nwritten) {
        if (storage_nwritten >= 64) FATAL("storage image: written texture capacity exceeded");
        storage_written[storage_nwritten++] = entry->tex;
    }
    return (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
}
static void storage_images_finish(VkCommandBuffer cb) {
    if (!storage_nwritten) return;
    // The recorded dispatch now owns these contents. Later sampled bindings
    // must retain its result instead of uploading stale guest texture bytes.
    for (int i = 0; i < storage_nwritten; i++) renderer_mark_gpu_owned(storage_written[i]);
    vk_full_barrier(cb);
    storage_nwritten = 0;
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

// descriptor sets written in the pending submission, by a hash of their writes (stage_set)
#define SET_CACHE 1024
static struct { uint64_t key, seq; VkDescriptorSet set; } set_cache[SET_CACHE];

// (set reuse) what a set holds, one entry per descriptor written, in stage_set's order (fixed per program)
typedef struct { uint32_t binding, type; uint64_t a, b, c; } SetEnt;
typedef struct SetCache { uint64_t seq; uint32_t view_gen, ds_gen; int n; VkDescriptorSet set; SetEnt e[MAX_BIND]; } SetCache;

// dyn / ndyn: the set's dynamic offsets are appended at dyn[*ndyn] (binding order, as vkCmdBindDescriptorSets takes
// them); NULL for programs without dynamic buffers (compute).
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[RECOLOUR_TINT_FLOATS], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, uint32_t *ndyn) {
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    uint32_t dbind[MAX_BIND], doff[MAX_BIND];
    uint32_t readonly_table[GMAP_MAX + 1][4];
    VkDescriptorBufferInfo readonly_buffer = {0};
    if (si->uses_gmem_readonly) readonly_buffer = readonly_mirror_info(stage, readonly_table);
    int n = 0, nd = 0;
    int sg = stage == 1 ? 1 : 0;
    bool lean = cbuf_lean();
    float rs[66][4];  // ([65]: mirror mode, renderer.m) (lean: only rows 0, 1..ntex and 65 are set and read)
    if (!lean) memset(rs, 0, sizeof rs);
    rs[0][0] = rs[0][1] = 1; rs[0][2] = rs[0][3] = 0;
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w; rs[65][1] = rs[65][2] = rs[65][3] = 0;
    if (si->uses_gmem && g_debug_frame && getenv("LARECOMP_LOG_SBUF")) {  // (debug) a gmem shader's storage buffers
        LOG("[sbuf] stage %d code %llx%s:", stage, (unsigned long long)st.program->stages[stage].code_addr,
            si->uses_gmem_store ? " (stores)" : "");
        for (int i = 0; i < 16; i++)
            if (st.sbuf[stage][i].addr) LOG(" %d:%llx+%llx", i, (unsigned long long)st.sbuf[stage][i].addr, (unsigned long long)st.sbuf[stage][i].size);
        LOG("\n");
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (b->binding < 32) FATAL("storage image: reflected binding below resource base");
            ii[n] = storage_image_info(stage, si, b->binding - 32);
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2], fr[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) {
                ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1];
                fr[0] = tex_fr[sg][ti][0]; fr[1] = tex_fr[sg][ti][1];
            }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh, fr, p->fp[sg].hash);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; fr[0] = fr[1] = 1; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
                tex_fr[sg][ti][0] = fr[0]; tex_fr[sg][ti][1] = fr[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (is_ubo(b->type) && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = si->uses_gmem_readonly ? readonly_buffer
                    : b->binding != GFAST_BUFFER_INDEX ? gmem_info()
                    : vk_gmem_mirror() ? (mirror_init(), (VkDescriptorBufferInfo){mir_buf, 0, MIR_SIZE}) : gfast_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || !is_ubo(b->type) || (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX &&
             !(b->binding == GRO_MAP_BUFFER_INDEX && si->uses_gmem_readonly))) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GRO_MAP_BUFFER_INDEX && si->uses_gmem_readonly) {
            uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
            VkSpan map = vk_transient_copy(readonly_table, sizeof readonly_table, align > 256 ? align : 256);
            readonly_mirror_flush_span(map, sizeof readonly_table);
            bi[n] = ubo_span(map, sizeof readonly_table);
        } else if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            uint32_t mt[(GMAP_MAX + 1) * 4];
            if (vk_gmem_mirror()) {
                if (stage < 2 && mir_graphics_prepared) {
                    if (mir_graphics_seq != vk_pending_seq()) FATAL("[vk] graphics mirror submission changed before draw\n");
                    memcpy(mt, mir_graphics_tables[stage], sizeof mt);
                } else mirror_table(stage, si->uses_gmem_store, stage == 5 ? vk_cmd() : NULL, mt);
            }
            bi[n] = ubo_span(vk_transient_copy(vk_gmem_mirror() ? mt : gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) {  // scale x, y, content part x, y
                rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; rs[1 + t][2] = tex_fr[sg][t][0]; rs[1 + t][3] = tex_fr[sg][t][1];
            }
            if (lean) bi[n] = ubo_span(rscale_dedupe(sg, (const float (*)[4])rs, si->ntex < 64 ? si->ntex : 64), sizeof rs);
            else bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[RECOLOUR_TINT_FLOATS] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, sizeof zero, 256), sizeof zero);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    // dynamic buffers: the offset moves to the bind (the descriptor keeps buffer and range)
    for (int i = 0; i < n; i++)
        if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            int j = nd++;  // (insertion by binding: the order vkCmdBindDescriptorSets takes the offsets in)
            while (j > 0 && dbind[j - 1] > w[i].dstBinding) { dbind[j] = dbind[j - 1]; doff[j] = doff[j - 1]; j--; }
            dbind[j] = w[i].dstBinding;
            doff[j] = (uint32_t)bi[i].offset;
            bi[i].offset = 0;
        }
    if (nd) {
        if (!dyn || !ndyn) FATAL("stage_set: dynamic buffers without an offset array\n");
        memcpy(dyn + *ndyn, doff, (size_t)nd * sizeof doff[0]);
        *ndyn += (uint32_t)nd;
    }
    extern uint32_t vk_view_gen;
    SetCache *c = NULL;
    SetEnt e[MAX_BIND];
    if (p->reuse && set < 2) {  // the same contents as this program's last set of this index: bind that one again
        if (!p->scache[set]) p->scache[set] = calloc(1, sizeof *p->scache[set]);
        c = p->scache[set];
        memset(e, 0, (size_t)n * sizeof e[0]);  // (padding: compared with memcmp)
        for (int i = 0; i < n; i++) {
            e[i].binding = w[i].dstBinding;
            e[i].type = (uint32_t)w[i].descriptorType;
            if (w[i].pImageInfo) {
                e[i].a = (uint64_t)(uintptr_t)ii[i].sampler; e[i].b = (uint64_t)(uintptr_t)ii[i].imageView; e[i].c = (uint64_t)ii[i].imageLayout;
            } else {
                e[i].a = (uint64_t)(uintptr_t)bi[i].buffer; e[i].b = bi[i].offset; e[i].c = bi[i].range;
            }
        }
        if (c->set && c->seq == vk_pending_seq() && c->view_gen == vk_view_gen && c->ds_gen == ds_gen && c->n == n &&
            !memcmp(c->e, e, (size_t)n * sizeof e[0])) {
            prof_sets_reused++;
            return c->set;
        }
    }
    // The same set again in this submission from another program (same layout, same buffers / views / samplers): the
    // one written then - allocating and writing sets is a large part of a draw on phones (Turnip copies each
    // descriptor into its pool). (Dynamic offsets are the bind's: not part of it.) LARECOMP_VK_SET_CACHE=0: off.
    static int cache = -1;
    if (cache < 0) cache = !getenv("LARECOMP_VK_SET_CACHE") || atoi(getenv("LARECOMP_VK_SET_CACHE")) != 0;
    uint64_t h = 0, seq = 0;
    uint32_t slot = 0;
    if (cache) {
        uint64_t g[2] = {(uint64_t)(uintptr_t)layout, vk_view_gen | (uint64_t)ds_gen << 32};
        h = hash_words(0x51ed27, g, sizeof g);
        for (int i = 0; i < n; i++) {
            uint64_t a[4] = {w[i].dstBinding | (uint64_t)w[i].descriptorType << 32, 0, 0, 0};
            if (w[i].pImageInfo) a[1] = (uint64_t)w[i].pImageInfo->sampler, a[2] = (uint64_t)w[i].pImageInfo->imageView, a[3] = w[i].pImageInfo->imageLayout;
            else a[1] = (uint64_t)w[i].pBufferInfo->buffer, a[2] = w[i].pBufferInfo->offset, a[3] = w[i].pBufferInfo->range;
            h = hash_words(h, a, sizeof a);
        }
        h |= 1;
        seq = vk_pending_seq();
        slot = (uint32_t)(h ^ (h >> 29)) & (SET_CACHE - 1);
        if (set_cache[slot].key == h && set_cache[slot].seq == seq) {
            stat_set_hits++;
            VkDescriptorSet ds = set_cache[slot].set;
            if (c) {
                c->set = ds; c->seq = seq; c->view_gen = vk_view_gen; c->ds_gen = ds_gen; c->n = n;
                memcpy(c->e, e, (size_t)n * sizeof e[0]);
            }
            return ds;
        }
    }
    VkDescriptorSet ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) rec_UpdateDescriptorSets((uint32_t)n, w);
    prof_sets_new++;
    if (c) {
        c->set = ds; c->seq = vk_pending_seq(); c->view_gen = vk_view_gen; c->ds_gen = ds_gen; c->n = n;
        memcpy(c->e, e, (size_t)n * sizeof e[0]);
    }
    if (cache) set_cache[slot].key = h, set_cache[slot].seq = seq, set_cache[slot].set = ds;
    return ds;
}

// ================================================================== compute
struct CsProg { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; uint64_t code; };

static void build_compute(void *arg) {
    CsProg *cp = arg;
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t id = cp->p.fp[0].hash;
    int bs = build_begin("pipeline cs", cp->code, 0);
    build_diag_begin_compute(id, cp->code, cp->local);
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) {
        atomic_store(&cp->p.state, 2); build_end(bs);
        build_diag_done("pipeline cs", id, t0, 2); return;
    }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    if (!m) {
        LOG("[compute] cs %llx: shader module creation failed\n", (unsigned long long)cp->code);
        atomic_store(&cp->p.state, 2);
        build_end(bs);
        build_diag_done("pipeline cs", id, t0, 2);
        return;
    }
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    SLOW_FEEDBACK(ci, 1);
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&g_pipes_built, 1);
    SLOW_FEEDBACK_HIT();
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    build_end(bs);
    build_diag_done("pipeline cs", id, t0, atomic_load(&cp->p.state));
    if (r != VK_SUCCESS) LOG("[compute] cs %llx: pipeline failed: %d\n", (unsigned long long)cp->code, r);
    slow_build(t0, "pipeline cs", cp->code, 0);
}


static uint64_t cs_content_key(uint64_t code_hash, const uint32_t local[3]) {
    uint64_t h = code_hash;
    for (int i = 0; i < 3; i++) { h ^= local[i]; h *= 0x100000001B3ull; }
    return h;
}

static bool translate_compute(CsProg *cp, const uint8_t *code, size_t max, size_t constants, uint64_t addr) {
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    cp->si = maxwell_translate_sized(code, max, constants, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)addr);
        atomic_store(&cp->p.state, 2);
        return false;
    }
    return true;
}

typedef struct { CsProg *cp; uint8_t *snapshot; size_t code_bytes, constant_bytes; uint64_t stage_hash; } HsCsTranslate;
static void hs_cs_translate_job(void *arg) {
    HsCsTranslate *j = arg;
    CsProg *cp = j->cp;
    bool ok = translate_compute(cp, j->snapshot, j->code_bytes, j->constant_bytes, j->stage_hash);
    free(j->snapshot);  // ShaderInfo owns translated text from here on.
    pthread_mutex_lock(&hs_lock);
    hs_cs_pending_bytes -= j->code_bytes + j->constant_bytes;
    pthread_mutex_unlock(&hs_lock);
    free(j);
    if (ok) run_job_q(build_compute, cp, true);  // published zero-state object becomes ready with release-store in build_compute
}
static bool hs_cs_local_supported(const uint32_t local[3]) {
    uint64_t invocations = 1;
    for (int i = 0; i < 3; i++) {
        uint32_t limit = g_vk.props.limits.maxComputeWorkGroupSize[i];
        if (!limit || !local[i] || local[i] > limit || invocations > UINT64_MAX / local[i]) return false;
        invocations *= local[i];
    }
    return g_vk.props.limits.maxComputeWorkGroupInvocations &&
           invocations <= g_vk.props.limits.maxComputeWorkGroupInvocations;
}
static CsProg *hs_cs_replay(uint32_t record_size, const HsCsHeader *h, const uint8_t *payload, bool *fresh) {
    *fresh = false;
    if (!hs_cs_size_valid(record_size, h, payload) || !hs_cs_local_supported(h->local)) return NULL;
    uint32_t bytes = h->code_bytes + h->constant_bytes;
    uint64_t key = hs_cs_content_key(h);
    pthread_mutex_lock(&hs_lock);
    for (size_t i = 0; i < hs_cs_n; i++) {
        if (hs_cs_tab[i].key == key) { pthread_mutex_unlock(&hs_lock); return NULL; }
    }
    if (hs_cs_n == HS_CS_TAB) { pthread_mutex_unlock(&hs_lock); return NULL; }
    if (bytes > HS_CS_PENDING_BYTES - hs_cs_pending_bytes) { pthread_mutex_unlock(&hs_lock); return NULL; }
    HsCsTranslate *job = calloc(1, sizeof *job);
    CsProg *cp = calloc(1, sizeof *cp);
    uint8_t *copy = malloc(bytes);
    if (!job || !cp || !copy) {
        free(job); free(cp); free(copy); pthread_mutex_unlock(&hs_lock); return NULL;
    }
    memcpy(copy, payload, bytes);
    memcpy(cp->local, h->local, sizeof cp->local);
    cp->code = h->stage_hash;
    cp->p.fp[0].hash = h->stage_hash;  // diagnostic identity only; guest fingerprints live per address below
    cp->p.state = 0;
    *job = (HsCsTranslate){cp, copy, h->code_bytes, h->constant_bytes, h->stage_hash};
    size_t slot = hs_cs_n++;
    hs_cs_tab[slot].key = key; hs_cs_tab[slot].stage_hash = h->stage_hash;
    hs_cs_tab[slot].code_bytes = h->code_bytes; hs_cs_tab[slot].constant_bytes = h->constant_bytes;
    memcpy(hs_cs_tab[slot].local, h->local, sizeof h->local);
    hs_cs_tab[slot].shader_key = h->key; hs_cs_tab[slot].cp = cp;
    hs_cs_pending_bytes += bytes;
    pthread_mutex_unlock(&hs_lock);
    *fresh = true;
    run_translate_q(hs_cs_translate_job, job, true);
    return cp;
}
static CsProg *hs_cs_take(uint64_t stage_hash, uint32_t code_bytes, uint32_t constant_bytes, const uint32_t local[3], const ShaderKey *key) {
    HsCsHeader h = {.stage_hash = stage_hash, .code_bytes = code_bytes, .constant_bytes = constant_bytes,
                    .local = {local[0], local[1], local[2]}, .key = *key};
    uint64_t k = hs_cs_content_key(&h);
    pthread_mutex_lock(&hs_lock);
    CsProg *cp = NULL;
    for (size_t i = 0; i < hs_cs_n; i++) {
        if (hs_cs_tab[i].key != k || hs_cs_tab[i].stage_hash != stage_hash ||
            hs_cs_tab[i].code_bytes != code_bytes || hs_cs_tab[i].constant_bytes != constant_bytes ||
            memcmp(hs_cs_tab[i].local, local, sizeof hs_cs_tab[i].local) ||
            memcmp(&hs_cs_tab[i].shader_key, key, sizeof *key)) continue;
        CsProg *candidate = hs_cs_tab[i].cp;
        if (atomic_load_explicit(&candidate->p.state, memory_order_acquire) == 1) cp = candidate;
        break;
    }
    pthread_mutex_unlock(&hs_lock);
    return cp;
}

static struct CsEnt { uint64_t addr; uint32_t local[3], code_bytes, constant_bytes; CsProg *cp; ShaderFp fp; } g_cs_tab[256];
static int hs_cs_state(CsProg *cp) { return atomic_load_explicit(&cp->p.state, memory_order_acquire); }
static void cs_remember_use(const NvnShaderStage *ss, const uint32_t local[3], CsProg *cp) {
    if (g_cs_n >= 256) return;
    struct CsEnt *e = &g_cs_tab[g_cs_n++];
    e->addr = ss->code_addr; memcpy(e->local, local, sizeof e->local);
    e->code_bytes = ss->const_offset; e->constant_bytes = ss->const_size; e->cp = cp;
    memset(&e->fp, 0, sizeof e->fp);  // vk_exec_reset clears g_cs_n; reused entries need a fresh fingerprint
    shader_fp_check(&e->fp, ss, "cs");
}

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    uint32_t local[3];
    for (int i = 0; i < 3; i++) {
        local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!local[i]) local[i] = 1;
    }
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++)
        if (tab[i].addr == ss->code_addr && !memcmp(tab[i].local, local, sizeof local) &&
            tab[i].code_bytes == ss->const_offset && tab[i].constant_bytes == ss->const_size) {
            shader_fp_check(&tab[i].fp, ss, "cs");
            return tab[i].cp;
        }
    uint64_t ch = shader_stage_hash(ss);
    ShaderKey compute_key = {0}; compute_key.glsl = 1;
    // Persist only a bounded immutable snapshot whose exact bytes hash to the live stage identity.
    // Failure to prove/copy the declared code+C1 span leaves normal foreground translation intact.
    if (headstart_on() && ch && ss->const_offset) {
        enum NvnShaderSnapshotResult why;
        const uint8_t *snapshot = hs_capture_stage(ss, ss->const_offset, ss->const_size, &why);
        if (snapshot && shader_code_hash(snapshot, (size_t)ss->const_offset + ss->const_size) == ch) {
            HsCsHeader h = {.stage_hash = ch, .local = {local[0], local[1], local[2]},
                            .code_bytes = ss->const_offset, .constant_bytes = ss->const_size, .key = compute_key};
            hs_cs_record_queue(&h, snapshot);
            NvnDiscoveryRecipe recipe = {.rec = {.kind = 'C', .vs = ch,
                .local = {local[0], local[1], local[2]}}, .vkey = compute_key,
                .code = {snapshot, NULL}, .code_bytes = {ss->const_offset, 0},
                .constant_bytes = {ss->const_size, 0}};
            discovery_record(&recipe, ch);
        }
    }
    CsProg *cp = ch && prep_cs_n ? PREP_GET(prep_cs, cs_content_key(ch, local)) : NULL;  // prepared before the game
    if (cp) {
        cs_remember_use(ss, local, cp);
        return cp;
    }
    cp = ch && ss->const_offset ? hs_cs_take(ch, ss->const_offset, ss->const_size, local, &compute_key) : NULL;  // ready replay only; never wait on head-start work
    if (cp) {
        cs_remember_use(ss, local, cp);
        return cp;
    }
    if (shader_rec_recording()) {
        ShaderRec rec = {.kind = 'C', .vs = ch, .local = {local[0], local[1], local[2]}};
        shader_rec_note(&rec);  // only variants the game must build; never on a prepared/replay hit
    }
    shader_first_visit("compute", ch, 0, cs_content_key(ch, local));
    cp = calloc(1, sizeof *cp);
    cp->code = ss->code_addr;  // (for foreground build logs)
    memcpy(cp->local, local, sizeof local);
    cp->p.fp[0].hash = ch;     // build diagnostic identity; address-specific stale fingerprints live in g_cs_tab
    cs_remember_use(ss, local, cp);
    if (!translate_compute(cp, (const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000,
                           ss->const_offset ? ss->const_size : SIZE_MAX, ss->code_addr))
        return cp;
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

// ================================================================== shader preparation (shader_prep.h)
// Each record: translated here (one thread, the translator isn't re-entrant), compiled and built on the worker
// threads (run_job), kept in the prep_* tables for the game.
typedef struct { void (*fn)(void *); void *arg; } PrepJob;
static void prep_job_run(void *a) {
    PrepJob *j = a;
    j->fn(j->arg);
    free(j);
    prep_pending--;
}
static void prep_job(void (*fn)(void *), void *arg) {
    PrepJob *j = malloc(sizeof *j);
    j->fn = fn, j->arg = arg;
    prep_pending++;
    run_job(prep_job_run, j);
}

static _Atomic int prep_pipe_dups;  // records whose pipeline another record already made
bool renderer_prepare_begin(void) { return g_vk.dev != VK_NULL_HANDLE; }
int renderer_prepare_pending(void) { return prep_pending; }
static void gpl_probe(void);
static void gpl_probe_note(Pipe *pp);
void renderer_prepare_end(void) {
    vk_pipeline_cache_save();  // (otherwise only kept at exit)
    static bool logged;        // (the counts: once per process, the background preparation runs once)
    if (!logged) {
        logged = true;
        pthread_mutex_lock(&prep_lock);
        size_t n = prep_pipes_n;
        pthread_mutex_unlock(&prep_lock);
        LOG("[vk] prepared %zu pipelines (%d records shared one, vertex input %s)\n", n, atomic_load(&prep_pipe_dups),
            g_vk.vids ? "dynamic" : "in each pipeline");
        if (g_vk.gpl_probe) gpl_probe();
    }
}

typedef struct { Pipe *pp; ShaderRec r; } PrepPipe;
// Both legacy preparation and durable recipes derive a fresh host key from
// original guest inputs and CURRENT translated interfaces/capability mappings.
static void prepare_pipe_key(PipeKey *key, Prog *pr, const ShaderRec *r) {
    // the pass's attachment formats as begin_pass makes them for these targets
    VkFormat color[8] = {0}, depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    uint32_t ncolor = r->ncolor;
    for (uint32_t i = 0; i < ncolor && i < 8; i++) {
        if (!r->color[i]) continue;
        VkFormat b = vk_image_format(r->color[i], NULL);
        color[i] = r->cview[i] && vk_format(r->color[i]) == b ? vk_format(r->cview[i]) : b;
    }
    while (ncolor && !r->color[ncolor - 1]) ncolor--;
    if (r->depth && renderer_format_is_depth(r->depth)) {
        VkImageAspectFlags asp;
        VkFormat b = vk_image_format(r->depth, &asp);
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) depth = b;
        if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = b;
    }
    const PipeIn in = {r->attrib, r->nattrib, r->stream, r->nstream, r->color, r->channel_mask, r->blend_on, r->blend,
                       r->a2c, r->topo, r->poly_fill};
    pipe_key_fill(key, pr, &in, color, ncolor, depth, stencil, r->samples ? r->samples : 1, NULL);
}
static void prep_pipeline(void *arg) {
    PrepPipe *a = arg;
    Prog *pr = a->pp->k.prog;
    // its program's compile was queued before (the queue is first in, first out): wait for it to finish
    while (atomic_load(&pr->state) == 0) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    if (atomic_load(&pr->state) != 1) { atomic_store(&a->pp->state, 2); free(a); return; }
    prepare_pipe_key(&a->pp->k, pr, &a->r);
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &a->pp->k, sizeof a->pp->k);
    if (!ph) ph = 1;
    // Another record makes the same pipeline (with dynamic vertex input: every record differing only in its vertex
    // layout): built once. Claimed before the build, so records on other worker threads at the same time see it too
    // (the game skips a draw whose prepared pipeline is still building or failed, as with one it builds itself).
    pthread_mutex_lock(&prep_lock);
    bool dup = prep_get(prep_pipes, prep_pipes_cap, ph) != NULL;
    if (!dup) prep_put(&prep_pipes, &prep_pipes_cap, &prep_pipes_n, ph, a->pp);
    pthread_mutex_unlock(&prep_lock);
    if (dup) { atomic_fetch_add(&prep_pipe_dups, 1); free(a->pp); free(a); return; }
    build_pipeline(a->pp);
    if (g_vk.gpl_probe) gpl_probe_note(a->pp);
    free(a);
}

bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vsb, const ShaderBlob *fsb) {
    if (r->kind == 'C') {
        uint64_t ck = cs_content_key(r->vs, r->local);
        if (PREP_GET(prep_cs, ck)) return true;
        CsProg *cp = calloc(1, sizeof *cp);
        cp->code = r->vs;  // (prepared: its content hash, for the build logs)
        memcpy(cp->local, r->local, sizeof cp->local);
        if (!translate_compute(cp, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000,
                               vsb->const_offset ? vsb->const_size : SIZE_MAX, 0)) return false;
        PREP_PUT(prep_cs, ck, cp);
        prep_job(build_compute, cp);
        return true;
    }
    // the keys the draw path makes for this state (draw_)
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = (r->flags & SR_FLIP_Y) != 0;
    vkey.depth_minus_one = (r->flags & SR_DEPTH_MINUS_ONE) != 0;
    vkey.flip_x = fkey.flip_x = (r->flags & SR_FLIP_X) != 0;
    for (int i = 0; i < r->nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(r->attrib[i] & 0xFF);
    for (int i = 0; i < r->ncolor && i < 8; i++) if (r->color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(r->color[i]);
    uint64_t ck = prog_content_key(r->vs, r->fs, &vkey, &fkey);
    Prog *p = PREP_GET(prep_progs, ck);
    if (!p) {
        p = calloc(1, sizeof *p);
        p->code[0] = (uintptr_t)vsb->code;
        p->code[1] = (uintptr_t)fsb->code;
        p->cmax[0] = vsb->const_offset ? vsb->const_offset : 0x10000;
        p->cmax[1] = fsb->const_offset ? fsb->const_offset : 0x10000;
        p->vkey = vkey;
        p->fkey = fkey;
        p->prep_vh = r->vs, p->prep_fh = r->fs;
        bool ok = translate_program_now(p);
        p->code[0] = p->code[1] = 0;  // (the game files' copies are gone after this call)
        PREP_PUT(prep_progs, ck, p);  // (failed ones too: the game then skips them as it would its own)
        if (!ok) return false;
        prep_job(compile_program, p);
    }
    if (atomic_load(&p->state) == 2) return false;
    PrepPipe *a = calloc(1, sizeof *a);
    a->pp = calloc(1, sizeof *a->pp);
    a->pp->k.prog = p;
    a->r = *r;
    prep_job(prep_pipeline, a);
    return true;
}

// ---- durable guest discovery: portable inputs, never compiled host keys.
// Queue ownership is independent of the store mutex: guest draws never wait for
// file I/O. The writer batches at most one second, and shutdown joins it before
// the device/cache teardown. All queued pointers refer to our own copied bytes.
#define DISCOVERY_BYTES (UINT64_C(32) * 1024 * 1024)
#define DISCOVERY_RECORDS 4096
#define DISCOVERY_JOBS 64
#define DISCOVERY_SEEN 8192
static pthread_mutex_t discovery_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t discovery_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t discovery_store_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnDiscoveryStore discovery_store;
static bool discovery_store_tried, discovery_store_ok, discovery_replace_pending;
typedef struct DiscoveryJob {
    struct DiscoveryJob *next;
    NvnDiscoveryRecipe recipe;
    uint64_t bytes;
    uint8_t payload[];
} DiscoveryJob;
static DiscoveryJob *discovery_head, *discovery_tail;
static pthread_t discovery_thread;
static bool discovery_started, discovery_stopping, discovery_joining, discovery_joined, discovery_unavailable;
static uint64_t discovery_pending_bytes;
static unsigned discovery_pending;
static uint64_t discovery_seen[DISCOVERY_SEEN];
static size_t discovery_retained_bytes; // immutable program closures retained for this process

static bool discovery_recipe_replayable(const NvnDiscoveryRecipe *r) {
    if (r->rec.kind != 'C') return hs_program_replayable(r->code[0], r->code_bytes[0],
        r->constant_bytes[0], r->code[1], r->code_bytes[1], r->constant_bytes[1]);
    // Match maxwell_translate_sized's compute entry point: raw instructions or
    // the optional SDK header, with scheduling phase relative to that entry.
    if (!r->code[0] || r->code_bytes[0] < 4) return false;
    uint32_t magic;
    memcpy(&magic, r->code[0], sizeof magic);
    uint64_t start = magic == UINT32_C(0x12345679) ? 0x100 : 0;
    if (r->code_bytes[0] < start) return false;
    for (uint64_t off = start, instruction = 0; off + 8 <= r->code_bytes[0]; off += 8, instruction++) {
        if (instruction % 4 == 0) continue;
        uint64_t word;
        memcpy(&word, r->code[0] + (size_t)off, sizeof word);
        if ((word & UINT64_C(0xfff0000000000000)) == UINT64_C(0xe250000000000000) &&
            !r->constant_bytes[0]) return false;
    }
    return true;
}

// Store operations run only on the writer/headstart thread with this mutex held.
static bool discovery_open_locked(void) {
    if (!discovery_store_tried) {
        discovery_store_tried = true;
        uint8_t game[32];
        if (shared_gpu_game_identity(game)) {
            mkdir("cache", 0755);
            NvnDiscoveryOpenResult status = nvn_discovery_open(&discovery_store,
                "cache/shader_discovery.bin", game, shared_sha256_bytes,
                DISCOVERY_BYTES, DISCOVERY_RECORDS);
            discovery_store_ok = status == NVN_DISCOVERY_OPEN_EMPTY ||
                status == NVN_DISCOVERY_OPEN_LOADED || status == NVN_DISCOVERY_OPEN_RECOVERED;
            if (status == NVN_DISCOVERY_OPEN_INCOMPATIBLE || status == NVN_DISCOVERY_OPEN_CORRUPT ||
                status == NVN_DISCOVERY_OPEN_LIMIT) {
                // Keep old bytes intact until a newly learned valid recipe can
                // be committed by the codec's atomic replacement transaction.
                discovery_store_ok = nvn_discovery_init_empty(&discovery_store,
                    "cache/shader_discovery.bin", game, shared_sha256_bytes,
                    DISCOVERY_BYTES, DISCOVERY_RECORDS);
                discovery_replace_pending = discovery_store_ok;
            }
            LOG("[vk] shader discovery: open %d, %zu portable recipes%s\n", status,
                discovery_store_ok ? nvn_discovery_count(&discovery_store) : 0,
                discovery_replace_pending ? "; old file preserved until new recipes commit" :
                discovery_store_ok ? "" : "; existing file preserved, foreground retained");
        }
    }
    return discovery_store_ok;
}
static bool discovery_commit_locked(void) {
    if (discovery_replace_pending && !nvn_discovery_count(&discovery_store)) return true;
    bool changed = discovery_store.dirty;
    bool ok = nvn_discovery_commit(&discovery_store);
    if (ok) discovery_replace_pending = false;
    if (ok && changed) {
        char directory[4096];
        int n = snprintf(directory, sizeof directory, "%s/shader-profiles", rt_data_root());
        if (n > 0 && n < (int)sizeof directory &&
            !shader_preparation_export(&discovery_store, directory))
            LOG("[vk] shader preparation: metadata export deferred; learned journal retained\n");
    }
    return ok;
}
#ifdef __ANDROID__
typedef struct {
    JNIEnv *env;
    jobject manager;
    AAsset *asset;
} DiscoveryCatalogAsset;
static void discovery_catalog_close(void *context) {
    DiscoveryCatalogAsset *catalog = context;
    if (catalog->asset) AAsset_close(catalog->asset);
    // All reader operations run synchronously on the same coordinator thread.
    // Keep Java's AssetManager alive until the native AAsset has been closed.
    if (catalog->manager) (*catalog->env)->DeleteLocalRef(catalog->env, catalog->manager);
    memset(catalog, 0, sizeof *catalog);
}
static ShaderProfileResult discovery_catalog_open(void *context, const char *asset, uint64_t *size) {
    DiscoveryCatalogAsset *catalog = context;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return SHADER_PROFILE_ERROR;
    catalog->env = env;
    jobject activity = NULL;
    jclass activity_class = NULL;
    ShaderProfileResult result = SHADER_PROFILE_ERROR;
    if ((*env)->ExceptionCheck(env)) goto done;
    activity = (jobject)SDL_GetAndroidActivity();
    if ((*env)->ExceptionCheck(env) || !activity) goto done;
    activity_class = (*env)->GetObjectClass(env, activity);
    if ((*env)->ExceptionCheck(env) || !activity_class) goto done;
    jmethodID get_assets = (*env)->GetMethodID(env, activity_class, "getAssets", "()Landroid/content/res/AssetManager;");
    if ((*env)->ExceptionCheck(env) || !get_assets) goto done;
    catalog->manager = (*env)->CallObjectMethod(env, activity, get_assets);
    if ((*env)->ExceptionCheck(env) || !catalog->manager) goto done;
    AAssetManager *manager = AAssetManager_fromJava(env, catalog->manager);
    if ((*env)->ExceptionCheck(env) || !manager) goto done;
    // APK assets only. SDL_IOFromFile(relative) first checks internal storage,
    // which could otherwise mask a bundled profile with a stale imported file.
    catalog->asset = AAssetManager_open(manager, asset, AASSET_MODE_RANDOM);
    if (!catalog->asset) { result = SHADER_PROFILE_MISSING; goto done; }
    off64_t length = AAsset_getLength64(catalog->asset);
    if (length < 0) goto done;
    *size = (uint64_t)length;
    result = SHADER_PROFILE_LOADED;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (activity_class) (*env)->DeleteLocalRef(env, activity_class);
    if (activity) (*env)->DeleteLocalRef(env, activity);
    if (result != SHADER_PROFILE_LOADED) discovery_catalog_close(context);
    return result;
}
static bool discovery_catalog_read(void *context, size_t offset, void *bytes, size_t size) {
    DiscoveryCatalogAsset *catalog = context;
    if (!catalog->asset || offset > SHADER_PROFILE_MAX_BYTES || size > 65536) return false;
    off64_t length = AAsset_getLength64(catalog->asset);
    if (length < 0 || offset > (uint64_t)length || size > (uint64_t)length - offset) return false;
    return AAsset_seek64(catalog->asset, (off64_t)offset, SEEK_SET) == (off64_t)offset &&
        AAsset_read(catalog->asset, bytes, size) == (int)size;
}
#else
typedef SDL_IOStream *DiscoveryCatalogAsset;
static ShaderProfileResult discovery_catalog_open(void *context, const char *asset, uint64_t *size) {
    SDL_IOStream **stream = context;
    *stream = SDL_IOFromFile(asset, "rb");
    if (!*stream) return SHADER_PROFILE_MISSING;
    Sint64 length = SDL_GetIOSize(*stream);
    if (length < 0) {
        SDL_CloseIO(*stream);
        *stream = NULL;
        return SHADER_PROFILE_ERROR;
    }
    *size = (uint64_t)length;
    return SHADER_PROFILE_LOADED;
}
static bool discovery_catalog_read(void *context, size_t offset, void *bytes, size_t size) {
    SDL_IOStream *stream = *(SDL_IOStream **)context;
    return stream && SDL_SeekIO(stream, (Sint64)offset, SDL_IO_SEEK_SET) == (Sint64)offset &&
        SDL_ReadIO(stream, bytes, size) == size;
}
static void discovery_catalog_close(void *context) {
    SDL_IOStream **stream = context;
    if (*stream) SDL_CloseIO(*stream);
    *stream = NULL;
}
#endif
static void discovery_seed_profiles(void) {
    pthread_mutex_lock(&discovery_store_lock);
    if (discovery_open_locked()) {
        char directory[4096];
        int n = snprintf(directory, sizeof directory, "%s/shader-profiles", rt_data_root());
        if (n > 0 && n < (int)sizeof directory) {
            extern const char *g_romfs_dir;
            ShaderPreparationStats stats = {0};
            DiscoveryCatalogAsset asset = {0};
            ShaderProfileCatalogReader catalog = {discovery_catalog_open, discovery_catalog_read,
                discovery_catalog_close, &asset};
            bool seeded = shader_preparation_seed_catalog(&discovery_store, g_romfs_dir, directory,
                NULL, &catalog, &stats);
            if (!discovery_commit_locked())
                LOG("[vk] shader preparation: profile seed commit failed; foreground fallback retained\n");
            // Migrate an existing learned journal to profiles once. Warm profiles
            // skip both archive scanning and unchanged metadata export.
            if (!stats.appended && stats.profiles < 2 && discovery_store.count)
                shader_preparation_export(&discovery_store, directory);
            LOG("[vk] shader preparation profiles: %zu loaded, %zu new, %zu reused, %zu missing, "
                "%zu rejected, %zu incompatible; source %zu stages%s%s\n",
                stats.profiles, stats.appended, stats.reused, stats.missing, stats.rejected,
                stats.incompatible, stats.source_stages,
                stats.source_attempted ? stats.source_complete ? " indexed" : " partial/unavailable" : " not needed",
                seeded ? "" : "; foreground fallback retained");
            if (stats.catalog_profiles)
                LOG("[vk] shader catalog: %zu recipes, metadata %s\n", stats.catalog_recipes,
                    stats.catalog_persisted ? "retained" : "persistence deferred");
        }
    }
    pthread_mutex_unlock(&discovery_store_lock);
}
static void *discovery_writer(void *unused) {
    (void)unused;
    plat_thread_name("shader-recipes");
    for (;;) {
        pthread_mutex_lock(&discovery_lock);
        while (!discovery_head && !discovery_stopping)
            pthread_cond_wait(&discovery_cond, &discovery_lock);
        if (!discovery_head && discovery_stopping) {
            pthread_mutex_unlock(&discovery_lock);
            break;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec++;
        while (!discovery_stopping && discovery_pending < DISCOVERY_JOBS) {
            int result = pthread_cond_timedwait(&discovery_cond, &discovery_lock, &deadline);
            if (result == ETIMEDOUT) break;
        }
        DiscoveryJob *batch = discovery_head;
        discovery_head = discovery_tail = NULL;
        pthread_mutex_unlock(&discovery_lock);
        pthread_mutex_lock(&discovery_store_lock);
        bool available = discovery_open_locked();
        unsigned appended = 0;
        for (DiscoveryJob *job = batch; job; job = job->next) {
            // Hash the immutable copy on this worker, not live guest memory.
            for (int i = 0; i < 2; ++i) {
                uint32_t n = job->recipe.code_bytes[i] + job->recipe.constant_bytes[i];
                uint64_t hash = n ? shader_code_hash(job->recipe.code[i], n) : 0;
                if (i) job->recipe.rec.fs = hash; else job->recipe.rec.vs = hash;
            }
            bool proved = discovery_recipe_replayable(&job->recipe);
            if (available && proved && nvn_discovery_append(&discovery_store, &job->recipe) == NVN_DISCOVERY_APPEND_APPENDED)
                appended++;
        }
        if (available && !discovery_commit_locked()) {
            discovery_store_ok = false;
            LOG("[vk] shader discovery: persistence failed; foreground compilation retained\n");
        }
        if (appended) LOG("[vk] shader discovery: learned %u recipes, %zu retained\n", appended,
            nvn_discovery_count(&discovery_store));
        pthread_mutex_unlock(&discovery_store_lock);
        while (batch) {
            DiscoveryJob *next = batch->next;
            pthread_mutex_lock(&discovery_lock);
            discovery_pending--;
            discovery_pending_bytes -= batch->bytes;
            pthread_mutex_unlock(&discovery_lock);
            free(batch);
            batch = next;
        }
    }
    return NULL;
}
static void discovery_record(const NvnDiscoveryRecipe *recipe, uint64_t content) {
    if (!headstart_on() || !content) return;
    uint64_t bytes = 0;
    for (int i = 0; i < 2; ++i) {
        uint64_t n = (uint64_t)recipe->code_bytes[i] + recipe->constant_bytes[i];
        if (n > KERNEL_READABLE_COPY_MAX || n > DISCOVERY_BYTES - bytes ||
            (n && !recipe->code[i])) return;
        bytes += n;
    }
    if (!recipe->code_bytes[0]) return;
    // This is an in-process coverage memo only. Wire identities and dedup are
    // validated independently by the codec; a full memo retains normal fallback.
    uint64_t key = hash_words(content, &recipe->rec, sizeof recipe->rec);
    key = hash_words(key, &recipe->vkey, sizeof recipe->vkey);
    key = hash_words(key, &recipe->fkey, sizeof recipe->fkey);
    if (!key) key = 1;
    unsigned slot = (unsigned)(key ^ (key >> 32)) & (DISCOVERY_SEEN - 1), probes = 0;
    pthread_mutex_lock(&discovery_lock);
    while (probes < DISCOVERY_SEEN && discovery_seen[slot] && discovery_seen[slot] != key) {
        slot = (slot + 1) & (DISCOVERY_SEEN - 1); probes++;
    }
    if (probes == DISCOVERY_SEEN || discovery_seen[slot] || discovery_unavailable || discovery_stopping ||
        discovery_pending == DISCOVERY_JOBS || bytes > DISCOVERY_BYTES - discovery_pending_bytes) {
        pthread_mutex_unlock(&discovery_lock);
        return;
    }
    DiscoveryJob *job = calloc(1, sizeof *job + (size_t)bytes);
    if (!job) { pthread_mutex_unlock(&discovery_lock); return; }
    job->recipe = *recipe;
    job->bytes = bytes;
    uint8_t *at = job->payload;
    for (int i = 0; i < 2; ++i) {
        size_t n = (size_t)recipe->code_bytes[i] + recipe->constant_bytes[i];
        if (n) memcpy(at, recipe->code[i], n);
        job->recipe.code[i] = n ? at : NULL;
        at += n;
    }
    if (!discovery_started) {
        if (pthread_create(&discovery_thread, NULL, discovery_writer, NULL)) {
            discovery_unavailable = true;
            pthread_mutex_unlock(&discovery_lock); free(job); return;
        }
        discovery_started = true;
    }
    if (discovery_tail) discovery_tail->next = job; else discovery_head = job;
    discovery_tail = job;
    discovery_pending++;
    discovery_pending_bytes += bytes;
    discovery_seen[slot] = key;
    pthread_cond_signal(&discovery_cond);
    pthread_mutex_unlock(&discovery_lock);
}
static void discovery_finish(void) {
    pthread_mutex_lock(&discovery_lock);
    discovery_stopping = true;
    pthread_cond_broadcast(&discovery_cond);
    while (discovery_joining) pthread_cond_wait(&discovery_cond, &discovery_lock);
    bool join = discovery_started && !discovery_joined;
    if (join) discovery_joining = true;
    pthread_mutex_unlock(&discovery_lock);
    if (join) {
        pthread_join(discovery_thread, NULL);
        pthread_mutex_lock(&discovery_lock);
        discovery_joined = true;
        discovery_joining = false;
        pthread_cond_broadcast(&discovery_cond);
        pthread_mutex_unlock(&discovery_lock);
    }
    pthread_mutex_lock(&discovery_store_lock);
    if (discovery_store_ok && !discovery_commit_locked())
        LOG("[vk] shader discovery: final persistence failed\n");
    pthread_mutex_unlock(&discovery_store_lock);
}

// Exact input keys are supplied by the portable record; the fragment output
// interface is regenerated by translate_program_now after translating this VS.
// Reuse a strict-current headstart program if present, otherwise admit one fresh
// immutable closure. The temporary decoded recipe is never retained by a job.
static Prog *prepare_program_exact(const NvnDiscoveryRecipe *r, bool *fresh) {
    *fresh = false;
    uint64_t ch = prog_content_hash(r->code[0], r->code_bytes[0], r->constant_bytes[0],
        r->code[1], r->code_bytes[1], r->constant_bytes[1], &r->vkey, &r->fkey);
    Prog *p = hs_take(ch);
    if (p) return p;
    size_t n[2] = {(size_t)r->code_bytes[0] + r->constant_bytes[0],
                   (size_t)r->code_bytes[1] + r->constant_bytes[1]};
    size_t bytes = n[0] + n[1];
    if (bytes > DISCOVERY_BYTES - discovery_retained_bytes) return NULL;
    uint8_t *copy = malloc(bytes);
    p = calloc(1, sizeof *p);
    if (!copy || !p) { free(copy); free(p); return NULL; }
    memcpy(copy, r->code[0], n[0]);
    if (n[1]) memcpy(copy + n[0], r->code[1], n[1]);
    for (int i = 0; i < 2; ++i) {
        p->read_code[i] = n[i] ? copy + (i ? n[0] : 0) : NULL;
        p->code[i] = (uintptr_t)p->read_code[i];
        p->cmax[i] = r->code_bytes[i]; p->csize[i] = r->constant_bytes[i];
    }
    p->vkey = r->vkey; p->fkey = r->fkey; p->chash = ch; p->headstart = true;
    p->fp[0].hash = r->rec.vs; p->fp[1].hash = r->rec.fs;
    Prog *winner = hs_add(p, false, fresh);
    if (winner != p) { free(copy); free(p); return winner; }
    // hs_add deliberately returns an untracked foreground fallback when its
    // bounded table is full. Do not queue an unshareable preparation object.
    if (!*fresh || hs_take(ch) != p) { free(copy); free(p); *fresh = false; return NULL; }
    discovery_retained_bytes += bytes;
    return p;
}
// Keep a small number of recipe dependencies in flight. A builder is only
// given a pipeline after its modules are ready; no pool worker waits for work
// queued behind itself. The coordinator owns no decoded shader byte buffers.
#define DISCOVERY_REPLAY_WINDOW 16
typedef enum { DISCOVERY_REPLAY_FREE, DISCOVERY_REPLAY_PROGRAM,
    DISCOVERY_REPLAY_PIPELINE, DISCOVERY_REPLAY_COMPUTE } DiscoveryReplayPhase;
typedef struct {
    DiscoveryReplayPhase phase;
    size_t source_index;
    ShaderRec rec;
    HsCsHeader compute;
    Prog *program;
    Pipe *pipeline;
    CsProg *cs;
    bool owned;
} DiscoveryReplayEntry;
typedef struct { unsigned programs, pipelines, computes, reused, skipped, deferred; } DiscoveryReplayStats;
typedef enum { DISCOVERY_ADMITTED, DISCOVERY_SKIPPED, DISCOVERY_DEFERRED } DiscoveryAdmission;

static bool discovery_read_recipe(size_t index, NvnDiscoveryRecipe *recipe) {
    pthread_mutex_lock(&discovery_store_lock);
    bool read = nvn_discovery_read_at(&discovery_store, index, recipe);
    pthread_mutex_unlock(&discovery_store_lock);
    return read;
}
static HsCsHeader discovery_compute_header(const NvnDiscoveryRecipe *r) {
    return (HsCsHeader){.stage_hash = r->rec.vs,
        .local = {r->rec.local[0], r->rec.local[1], r->rec.local[2]},
        .code_bytes = r->code_bytes[0], .constant_bytes = r->constant_bytes[0], .key = r->vkey};
}
// Unlike foreground hs_cs_take, a preparation dependency may refer to a
// published object that is still building. Its terminal state is awaited by the
// coordinator, outside the translator and builder queues.
static CsProg *discovery_compute_find_locked(const HsCsHeader *h) {
    uint64_t content = hs_cs_content_key(h);
    for (size_t j = 0; j < hs_cs_n; ++j) {
        if (hs_cs_tab[j].key == content && hs_cs_tab[j].stage_hash == h->stage_hash &&
            hs_cs_tab[j].code_bytes == h->code_bytes && hs_cs_tab[j].constant_bytes == h->constant_bytes &&
            !memcmp(hs_cs_tab[j].local, h->local, sizeof h->local) &&
            !memcmp(&hs_cs_tab[j].shader_key, &h->key, sizeof h->key)) return hs_cs_tab[j].cp;
    }
    return NULL;
}
static DiscoveryAdmission discovery_admit_one(DiscoveryReplayEntry *entry,
    size_t index, DiscoveryReplayStats *stats) {
    NvnDiscoveryRecipe r = {0};
    if (!discovery_read_recipe(index, &r)) { stats->skipped++; return DISCOVERY_SKIPPED; }
    DiscoveryAdmission result = DISCOVERY_SKIPPED;
    if (!discovery_recipe_replayable(&r)) goto out;
    *entry = (DiscoveryReplayEntry){.source_index = index, .rec = r.rec};
    if (r.rec.kind == 'C') {
        HsCsHeader h = discovery_compute_header(&r);
        bool fresh = false;
        pthread_mutex_lock(&hs_lock);
        CsProg *cp = discovery_compute_find_locked(&h);
        pthread_mutex_unlock(&hs_lock);
        if (!cp) cp = hs_cs_replay((uint32_t)(sizeof h + h.code_bytes + h.constant_bytes),
            &h, r.code[0], &fresh);
        if (!cp) {
            // Another producer may have inserted this object after the lookup,
            // or consumed the temporary compute-copy budget. Only the latter
            // is retryable: invalid metadata, a full table and allocation
            // failure remain bounded cache misses, not infinite retry loops.
            pthread_mutex_lock(&hs_lock);
            cp = discovery_compute_find_locked(&h);
            uint64_t bytes = (uint64_t)h.code_bytes + h.constant_bytes;
            bool deferred = !cp && hs_cs_n < HS_CS_TAB && bytes <= HS_CS_PENDING_BYTES &&
                hs_cs_pending_bytes && bytes > HS_CS_PENDING_BYTES - hs_cs_pending_bytes;
            pthread_mutex_unlock(&hs_lock);
            if (!cp) {
                uint32_t size = (uint32_t)(sizeof h + bytes);
                if (deferred && hs_cs_size_valid(size, &h, r.code[0]) && hs_cs_local_supported(h.local)) {
                    result = DISCOVERY_DEFERRED;
                    stats->deferred++;
                }
                goto out;
            }
        }
        entry->phase = DISCOVERY_REPLAY_COMPUTE;
        entry->cs = cp;
        entry->compute = h;
        entry->owned = fresh;
        if (fresh) { atomic_fetch_add(&hs_total, 1); stats->computes++; }
        else stats->reused++;
    } else {
        bool fresh = false;
        Prog *p = prepare_program_exact(&r, &fresh);
        if (!p) goto out;
        entry->phase = DISCOVERY_REPLAY_PROGRAM;
        entry->program = p;
        entry->owned = fresh;
        if (fresh) {
            atomic_fetch_add(&hs_total, 1);
            run_translate_q(translate_program, p, true);
            stats->programs++;
        } else stats->reused++;
    }
    result = DISCOVERY_ADMITTED;
out:
    nvn_discovery_recipe_release(&r);
    if (result == DISCOVERY_SKIPPED) stats->skipped++;
    return result;
}
static void discovery_compute_writeback(const DiscoveryReplayEntry *entry) {
    // The store can grow/reallocate while replay is active. Re-read just this
    // successful source recipe, rather than retaining a pointer or second copy
    // of every compute payload through the complete pipeline build.
    NvnDiscoveryRecipe r = {0};
    if (!discovery_read_recipe(entry->source_index, &r)) return;
    HsCsHeader got = discovery_compute_header(&r);
    const HsCsHeader *h = &entry->compute;
    if (r.rec.kind == 'C' && got.stage_hash == h->stage_hash &&
        got.code_bytes == h->code_bytes && got.constant_bytes == h->constant_bytes &&
        !memcmp(got.local, h->local, sizeof h->local) && !memcmp(&got.key, &h->key, sizeof h->key) &&
        discovery_recipe_replayable(&r)) hs_record_compute_once(h, r.code[0]);
    nvn_discovery_recipe_release(&r);
}
static bool discovery_pump_one(DiscoveryReplayEntry *entry, DiscoveryReplayStats *stats) {
    if (entry->phase == DISCOVERY_REPLAY_COMPUTE) {
        int state = hs_cs_state(entry->cs);
        if (!state) return false;
        if (entry->owned) {
            if (state == 1) discovery_compute_writeback(entry);
            atomic_fetch_add(&hs_done, 1);
        }
        entry->phase = DISCOVERY_REPLAY_FREE;
        return true;
    }
    if (entry->phase == DISCOVERY_REPLAY_PROGRAM) {
        int state = atomic_load_explicit(&entry->program->state, memory_order_acquire);
        if (!state) return false;
        if (entry->owned) {
            if (state == 1) hs_record_program_closure(entry->program);
            atomic_fetch_add(&hs_done, 1);
        }
        entry->owned = false;
        if (state != 1) { entry->phase = DISCOVERY_REPLAY_FREE; stats->skipped++; return true; }
        PipeKey key;
        prepare_pipe_key(&key, entry->program, &entry->rec);
        uint64_t ph = pipe_key_hash(&key);
        Pipe *pp = hs_pipe_take(ph);
        if (!pp) pp = PREP_GET(prep_pipes, ph);
        if (!pp) {
            Pipe *candidate = calloc(1, sizeof *candidate);
            if (candidate) {
                candidate->k = key;
                pp = hs_pipe_put(ph, candidate, &entry->owned);
                if (pp != candidate) free(candidate);
            }
        }
        if (!pp) { entry->phase = DISCOVERY_REPLAY_FREE; stats->skipped++; return true; }
        entry->pipeline = pp;
        entry->phase = DISCOVERY_REPLAY_PIPELINE;
        if (entry->owned) {
            atomic_fetch_add(&hs_total, 1);
            run_job_q(build_pipeline, pp, true);
            stats->pipelines++;
        } else stats->reused++;
        return true;
    }
    if (entry->phase == DISCOVERY_REPLAY_PIPELINE) {
        int state = atomic_load_explicit(&entry->pipeline->state, memory_order_acquire);
        if (!state) return false;
        if (entry->owned) {
            if (state == 1) hs_record_pipe(&entry->pipeline->k);
            atomic_fetch_add(&hs_done, 1);
        }
        entry->phase = DISCOVERY_REPLAY_FREE;
        return true;
    }
    return false;
}
static DiscoveryReplayStats discovery_replay_schedule(size_t count, unsigned window) {
    DiscoveryReplayStats stats = {0};
    if (!window || window > DISCOVERY_REPLAY_WINDOW || count > DISCOVERY_RECORDS) {
        stats.skipped = count <= UINT32_MAX ? (unsigned)count : UINT32_MAX;
        return stats;
    }
    DiscoveryReplayEntry *entries = calloc(window, sizeof *entries);
    if (!entries) { stats.skipped = (unsigned)count; return stats; }
    size_t next = 0;
    unsigned active = 0;
    while (next < count || active) {
        bool progress = false;
        for (unsigned i = 0; i < window; ++i) {
            if (entries[i].phase == DISCOVERY_REPLAY_FREE) continue;
            if (discovery_pump_one(&entries[i], &stats)) {
                progress = true;
                if (entries[i].phase == DISCOVERY_REPLAY_FREE) active--;
            }
        }
        unsigned slot = 0;
        while (slot < window && next < count) {
            if (entries[slot].phase != DISCOVERY_REPLAY_FREE) { slot++; continue; }
            DiscoveryAdmission result = discovery_admit_one(&entries[slot], next, &stats);
            if (result == DISCOVERY_DEFERRED) break;
            next++;
            progress = true;
            if (result == DISCOVERY_ADMITTED) { active++; slot++; }
        }
        if (!progress) SDL_Delay(1);
    }
    free(entries);
    return stats;
}
static void discovery_replay(void) {
    pthread_mutex_lock(&discovery_store_lock);
    size_t count = discovery_open_locked() ? nvn_discovery_count(&discovery_store) : 0;
    pthread_mutex_unlock(&discovery_store_lock);
    // Freeze this launch's inventory. Newly learned appends are for next start.
    DiscoveryReplayStats s = discovery_replay_schedule(count, DISCOVERY_REPLAY_WINDOW);
    LOG("[vk] shader discovery: replayed %zu recipes, %u programs, %u pipelines, %u compute queued, %u reused, %u skipped (%u budget deferrals)\n",
        count, s.programs, s.pipelines, s.computes, s.reused, s.skipped, s.deferred);
}

// ================================================================== graphics pipeline library probe (measurement only)
// LARECOMP_VK_GPL_PROBE=1 (or =<N>): after shader preparation, the first N prepared pipelines (20; in the order the
// list's records built them) are built again three ways and timed, on the preparing thread, one at a time:
//   full:      one vkCreateGraphicsPipelines, as the game builds them;
//   libraries: the four VK_EXT_graphics_pipeline_library parts (vertex input, pre-rasterization with the vertex shader,
//              fragment shader, fragment output) plus a fast link (no link-time optimization);
//   optimized: the same parts linked again with link-time optimization (what would replace a fast-linked pipeline).
// Every build is cold: no pipeline cache, and a specialization constant no shader reads (a new value per build and
// run) makes each one new to the driver's own caches. The results are destroyed; only [gpl] log lines remain.
// Production reuse is independently enabled by LARECOMP_VK_GPL=1; the probe never enters its cache.
#ifdef VK_EXT_graphics_pipeline_library
#define GPL_PROBE_MAX 256
static Pipe *gpl_sample[GPL_PROBE_MAX];
static int gpl_nsample;
static void gpl_probe_note(Pipe *pp) {
    if (atomic_load(&pp->state) != 1) return;
    pthread_mutex_lock(&prep_lock);
    if (gpl_nsample < GPL_PROBE_MAX) gpl_sample[gpl_nsample++] = pp;
    pthread_mutex_unlock(&prep_lock);
}

static double gpl_ms_since(uint64_t t0) { return (double)(SDL_GetTicksNS() - t0) / 1e6; }

// One pipeline: [0] full, [1] vertex input, [2] pre-rasterization, [3] fragment shader, [4] fragment output, [5] fast
// link, [6] optimized link (ms). false: a build failed (logged).
static bool gpl_probe_one(const PipeKey *k, uint32_t nonce, double t[7]) {
    PipeCI c;
    pipe_ci_fill(&c, k);
    VkSpecializationMapEntry me = {0x7fff, 0, 4};  // (no shader declares this id: it only makes the build unique)
    VkSpecializationInfo spec = {1, &me, 4, &nonce};
    c.stages[0].pSpecializationInfo = c.stages[1].pSpecializationInfo = &spec;
    VkPipeline full = VK_NULL_HANDLE, lib[4] = {0}, fast = VK_NULL_HANDLE, opt = VK_NULL_HANDLE;
    const char *fail = NULL;
    VkResult r;
    uint64_t t0 = SDL_GetTicksNS();
    r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &c.pi, NULL, &full);
    t[0] = gpl_ms_since(t0);
    if (r != VK_SUCCESS) { fail = "full"; goto out; }
    nonce ^= 0x80000000u;  // (the libraries: new to the caches too, not the full pipeline's shaders again)
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    static const char *part_name[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
    for (int i = 0; i < 4; i++) {
        VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                     .pNext = &c.ri, .flags = part[i]};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
            .pDynamicState = &c.dy, .layout = c.pi.layout};
        switch (i) {
        case 0: pi.pVertexInputState = c.pi.pVertexInputState; pi.pInputAssemblyState = &c.ia; break;
        case 1: pi.stageCount = 1; pi.pStages = &c.stages[0]; pi.pViewportState = &c.vs; pi.pRasterizationState = &c.rs; break;
        case 2: pi.stageCount = 1; pi.pStages = &c.stages[1]; pi.pDepthStencilState = &c.ds; pi.pMultisampleState = &c.ms; break;
        case 3: pi.pColorBlendState = &c.cb; pi.pMultisampleState = &c.ms; break;
        }
        t0 = SDL_GetTicksNS();
        r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &lib[i]);
        t[1 + i] = gpl_ms_since(t0);
        if (r != VK_SUCCESS) { fail = part_name[i]; goto out; }
    }
    for (int o = 0; o < 2; o++) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4,
                                             .pLibraries = lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = o ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0, .layout = c.pi.layout};
        t0 = SDL_GetTicksNS();
        r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, o ? &opt : &fast);
        t[5 + o] = gpl_ms_since(t0);
        if (r != VK_SUCCESS) { fail = o ? "optimized link" : "fast link"; goto out; }
    }
out:
    if (fail) LOG("[gpl] probe: %s build failed (%d)\n", fail, r);
    VkPipeline all[7] = {full, lib[0], lib[1], lib[2], lib[3], fast, opt};
    for (int i = 0; i < 7; i++) if (all[i]) vkDestroyPipeline(g_vk.dev, all[i], NULL);
    return !fail;
}

static void gpl_probe(void) {
    const char *e = getenv("LARECOMP_VK_GPL_PROBE");
    int want = e && atoi(e) > 1 ? atoi(e) : 20;
    if (want > GPL_PROBE_MAX) want = GPL_PROBE_MAX;
    pthread_mutex_lock(&prep_lock);
    int n = gpl_nsample < want ? gpl_nsample : want;
    pthread_mutex_unlock(&prep_lock);
    if (!n) { LOG("[gpl] probe: no prepared pipelines to measure\n"); return; }
    LOG("[gpl] probe: building %d prepared pipelines three ways (cold, one at a time; vertex input %s)\n", n,
        g_vk.vids ? "dynamic" : "in each pipeline");
    double sum[7] = {0}, mx[7] = {0};
    int ok = 0;
    uint32_t nonce = (uint32_t)SDL_GetTicksNS() & 0x7fffffffu;
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < n; i++) {
        const PipeKey *k = &gpl_sample[i]->k;
        double t[7] = {0};
        if (!gpl_probe_one(k, nonce + (uint32_t)i, t)) continue;
        ok++;
        for (int j = 0; j < 7; j++) { sum[j] += t[j]; if (t[j] > mx[j]) mx[j] = t[j]; }
        LOG("[gpl] %2d vs/fs %016llx %016llx: full %.1f ms; libraries %.1f + %.1f + %.1f + %.1f ms, fast link %.2f ms; "
            "optimized link %.1f ms\n", i, (unsigned long long)k->prog->prep_vh, (unsigned long long)k->prog->prep_fh,
            t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
    }
    if (!ok) { LOG("[gpl] probe: every build failed\n"); return; }
    double libs = sum[1] + sum[2] + sum[3] + sum[4];
    LOG("[gpl] probe summary, %d pipelines (%.0f ms in all): full %.0f ms (avg %.1f, max %.1f); "
        "libraries %.0f ms (vertex input %.1f, pre-rasterization %.0f, fragment shader %.0f, fragment output %.1f) "
        "+ fast link %.1f ms (avg %.2f, max %.2f); optimized link %.0f ms (avg %.1f, max %.1f)\n",
        ok, gpl_ms_since(t0), sum[0], sum[0] / ok, mx[0], libs, sum[1], sum[2], sum[3], sum[4], sum[5], sum[5] / ok, mx[5],
        sum[6], sum[6] / ok, mx[6]);
}
#else
static void gpl_probe_note(Pipe *pp) { (void)pp; }
static void gpl_probe(void) {}
#endif

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    // A dispatch isn't skipped while its pipeline builds (draws are): what it writes, later shaders read - skipped,
    // the game's light lists stayed unwritten and the shaders walking them ran until the GPU's hang detection
    // (device lost while shaders compiled, ~1 run in 10 on an S20 FE)
    if (cp && atomic_load(&cp->p.state) == 0) {
        uint64_t t0 = SDL_GetTicksNS();
        while (atomic_load(&cp->p.state) == 0) SDL_Delay(1);
        stat_wait_ns += SDL_GetTicksNS() - t0;
        g_blocked_ns += SDL_GetTicksNS() - t0;  // (a first-time build: not the frame's own cost, for the pacer)
    }
    if (!cp || atomic_load(&cp->p.state) != 1) { SKIP_CS(!cp ? SK_NO_STAGES : atomic_load(&cp->p.state) == 2 ? SK_CS_FAILED : SK_CS_BUILDING); return; }
    {   // LARECOMP_SKIP_CS=<code address hex> (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) { const char *e = getenv("LARECOMP_SKIP_CS"); skip = e ? strtoull(e, NULL, 16) : 0; }
        if (skip && st.program->stages[5].code_addr == skip) { SKIP_CS(SK_DEBUG_SKIP); return; }
    }
    if (cp->si->uses_gmem && getenv("LARECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    mirror_normalize_compute(cp->si);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (cp->si->uses_gmem_map && !vk_gmem_mirror()) gfast_prepare(cb);
    mir_nstored = 0;  // one operation, independent of descriptor-set reuse
    storage_nwritten = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL, NULL, NULL);
    // after everything before it: a dispatch reads what passes, copies and dispatches wrote (the pass it follows ended
    // just now) - and with mirrored storage, what was recorded before (an earlier submission's too) may still read
    // the slots it rewrites (a list rewritten under a shader walking it hung the GPU)
    vk_full_barrier(cb);
    bc_mark(cb, "dispatch %ux%ux%u cs %llx%s", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr,
            cp->si->uses_gmem_store ? " (stores)" : "");
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    pg_mark(cb, "dispatch %ux%ux%u cs %llx%s", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr,
            cp->si->uses_gmem_store ? " (stores)" : "");
    if (gc_shift && cp->si->uses_gmem) gmem_chunk_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, GMEM_CHUNK_SET(STAGE_COMPUTE), 1u << STAGE_COMPUTE);
    rec_CmdDispatch(cb, gx, gy, gz);
    storage_images_finish(cb);
    pg_mark(cb, "(after dispatch)");
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) {
        if (vk_gmem_mirror()) mirror_after_dispatch(cb);  // (its slots: readbacks; read by what follows)
        else gfast_finish(cb);
    }
    if (bc_on > 0) {  // (breadcrumb: the dispatch done)
        vk_full_barrier(cb);
        bc_mark(cb, "after dispatch cs %llx", (unsigned long long)st.program->stages[5].code_addr);
    }
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5);
    stat_dispatches++;
    work_since_submit += 4;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);
static void material_probe_poll(void);
static void material_probe_draw(const Prog *p, const PipeKey *key, const CmdRec *r, bool indexed,
                                const VkDescriptorImageInfo *images, unsigned runs, const VtxLayout *layout);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    if (tris > (0xFFFFFFFFu - 4) / (3 * 4)) FATAL("[vk] expand_indices triangle count overflows its allocation\n");
    uint32_t *out = malloc((size_t)tris * 3 * 4 + 4);
    if (!out) FATAL("[vk] expand_indices allocation failed\n");
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

// The draw's state as a variant record (shader_prep.h; as renderer.m: both backends record the same lines)
static void rec_inputs_from_state(ShaderRec *r, bool flip_x, uint32_t mode) {
    memset(r, 0, sizeof *r);
    r->kind = 'G';
    r->flags = (nvn_window_origin() == 0 ? SR_FLIP_Y : 0) | (nvn_depth_mode() == 0 ? SR_DEPTH_MINUS_ONE : 0) | (flip_x ? SR_FLIP_X : 0);
    r->nattrib = (uint8_t)(st.nattrib < 0 ? 0 : st.nattrib < 16 ? st.nattrib : 16);
    memcpy(r->attrib, st.attrib, sizeof r->attrib[0] * r->nattrib);
    r->nstream = (uint8_t)(st.nstream < 0 ? 0 : st.nstream < 16 ? st.nstream : 16);
    memcpy(r->stream, st.stream, sizeof r->stream[0] * r->nstream);
    r->ncolor = (uint8_t)(st.rt.ncolor < 8 ? st.rt.ncolor : 8);
    r->samples = 1;
    for (int i = 0; i < r->ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        r->color[i] = t->format;
        if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4)) r->cview[i] = st.rt.cview[i].format;
        if (t->samples > 1) r->samples = t->samples;
        r->channel_mask |= st.channel_mask & (0xFu << (i * 4));
        if (((st.color_state[0] >> i) & 1) && !rt_kind(t->format)) {
            r->blend_on |= (uint8_t)(1u << i);
            memcpy(r->blend[i], st.blend[i], 8);
        }
    }
    if (st.rt.depth) {
        r->depth = st.rt.depth->format;
        if (!r->ncolor && st.rt.depth->samples > 1) r->samples = st.rt.depth->samples;
    }
    r->a2c = st.ms[0xC] != 0;
    r->topo = shader_topo_class(mode);
    r->poly_fill = st.poly[2] != 1;
}
static void rec_from_state(ShaderRec *r, bool flip_x, uint32_t mode) {
    rec_inputs_from_state(r, flip_x, mode);
    r->vs = shader_stage_hash(&st.program->stages[0]);
    r->fs = shader_stage_hash(&st.program->stages[1]);
}

// LARECOMP_PREP_CHECK=1 (diagnostics): a pipeline the game builds although its shaders were prepared - logs the
// fields its key differs in from the closest prepared pipeline of the same shaders (and the draw's variant record)
static bool prep_check_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_PREP_CHECK") && atoi(getenv("LARECOMP_PREP_CHECK"));
    return v;
}

// Fields of two pipeline keys that differ: their number, and (out) "field: prepared / draw" for each
static int pk_diff(const PipeKey *a, const PipeKey *b, char *out, size_t n) {
    int d = 0;
    size_t o = 0;
    if (out && n) out[0] = 0;
#define PKD(cond, ...) do { if (cond) { d++; if (out && o < n) o += (size_t)snprintf(out + o, n - o, __VA_ARGS__); } } while (0)
    PKD(a->prog != b->prog, " program %p/%p (ready %d/%d);", (void *)a->prog, (void *)b->prog, a->prog ? (int)a->prog->state : -1,
        b->prog ? (int)b->prog->state : -1);
    PKD(a->ncolor != b->ncolor, " ncolor %u/%u;", a->ncolor, b->ncolor);
    for (int i = 0; i < 8; i++) PKD(a->color[i] != b->color[i], " color%d %d/%d;", i, a->color[i], b->color[i]);
    PKD(a->depth != b->depth, " depth %d/%d;", a->depth, b->depth);
    PKD(a->stencil != b->stencil, " stencil %d/%d;", a->stencil, b->stencil);
    PKD(a->samples != b->samples, " samples %u/%u;", a->samples, b->samples);
    PKD(a->attr_mask != b->attr_mask, " attr_mask %#x/%#x;", a->attr_mask, b->attr_mask);
    for (int i = 0; i < 32; i++) {
        PKD(a->attr_fmt[i] != b->attr_fmt[i], " attr%d fmt %u/%u;", i, a->attr_fmt[i], b->attr_fmt[i]);
        PKD(a->attr_off[i] != b->attr_off[i], " attr%d off %u/%u;", i, a->attr_off[i], b->attr_off[i]);
        PKD(a->attr_bind[i] != b->attr_bind[i], " attr%d bind %u/%u;", i, a->attr_bind[i], b->attr_bind[i]);
    }
    PKD(a->used_streams != b->used_streams, " streams %#x/%#x;", a->used_streams, b->used_streams);
    for (int s = 0; s < 17; s++) {
        PKD(a->stride[s] != b->stride[s], " stride%d %u/%u;", s, a->stride[s], b->stride[s]);
        PKD(a->divisor[s] != b->divisor[s], " divisor%d %u/%u;", s, a->divisor[s], b->divisor[s]);
    }
    for (int i = 0; i < 8; i++) {
        PKD(memcmp(a->blend[i], b->blend[i], 8), " blend%d differs;", i);
        PKD(a->write_mask[i] != b->write_mask[i], " write_mask%d %#x/%#x;", i, a->write_mask[i], b->write_mask[i]);
    }
    PKD(a->blend_on != b->blend_on, " blend_on %#x/%#x;", a->blend_on, b->blend_on);
    PKD(a->a2c != b->a2c, " a2c %u/%u;", a->a2c, b->a2c);
    PKD(a->topo_class != b->topo_class, " topo %u/%u;", a->topo_class, b->topo_class);
    PKD(a->poly_fill != b->poly_fill, " fill %u/%u;", a->poly_fill, b->poly_fill);
    if (!d && memcmp(a, b, sizeof *a)) {  // (no field differs: other bytes - padding)
        const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
        for (size_t i = 0; i < sizeof *a; i++) PKD(x[i] != y[i], " byte %zu %02x/%02x;", i, x[i], y[i]);
    }
#undef PKD
    return d;
}

static void prep_check(const PipeKey *k, uint64_t vh, uint64_t fh, const ShaderRec *rec) {
    char line[2048], diff[2048] = "";
    shader_rec_format(rec, line, sizeof line);
    int nprep = 0, best_d = 1 << 30, progs = 0;
    pthread_mutex_lock(&prep_lock);
    for (size_t i = 0; i < prep_progs_cap; i++) {
        Prog *q = prep_progs[i].obj;
        if (prep_progs[i].key && q && q->prep_vh == vh && q->prep_fh == fh) progs++;
    }
    const Pipe *best = NULL;
    for (size_t i = 0; i < prep_pipes_cap; i++) {
        const Pipe *p = prep_pipes[i].obj;
        if (!prep_pipes[i].key || !p || p->k.prog->prep_vh != vh || p->k.prog->prep_fh != fh) continue;
        nprep++;
        int d = pk_diff(&p->k, k, NULL, 0);
        if (d < best_d) best_d = d, best = p;
    }
    if (best) pk_diff(&best->k, k, diff, sizeof diff);
    pthread_mutex_unlock(&prep_lock);
    LOG("[prepcheck] %016llx %016llx: program %s (%d prepared with these shaders), %d prepared pipelines, closest "
        "differs (prepared/draw) in:%s\n[prepcheck]   draw: %s\n", (unsigned long long)vh, (unsigned long long)fh,
        k->prog->prep_vh ? "prepared" : "not prepared", progs, nprep, best ? diff : " (none)", line);
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
// LARECOMP_LOG_SKIPS=1: why draws were skipped (reason: 1 no program, 2/3 program not ready, 4 skip list, 5 the pot
// level's shader, 6 topology, 7 pipeline not built yet), each reason's count every 2 s with the last fragment shader address
static void skip_why(int why, uint64_t fs) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_SKIPS") != NULL;
    if (!on) return;
    static uint64_t cnt[8], last_fs[8], at;
    cnt[why & 7]++, last_fs[why & 7] = fs;
    uint64_t now = SDL_GetTicksNS();
    if (now - at > 2000000000ull) {
        for (int i = 1; i < 8; i++)
            if (cnt[i]) LOG("[skips] reason %d: %llu (fs %llx)\n", i, (unsigned long long)cnt[i], (unsigned long long)last_fs[i]);
        memset(cnt, 0, sizeof cnt), at = now;
    }
}
static uint64_t prof_tp_;
#include "depth_replay.h"

// Metadata only, called after an accepted draw mark in the one bounded capture.
// Keep outside the standalone profiler region: its tests need no renderer types.
static unsigned pg_log_shader_metadata(const char *stage, uint64_t guest, uint64_t hash,
                                   unsigned fingerprint_state, const ShaderInfo *info) {
    static const char *const kinds[] = {"straight", "nested", "flag", "pc"};
    static struct { const ShaderInfo *info; uint64_t hash; char stage; } seen[256];
    static unsigned nseen;
    static bool truncated;
    if (!info) return 0;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i].info == info && seen[i].hash == hash && seen[i].stage == stage[0]) return i + 1;
    if (nseen == 256) {
        if (!truncated) LOG("[shadermeta] stage summary limit 256 reached; draw joins remain available\n");
        truncated = true;
        return 0;
    }
    unsigned id = ++nseen;
    seen[id - 1].info = info; seen[id - 1].hash = hash; seen[id - 1].stage = stage[0];
    uint32_t types = 0;
    uint64_t shadow = 0, image = 0, bindless = 0, uint_tex = 0;
    for (int i = 0; i < info->ntex && i < 64; i++) {
        const ShaderTexture *t = &info->tex[i];
        if (t->type < 32) types |= 1u << t->type;
        if (t->shadow) shadow |= UINT64_C(1) << i;
        if (t->is_image) image |= UINT64_C(1) << i;
        if (t->bindless) bindless |= UINT64_C(1) << i;
        if (t->is_uint) uint_tex |= UINT64_C(1) << i;
    }
    LOG("[shadermeta] id %u frame %llu mark %d stage %s guest %llx hash %016llx fingerprint_state %u flow %s stacks_resolved %u static_stacks %u insts %u regs %u half_regs %u half_conversions %u local_words %u labels %u brx %u tables %u cbuf %x dynamic %x gmem %u map %u readonly %u store %u depth %u discard %u textures %d types %x shadow %llx image %llx bindless %llx uint %llx\n",
        id, (unsigned long long)pg_frame, pg_n - 1, stage, (unsigned long long)guest, (unsigned long long)hash,
        fingerprint_state, info->flow_kind < 4 ? kinds[info->flow_kind] : "unknown",
        info->stacks_resolved, info->static_stacks, info->num_insts, info->register_count,
        info->half_register_count, info->half_conversion_count, info->local_words,
        info->branch_labels, info->brx_count, info->brx_tables, info->cbuf_mask, info->cbuf_dynamic,
        info->uses_gmem, info->uses_gmem_map, info->uses_gmem_readonly, info->uses_gmem_store,
        info->writes_depth, info->uses_discard, info->ntex, types, (unsigned long long)shadow,
        (unsigned long long)image, (unsigned long long)bindless, (unsigned long long)uint_tex);
    return id;
}

static void draw_(CmdRec *r, bool indexed) {
    stencil_probe_poll();
    material_probe_poll();
    sampling_trace_poll();
    depth_replay_poll();
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { skip_why(1, 0); SKIP(SK_NO_PROGRAM); return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    vkey.tex_uint = sampled_uint_mask(0);
    fkey.tex_uint = sampled_uint_mask(1);
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    int program_state = pr ? graphics_wait_ready(&pr->state) : 2;
    if (!pr || program_state != 1) { skip_why(2, st.program ? st.program->stages[1].code_addr : 0); SKIP(!pr ? SK_NO_STAGES : program_state == 2 ? SK_PROG_FAILED : SK_PROG_BUILDING); return; }
    g_mirror_w = 0;
    bool scene_composite = false;  // (frame generation copies the picture right after it: framegen.c)
    {   // mirror mode (runtime/hle/mirror.c; as renderer.m): the scene's composite into the window image is mirrored
        extern bool mirror_draw(bool composite);
        extern bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && mirror_scene_size(t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
            extern void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
            if (t0) scene_size_log(composite, t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
        }
        scene_composite = composite;
        if (mirror_draw(composite)) {
            vkey.flip_x = fkey.flip_x = true;
            pr = get_program(st.program, &vkey, &fkey);
            program_state = pr ? graphics_wait_ready(&pr->state) : 2;
            if (!pr || program_state != 1) { skip_why(3, st.program ? st.program->stages[1].code_addr : 0); SKIP(!pr ? SK_NO_STAGES : program_state == 2 ? SK_PROG_FAILED : SK_PROG_BUILDING); return; }
            g_mirror_w = (float)st.rt.color[0]->width;
        }
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        // (once per program: 512 dependent steps, redone at every fragment shader change, were ~1 us a draw on phones)
        uint64_t fs = st.program->stages[1].code_addr;
        if (!pr->fs_hashed || pr->code[1] != fs) {
            pr->fs_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
            pr->fs_hashed = pr->code[1] == fs;
        }
        uint64_t last_hash = pr->fs_hash;
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { skip_why(4, st.program ? st.program->stages[1].code_addr : 0); SKIP(SK_DEBUG_SKIP); return; }
        extern bool pot_level_active(void);
        if (shared_title_las() && last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { skip_why(5, st.program ? st.program->stages[1].code_addr : 0); SKIP(SK_HIDDEN); return; }
        {   // LARECOMP_DUMP_UBO=<fs code addr hex> (as renderer.m): that shader's uniform buffers in the debug frame
            static uint64_t dump_fs = 1;
            if (dump_fs == 1) { const char *e = getenv("LARECOMP_DUMP_UBO"); dump_fs = e ? strtoull(e, NULL, 16) : 0; }
    if (g_debug_frame && dump_fs && fs == dump_fs && st.sbuf[1][0].addr && st.sbuf[1][1].addr) {
        // (the light lists: tiles' entries in use, a test of the culling)
        const uint32_t *l = (const uint32_t *)(uintptr_t)st.sbuf[1][0].addr, *t = (const uint32_t *)(uintptr_t)st.sbuf[1][1].addr;
        uint64_t used = 0, nt = st.sbuf[1][1].size / 4, mx = 0;
        for (uint64_t k = 0; k < st.sbuf[1][0].size / 4; k++) used += l[k] != 0xffffffffu;
        for (uint64_t k = 0; k < nt; k++) if (t[k] > mx) mx = t[k];
        LOG("[lights] list words in use %llu of %llu, tile table %llu entries, max %llu\n", (unsigned long long)used,
            (unsigned long long)(st.sbuf[1][0].size / 4), (unsigned long long)nt, (unsigned long long)mx);
    }
            if (g_debug_frame && dump_fs && fs == dump_fs)
                for (int i = 0; i < 18; i++) {
                    if (!st.ubuf[1][i].addr) continue;
                    const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr;
                    LOG("[ubo] f%d (%llu):", i, (unsigned long long)st.ubuf[1][i].size);
                    for (uint64_t k = 0; k < st.ubuf[1][i].size / 4 && k < 1200; k++) LOG(" %g", f[k]);
                    LOG("\n");
                }
        }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (shared_title_las() && last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (shared_title_las() && st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    mirror_normalize_graphics(pr->vi, pr->fi);
    if (g_debug_frame) debug_draw(pr, r, indexed);
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check(pr);
    resolve_feedback(pr);
    stencil_probe_consumers(pr);
    projection_probe_before_draw(pr);
    snap_reason = "draw-depth-stencil-write";
    if (st.rt.depth && snapshot_draw_writes_depth_stencil()) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    mirror_prepare_graphics(pr->vi, pr->fi);
    mirror_uniform_prepare_graphics(pr->vi, pr->fi);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) {
        if (mir_graphics_prepared) FATAL("[vk] storing graphics topology is unsupported\n");
        skip_why(6, st.program ? st.program->stages[1].code_addr : 0); SKIP(SK_TOPOLOGY); return;
    }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    uint32_t rt_fmt[8] = {0};
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) rt_fmt[i] = st.rt.color[i] ? st.rt.color[i]->format : 0;
    const PipeIn in = {st.attrib, st.nattrib, (const uint32_t (*)[2])st.stream, st.nstream, rt_fmt, st.channel_mask,
                       st.color_state[0], (const uint8_t (*)[8])st.blend, st.ms[0xC] != 0, cls, st.poly[2] != 1};
    bool memo_on = key_memo_on();
    if (memo_on && !pr->pmemo) pr->pmemo = calloc(1, sizeof *pr->pmemo);
    struct PipeMemo *pm = memo_on ? pr->pmemo : NULL;
    int nat = st.nattrib < 0 ? 0 : st.nattrib > 16 ? 16 : st.nattrib, nst = st.nstream < 0 ? 0 : st.nstream > 16 ? 16 : st.nstream;
    uint32_t npc = pass_ncolor < 8 ? pass_ncolor : 8;
    prof_key_draws++;
    PipeKey k;
    VtxLayout vl;  // the draw's vertex layout (on a memo hit: the memo's, made from the same inputs)
    Pipe *pp;
    if (pm && pm->pp && pm->gen == key_gen && pm->ncolor == pass_ncolor && pm->samples == pass_samples && pm->depth == pass_depth &&
        pm->stencil == pass_stencil && pm->nattrib == st.nattrib && pm->nstream == st.nstream && pm->channel_mask == in.channel_mask &&
        pm->blend_on == in.blend_on && pm->a2c == in.a2c && pm->cls == in.topo_class && pm->poly_fill == in.poly_fill &&
        !memcmp(pm->color, pass_color, npc * sizeof pass_color[0]) && !memcmp(pm->rt_fmt, rt_fmt, sizeof rt_fmt) &&
        !memcmp(pm->attrib, st.attrib, (size_t)nat * 4) && !memcmp(pm->stream, st.stream, (size_t)nst * 8) &&
        !memcmp(pm->blend, st.blend, sizeof pm->blend)) {
        prof_pipe_memo++;
        pp = pm->pp;
        vl = pm->vl;  // (the key leaves the layout out with dynamic vertex input: kept with the memo)
    } else {
        pipe_key_fill(&k, pr, &in, pass_color, pass_ncolor, pass_depth, pass_stencil, pass_samples, &vl);
        if (headstart_on() && pr->chash && pr->read_code[0] && pr->cmax[0] <= UINT32_MAX && pr->cmax[1] <= UINT32_MAX &&
            pr->csize[0] <= UINT32_MAX && pr->csize[1] <= UINT32_MAX) {
            NvnDiscoveryRecipe recipe = {.vkey = pr->vkey, .fkey = pr->fkey,
                .code = {pr->read_code[0], pr->read_code[1]},
                .code_bytes = {(uint32_t)pr->cmax[0], (uint32_t)pr->cmax[1]},
                .constant_bytes = {(uint32_t)pr->csize[0], (uint32_t)pr->csize[1]}};
            rec_inputs_from_state(&recipe.rec, vkey.flip_x, mode);
            // Retain original routing inputs, including values masked out by
            // this translator's current output interface. A later compiler may
            // report additional outputs/attributes and must rederive the key.
            recipe.rec.channel_mask = st.channel_mask;
            recipe.rec.blend_on = (uint8_t)st.color_state[0];
            memcpy(recipe.rec.blend, st.blend, sizeof recipe.rec.blend);
            PipeKey reproduced;
            prepare_pipe_key(&reproduced, pr, &recipe.rec);
            // Special target/view policies not representable by ShaderRec must
            // retain foreground fallback rather than inventing a portable key.
            if (!memcmp(&reproduced, &k, sizeof k)) discovery_record(&recipe, pr->chash);
        }
        uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
        if (!ph) ph = 1;
        uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
        while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
        pp = pipe_tab[pslot].pipe;
        if (!pp) {
            if (shader_rec_recording()) {
                ShaderRec rec;
                rec_from_state(&rec, vkey.flip_x, mode);
                shader_rec_note(&rec);
            }
            pp = prep_pipes_n ? PREP_GET(prep_pipes, ph) : NULL;  // prepared before the game
            if (pp && memcmp(&pp->k, &k, sizeof k)) pp = NULL;
            if (!pp) pp = hs_pipe_take(ph);  // (built ahead by the shader head start)
            if (!pp) {
                uint64_t vh = hs_snapshot_stage_hash(&st.program->stages[0], pr->read_code[0]);
                uint64_t fh = hs_snapshot_stage_hash(&st.program->stages[1], pr->read_code[1]);
                shader_first_visit("pipeline", vh, fh, ph);
                if (prep_check_on()) {
                    ShaderRec rec;
                    rec_from_state(&rec, vkey.flip_x, mode);
                    prep_check(&k, vh, fh, &rec);
                }
                pp = calloc(1, sizeof *pp);
                pp->k = k;
                Pipe *candidate = pp;
                bool fresh;
                Pipe *winner = hs_pipe_put(ph, candidate, &fresh);
                if (winner) {
                    pp = winner;
                    if (winner != candidate) free(candidate);
                    if (fresh) hs_record_pipe(&k);  // (a new one: into cache/headstart.bin)
                }
                // A full auxiliary table retains the foreground fallback. A
                // lost insertion race adopts the winner and queues no duplicate.
                if (fresh || !winner) run_job(build_pipeline, pp);
            }
            pipe_tab[pslot].key = ph;
            pipe_tab[pslot].pipe = pp;
        }
        if (pm) {
            pm->gen = key_gen; pm->ncolor = pass_ncolor; pm->samples = pass_samples; pm->depth = pass_depth; pm->stencil = pass_stencil;
            pm->nattrib = st.nattrib; pm->nstream = st.nstream; pm->channel_mask = in.channel_mask; pm->blend_on = in.blend_on;
            pm->a2c = in.a2c; pm->cls = in.topo_class; pm->poly_fill = in.poly_fill;
            memcpy(pm->color, pass_color, sizeof pm->color); memcpy(pm->rt_fmt, rt_fmt, sizeof pm->rt_fmt);
            memcpy(pm->attrib, st.attrib, sizeof pm->attrib); memcpy(pm->stream, st.stream, sizeof pm->stream);
            memcpy(pm->blend, st.blend, sizeof pm->blend);
            pm->pp = pp;
            pm->vl = vl;
        }
    }
    {   // Workers still compile in parallel; the first consumer waits rather than dropping its draw.
        int ps = graphics_wait_ready(&pp->state);
        bool stores = pr->vi->uses_gmem_store || pr->fi->uses_gmem_store;
        if (stores && ps != 1) FATAL("[vk] storing graphics pipeline failed; refusing to discard shader writes\n");
        if (ps != 1) { skip_why(7, st.program->stages[1].code_addr); SKIP(ps == 2 ? SK_PIPE_FAILED : SK_PIPE_BUILDING); return; }
    }
    ss_enabled();
    if (ss_need(SS_PIPE, ss.pipe == pp)) { ss.pipe = pp; rec_CmdBindPipelineObj(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp); }
    if (g_vk.vids && ss_need(SS_VI, !memcmp(&ss.vi, &vl, sizeof vl))) { ss.vi = vl; set_vertex_input(cb, &vl); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; rec_CmdSetDepthTestEnable(cb, dt); }
    VkBool32 dw = dt && st.ds[1];
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; rec_CmdSetDepthWriteEnable(cb, dw); }
    if (dw) { extern void fg_note_draw(NvnTexture *depth, uint32_t pass_w, uint32_t pass_h); fg_note_draw(st.rt.depth, pass_w, pass_h); }
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; rec_CmdSetDepthCompareOp(cb, dop); }
    bool sten = st.ds[3] && pass_stencil;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; rec_CmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            rec_CmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            rec_CmdSetStencilCompareMask(cb, face, v[4]);
            rec_CmdSetStencilWriteMask(cb, face, v[5]);
            rec_CmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); rec_CmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; rec_CmdSetCullMode(cb, cm); }
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; rec_CmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; rec_CmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; rec_CmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; rec_CmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); rec_CmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    // The hooks below look for known textures (texpatch ids, TexBackend.tp > 0) among the fragment textures: one pass
    // finds the texture objects and whether any is known; the loops only run when one is (most draws have none).
    // LARECOMP_VK_NO_HOOK_PASS=1: every loop looks its textures up, as before.
    static int hook_pass = -1;
    if (hook_pass < 0) hook_pass = getenv("LARECOMP_VK_NO_HOOK_PASS") == NULL;
    NvnTexture *ftex[64];
    bool fknown = true;
    if (hook_pass) {
        fknown = false;
        for (int i = 0; i < pr->fi->ntex; i++) {
            ftex[i] = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = ftex[i] ? (const TexBackend *)ftex[i]->backend : NULL;
            if (b && b->tp > 0) fknown = true;
        }
        prof_hook_draws++;
        if (fknown) prof_hook_known++;
    }
#define FTEX(i) (hook_pass ? ftex[i] : shader_texture_object(1, &pr->fi->tex[i]))
    if (shared_title_las()) {   // the Items page's cursor brackets while the cursor is on the companion card (runtime/hle/pets.c)
        extern bool pets_hide_item_cursor(void);
        extern bool texpatch_is_item_cursor(int id);
        if (pets_hide_item_cursor() && fknown)
            for (int i = 0; i < pr->fi->ntex; i++) {
                NvnTexture *t = FTEX(i);
                const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                if (b && b->tp > 0 && texpatch_is_item_cursor(b->tp - 1)) {
                    if (mir_graphics_prepared) FATAL("[vk] hidden draw cannot discard shader stores\n");
                    SKIP(SK_HIDDEN); return;
                }
            }
    }
    float tint[RECOLOUR_TINT_FLOATS] = {0};
    int recol[2] = {0, 0};  // Cosmetics recolour textures' classes (runtime/nvn/recolour.c)
    if (shared_title_las() && pr->fi->uses_tint) {
        for (int i = 0; i < pr->fi->ntex && fknown; i++) {
            NvnTexture *t = FTEX(i);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0) recolour_add(recol, recolour_class_of(b->tp - 1));
        }
        recolour_tint_draw(recol, tint);  // (the highlights below win)
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        // (uniform buffer 2 = the model matrix, hardware cbuf 5 - only when the vertex shader reads it: a binding left
        // from an earlier draw is another object's matrix)
        if (((pr->vi->cbuf_mask >> 5) & 1) && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        // the gold Escape Bottle (runtime/hle/escape.c): draws sampling the bottle's item icon / model map
        // and the HUD hearts (heart colour, Cosmetics tab)
        extern int texpatch_bottle_kind(int id);
        extern int texpatch_heart_kind(int id);
        extern void texpatch_heart_tint(int kind, float tint[8]);
        int bottle = 0, heart = 0;
        for (int i = 0; i < pr->fi->ntex && fknown; i++) {
            NvnTexture *t = FTEX(i);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0 && !bottle) bottle = texpatch_bottle_kind(b->tp - 1);
            if (b && b->tp > 0 && !heart) heart = texpatch_heart_kind(b->tp - 1);
        }
        if (heart) texpatch_heart_tint(heart, tint);
        if (bottle) {
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = bottle == 3 || bottle == 6 ? 3 : 0;  // (fairy / Piece of Power models: their bone matrices - souls.c, triforce.c)
            escape_bottle_tint(bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
    }
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    uint32_t dyn[2 * MAX_BIND], ndyn = 0;  // (set reuse: the sets' dynamic offsets, vertex then fragment)
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndyn);
    uint32_t ndyn0 = ndyn;  // (the vertex set's)
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2], fr[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh, fr, pr->fp[1].hash);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
        tex_fr[1][i][0] = fr[0]; tex_fr[1][i][1] = fr[1];
        // the Hint Sheet (runtime/hle/hint_sheet.c): the dungeon map's icon in place of the first bottle's
        // (tp read after texture_info: its upload check may have identified the texture just now)
        NvnTexture *t = FTEX(i);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        extern int texpatch_bottle_kind(int id);
        if (shared_title_las() && b && b->tp > 0 && texpatch_bottle_kind(b->tp - 1) == 1) {
            extern bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size);
            extern NvnTexture *texpatch_hint_icon(void);
            NvnTexture *ht;
            if (hint_sheet_icon_draw((const float *)(uintptr_t)st.ubuf[0][0].addr, (uint32_t)st.ubuf[0][0].size) &&
                (ht = texpatch_hint_icon())) {
                static NvnTextureView none;
                fii[i].imageView = vk_view(ht, &none, false, false, pr->fi->tex[i].type);
            }
        }
    }
#undef FTEX
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (shared_title_las() && fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        extern bool widescreen_ui_pane_partial(const float *u, uint64_t size);  // (a smaller layout pane: boxed UI)
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad &&
                               !widescreen_ui_pane_partial((const float *)(uintptr_t)st.ubuf[0][0].addr, st.ubuf[0][0].size);
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        if (fullscreen_quad && !reads_scene) {  // (interiors at the wide render resolution: their scene has its own size,
            // widescreen.c, but the window's aspect - not a 16:9 UI picture)
            int pw, ph;
            pass_size(&pw, &ph);
            float pa = ph ? (float)pw / (float)ph : 0;
            for (int i = 0; i < pr->fi->ntex && i < 64 && pa > 1.8f; i++) {
                float ta = tex_wh[1][i][1] ? (float)tex_wh[1][i][0] / (float)tex_wh[1][i][1] : 0;
                if (ta > 1.8f && fabsf(ta - pa) < 0.02f * pa) reads_scene = true;
            }
        }
        static uint64_t wide_frame = ~0ull;
        extern uint64_t renderer_exec_frame(void);  // (the frame being translated)
        if (reads_scene) wide_frame = renderer_exec_frame();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_exec_frame());
        if (!composite) wide_frame = ~0ull;
        if (composite) g_full_drawn = true;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
            if (pw > 0 && ph > 0) {  // (the box, as fractions of the target: device.c bars_backdrop)
                g_box_drawn = true;
                g_box_rt = st.rt.color[0]->backend;
                g_box_frac[0] = vp_ox / pw, g_box_frac[1] = (vp_ox + st.rt.color[0]->width * u) / pw;
                g_box_frac[2] = vp_oy / ph, g_box_frac[3] = (vp_oy + st.rt.color[0]->height * u) / ph;
            }
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, dyn, &ndyn);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    {   // (only what changed: equal handles are sets of this Prog's own layouts - stage_set's caches - and their dynamic
        // offsets the same as bound)
        bool k0 = ss_on && (ss.known & SS_DS) && ss.ds[0] == vset && ss.ndyn >= ndyn0 && !memcmp(ss.dyn, dyn, ndyn0 * sizeof dyn[0]);
        bool k1 = k0 && ss.ds[1] == fset && ss.ndyn == ndyn && !memcmp(ss.dyn + ndyn0, dyn + ndyn0, (ndyn - ndyn0) * sizeof dyn[0]);
        if (!k1) {
            uint32_t first_set = k0 ? 1 : 0, d0 = k0 ? ndyn0 : 0;
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, first_set, 2 - first_set, sets + first_set,
                                      ndyn - d0, ndyn - d0 ? dyn + d0 : NULL);
            ss.ds[0] = vset, ss.ds[1] = fset, ss.known |= SS_DS;
            ss.ndyn = ndyn;
            memcpy(ss.dyn, dyn, ndyn * sizeof dyn[0]);
        }
    }
    if (gc_shift && (pr->vi->uses_gmem || pr->fi->uses_gmem))
        gmem_chunk_bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, GMEM_CHUNK_SET(STAGE_FRAGMENT),
                        (pr->vi->uses_gmem ? 1u << STAGE_VERTEX : 0) | (pr->fi->uses_gmem ? 1u << STAGE_FRAGMENT : 0));
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (st.prim_restart && itype != 0 && getenv("LARECOMP_LOG_RESTART")) {  // (debug) restart indices Vulkan won't see
            uint32_t isz = itype == 1 ? 2 : 4, vkr = isz == 2 ? 0xFFFF : 0xFFFFFFFFu, hits = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = isz == 2 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
                if (v == (isz == 2 ? (restart & 0xFFFF) : restart)) hits++;
            }
            bool strip = mode == 3 || mode == 5;
            if (hits && (!strip || (isz == 2 ? (restart & 0xFFFF) : restart) != vkr || !native)) {
                static uint64_t told[64]; static int nt;
                uint64_t key = (uint64_t)mode << 40 | (uint64_t)isz << 36 | restart, seen = 0;
                for (int i = 0; i < nt; i++) seen |= told[i] == key;
                if (!seen && nt < 64) {
                    told[nt++] = key;
                    LOG("[restart] mode %d, %u-bit indices, restart %#x (%u in %u indices), native %d, fs hash %016llx\n", mode, isz * 8,
                        restart, hits, count, native, (unsigned long long)g_cur_fs_hash);
                }
            }
        }
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                max_index = index_max(src, isz, count, st.prim_restart, restart, max_index);
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        rec_CmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(vl.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            sp = (VkSpan){zero_buffer(), ZBUF_SIZE, NULL};  // (0, 0, 0, 1)
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                rec_CmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                max_index = index_max(scan_src, scan_isz, icount, st.prim_restart, scan_restart, max_index);
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = vl.stride[s], need;
            if (!stride) need = 256;
            else if (vl.divisor[s]) need = ((uint64_t)base_inst + inst + vl.divisor[s] - 1) / vl.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) sp = (VkSpan){zero_buffer(), 0, NULL};
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        rec_CmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted
    // (as renderer.m; gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    // Only for a vertex shader that reads storage buffer 0 (its binding stays from the last draw that set it).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && (pr->vi->ssbo_read & 1) && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_inst + inst <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t i = 0; i < inst; i++) {
                bool g = pot_instance_gold(m + (size_t)(base_inst + i) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = i; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    material_probe_draw(pr, &pp->k, r, indexed, fii, (unsigned)nruns, &vl);
    int depth_replay_slot = depth_replay_select(pr, &pp->k, &vl);
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[RECOLOUR_TINT_FLOATS] = {0};
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            else if (shared_title_las()) recolour_tint_draw(recol, rtint);
            uint32_t rdyn[MAX_BIND], nrdyn = 0;
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii, rdyn, &nrdyn);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, nrdyn, nrdyn ? rdyn : NULL);
            ss.known &= ~SS_DS;  // (the next draw binds both again)
        }
        if (!use_index) rec_CmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else rec_CmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    if (depth_replay_slot >= 0) {
        depth_replay_capture(depth_replay_slot, use_index, use_index ? icount : count, first, base_vertex,
                             base_inst, inst, (unsigned)nruns);
        cb = vk_cmd();
    }
    mirror_finish_graphics(cb);
    uniform_forward_graphics_prepared = false;
    projection_probe_after_draw();
    cb = vk_cmd();
    if (pg_state == 1 && pg_draws_on()) pg_mark(cb, "draw fs %llx vs %llx", (unsigned long long)st.program->stages[1].code_addr,
                                                (unsigned long long)st.program->stages[0].code_addr);
    if (pg_state == 1 && pg_draws_on() && pg_n > 0) {
        uint64_t vc = (uint64_t)(use_index ? icount : count) * (inst ? inst : 1);
        pg_verts += vc, pg_vc[pg_n - 1] = vc;
        // Cached hashes are the admitted stage content identities; the fingerprint
        // state makes stale/unavailable identity explicit without rereading game bytes.
        uint64_t vh = pr->fp[0].hash ? pr->fp[0].hash : pr->prep_vh;
        uint64_t fh = pr->fp[1].hash ? pr->fp[1].hash : pr->prep_fh;
        unsigned vm = pg_log_shader_metadata("vs", st.program->stages[0].code_addr, vh, pr->fp[0].state, pr->vi);
        unsigned fm = pg_log_shader_metadata("fs", st.program->stages[1].code_addr, fh, pr->fp[1].state, pr->fi);
        LOG("[drawmeta] frame %llu mark %d program %016llx pipeline %016llx target %ux%u samples %u vertices %llu viewport %.0f,%.0f,%.0f,%.0f scissor %d,%d,%d,%d vs %llx hash %016llx fp %u metadata %u fs %llx hash %016llx fp %u metadata %u\n",
            (unsigned long long)pg_frame, pg_n - 1, (unsigned long long)pr->chash,
            (unsigned long long)pipe_key_hash(&pp->k), pass_w, pass_h, pass_samples, (unsigned long long)vc,
            st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
            st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3],
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)vh, pr->fp[0].state, vm,
            (unsigned long long)st.program->stages[1].code_addr, (unsigned long long)fh, pr->fp[1].state, fm);
    }
    dl_note("  draw vs %llx fs %llx (hash %016llx) count %u inst %u", (unsigned long long)st.program->stages[0].code_addr,
            (unsigned long long)st.program->stages[1].code_addr, (unsigned long long)g_cur_fs_hash, (unsigned)count, (unsigned)inst);
    if (dl_on == 3) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=3: each draw its own submission)
    if (scene_composite) {
        extern bool fg_enabled(void);
        extern void fg_scene_composited(VkCommandBuffer cb, TexBackend *rt);
        if (fg_enabled()) { vk_end_pass(); fg_scene_composited(vk_cmd(), st.rt.color[0]->backend); }
    }
    stat_draws++;
    cum_draws++;
    work_since_submit++;
    bc_draw(g_cur_fs_hash);
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
    }
}

// LARECOMP_MATERIAL_PROBE=1 + material-probe.request: one logical present's
// already prepared draw/clear state (first 2048 draws and 256 clears).
// The diagnostic adds no view lookup, upload, GPU readback or submit.
// CPU metadata and addresses stay in the app-private diagnostic file.
#define MATERIAL_PROBE_MAX_DRAWS 2048u
// An explicitly requested frame's first selected draw only. These are
// original CPU source bytes, not a readback of the renderer's cached GPU input.
#define MATERIAL_PROJECTION_MAX_BYTES (2u * 1024u * 1024u)
#define MATERIAL_PROJECTION_PAYLOAD_BYTES (MATERIAL_PROJECTION_MAX_BYTES - 65536u)
static bool material_projection_done;
static uint64_t material_probe_frame;
static uint64_t material_projection_fs = UINT64_C(0x44b584f0d922d49d);
static bool material_projection_full_stage_hash;
static bool material_projection_selector(const uint8_t *text, size_t size, uint64_t *hash, bool *full_stage) {
    if (!hash || !full_stage || (!text && size) || size > 64) return false;
    if (!size) { *hash = UINT64_C(0x44b584f0d922d49d); *full_stage = false; return true; }
    static const char prefix[] = "projection_fs=";
    size_t length = sizeof prefix - 1 + 16;
    if (size != length && !(size == length + 1 && text[length] == '\n')) return false;
    if (memcmp(text, prefix, sizeof prefix - 1)) return false;
    uint64_t value = 0;
    for (size_t i = sizeof prefix - 1; i < length; ++i) {
        unsigned c = text[i], n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return false;
        value = (value << 4) | n;
    }
    *hash = value; *full_stage = true;
    return true;
}
typedef struct {
    FILE *meta, *data;
    const SessionLayout *layout;
    uint64_t saved;
} MaterialProjection;

static bool material_projection_range(const SessionLayout *layout, uint64_t address, uint64_t size) {
    return layout && size && address >= layout->window.lo && address < layout->window.hi &&
           size <= layout->window.hi - address && address <= INT64_MAX && size <= INT64_MAX - address;
}

// The kernel validates all pages and copies under its mapping lock. There is
// no raw-pointer/proc-memory fallback if proof fails; failed payloads emit no bytes.
static uint8_t *material_projection_payload(MaterialProjection *p, const char *name, uint64_t address, uint64_t size) {
    const char *status = NULL;
    uint8_t *bytes = NULL;
    if (!material_projection_range(p->layout, address, size)) status = "outside-active-guest-window";
    else if (p->saved > MATERIAL_PROJECTION_PAYLOAD_BYTES || size > MATERIAL_PROJECTION_PAYLOAD_BYTES - p->saved) status = "payload-budget";
    else if (!(bytes = malloc((size_t)size))) status = "allocation-failed";
    if (!status && !kernel_copy_readable(bytes, address, size)) status = "kernel-copy-failed";
    uint64_t offset = p->saved;
    if (!status) {
        size_t written = fwrite(bytes, 1, (size_t)size, p->data);
        p->saved += written;
        if (written != size) status = "payload-write-failed";
    }
    fprintf(p->meta, "payload name=%s address=%llx requested=%llu offset=%llu bytes=%llu status=%s\n",
            name, (unsigned long long)address, (unsigned long long)size, (unsigned long long)offset,
            (unsigned long long)(status ? 0 : size), status ? status : "captured");
    if (status) { free(bytes); return NULL; }
    return bytes;
}

static void material_probe_view(FILE *f, const char *name, const NvnTextureView *v, bool has_view);
static void material_probe_texture(FILE *f, const NvnTexture *t);

static void material_sampler_metadata(FILE *f, int slot, uint32_t id, const NvnSamplerDesc *sampler, bool known) {
    fprintf(f, "sampler tex=%d id=%u source=%s", slot, id, sampler ? "pool" : known ? "renderer-default" : "unresolved");
    if (sampler) {
        uint32_t border[4]; memcpy(border, sampler->border, sizeof border);
        fprintf(f, " min=%u mag=%u wrap=%u,%u,%u compare_mode=%u compare_func=%u reduction=%u"
                   " lod_min=%.9g lod_max=%.9g lod_bias=%.9g max_aniso=%.9g"
                   " border=%.9g,%.9g,%.9g,%.9g border_bits=%08x,%08x,%08x,%08x",
                sampler->min_filter, sampler->mag_filter, sampler->wrap[0], sampler->wrap[1], sampler->wrap[2],
                sampler->compare_mode, sampler->compare_func, sampler->reduction,
                sampler->lod_min, sampler->lod_max, sampler->lod_bias, sampler->max_aniso,
                sampler->border[0], sampler->border[1], sampler->border[2], sampler->border[3],
                border[0], border[1], border[2], border[3]);
    }
    fputc('\n', f);
}

// Pool descriptors are host-owned state already selected by texture_info. This
// resolves a proven CB-backed handle through the same bounded-range resolver
// as rendering (a four-byte guest read for UBO handles), and creates no Vulkan
// view/sampler. Bindless handle zero can legitimately select pool entry zero.
static void material_projection_samplers(FILE *f, const Prog *p, const VkDescriptorImageInfo *images) {
    for (int i = 0; p->fi && i < p->fi->ntex && i < 64; ++i) {
        // Leave at least 16 KiB of the reserved metadata budget for bounded
        // vertex/index records. Report suppression instead of exceeding the
        // aggregate 2 MiB sidecar limit on a shader with many descriptors.
        long metadata_bytes = ftell(f);
        if (metadata_bytes < 0 || metadata_bytes > 48 * 1024) {
            fprintf(f, "samplers status=metadata-budget next_tex=%d remaining=%d\n", i, p->fi->ntex - i);
            break;
        }
        const ShaderTexture *image = &p->fi->tex[i];
        uint64_t handle = 0;
        bool known = !image->is_image && sampled_texture_handle(1, image, &handle);
        uint32_t tic = (uint32_t)handle & 0xfffff, tsc = (uint32_t)(handle >> 20) & 0xfff;
        const NvnTexEntry *entry = known && (handle || image->bindless) && st.tpool && st.tpool->entries && tic < (uint32_t)st.tpool->count ?
                                  &((const NvnTexEntry *)st.tpool->entries)[tic] : NULL;
        const NvnSamplerDesc *sampler = known && (handle || image->bindless) && st.spool && st.spool->entries && tsc < (uint32_t)st.spool->count ?
                                       &((const NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
        fprintf(f, "sampled tex=%d binding=%d handle_known=%u handle=%llx tic=%u sampler_id=%u type=%u shadow=%u uint=%u"
                   " bindless=%u cbuf=%u offset=%x descriptor_view=%llx descriptor_sampler=%llx layout=%d",
                i, 32 + i, (unsigned)known, (unsigned long long)handle, tic, tsc, image->type,
                (unsigned)image->shadow, (unsigned)image->is_uint, (unsigned)image->bindless,
                image->bindless_cbuf, image->bindless_offset, (unsigned long long)(uintptr_t)images[i].imageView,
                (unsigned long long)(uintptr_t)images[i].sampler, images[i].imageLayout);
        material_probe_texture(f, entry ? entry->tex : NULL);
        if (entry) material_probe_view(f, "view", &entry->view, entry->has_view);
        fputc('\n', f);
        material_sampler_metadata(f, i, tsc, sampler, known);
    }
}

static void material_projection_shader(MaterialProjection *cap, const Prog *p, unsigned stage) {
    const NvnShaderStage *shader = &st.program->stages[stage];
    uint64_t instructions = shader->const_offset, constants = shader->const_size;
    fprintf(cap->meta, "shader stage=%u guest=%llx code_span_bytes=%llu headers_included=1 constant_offset=%llu constant_bytes=%llu"
                       " cached_stage_hash=%016llx cached_bytes=%u source=current-guest-cpu not_exact_gpu_cache=1\n",
            stage, (unsigned long long)shader->code_addr, (unsigned long long)instructions,
            (unsigned long long)instructions, (unsigned long long)constants,
            (unsigned long long)p->fp[stage].hash, p->fp[stage].len);
    if (!instructions || constants > UINT64_MAX - instructions) {
        fprintf(cap->meta, "shader stage=%u status=unbounded-or-overflow\n", stage); return;
    }
    char name[24]; snprintf(name, sizeof name, "shader-%s", stage ? "fs" : "vs");
    uint8_t *bytes = material_projection_payload(cap, name, shader->code_addr, instructions + constants);
    if (!bytes) return;
    uint64_t hash = shader_code_hash(bytes, (size_t)(instructions + constants));
    fprintf(cap->meta, "shader stage=%u snapshot_stage_hash=%016llx cached_identity_match=%u\n",
            stage, (unsigned long long)hash,
            (unsigned)(hash == p->fp[stage].hash && instructions + constants == p->fp[stage].len));
    free(bytes);
}

static void material_projection_uniforms(MaterialProjection *cap, const Prog *p) {
    char name[32];
    // Vertex inputs/bone palettes get first claim on the shared payload budget.
    // Describe all bound or reflected buffers; never export an unused huge table
    // merely because the game left it bound. Unbound reflected buffers are zero.
    for (unsigned stage = 0; stage < 2; ++stage) {
        const ShaderInfo *info = stage ? p->fi : p->vi;
        for (unsigned cb = 3; cb <= 20; ++cb) {
            Range range = st.ubuf[stage][cb - 3];
            bool used = info && (info->cbuf_mask & (1u << cb));
            if (!used && !range.addr) continue;
            fprintf(cap->meta, "ubo stage=%u cbuf=%u slot=%u guest=%llx bound_bytes=%llu reflected_used=%u dynamic=%u"
                              " shader_bytes=%u status=%s\n", stage, cb, cb - 3,
                    (unsigned long long)range.addr, (unsigned long long)range.size, (unsigned)used,
                    (unsigned)(info && (info->cbuf_dynamic & (1u << cb))), info && cb < 18 ? info->cbuf_size[cb] : 0,
                    !range.addr ? "unbound-zero" : !used ? "metadata-only-not-reflected" : "capture-requested");
            if (!used || !range.addr) continue;
            snprintf(name, sizeof name, "ubo-%u-%u", stage, cb);
            free(material_projection_payload(cap, name, range.addr, range.size));
        }
    }
}
// Diagnostic CPU originals only. Never resolve a virtual pool or substitute an
// arbitrary host pointer; kernel_copy_readable still proves every copied page.
static const char *material_projection_texture_span(const NvnTexture *t, uint64_t *size) {
    if (!t || !size) return "no-texture";
    const TexBackend *b = t->backend;
    if (__atomic_load_n(&t->gpu_write_owned, __ATOMIC_ACQUIRE) || (b && b->gpu_owned)) return "gpu-owned";
    const NvnPool *pool = t->pool;
    if (!t->addr || !pool || !pool->storage) return "no-cpu-backing";
    if (pool->flags & 0x200) return "virtual-pool";
    if (pool->size > UINT64_MAX - pool->storage || t->pool_offset > pool->size || t->pool_offset > UINT64_MAX - pool->storage ||
        pool->storage + t->pool_offset != t->addr) return "pool-address-mismatch";
    const TexLayout *l = &t->layout;
    const FormatInfo *fmt = nvn_format_known(t->format) ? nvn_format_info(t->format) : NULL;
    // Bound all arithmetic performed by the existing Tegra layout functions,
    // including their uint32 intermediate products and mip/block shifts.
    if (!fmt || l->fmt != fmt || !fmt->bpb || fmt->bpb > 16 || !fmt->bw || fmt->bw > 16 || !fmt->bh || fmt->bh > 16 ||
        !l->width || l->width > 65536 || !l->height || l->height > 65536 || !l->depth || l->depth > 64 ||
        l->layers > 64 || !l->levels || l->levels > 16 || l->block_height_log2 > 5 || l->block_depth_log2 > 5)
        return "unproven-layout";
    uint64_t bytes;
    if (nvn_texture_row_linear(t->flags)) {
        if (l->levels != 1 || l->depth != 1 || l->layers > 1) return "unsupported-linear-layout";
        if (!nvn_texture_row_span(l, t->stride, &bytes)) return "linear-stride";
    } else bytes = tegra_total_size(l);
    if (!bytes || bytes > 65536) return "texture-byte-limit";
    if (bytes > pool->size - t->pool_offset) return "pool-range";
    *size = bytes;
    return NULL;
}
static void material_projection_texels(MaterialProjection *cap, const Prog *p) {
    unsigned inspected = 0;
    for (unsigned stage = 0; stage < 2; ++stage) {
        const ShaderInfo *info = stage ? p->fi : p->vi;
        for (int i = 0; info && i < info->ntex && i < 64; ++i) {
            if (inspected++ >= 8) { fprintf(cap->meta, "texels status=entry-limit max_entries=8\n"); return; }
            long metadata_bytes = ftell(cap->meta);
            if (metadata_bytes < 0 || metadata_bytes > 60 * 1024) { fprintf(cap->meta, "texels status=metadata-budget\n"); return; }
            const ShaderTexture *image = &info->tex[i];
            uint64_t handle = 0, bytes = 0;
            bool known = !image->is_image && sampled_texture_handle(stage, image, &handle);
            uint32_t tic = (uint32_t)handle & 0xfffff;
            const NvnTexEntry *entry = known && (handle || image->bindless) && st.tpool && st.tpool->entries && st.tpool->count > 0 && tic < (uint32_t)st.tpool->count ?
                                      &((const NvnTexEntry *)st.tpool->entries)[tic] : NULL;
            const NvnTexture *t = entry ? entry->tex : NULL;
            const char *status = material_projection_texture_span(t, &bytes);
            fprintf(cap->meta, "texels stage=%u tex=%d handle_known=%u handle=%llx tic=%u target_id=%llu"
                               " source=original-cpu-storage not_exact_gpu_cache=1 storage_layout=%s stride=%llu storage_bytes=%llu status=%s",
                    stage, i, (unsigned)known, (unsigned long long)handle, tic, (unsigned long long)(t ? t->debug_id : 0),
                    t && nvn_texture_row_linear(t->flags) ? "linear" : "tegra-gob", (unsigned long long)(t ? t->stride : 0),
                    (unsigned long long)bytes, status ? status : "capture-requested");
            material_probe_texture(cap->meta, t);
            if (entry) material_probe_view(cap->meta, "view", &entry->view, entry->has_view);
            fputc('\n', cap->meta);
            if (!status) {
                char name[32]; snprintf(name, sizeof name, "texels-%u-%d", stage, i);
                free(material_projection_payload(cap, name, t->addr, bytes));
            }
        }
    }
}

static void material_projection_capture(const Prog *p, const CmdRec *r, bool indexed, unsigned draw,
                                        const VkDescriptorImageInfo *images) {
    uint64_t hash = material_projection_full_stage_hash ? p->fp[1].hash : g_cur_fs_hash;
    if (material_projection_done || hash != material_projection_fs) return;
    material_projection_done = true;
    MaterialProjection cap = {.layout = session_layout_current()};
    cap.meta = fopen("material-probe-projection.log", "w");
    if (!cap.meta) return;
    cap.data = fopen("material-probe-projection.bin", "wb");
    if (!cap.data) { fputs("projection-source schema=1 status=payload-open-failed\n", cap.meta); fclose(cap.meta); return; }
    fprintf(cap.meta, "projection-source schema=2 frame=%llu draw=%u projection_fs=%016llx hash_kind=%s legacy_fs_hash=%016llx"
                      " stage_fs_hash=%016llx source=cpu-guest-memory not_exact_gpu_cache=1 max_total_bytes=%u\n",
            (unsigned long long)material_probe_frame, draw, (unsigned long long)material_projection_fs,
            material_projection_full_stage_hash ? "full-stage" : "legacy-prefix", (unsigned long long)g_cur_fs_hash,
            (unsigned long long)p->fp[1].hash, MATERIAL_PROJECTION_MAX_BYTES);
    fprintf(cap.meta, "draw indexed=%u mode=%llu count=%llu first_vertex=%llu index_type=%llu index_address=%llx first_index=0 index_pointer_contains_offset=1 base_vertex=%d base_instance=%u instances=%u primitive_restart=%u restart_index=%u\n",
            (unsigned)indexed, (unsigned long long)r->x[0], (unsigned long long)r->x[2],
            (unsigned long long)(indexed ? 0 : r->x[1]), (unsigned long long)(indexed ? r->x[1] : 0),
            (unsigned long long)(indexed ? r->x[3] : 0), indexed ? (int32_t)r->x[4] : 0,
            (uint32_t)r->x[indexed ? 5 : 3], (uint32_t)r->x[indexed ? 6 : 4], (unsigned)st.prim_restart, st.restart_index);
    for (unsigned i = 0; i < 7; ++i) fprintf(cap.meta, "argument x%u=%016llx\n", i + 1, (unsigned long long)r->x[i]);
    material_projection_shader(&cap, p, 0);
    material_projection_shader(&cap, p, 1);
    char name[32];
    material_projection_uniforms(&cap, p);
    material_projection_samplers(cap.meta, p, images);
    uint64_t last = 0;
    bool bounded_vertices = false;
    if (indexed && r->x[1] <= 2 && r->x[2] <= UINT32_MAX) {
        uint64_t width = UINT64_C(1) << r->x[1], count = r->x[2];
        uint8_t *indices = material_projection_payload(&cap, "indices", r->x[3], count * width);
        if (indices) {
            uint32_t maximum = 0, minimum = UINT32_MAX;
            for (uint64_t i = 0; i < count; ++i) {
                uint32_t v = 0; memcpy(&v, indices + i * width, (size_t)width);
                uint32_t restart = st.restart_index & (width == 4 ? UINT32_MAX : (1u << (8u * width)) - 1u);
                if (st.prim_restart && v == restart) continue;
                if (v > maximum) maximum = v;
                if (v < minimum) minimum = v;
            }
            int64_t low = (int64_t)minimum + (int32_t)r->x[4], high = (int64_t)maximum + (int32_t)r->x[4];
            bounded_vertices = minimum != UINT32_MAX && low >= 0 && high <= UINT32_MAX;
            if (bounded_vertices) last = (uint64_t)high;
            fprintf(cap.meta, "index_bounds minimum=%u maximum=%u effective_low=%lld effective_high=%lld usable=%u\n",
                    minimum, maximum, (long long)low, (long long)high, (unsigned)bounded_vertices);
            free(indices);
        }
    } else if (!indexed && r->x[1] <= UINT32_MAX && r->x[2] && r->x[2] <= UINT32_MAX - r->x[1]) {
        last = r->x[1] + r->x[2] - 1; bounded_vertices = true;
    } else fputs("geometry status=unsupported-draw-range\n", cap.meta);
    uint32_t streams = 0, stream_tail[16] = {0};
    for (unsigned i = 0; i < 32; ++i) if (p->vi && (p->vi->attr_mask & (1u << i))) {
        uint32_t word = i < (unsigned)st.nattrib && i < 16 ? st.attrib[i] : 0;
        unsigned stream = (word >> 8) & 31u;
        fprintf(cap.meta, "attribute slot=%u word=%08x format=%u stream=%u relative_offset=%u\n", i, word, word & 255u, stream, word >> 13);
        if ((word & 255u) && stream < 16) {
            streams |= 1u << stream;
            uint32_t end = (word >> 13) + 16u; // at most four 32-bit attribute components
            if (end > stream_tail[stream]) stream_tail[stream] = end;
        }
    }
    for (unsigned s = 0; s < 16; ++s) if (streams & (1u << s)) {
        uint64_t stride = s < (unsigned)st.nstream ? st.stream[s][0] : 0;
        uint64_t divisor = stride && s < (unsigned)st.nstream ? st.stream[s][1] : 0;
        uint64_t count = bounded_vertices ? last + 1 : 0;
        if (divisor) {
            uint64_t base = (uint32_t)r->x[indexed ? 5 : 3], instances = (uint32_t)r->x[indexed ? 6 : 4];
            count = (base + (instances ? instances : 1) + divisor - 1) / divisor;
        }
        fprintf(cap.meta, "stream slot=%u address=%llx bound_bytes=%llu stride=%llu divisor=%llu vertex_limit=%llu\n", s,
                (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size,
                (unsigned long long)stride, (unsigned long long)divisor, (unsigned long long)count);
        uint64_t tail = stream_tail[s] > 256 ? stream_tail[s] : 256;
        if (!count || (stride && count > (UINT64_MAX - tail) / stride)) { fprintf(cap.meta, "stream slot=%u status=unbounded-or-overflow\n", s); continue; }
        uint64_t size = count * stride + tail;
        if (size > st.vbuf[s].size) size = st.vbuf[s].size;
        snprintf(name, sizeof name, "vertices-%u", s);
        free(material_projection_payload(&cap, name, st.vbuf[s].addr, size));
    }
    // Geometry/UBOs keep priority under the unchanged aggregate payload cap.
    material_projection_texels(&cap, p);
    fprintf(cap.meta, "end saved_bytes=%llu source=cpu-guest-memory not_exact_gpu_cache=1\n", (unsigned long long)cap.saved);
    fclose(cap.data); fclose(cap.meta);
}
static int material_probe_enabled = -1;
static FILE *material_probe_file;
static uint64_t material_probe_polled = UINT64_MAX;
static unsigned material_probe_count;
static unsigned material_probe_clear_count;
static uint64_t material_probe_clear_omitted;
static bool material_probe_bound_enabled(void) {
    return material_probe_file && material_probe_count < MATERIAL_PROBE_MAX_DRAWS && material_probe_frame == vk_frame_counter();
}
static void material_probe_finish(const char *reason) {
    if (!material_probe_file) return;
    fprintf(material_probe_file, "end frame=%llu draws=%u clears=%u clears_omitted=%llu reason=%s\n",
            (unsigned long long)material_probe_frame, material_probe_count, material_probe_clear_count,
            (unsigned long long)material_probe_clear_omitted, reason);
    fclose(material_probe_file);
    material_probe_file = NULL;
    LOG("[material] captured frame %llu (%u draws, %s) in material-probe.log\n",
        (unsigned long long)material_probe_frame, material_probe_count, reason);
}
static void material_probe_poll(void) {
    if (material_probe_enabled < 0) {
        const char *env = getenv("LARECOMP_MATERIAL_PROBE");
        material_probe_enabled = (env && atoi(env) != 0) || sampling_trace_on();
    }
    if (!material_probe_enabled) return;
    uint64_t frame = vk_frame_counter();
    if (material_probe_file && frame != material_probe_frame) material_probe_finish("present-ended");
    if (frame == material_probe_polled) return;
    material_probe_polled = frame;
    struct stat sb;
    if (stat("material-probe.request", &sb) || !S_ISREG(sb.st_mode)) return;
    FILE *request = fopen("material-probe.request", "rb");
    if (!request) return;
    uint8_t selector[64];
    struct stat opened;
    bool regular = !fstat(fileno(request), &opened) && S_ISREG(opened.st_mode) && opened.st_size >= 0 && opened.st_size <= 64;
    size_t selector_bytes = regular ? fread(selector, 1, sizeof selector, request) : 0;
    bool read_ok = regular && !ferror(request);
    fclose(request);
    uint64_t selected; bool full_stage;
    if (!read_ok || !material_projection_selector(selector, selector_bytes, &selected, &full_stage)) {
        if (!unlink("material-probe.request")) LOG("[material] rejected malformed projection selector\n");
        return;
    }
    FILE *file = fopen("material-probe.log", "w");
    if (!file) return;
    if (unlink("material-probe.request")) { fclose(file); return; }
    material_probe_file = file; material_probe_frame = frame; material_probe_count = 0;
    material_probe_clear_count = 0; material_probe_clear_omitted = 0;
    material_projection_done = false;
    material_projection_fs = selected; material_projection_full_stage_hash = full_stage;
    fprintf(file, "material-probe schema=4 frame=%llu max_draws=%u max_clears=256 cpu_metadata_only=1 projection_fs=%016llx hash_kind=%s capture_starts=request-observation\n",
            (unsigned long long)frame, MATERIAL_PROBE_MAX_DRAWS, (unsigned long long)selected,
            full_stage ? "full-stage" : "legacy-prefix");
}
static void material_probe_view(FILE *f, const char *name, const NvnTextureView *v, bool has_view) {
    fprintf(f, " %s has=%u", name, (unsigned)has_view);
    if (has_view)
        fprintf(f, " setmask=%x levels=%u+%u layers=%u+%u format=%x target=%u ds=%u swizzle=%02x%02x%02x%02x",
                v->setmask, v->base_level, v->num_levels, v->min_layer, v->num_layers,
                v->format, v->target, v->ds_mode, v->swizzle[0], v->swizzle[1], v->swizzle[2], v->swizzle[3]);
}
static void material_probe_texture(FILE *f, const NvnTexture *t) {
    const TexBackend *b = t ? t->backend : NULL;
    fprintf(f, " texture=%llx", (unsigned long long)(uintptr_t)t);
    if (!t) return;
    fprintf(f, " format=%x extent=%ux%ux%u levels=%u samples=%u target=%u flags=%x swizzle=%02x%02x%02x%02x"
               " guest=%llx bytes=%llu pool=%llx pool_offset=%llu pool_bytes=%llu stride=%llu"
               " vk_format=%d vk_extent=%ux%u vk_image=%llx gpu_owned=%u",
            t->format, t->width, t->height, t->depth, t->levels, t->samples, t->target, t->flags,
            t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3],
            (unsigned long long)t->addr, (unsigned long long)(t->layout.fmt ? tegra_total_size(&t->layout) : 0),
            (unsigned long long)(t->pool ? t->pool->storage : 0), (unsigned long long)t->pool_offset,
            (unsigned long long)(t->pool ? t->pool->size : 0), (unsigned long long)t->stride,
            b ? b->format : 0, b ? b->width : 0, b ? b->height : 0,
            (unsigned long long)(uintptr_t)(b ? b->image : VK_NULL_HANDLE), (unsigned)(b && b->gpu_owned));
}
static void material_probe_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval,
                                 bool stencil, uint32_t sval, uint32_t smask, bool explicit_texture, VkRect2D normal,
                                 VkRect2D resolved, const char *path) {
    if (!material_probe_file || material_probe_frame != vk_frame_counter()) return;
    if (material_probe_clear_count >= 256) {
        if (material_probe_clear_omitted != UINT64_MAX) material_probe_clear_omitted++;
        return;
    }
    FILE *f = material_probe_file;
    unsigned event = material_probe_clear_count++;
    const NvnTexture *t = index < 0 ? st.rt.depth : index < st.rt.ncolor ? st.rt.color[index] : NULL;
    const NvnTextureView *v = index < 0 ? &st.rt.dview : index < 8 ? &st.rt.cview[index] : NULL;
    bool has_view = index < 0 ? st.rt.has_dview : index < 8 && st.rt.has_cview[index];
    unsigned level = has_view && (v->setmask & 1) ? v->base_level : 0;
    unsigned layer = has_view && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t bits[4] = {0}, depth_bits;
    if (color) memcpy(bits, color, sizeof bits);
    memcpy(&depth_bits, &dval, sizeof depth_bits);
    fprintf(f, "clear=%u frame=%llu draws_before=%u origin=%s index=%d kind=%d color_bits=%08x,%08x,%08x,%08x mask=%08x"
               " depth_enable=%u depth_value=%.9g depth_bits=%08x stencil_enable=%u stencil_value=%08x stencil_mask=%08x"
               " full_scope_test=%u selected_path=%s normal=%d,%d,%u,%u resolved=%d,%d,%u,%u full_pass=%u"
               " pass=%ux%u pass_scale=%.9g,%.9g raw_scissor=%d,%d,%d,%d raw_viewport=%.9g,%.9g,%.9g,%.9g"
               " target_id=%llu mip=%u layer_or_z=%u mip_extent=%ux%u\n",
            event, (unsigned long long)material_probe_frame, material_probe_count,
            explicit_texture ? "explicit-texture" : index < 0 ? "depth-stencil" : "color", index, kind,
            bits[0], bits[1], bits[2], bits[3], mask, (unsigned)depth, dval, depth_bits,
            (unsigned)stencil, sval, smask, (unsigned)(!explicit_texture && clear_scope_full_test()), path,
            normal.offset.x, normal.offset.y, normal.extent.width, normal.extent.height,
            resolved.offset.x, resolved.offset.y, resolved.extent.width, resolved.extent.height,
            (unsigned)(resolved.offset.x == 0 && resolved.offset.y == 0 && resolved.extent.width == pass_w && resolved.extent.height == pass_h),
            pass_w, pass_h, pass_sx, pass_sy, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3],
            st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
            (unsigned long long)(t ? t->debug_id : 0), level, layer,
            t && level < 32 ? (t->width >> level ? t->width >> level : 1) : 0,
            t && level < 32 ? (t->height >> level ? t->height >> level : 1) : 0);
    material_probe_view(f, "clear_view", v, has_view);
    material_probe_texture(f, t);
    fprintf(f, "\n");
}
// Host metadata only: selected formats come from the layout already used by
// this draw, including the dynamic-vertex-input path. Never query Vulkan or
// read vertex payloads just to diagnose an attribute conversion.
static void material_attribute_metadata(FILE *f, const ShaderInfo *shader, const ShaderKey *key,
        const VtxLayout *layout, const uint32_t *attributes, int nattrib,
        const uint32_t streams[16][2], int nstream, const Range *buffers) {
    uint32_t required = shader ? shader->attr_mask : 0;
    fprintf(f, " vertex_inputs required_mask=%08x supplied_count=%d stream_count=%d unlogged_required_mask=%08x\n",
            required, nattrib, nstream, required & 0xffff0000u);
    for (unsigned i = 0; i < 16; ++i) {
        bool supplied = (int)i < nattrib, needed = (required & (1u << i)) != 0;
        if (!supplied && !needed) continue;
        uint32_t raw = supplied ? attributes[i] : 0, format = raw & 255u, source = (raw >> 8) & 31u;
        bool selected = layout && (layout->attr_mask & (1u << i));
        uint32_t binding = selected ? layout->attr_bind[i] : UINT32_MAX;
        bool bound = selected && binding < 17;
        bool source_bound = supplied && source < 16 && (int)source < nstream;
        fprintf(f, " attribute slot=%u supplied=%u required=%u raw=%08x nvn_format=%x source_stream=%u relative_offset=%u"
                   " derived_kind=%d shader_kind=%u nominal_vk_format=%d selected_known=%u selected_vk_format=%u"
                   " binding=%u binding_offset=%u stride=%u divisor=%u source_stride=%u source_divisor=%u bound_bytes=%llu\n",
                i, (unsigned)supplied, (unsigned)needed, raw, format, source, raw >> 13, attr_kind(format),
                key ? key->attr_kind[i] : 0, format ? vertex_format(format) : VK_FORMAT_UNDEFINED,
                (unsigned)selected, selected ? layout->attr_fmt[i] : 0, binding, selected ? layout->attr_off[i] : 0,
                bound ? layout->stride[binding] : 0, bound ? layout->divisor[binding] : 0,
                source_bound ? streams[source][0] : 0, source_bound ? streams[source][1] : 0,
                (unsigned long long)(bound ? buffers[binding].size : 0));
    }
}

static void material_probe_draw(const Prog *p, const PipeKey *key, const CmdRec *r, bool indexed,
                                const VkDescriptorImageInfo *images, unsigned runs, const VtxLayout *layout) {
    FILE *f = material_probe_file;
    if (!f || material_probe_count >= MATERIAL_PROBE_MAX_DRAWS) return;
    unsigned draw = material_probe_count++;
    material_projection_capture(p, r, indexed, draw, images);
    fprintf(f, "draw=%u frame=%llu indexed=%u mode=%llu count=%llu runs=%u vs=%llx fs=%llx"
               " vs_hash=%016llx fs_hash=%016llx vs_len=%u fs_len=%u vs_const=%u+%u fs_const=%u+%u color=%02x%02x%02x%02x"
               " blend0=%02x%02x%02x%02x%02x%02x%02x%02x channel_mask=%08x pipe_blend=%u pipe_mask=%x"
               " vp=%.3f,%.3f,%.3f,%.3f scissor=%d,%d,%d,%d ds=%02x%02x%02x%02x%02x%02x%02x%02x\n",
            draw, (unsigned long long)material_probe_frame, (unsigned)indexed,
            (unsigned long long)r->x[0], (unsigned long long)r->x[2], runs,
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
            (unsigned long long)p->fp[0].hash, (unsigned long long)p->fp[1].hash, p->fp[0].len, p->fp[1].len,
            st.program->stages[0].const_offset, st.program->stages[0].const_size,
            st.program->stages[1].const_offset, st.program->stages[1].const_size,
            st.color_state[0], st.color_state[1], st.color_state[2], st.color_state[3],
            st.blend[0][0], st.blend[0][1], st.blend[0][2], st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6], st.blend[0][7],
            st.channel_mask, (unsigned)(key->blend_on & 1), key->write_mask[0],
            st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
            st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3],
            st.ds[0], st.ds[1], st.ds[2], st.ds[3], st.ds[4], st.ds[5], st.ds[6], st.ds[7]);
    material_attribute_metadata(f, p->vi, &p->vkey, layout, st.attrib, st.nattrib, st.stream, st.nstream, st.vbuf);
    fprintf(f, " depth_stencil requested_depth_test=%u requested_depth_write=%u depth_func=%u requested_stencil_test=%u"
               " attachment_depth_format=%d attachment_stencil_format=%d\n",
            st.ds[0], st.ds[1], st.ds[2], st.ds[3], key->depth, key->stencil);
    // ss is the state already recorded for this draw, including diagnostic
    // overrides and viewport clamping. Do not reconstruct it from requests.
    fprintf(f, " raster polygon=%02x%02x%02x%02x offset_factor=%.9g offset_units=%.9g offset_clamp=%.9g"
               " depth_range=%.9g,%.9g state_known=%08x resolved_cull=%u resolved_front=%u"
               " resolved_bias_enable=%u resolved_bias_constant=%.9g resolved_bias_slope=%.9g resolved_bias_clamp=%.9g"
               " resolved_depth_enable=%u resolved_depth_write=%u resolved_depth_compare=%u"
               " resolved_depth_range=%.9g,%.9g flip_y=%u depth_minus_one=%u vertex_clip_mask=%02x\n",
            st.poly[0], st.poly[1], st.poly[2], st.poly[3], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2],
            st.depth_range[0][0], st.depth_range[0][1], ss.known, (unsigned)ss.cull, (unsigned)ss.ff,
            (unsigned)ss.dbe, ss.db[1], ss.db[0], ss.db[2], (unsigned)ss.dt, (unsigned)ss.dw, (unsigned)ss.dop,
            ss.vp.minDepth, ss.vp.maxDepth, (unsigned)p->vkey.flip_y, (unsigned)p->vkey.depth_minus_one,
            (unsigned)(p->vi ? p->vi->clip_mask : 0));
    uint32_t ops = st.ds[5] | (uint32_t)st.ds[6] << 8 | (uint32_t)st.ds[7] << 16;
    for (unsigned face = 0; face < 2; ++face) {
        uint32_t o = ops >> (12 * face);
        fprintf(f, " stencil face=%s ref=%08x compare_mask=%08x write_mask=%08x func=%u fail=%u depth_fail=%u depth_pass=%u\n",
                face ? "back" : "front", st.stencil_ref[face], st.stencil_vmask[face], st.stencil_mask[face],
                (st.ds[4] >> (4 * face)) & 15u, o & 15u, (o >> 4) & 15u, (o >> 8) & 15u);
    }
    // No alpha-reference value is retained by this renderer. Unknown is distinct
    // from a real zero; collecting this state must not implement/change alpha test.
    fprintf(f, " fragment out_mask=%08x uses_discard=%u writes_depth=%u alpha_test_func=%u logic_op=%u alpha_ref_available=0\n",
            p->fi->out_mask, (unsigned)p->fi->uses_discard, (unsigned)p->fi->writes_depth, st.color_state[2], st.color_state[1]);
    const uint8_t *blend = key->blend[0];
    fprintf(f, " blend_vk enabled=%u src_color=%d dst_color=%d src_alpha=%d dst_alpha=%d color_op=%d alpha_op=%d rt_format=%d\n",
            (unsigned)(key->blend_on & 1), key->blend_on & 1 ? blend_factor(blend[1]) : 0,
            key->blend_on & 1 ? blend_factor(blend[2]) : 0, key->blend_on & 1 ? blend_factor(blend[3]) : 0,
            key->blend_on & 1 ? blend_factor(blend[4]) : 0, key->blend_on & 1 ? blend_op(blend[5]) : 0,
            key->blend_on & 1 ? blend_op(blend[6]) : 0, key->color[0]);
    fprintf(f, " attachments color_count=%d pipeline_color_count=%u\n", st.rt.ncolor, key->ncolor);
    for (int rt = 0; rt < st.rt.ncolor && rt < 8; ++rt) {
        fprintf(f, " rt%d attachment_format=%d write_mask=%x", rt, key->color[rt], key->write_mask[rt]);
        material_probe_texture(f, st.rt.color[rt]);
        material_probe_view(f, "view", &st.rt.cview[rt], st.rt.has_cview[rt]); fputc('\n', f);
    }
    fputs(" depth_attachment", f); material_probe_texture(f, st.rt.depth);
    material_probe_view(f, "view", &st.rt.dview, st.rt.has_dview); fputc('\n', f);
    for (int i = 0; i < p->fi->ntex && i < 64; i++) {
        const ShaderTexture *image = &p->fi->tex[i];
        int slot = image->is_image ? (int)(image->handle_offset / 4) : ((int)image->handle_offset - 0x20) / 8;
        uint64_t handle = 0;
        bool known;
        if (image->is_image) { known = !image->bindless && slot >= 0 && slot < 8; if (known) handle = st.img[1][slot]; }
        else known = sampled_texture_handle(1, image, &handle);
        uint32_t tic = (uint32_t)handle & 0xfffff;
        const NvnTexEntry *entry = known && (handle || image->bindless) && st.tpool && st.tpool->entries && tic < (uint32_t)st.tpool->count ?
                                  &((const NvnTexEntry *)st.tpool->entries)[tic] : NULL;
        fprintf(f, " tex=%d binding=%d handle_known=%u handle=%llx tic=%u sampler_id=%u type=%u shadow=%u image=%u"
                   " bindless=%u cbuf=%u offset=%x descriptor_view=%llx descriptor_sampler=%llx layout=%d",
                i, 32 + i, (unsigned)known, (unsigned long long)handle, tic, (unsigned)(handle >> 20) & 0xfff,
                image->type, (unsigned)image->shadow, (unsigned)image->is_image, (unsigned)image->bindless,
                image->bindless_cbuf, image->bindless_offset, (unsigned long long)(uintptr_t)images[i].imageView,
                (unsigned long long)(uintptr_t)images[i].sampler, images[i].imageLayout);
        material_probe_texture(f, entry ? entry->tex : NULL);
        if (entry) { material_probe_view(f, "view", &entry->view, entry->has_view); fprintf(f, " dead_format=%x", entry->dead_format); }
        fputc('\n', f);
        if (!image->is_image) {
            uint32_t tsc = (uint32_t)(handle >> 20) & 0xfffu;
            const NvnSamplerDesc *sampler = known && (handle || image->bindless) && st.spool && st.spool->entries &&
                st.spool->count > 0 && tsc < (uint32_t)st.spool->count ? &((const NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
            material_sampler_metadata(f, i, tsc, sampler, known);
        }
    }
    for (int stage = 0; stage < 2; stage++) {
        int logged = 0;
        for (int i = 0; i < 18 && logged < 6; i++) if (st.ubuf[stage][i].addr && st.ubuf[stage][i].size) {
            fprintf(f, " ubo stage=%d slot=%d cbuf=%d guest=%llx bytes=%llu\n", stage, i, i + 3,
                    (unsigned long long)st.ubuf[stage][i].addr, (unsigned long long)st.ubuf[stage][i].size);
            logged++;
        }
    }
    fflush(f);
    if (material_probe_count == MATERIAL_PROBE_MAX_DRAWS) material_probe_finish("draw-limit");
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw || b->bcn) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    alias_catch_up(t);
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)lroundf(t->width * tsx), (int32_t)lroundf(t->height * tsy), 1}},  // (the contents)
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        rec_CmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static bool draw_copy(VkCommandBuffer cb, NvnTexture *st_, NvnTexture *dt, TexBackend *s, TexBackend *d, uint32_t slv, uint32_t sly,
                      uint32_t dlv, uint32_t dly, int sx0, int sy0, int sw, int sh, int dx0, int dy0, int dw, int dh, bool linear);
// Scaled depth copies (the game's depth pyramid) are blits, but a depth format needn't support being a blit destination:
// Intel's drivers don't for D32_SFLOAT_S8_UINT (validation on a UHD 770; a suspect in the UHD 630's crash after a
// depth pass, issues #8, #46). There the depth goes the long way round with transfers only: to a buffer, into an
// unsigned-integer colour image with the same bits per texel, blitted (nearest: bits kept) into a second one at the
// new size, back to the buffer, and into the depth image. The regions are copy_texture's, already cut to the
// contents' extent (Dynamic resolution's sub-rect, issue #191's content_extent) and kept inside their levels.
// The scratch buffer and images are sized by the source and destination *levels* (not the regions, which follow the
// render scale), so a changing scale doesn't regrow them; outgrown ones are destroyed once the submissions that may
// use them are done. All of it (and depth_blit_ok's table) belongs to the translating thread: the render thread, or
// the game's GPU thread with LARECOMP_RENDER_THREAD=0 - copy_texture runs nowhere else.
static bool depth_blit_ok(VkFormat f) {
    static VkFormat known[8];
    static bool ok[8];
    static int n;
    for (int i = 0; i < n; i++) if (known[i] == f) return ok[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
    bool r = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (n < 8) { known[n] = f; ok[n++] = r; }
    if (!r) LOG("[vk] depth format %d can't be blitted here: scaled depth copies go through a colour image\n", f);
    return r;
}

typedef struct { VkImage img; VmaAllocation mem; uint32_t w, h; } ScratchImage;

static bool scratch_image(ScratchImage *si, uint32_t w, uint32_t h, VkFormat fmt) {
    if (si->img && si->w >= w && si->h >= h) return true;
    if (si->w > w) w = si->w;
    if (si->h > h) h = si->h;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return false;
    if (si->img) vk_defer_image(si->img, si->mem);  // (no views: never sampled or rendered to)
    vk_image_init_layout(vk_upload_cmd(), img, VK_IMAGE_ASPECT_COLOR_BIT);
    *si = (ScratchImage){img, mem, w, h};
    return true;
}

static void depth_blit_via_color(VkCommandBuffer cb, TexBackend *s, TexBackend *d, uint32_t slv, uint32_t sly, int sx, int sy,
                                 int sw, int sh, uint32_t dlv, uint32_t dly, int dx, int dy, int dw, int dh) {
    if (s->samples != 1 || d->samples != 1) return;  // (buffer copies and blits are single-sample only, as before)
    // bits per texel of the depth aspect in a buffer: 4 (D32 / X8_D24 variants) or 2 (D16)
    uint32_t bpp = s->format == VK_FORMAT_D16_UNORM || s->format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
    VkFormat cf = bpp == 2 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT;
    static VkBuffer buf;
    static VmaAllocation buf_mem;
    static VkDeviceSize buf_size;
    static struct { VkBuffer buf; VmaAllocation mem; uint64_t seq; } old[8];  // outgrown buffers, until their work is done
    static ScratchImage a[2], b[2];  // per colour format (R16 / R32)
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < 8; i++)
        if (old[i].buf && old[i].seq <= done) { vmaDestroyBuffer(g_vk.vma, old[i].buf, old[i].mem); old[i].buf = VK_NULL_HANDLE; }
    uint32_t slw = level_extent(s->width, slv), slh = level_extent(s->height, slv);
    uint32_t dlw = level_extent(d->width, dlv), dlh = level_extent(d->height, dlv);
    VkDeviceSize need = (VkDeviceSize)(slw * slh > dlw * dlh ? slw * slh : dlw * dlh) * bpp;
    if (need > buf_size) {
        if (need < buf_size * 2) need = buf_size * 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return;
        if (buf) {
            int i = 0;
            while (i < 8 && old[i].buf) i++;
            if (i < 8) old[i].buf = buf, old[i].mem = buf_mem, old[i].seq = vk_pending_seq();
            // (else kept, never freed: 8 outgrown in flight at once, with each at least twice the last - not seen)
        }
        buf = nb; buf_mem = nm; buf_size = need;
    }
    int k = bpp == 2 ? 0 : 1;
    if (!scratch_image(&a[k], slw, slh, cf) || !scratch_image(&b[k], dlw, dlh, cf)) return;
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, slv, sly, 1}, .imageOffset = {sx, sy, 0},
                           .imageExtent = {(uint32_t)sw, (uint32_t)sh, 1}};
    rec_CmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_full_barrier(cb);
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = (VkOffset3D){0, 0, 0};
    rec_CmdCopyBufferToImage(cb, buf, a[k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {dw, dh, 1}}};
    rec_CmdBlitImage(cb, a[k].img, VK_IMAGE_LAYOUT_GENERAL, b[k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    c.imageExtent = (VkExtent3D){(uint32_t)dw, (uint32_t)dh, 1};
    rec_CmdCopyImageToBuffer(cb, b[k].img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_full_barrier(cb);
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, dlv, dly, 1};
    c.imageOffset = (VkOffset3D){dx, dy, 0};
    rec_CmdCopyBufferToImage(cb, buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)lroundf(st_->width * ssx), sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)lroundf(st_->height * ssy);
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)lroundf(dt->width * dsx), dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)lroundf(dt->height * dsy);
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the contents' extent in the level, and every region stays inside its level: scaled
        // allocations (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies /
        // blits are undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's
        // users). Dynamic resolution draws into the top-left part of the allocation: the rest holds older frames, and
        // a whole-image copy of it into a texture of another scale (the save slot's screenshot, 192x108) took those
        // along (the frame small in a corner, stale frames around it)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        int scw = content_extent(slw, s->scalable, s->sx, s->ax), sch = content_extent(slh, s->scalable, s->sy, s->ay);
        int dcw = content_extent(dlw, d->scalable, d->sx, d->ax), dch = content_extent(dlh, d->scalable, d->sy, d->ay);
        bool same_part = slw == dlw && slh == dlh && scw == dcw && sch == dch;  // (contents in the same part of equal images)
        if (!sr[3]) sw = scw - sx0;
        if (!sr[4]) sh = sch - sy0;
        if (!dr[3]) dw = dcw - dx0;
        if (!dr[4]) dh = dch - dy0;
        if (same_part && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == scw && sh == sch && dw == dcw && dh == dch)
            sw = dw = slw, sh = dh = slh;  // (the same part of images of one size: the whole image, which may alias)
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases || nstale) { alias_catch_up(st_); s = alias_read(st_)->backend; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    bc_mark(cb, "copy texture %ux%u -> %ux%u (images %ux%u -> %ux%u)", src->tex ? ((NvnTexture *)(uintptr_t)src->tex)->width : 0,
            src->tex ? ((NvnTexture *)(uintptr_t)src->tex)->height : 0, dst->tex ? ((NvnTexture *)(uintptr_t)dst->tex)->width : 0,
            dst->tex ? ((NvnTexture *)(uintptr_t)dst->tex)->height : 0, s ? s->width : 0, s ? s->height : 0, d ? d->width : 0,
            d ? d->height : 0);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    {
        bool dep = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!dep && draw_copy(cb, st_, dt, s, d, slv, sly, dlv, dly, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
                              (flags & 1) && !rt_kind(st_->format) && !(sw == dw && sh == dh)))
            return;
    }
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    static int via_color = -1;  // LARECOMP_VK_DEPTH_BLIT_VIA_COLOR=1: always (tests on GPUs that can blit depth)
    if (via_color < 0) via_color = getenv("LARECOMP_VK_DEPTH_BLIT_VIA_COLOR") ? atoi(getenv("LARECOMP_VK_DEPTH_BLIT_VIA_COLOR")) : 0;
    if (depth && (sa & da & VK_IMAGE_ASPECT_DEPTH_BIT) && (via_color || !depth_blit_ok(d->format) || !depth_blit_ok(s->format))) {
        dl_note("  depth blit via colour %p lv%u (%d,%d %dx%d) -> %p lv%u (%d,%d %dx%d)", (void *)s->image, slv, sx0, sy0, sw, sh,
                (void *)d->image, dlv, dx0, dy0, dw, dh);
        depth_blit_via_color(cb, s, d, slv, sly, sx0, sy0, sw, sh, dlv, dly, dx0, dy0, dw, dh);
        return;
    }
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    rec_CmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

// ---- decoded video into its texture (textures.c renderer_upload_nv12 / upload_pending, render thread): the NV12
// planes go into two images (R8, R8G8: reused while the size stays), and a fullscreen draw converts them into the
// texture through a UNORM view (the bytes as the RGBA upload wrote them: no sRGB encoding). Recorded into the upload
// command buffer, ahead of the frame that samples the texture.
static bool nv12_plane(TexBackend *p, VkFormat f, uint32_t w, uint32_t h, VkCommandBuffer cb) {
    if (p->image) vk_views_forget(p);  // (a new size: the old image is left to the driver's teardown - one per movie size)
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *p = (TexBackend){0};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &p->image, &p->mem, NULL) != VK_SUCCESS) { p->image = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(cb, p->image, VK_IMAGE_ASPECT_COLOR_BIT);
    p->format = f; p->width = w; p->height = h; p->depth = 1; p->levels = p->layers = p->samples = 1;
    p->aspect = VK_IMAGE_ASPECT_COLOR_BIT; p->base_type = VK_IMAGE_VIEW_TYPE_2D; p->gpu_owned = true;
    return true;
}

void vk_convert_nv12(TexBackend *b, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    static TexBackend py, puv;
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout layout;
    static VkPipeline pipes[2];  // RGBA8, BGRA8 targets
    VkCommandBuffer cb = vk_upload_cmd();
    bool bgra = b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB;
    VkFormat raw = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if (!layout) {
        VkDescriptorSetLayoutBinding bd[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &dsl));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
        LOG("[movie] video frames converted to RGBA on the GPU\n");
    }
    if (!pipes[bgra]) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vk_builtin_shader("nv12.vert"), .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = vk_builtin_shader("nv12.frag"), .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                                .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                                   .pAttachments = &cba};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                               .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &raw};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                           .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                           .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout};
        if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pipes[bgra]) != VK_SUCCESS) {
            LOG("[movie] NV12 pipeline failed\n");
            pipes[bgra] = VK_NULL_HANDLE;
            return;
        }
    }
    if (py.width != w || py.height != h || !py.image)
        if (!nv12_plane(&py, VK_FORMAT_R8_UNORM, w, h, cb) || !nv12_plane(&puv, VK_FORMAT_R8G8_UNORM, w / 2, h / 2, cb)) return;
    VkComponentMapping ident = {0};  // (VK_COMPONENT_SWIZZLE_IDENTITY)
    VkImageView vy = vk_image_view(&py, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView vuv = vk_image_view(&puv, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8_UNORM, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView vt = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, raw, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!vy || !vuv || !vt) return;
    VkSpan s = vk_transient_copy(nv12, (uint64_t)uv_off + (uint64_t)stride * (h / 2), 16);
    VkBufferImageCopy c[2] = {
        {.bufferOffset = s.off, .bufferRowLength = stride, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {w, h, 1}},
        {.bufferOffset = s.off + uv_off, .bufferRowLength = stride / 2, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {w / 2, h / 2, 1}}};
    vk_full_barrier(cb);
    rec_CmdCopyBufferToImage(cb, s.buf, py.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c[0]);
    rec_CmdCopyBufferToImage(cb, s.buf, puv.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c[1]);
    vk_full_barrier(cb);
    VkDescriptorSet set = vk_alloc_set(dsl);
    VkDescriptorImageInfo ii[2] = {{vk_default_sampler(true), vy, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(true), vuv, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
    rec_UpdateDescriptorSets(2, wr);
    uint32_t rw = w < b->width ? w : b->width, rh = h < b->height ? h : b->height;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = vt,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {rw, rh}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[bgra]);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)rw, (float)rh, 0, 1};
    VkRect2D sc = {{0, 0}, {rw, rh}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
}

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        // (vertex: flip + crop, 32 bytes; fragment: the screen filter, 16 more)
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;
static VkPipeline post_pipeline_kind(const char *fs, int kind, VkFormat dsfmt);

// fmt: the output's format; kind: 0 a plain post pass; 1 SMAA edges also writing stencil 1 where they draw, 2 SMAA
// weights only where the stencil is 1 (g_vk.smaa_stencil; dsfmt: the stencil format)
static VkPipeline post_pipeline_full(const char *fs, VkFormat fmt, int kind, VkFormat dsfmt) {
    static struct { const char *fs; VkFormat fmt; int kind; VkFormat f; VkPipeline p; } cache[32];
    for (int i = 0; i < 32; i++)
        if (cache[i].p && cache[i].fs == fs && cache[i].fmt == fmt && cache[i].kind == kind && cache[i].f == dsfmt) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (kind == 1 || kind == 2) {
        ri.stencilAttachmentFormat = dsfmt;
        VkStencilOpState so = {.failOp = VK_STENCIL_OP_KEEP, .passOp = kind == 1 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP,
                               .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = kind == 1 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_EQUAL,
                               .compareMask = 0xFF, .writeMask = kind == 1 ? 0xFF : 0, .reference = 1};
        ds.stencilTestEnable = VK_TRUE;
        ds.front = ds.back = so;
    }
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 32; i++)
        if (!cache[i].p) { cache[i].fs = fs; cache[i].fmt = fmt; cache[i].kind = kind; cache[i].f = dsfmt; cache[i].p = p; break; }
    return p;
}
static VkPipeline post_pipeline_fmt(const char *fs, VkFormat fmt) { return post_pipeline_full(fs, fmt, 0, VK_FORMAT_UNDEFINED); }
static VkPipeline post_pipeline_kind(const char *fs, int kind, VkFormat dsfmt) {
    return post_pipeline_full(fs, VK_FORMAT_R8G8B8A8_UNORM, kind, dsfmt);
}

// Texture copies as a draw (copy_texture): a fullscreen draw sampling the source, through the 3D pipeline instead of
// the transfer one. Tried against the S20 FE's rare GPU hangs (breadcrumbs once stopped inside the game's frame copy):
// the hang moved on to the next step, so not the copy - kept for tests. Single-sample float / unorm colour images whose destination format renders, 2D
// levels / layers; anything else returns false (the transfer path).
// LARECOMP_VK_DRAW_COPY=1.
static bool draw_copy(VkCommandBuffer cb, NvnTexture *st_, NvnTexture *dt, TexBackend *s, TexBackend *d, uint32_t slv, uint32_t sly,
                      uint32_t dlv, uint32_t dly, int sx0, int sy0, int sw, int sh, int dx0, int dy0, int dw, int dh, bool linear) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_DRAW_COPY");
        on = e && atoi(e) != 0;
    }
    if (!on || s->samples != 1 || d->samples != 1 || s->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        s->depth > 1 || d->depth > 1 || rt_kind(st_->format) || rt_kind(dt->format))
        return false;
    // the same format: the bytes as they are (vkCmdCopyImage), sRGB read and written raw; different formats: converted as a
    // blit converts (sRGB decoded when read, encoded when written)
    VkFormat sf = s->format, df = d->format;
    if (sf == df) {
        sf = df = s->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : s->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
                                                                                                                       : s->format;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, df, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, d->format, &fp);  // (the image was made renderable only if this is)
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
    static const VkComponentMapping id = {0};
    VkImageView sv = vk_image_view(s, VK_IMAGE_VIEW_TYPE_2D, sf, slv, 1, sly, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView dv = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, df, dlv, 1, dly, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!sv || !dv) return false;
    VkPipeline pipe = post_pipeline_fmt("copy.frag", df);
    uint32_t slw = level_extent(s->width, slv), slh = level_extent(s->height, slv);
    uint32_t dlw = level_extent(d->width, dlv), dlh = level_extent(d->height, dlv);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRect2D area = {{dx0, dy0}, {(uint32_t)dw, (uint32_t)dh}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(linear), sv, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dlw, (float)dlh, 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &area);
    // source texel = src origin + (pixel - dst origin) * (src size / dst size), normalised by the source level's size
    float pc[8] = {(float)sx0, (float)sy0, (float)sw / (float)dw, (float)sh / (float)dh, (float)dx0, (float)dy0, 1.0f / (float)slw,
                   1.0f / (float)slh};
    rec_CmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        rec_CmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
// post_area_w / _h (> 0): only that top-left part of dst (Dynamic resolution: the contents' part)
static uint32_t post_area_w, post_area_h;
// (g_vk.smaa_stencil) a stencil image at w x h for SMAA's edge mask (kept until the size changes; NULL: none usable)
static TexBackend *smaa_stencil_image(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    static TexBackend img;
    static bool unusable;
    TexBackend *d = &img;
    if (unusable) return NULL;
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        static const VkFormat cand[3] = {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        VkFormat f = VK_FORMAT_UNDEFINED;
        for (int i = 0; i < 3 && !f; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, cand[i], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) f = cand[i];
        }
        if (!f) { unusable = true; LOG("[vk] SMAA stencil: no stencil format, off\n"); return NULL; }
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL) != VK_SUCCESS) { d->image = VK_NULL_HANDLE; return NULL; }
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1; d->samples = 1;
        d->aspect = f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(cb, d->image, d->aspect);
        LOG("[vk] SMAA stencil image %ux%u format %d\n", w, h, f);
    }
    return d;
}

static void post_pass_ex(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                         bool clear, TexBackend *sten, int kind);
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    post_pass_ex(cb, fs, srcs, n, dst, pc, clear, NULL, 0);
}
// sten / kind (g_vk.smaa_stencil): kind 1 clears sten and writes 1 where the pass draws, kind 2 draws only there
static void post_pass_ex(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                         bool clear, TexBackend *sten, int kind) {
    if (!sten) kind = 0;
    uint32_t aw = post_area_w && post_area_w < dst->width ? post_area_w : dst->width;
    uint32_t ah = post_area_h && post_area_h < dst->height ? post_area_h : dst->height;
    // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    VkPipeline pipe = post_pipeline_kind(fs, kind, sten ? sten->format : VK_FORMAT_UNDEFINED);
    vk_full_barrier(cb);
    bc_mark(cb, "post %s %ux%u", fs, dst->width, dst->height);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {aw, ah}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    VkRenderingAttachmentInfo sa = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (kind) {
        static const VkComponentMapping id = {0};
        sa.imageView = vk_image_view(sten, VK_IMAGE_VIEW_TYPE_2D, sten->format, 0, 1, 0, 1, id, sten->aspect);
        sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        sa.loadOp = kind == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.storeOp = kind == 1 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ri.pStencilAttachment = &sa;
    }
    rec_CmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    rec_UpdateDescriptorSets((uint32_t)n, w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {aw, ah}};  // (the viewport stays the whole image: gl_FragCoord is the pixel)
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, float fx, float fy, double ow_d, double oh_d, bool *upscaled) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    uint32_t cw = (uint32_t)lroundf(b->width * fx), ch = (uint32_t)lroundf(b->height * fy);  // (the contents' size)
    bool up = ow > cw + 1 || oh > ch + 1;  // (an upscale only)
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && up;
    // (SGSR, phones: the whole texture - phones allocate at the render scale, so the contents fill it)
    bool sgsr = g_settings.upscaler == UPSCALE_SGSR && up && cw == b->width && ch == b->height;
    *upscaled = fsr || sgsr;
    if (!smaa && !fsr && !sgsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        // Dynamic resolution: only the contents' part, and 64 pixels past it (the edge searches reach 2 x 16 pixels, so
        // the contents' pixels never read past it) - the window step's cost then scales with the factor like the game's
        post_area_w = cw + 64, post_area_h = ch + 64;
        // (g_vk.smaa_stencil, integrated GPUs) the weights only where the edge pass found edges (its shader discards
        // the others), as SMAA's reference integration does with a stencil
        TexBackend *sten = g_vk.smaa_stencil ? smaa_stencil_image(cb, b->width, b->height) : NULL;
        VkImageView in[3] = {v};
        post_pass_ex(cb, "smaa_edge.frag", in, 1, edges, m, true, sten, 1);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass_ex(cb, "smaa_weight.frag", wi, 3, weights, m, true, sten, 2);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        post_area_w = post_area_h = 0;
        v = post_view(out);
    }
    if (sgsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *s = post_image(3, ow, oh);
        float ps[8] = {1.0f / iw, 1.0f / ih, iw, ih, (float)ow, (float)oh};
        VkImageView si[3] = {v};
        post_pass(cb, "sgsr.frag", si, 1, s, ps, false);
        v = post_view(s);
    }
    if (fsr) {
        float iw = (float)cw, ih = (float)ch;  // (EASU fetches texels of the contents: a larger texture needs no uv scale)
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    bc_mark(cb, "present (window image)");
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr + 2 * sgsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : sgsr ? " sgsr" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr + 2 * sgsr;
    }
    return v;
}

static VkImageView g_present_src;  // the picture the last present drew from (post-processed): framegen.c samples it

// The bars' backdrop: the picture last presented (vk_present_draw), uv rect uvr sampled blurred and dimmed into the
// window rect (x, y, w, h)
void vk_present_backdrop(VkCommandBuffer cb, float x, float y, float w, float h, const float uvr[4], bool flip, float light) {
    if (!g_present_src || w < 1 || h < 1) return;
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), g_present_src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &wr);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    extern VkViewport vk_sc_viewport(float x, float y, float w, float h);
    VkViewport vp = vk_sc_viewport(x, y, w, h);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    float pc[12] = {flip ? 1.0f : 0.0f, (float)g_vk.sc_quarter, light, 0, uvr[0], uvr[1], uvr[2], uvr[3]};
    screen_filter_params(pc + 8);  // (the bars take the screen filter too, as the picture)
    rec_CmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &full);
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
VkImageView vk_last_present_src(void) { return g_present_src; }
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    // Dynamic: the image occupies the top-left part of the texture; FSR maps that part to its whole output, otherwise
    // the present samples the part
    float fx = b->scalable && b->ax > 0 ? b->sx / b->ax : 1, fy = b->scalable && b->ay > 0 ? b->sy / b->ay : 1;
    bool upscaled = false;
    VkImageView src = post_process(cb, b, fx, fy, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh, &upscaled);
    float pvc[8];
    memcpy(pvc, pv, sizeof pvc);
    if (!upscaled) { pvc[4] *= fx; pvc[5] *= fy; pvc[6] *= fx; pvc[7] *= fy; }
    pv = pvc;
    g_present_src = src;
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    extern VkViewport vk_sc_viewport(float x, float y, float w, float h);
    VkViewport vp = vk_sc_viewport(vx, vy, vw, vh);  // (turned with the swapchain's pre-rotation, as the triangle: pv[1])
    rec_CmdSetViewport(cb, 0, 1, &vp);
    float pc[12];
    memcpy(pc, pv, 32);
    pc[1] = (float)g_vk.sc_quarter;
    screen_filter_params(pc + 8);
    rec_CmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
// Only the part its contents occupy (Dynamic resolution: the top-left part; the rest holds older frames).
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    float fx = (float)content_extent((int)b->width, b->scalable, b->sx, b->ax) / (float)b->width;
    float fy = (float)content_extent((int)b->height, b->scalable, b->sy, b->ay) / (float)b->height;
    crop[0] *= fx; crop[1] *= fy; crop[2] *= fx; crop[3] *= fy;
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &wr);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    float pv[12] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    if (strstr(path, "frame_")) screen_filter_params(pv + 8);  // (shots of the game image: as shown)
    rec_CmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pv);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
// Early submits: a game hands over a frame's lists at once, and a list went to the GPU only once fully translated - the
// GPU idled while the shadow, depth and compute work at the list's start waited behind the rest (the S20 FE: ~10 ms a
// frame, and the game then waits on that frame's fence). Between commands, outside a render pass, the work recorded
// so far goes out once it holds enough draws. LARECOMP_VK_EARLY_SUBMIT=<draws> (0: off; default 48 on Android).
static void early_submit_check(void) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("LARECOMP_VK_EARLY_SUBMIT");
#ifdef __ANDROID__
        every = e ? atoi(e) : 48;
#else
        every = e ? atoi(e) : 0;
#endif
    }
    if (!every || work_since_submit < (uint32_t)every) return;
    // LARECOMP_VK_EARLY_SUBMIT_PASS=1: inside a pass too, at twice that - it ends there and the next draw resumes it
    // (attachments loaded). Measured on the S20 FE: more GPU time (tile load / store), no shorter waits: off
    static int in_split = -1;
    if (in_split < 0) in_split = getenv("LARECOMP_VK_EARLY_SUBMIT_PASS") ? atoi(getenv("LARECOMP_VK_EARLY_SUBMIT_PASS")) : 0;
    if (in_pass && (!in_split || work_since_submit < 2u * (uint32_t)every)) return;
    renderer_submit();  // (ends the pass)
}

static void exec_list(uint64_t handle, int depth) {
    uint64_t p = nvn_command_resolve(handle);
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: bind_program_stages((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS_DEFERRED:
            x = (void *)(uintptr_t)r->x[3];
            /* fall through: deferred arrays are read while submission is replayed. */
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS_DEFERRED:
            x = (void *)(uintptr_t)r->x[3];
            /* fall through */
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES_DEFERRED:
            x = (void *)(uintptr_t)r->x[3];
            /* fall through */
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0, false);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0, true);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = clear_scope_covers(d->width, d->height, st.scissor[0]) &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                if (clear_scope_full_test()) for (int i = 0; i < st.rt.ncolor; i++) {
                    NvnTexture *c = st.rt.color[i];
                    unsigned level = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
                    // Full-pass is not full-image when another attachment's mip
                    // reduces the area; unknown/extreme mips fail conservatively.
                    if (c && !clear_scope_attachment_covers(c->width, c->height, level, d->width, d->height)) whole = false;
                    const TexBackend *cb = c ? c->backend : NULL, *db = d->backend;
                    if (c && (!cb || !db)) whole = false; // No proof of the actual pass area yet.
                    if (cb && db && !clear_scope_attachment_covers(cb->width, cb->height, level, db->width, db->height)) whole = false;
                }
                if (clear_scope_full_test() && (d->levels > 1 || d->depth > 1 || !d->backend)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2], false);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            if (uniform_forward_enabled() && r->x[0] > UINT64_MAX - r->x[2])
                FATAL("[vk] explicit uniform update address overflows\n");
            mirror_uniform_explicit_update(r->x[0] + r->x[2], x, r->x[3]);
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            out[0] = 0;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            uint64_t *pair = prev_out;
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) LARECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = getenv("LARECOMP_TEST_COUNTER_DELAY_MS") ? atof(getenv("LARECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->start[q->n] = pair && (uint8_t *)out - (uint8_t *)pair == 16 ? pair : NULL;
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            wait_why = "buffer -> buffer copy";
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        early_submit_check();
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    PROF_T0();
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}
