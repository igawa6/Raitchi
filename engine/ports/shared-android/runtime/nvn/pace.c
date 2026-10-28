/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Frame pacing, shared by the Metal (metal.m) and Vulkan (vulkan/device.c) presents: each presented frame calls
// pace_frame() on the presenting thread, and the backends pass the game's timing counters through pace_end_counter().
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../game_addresses.h"
#include "../rt.h"
#include "../common_session.h"
#include "pace.h"
#include "../settings.h"
#include "slowframe.h"

// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks the mode itself every frame (its frame controller f_700000c910, from
// 0x700000ca44): the extra vsyncs it waits next become those its last frame missed, one fewer when that frame's CPU time
// and GPU time (its timing counters) were both under its limits (13.33 / 14.17 ms), kept between a least and a most
// (0 and 1, on the game manager). One missed vsync takes it to 30, and one quick frame - or one that came a vsync early
// after a late one - back to 60: no hysteresis. Our frames can be slow for reasons its GPU time doesn't show (command
// translation, a slower CPU), and a machine near the edge flipped 60 / 30 every few frames (a Steam Deck at 1x: 35-45
// fps, issue #40), also while the counters below reported slow frames.
// So each presented frame's busy time is measured (present to present, minus the time the presenting thread was
// blocked in kernel waits, sleeps and the window's image: idle); when frames don't fit 60 (12 of the last 60 over
// 16.2 ms or, in gameplay, shown a vsync late - see pace_frame -, and the median frame within ~3 ms of that: a few
// hitches or spikes in frames that otherwise fit don't count), the game is held in its 30 fps mode: its least extra
// vsyncs becomes 1, and the counters report a frame of at least PACE_SLOW_NS as before (its own dynamic resolution,
// which read them too, is off: tools/patches.py GAME_DYNRES_SITE).
// Its 30 fps mode is two world steps a frame (+0x4e0 counts the extra one down) and a present interval of 2 (+0x4e1 + 1,
// to nvnWindowSetPresentInterval). On the Switch the display waits out that interval; here nothing did, so a held
// frame that fit in one vsync was still shown after one: the game asked for interval 2 the whole hold, yet a Steam
// Deck's held frames were half 16.7 ms apart (35-45 fps, issue #40) and a Mac's with headroom 9 in 10. So while held,
// the presenting thread waits out the interval itself (hold_cadence): two vsyncs a frame.
// It can't flip back on a quick frame then; we let it go back on headroom, by what a frame would cost at 60: the game
// thread's busy time less its second world step, or the render thread's CPU time for the frame when that's more (at 30
// a render-bound machine's game thread barely waits for it: its busy time alone would look like headroom):
//   clear: under 11 ms on average and none of the last 60 over 14.5 ms, 60 frames in a row (2 s at 30) times the
//     backoff up to 4x (a light scene after a heavy one doesn't wait long);
//   enough: the median under 13.5 ms and at most 4 of the last 60 over 16.2 ms, 150 frames in a row (5 s at 30) times
//     the backoff.
// Both count gameplay frames of one scene only (issue #92: a Vega 8 went back to 60 on the pause menu, ~13 passes a
// frame, then ran gameplay at 40-55 fps - slow motion - for 2-4 s until held again): frames without the world on screen
// (the pause menu, title, file select: no scene composite, mirror.c) don't count, and a scene change (level, camera
// zone - a dungeon room -, the menu opening or closing, a load) starts both over. And when the backend times the GPU
// (pace_gpu_ns), its time per frame must fit too, on average under PACE_GPU_MS: CPU headroom says nothing on a
// GPU-bound machine (under PACE_GPU_LATE_MS, or PACE_GPU_LATE_BACKOFF_MS when backing off, when frames shown late
// started the hold). (Held, the game draws once per frame as at 60 - the second world step is CPU work only - so its
// GPU time per frame is what a frame at 60 would take, or more: a GPU at half load may clock down.)
// Backoff (issues #70, #40): a machine right at the edge (a handheld on a low power profile) gains headroom at 30 fps
// just by cooling down, goes back to 60, and doesn't fit again a few seconds later - 60 / 30 flicker. Falling back
// within 20 s of going back doubles the wait (up to 16x: 80 s for "enough"); a minute at 60 resets it.
// Hitches aren't frames that don't fit 60 (an M3 Max at 5120x1440, GPU 10-13 ms a frame, held at 30 after door jumps,
// save commits, bushes' items spawning, texture hash stalls): one stall makes the next frames come a vsync late in a
// burst (12 of 60, none by busy time), then the release waited for a GPU time it never reached. So:
// - for PACE_GRACE_NS after a hitch (pace_hitch: a level load or change, a door jump, a save commit, a movie, an item
//   spawned, a render scale change; or a frame over 100 ms) frames are left out of the count, late or busy;
// - the frames over must be spread through the window (some in each quarter of it: 15 frames, 1/4 s at 60), not
//   bunched as after a hitch nothing signalled.
// LARECOMP_PACE=0: off (the game's own choice, flips and all).
#define PACE_SLOW_NS 20000000ull
#define PACE_GPU_MS 15.0  // (16.7 ms a frame at 60, less some for the window image and jitter)
// A hold that frames shown late started (see pace_frame): the GPU's own time must leave room for the window's image and
// the present. 13 ms (78% of 16.7) on the first try; after a failed return to 60 (backing off), 10. Measured on an M3
// Max at 5120x1440: back at 13.8 ms, slow motion at once; at 10-13 ms (5.2-6.7 in light scenes) a steady 60.
#define PACE_GPU_LATE_MS 13.0
#define PACE_GPU_LATE_BACKOFF_MS 10.0
#define PACE_GRACE_NS 1000000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

// Holds the game in its 30 fps mode (see above): the least number of extra vsyncs its frame controller may choose
// (game manager +0x4e2, normally 0) is 1 while slow, the game's own value again after. Re-asserted every frame while held.
// Returns whether the game is held (not when it allows no 30 fps mode itself: its most, +0x4e3, is 0).
#define FW_MIN_EXTRA_VSYNCS 0x4e2  // (+0x4e3: the most, 1 - its 30 fps mode; +0x4e0 / +0x4e1: the current choice)
static bool hold_30(bool on) {
    if (!shared_title_las()) return false;
    static bool held;
    static uint8_t saved;
#ifdef PACE_TEST  // (tools/test_pace.c: a simulated game manager)
    extern uint64_t pace_test_game_mgr;
    uint64_t fw = pace_test_game_mgr;
#else
    uint64_t fw = *(volatile uint64_t *)(uintptr_t)GA(GAME_MGR);
#endif
    if (!fw) return held = false;  // (between boots / before the game set up)
    volatile uint8_t *min = (volatile uint8_t *)(uintptr_t)(fw + FW_MIN_EXTRA_VSYNCS), *max = min + 1;
    if (on && *max >= 1) {
        if (!held) saved = *min, held = true;
        *min = 1;
    } else if (held) {
        *min = saved, held = false;
    }
    return held;
}

// The held 30 fps mode's present interval (see above): called last in each frame's pace_frame, after its present. The
// game then waits for the next vsync (a frame that took 20 ms is shown 33 ms after the one before it: the wait is
// for the next vsync, not one already passed). Held, a frame that started after vsync S may not let the game start
// the next one before S + 2: if vsync S + 1 hasn't come yet, wait for it here. (The time counts as blocked: idle.)
extern volatile uint64_t g_vsync_count, g_vsync_ns;  // runtime/hle/vi.c
static void hold_cadence(bool held) {
    static uint64_t v_exit;  // the vsync count when the game last went on from here (its next frame: from the vsync after)
    // (the present already keeps the window's interval of 2 - nvnQueuePresentTexture, objects.c keep_interval -: a
    // second wait here, counted from a later vsync, would make some frames three vsyncs long)
    static int kept = -1;
    if (kept < 0) kept = !getenv("LARECOMP_PRESENT_INTERVAL") || atoi(getenv("LARECOMP_PRESENT_INTERVAL")) != 0;
    if (kept) return;
    if (held && v_exit) {
        uint64_t target = v_exit + 2, t0 = plat_now_ns(), n;
        // (bounded: a stalled vsync timer must not stall the game - two vsyncs and a bit at most)
        for (;;) {
            uint64_t vt = __atomic_load_n(&g_vsync_ns, __ATOMIC_ACQUIRE);  // (first: the count is then at least its vsync's,
            n = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);          // so a vsync in between only wakes us early)
            if (n >= target || plat_now_ns() - t0 > 40000000ull) break;
            uint64_t at = vt + (target - n) * 16666667ull + 250000ull;
            uint64_t now = plat_now_ns();
            plat_sleep_until_ns(at > now + 100000ull ? at : now + 100000ull);
        }
        g_blocked_ns += plat_now_ns() - t0;
    }
    v_exit = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);
}

static float median60(const float *v) {
    float s[60];
    memcpy(s, v, sizeof s);
    for (int i = 1; i < 60; i++)  // (insertion sort: 60 floats a frame)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) { float t = s[j]; s[j] = s[j - 1], s[j - 1] = t; }
    return (s[29] + s[30]) / 2;
}

// Any thread: a one-off stall (why: what, for the log) - the next PACE_GRACE_NS of frames don't count (see above).
static uint64_t hitch_at;
static const char *hitch_why;
void pace_hitch(const char *why) {
    __atomic_store_n(&hitch_why, why, __ATOMIC_RELAXED);
    __atomic_store_n(&hitch_at, plat_now_ns(), __ATOMIC_RELEASE);
}

bool pace_is_slow(void) { return pace_slow; }
void (*pace_busy_adjust)(double *busy, bool slow);  // (the backend: its render thread's and GPU's share, vulkan/exec.c)
uint64_t pace_frames_over(void) {
    uint64_t n = stat_pace_over;
    stat_pace_over = 0;
    return n;
}

// LARECOMP_SPIKE_MS=<ms> [LARECOMP_SPIKE_EVERY=<frames>, default 60]: the presenting (game) thread sleeps that long
// every that many frames from frame 2700 on (in play after a scripted load): a CPU-side hitch with the GPU idle
// (dynamic resolution tests). LARECOMP_SPIKE_FROM / LARECOMP_SPIKE_UNTIL=<frame>: other first / last frames (with
// EVERY=1: a machine too slow for 60 for a while - the 30 fps hold and the way back, frame pacing tests).
// LARECOMP_SPIKE_IDLE=1: the sleep counts as idle (blocked), as a wait for the GPU's frame through the window's image
// does - frames shown late with a short busy time (a GPU-bound machine).
static void spike_test(void) {
    static long ms = -1, every, frame, from, until, idle;
    if (ms < 0) {
        idle = getenv("LARECOMP_SPIKE_IDLE") && atoi(getenv("LARECOMP_SPIKE_IDLE"));
        ms = getenv("LARECOMP_SPIKE_MS") ? atol(getenv("LARECOMP_SPIKE_MS")) : 0;
        every = getenv("LARECOMP_SPIKE_EVERY") ? atol(getenv("LARECOMP_SPIKE_EVERY")) : 60;
        if (every < 1) every = 60;
        from = getenv("LARECOMP_SPIKE_FROM") ? atol(getenv("LARECOMP_SPIKE_FROM")) : 2700;
        until = getenv("LARECOMP_SPIKE_UNTIL") ? atol(getenv("LARECOMP_SPIKE_UNTIL")) : 0;
    }
    if (ms > 0 && ++frame > from && (!until || frame <= until) && frame % every == 0) {
        uint64_t t0 = plat_now_ns();
        plat_sleep_until_ns(t0 + (uint64_t)ms * 1000000);
        if (idle) g_blocked_ns += plat_now_ns() - t0;
    }
}

// The scene a frame shows, for the way back to 60 (see above): 0 when it isn't gameplay (no save in play, or no world on
// screen: the pause menu, title, file select), else the level and camera zone, hashed.
static uint64_t pace_scene(void) {
    extern int game_in_play(void);              // hle/warp.c
    extern bool world_on_screen(void);          // hle/mirror.c
    extern const char *game_level_name(void);   // hle/widescreen.c
    extern uint64_t game_camera_zone(void);     // hle/widescreen.c
    if (!game_in_play() || !world_on_screen()) return 0;
    uint64_t h = 1469598103934665603ull;  // (FNV-1a)
    for (const char *c = game_level_name(); *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    h = (h ^ game_camera_zone()) * 1099511628211ull;
    return h ? h : 1;
}

void pace_frame(void) {
    spike_test();
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_PACE"); on = !e || atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];  // (bit 0: over - busy or late; bit 1: over by busy time)
    static float recent[60];  // the last 60 frames' cost (ms at 60 fps)
    static int idx, nover, nbusy, calm, clear, backoff = 1;
    static bool late_hold;  // (the hold was started by frames shown late more than by busy ones)
    static uint64_t back_at;  // when frame pacing last went back to 60 (0: not yet)
    static double avg;  // cost (ms at 60 fps), moving average (~20 frames)
    static uint64_t last_tcpu, last_rcpu;
    extern uint64_t rq_render_cpu_ns(void);  // rqueue.c: the render thread's CPU time so far (0: no render thread)
    uint64_t now = plat_now_ns(), w = g_blocked_ns, tcpu = plat_thread_cpu_ns(), rcpu = rq_render_cpu_ns();
    slow_frame_present(now, tcpu);  // (slowframe.c: the "[slow]" line of a frame over LARECOMP_SLOW_FRAME_MS)
    uint64_t p = now - last, wt = w - last_wait, tc = tcpu - last_tcpu, rc = rcpu - last_rcpu;
    bool first = !last;
    last = now, last_wait = w, last_tcpu = tcpu, last_rcpu = rcpu;
    // GPU time per frame (pace_gpu_ns: the backend's GPU timer, cumulative; -1 without one), moving average (~20 frames).
    // (A frame's GPU time is read a few frames late and in bursts: the average smooths that.)
    extern int64_t pace_gpu_ns(void);
    static int64_t last_gpu = -1;
    static double gpu_avg = -1;  // ms (-1: no GPU timer)
    int64_t gpu = pace_gpu_ns();
    double gpu_ms = gpu >= 0 && last_gpu >= 0 && gpu >= last_gpu ? (gpu - last_gpu) / 1e6 : -1;
    last_gpu = gpu;
    static uint64_t scene;  // the scene clear / calm counted frames of (pace_scene; 0: none)
    static int gpu_said;    // (logged once per hold: the CPU had headroom but the GPU didn't)
    static int cpu_clear;   // gameplay frames in a row with clear headroom by CPU time alone
    if (first || p > 100000000ull) {  // (loading, a hitch: not a pace, and maybe a new scene)
        clear = calm = cpu_clear = 0, scene = 0;
        if (!first) {
            static char why[32];
            snprintf(why, sizeof why, "a %.0f ms frame", p / 1e6);
            __atomic_store_n(&hitch_why, (const char *)why, __ATOMIC_RELAXED);
            __atomic_store_n(&hitch_at, now, __ATOMIC_RELEASE);
        }
        hold_cadence(hold_30(pace_slow && on));
        return;
    }
    double busy = (p > wt ? p - wt : 0) / 1e6;
    if (pace_busy_adjust) pace_busy_adjust(&busy, pace_slow);
    // What the frame would cost at 60 fps: a frame of the game's 30 fps mode (two vsyncs or more) runs its world twice,
    // and the second step - about half this thread's CPU time then (measured: 3.0 -> 6.0 ms, 4.9 -> 9.3 ms) - is work
    // a 60 fps frame doesn't have. Without that, the 30 fps mode's frames looked 3-5 ms slower than they would be at 60
    // and the way back needed far more headroom than it should.
    double step2 = tc / 2e6 < busy / 2 ? tc / 2e6 : busy / 2;  // (at most half: a coarse thread clock can't take it all)
    double cost = p > 25000000ull ? busy - step2 : busy;
    // Held, the render thread has two vsyncs a frame and the game thread hardly waits for it: its CPU time for the frame
    // is what the frame would cost it at 60. (Not when free: there waiting for it is already in the busy time, and a
    // period can hold more or less than one frame of its work.)
    if (pace_slow && rc / 1e6 > cost && rc < 60000000ull) cost = rc / 1e6;
    avg = avg ? avg + (cost - avg) * 0.05 : cost;
    if (gpu_ms >= 0) gpu_avg = gpu_avg >= 0 ? gpu_avg + ((gpu_ms < 100 ? gpu_ms : 100) - gpu_avg) * 0.05 : gpu_ms;
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    extern __thread uint64_t g_rq_wait_ns;  // rqueue.c: waits for the render thread (translation), counted as busy
    static uint64_t last_rq;
    uint64_t rqw = g_rq_wait_ns - last_rq;
    last_rq = g_rq_wait_ns;
    if (log) {  // (plus this thread's CPU time per frame and the render thread's: work, independent of waits; and where
                // this thread's frame went in the renderer's frontend - rqueue.h)
        extern __thread uint64_t g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;
        static uint64_t last_cpu, l_lock, l_sub, l_gpu, l_pres, l_hooks;
        uint64_t cpu = plat_thread_cpu_ns();
        LOG("[pace] period %.2f blocked %.2f busy %.2f cost %.2f ms (queue wait %.2f, cpu %.2f, render cpu %.2f, gpu %.2f, gpu wait %.2f, "
            "submit %.2f, present %.2f, hooks %.2f, lock %.2f)%s\n", p / 1e6, wt / 1e6, busy, cost, rqw / 1e6,
            last_cpu ? (cpu - last_cpu) / 1e6 : 0.0, rc / 1e6, gpu_ms, (g_gpu_wait_ns - l_gpu) / 1e6, (g_submit_ns - l_sub) / 1e6,
            (g_present_ns - l_pres) / 1e6, (g_hooks_ns - l_hooks) / 1e6, (g_rq_lock_ns - l_lock) / 1e6,
            pace_scene() ? "" : " (no gameplay)");
        last_cpu = cpu;
        l_lock = g_rq_lock_ns, l_sub = g_submit_ns, l_gpu = g_gpu_wait_ns, l_pres = g_present_ns, l_hooks = g_hooks_ns;
    }
    // Not held, a frame shown a vsync late (25 ms or more after the one before: not a 50 Hz display's 20 ms) didn't fit
    // 60 either, whatever its busy time: what made it late can be idle here (waiting for the GPU's frame through the
    // window's image or the present: a machine GPU-bound at a high resolution), and the game still runs its world one
    // step for it half the time (its frame controller takes a late frame whose own CPU and GPU time were under its
    // limits as a hiccup, and goes back to 60) - slow motion at a steady 30 fps (an M3 Max at 5120x1440: 30.0 fps,
    // the pacer at 60, 4 of 60 frames over by busy time). It counts as over, at its period (gameplay frames only: a
    // load or a menu shown late isn't slow motion). Phones: in the game's 60 fps mode, past 20 ms already.
    extern uint32_t g_game_interval;  // (runtime/hle/framepace.c)
    bool late = !pace_slow && (p >= 25000000ull || (g_game_interval == 1 && p > 20000000ull)) && pace_scene();
    int b = cost > 16.2, o = b || late;
    stat_pace_over += o;
    // Within PACE_GRACE_NS of a hitch: left out (the window keeps the frames before it). Logged when the grace ends.
    static uint64_t grace_of;  // the hitch the left-out frames below are from
    static int ign_late, ign_busy, ign_n;
    static const char *ign_why;
    uint64_t ha = __atomic_load_n(&hitch_at, __ATOMIC_ACQUIRE);
    bool grace = ha && (int64_t)(now - ha) < (int64_t)PACE_GRACE_NS;
    if ((ign_late || ign_busy) && (!grace || ha != grace_of)) {
        LOG("[pace] left out %d of %d frames after %s: %d late, %d busy\n", ign_late + ign_busy, ign_n, ign_why,
            ign_late, ign_busy);
        ign_late = ign_busy = 0;
    }
    if (grace) {
        if (ha != grace_of) grace_of = ha, ign_n = 0, ign_why = __atomic_load_n(&hitch_why, __ATOMIC_RELAXED);
        ign_n++, ign_busy += b, ign_late += late && !b;
        if (!on) return;
        hold_cadence(hold_30(pace_slow));
        return;
    }
    nover += o - (over[idx] & 1);
    nbusy += b - (over[idx] >> 1);
    over[idx] = (uint8_t)(o | b << 1);
    recent[idx] = (float)(late && p / 1e6 > cost ? p / 1e6 : cost);
    idx = (idx + 1) % 60;
    if (!on || g_settings.frame_rate == FRAME_RATE_NATIVE_60) {  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
        extern volatile bool g_pace_slow;
        pace_slow = g_pace_slow = false;  // (60 asked for: the counters as they are, the game's tries at 60 let through)
        hold_30(false);
        return;
    }
    float med = median60(recent);
    if (!pace_slow && back_at && backoff > 1 && now - back_at > 60000000000ull) {
        backoff = 1;
        LOG("[pace] a minute at 60: headroom needed back to normal\n");
    }
    // Spread: frames over in each quarter of the window, oldest (idx) first - not one burst (see above).
    int quarters = 0;
    for (int q = 0; q < 4; q++) {
        int any = 0;
        for (int i = 0; i < 15 && !any; i++) any = over[(idx + q * 15 + i) % 60] & 1;
        quarters += any;
    }
    static bool bunch_said;
    if (nover < 12) bunch_said = false;
    if (!pace_slow && nover >= 12 && med >= 13.0f && quarters < 4 && !bunch_said) {
        bunch_said = true;
        LOG("[pace] %d of the last 60 frames over 16.2 ms busy or shown late (%d by busy time), but bunched in %d of 4 "
            "quarters: not holding (yet)\n", nover, nbusy, quarters);
    }
    if (!pace_slow && nover >= 12 && med >= 13.0f && quarters == 4) {
        pace_slow = true, calm = clear = cpu_clear = 0, gpu_said = 0;
        late_hold = nbusy < 12;
        if (back_at && now - back_at < 20000000000ull && backoff < 16) backoff *= 2;  // (flickering: wait longer)
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy or shown late, %d by busy time, median %.1f): "
            "holding the game's 30 fps mode%s\n", nover, nbusy, med, backoff > 1 ? " (backing off)" : "");
    } else if (pace_slow) {
        float peak = 0;
        for (int i = 0; i < 60; i++) peak = recent[i] > peak ? recent[i] : peak;
        uint64_t sc = pace_scene();
        if (sc != scene) clear = calm = cpu_clear = 0, scene = sc;  // (a new scene, the menu opening or closing: start over)
        // (no GPU timer: -1, CPU only. Held for frames shown late, the GPU must fit with more room: those frames' busy
        // time was short, so what made them late was the GPU's frame and the window's image - and their CPU headroom
        // here says nothing: PACE_GPU_LATE_MS, PACE_GPU_LATE_BACKOFF_MS once a return to 60 failed.)
        bool gpu_fits = gpu_avg < (!late_hold ? PACE_GPU_MS : backoff > 1 ? PACE_GPU_LATE_BACKOFF_MS : PACE_GPU_LATE_MS);
        int clear_n = 60 * (backoff < 4 ? backoff : 4);
        cpu_clear = sc && avg < 11.0 && peak < 14.5f ? cpu_clear + 1 : 0;
        if (cpu_clear == clear_n && !gpu_fits && !gpu_said++)
            LOG("[pace] %d gameplay frames with clear CPU headroom (%.1f ms a frame at 60 on average), but the GPU takes %.1f ms "
                "a frame: staying at 30\n", cpu_clear, avg, gpu_avg);
        clear = gpu_fits && cpu_clear ? clear + 1 : 0;
        calm = sc && gpu_fits && med < 13.5f && nover <= 4 ? calm + 1 : 0;
        if (clear >= clear_n || calm >= 150 * backoff) {
            char g[32];
            if (gpu_avg >= 0) snprintf(g, sizeof g, ", gpu %.1f ms", gpu_avg);
            else snprintf(g, sizeof g, ", gpu not timed");
            LOG("[pace] %d frames with %s headroom on gameplay frames (%.1f ms a frame at 60 on average, median %.1f, at most "
                "%.1f%s): back to the game's own choice\n", clear >= clear_n ? clear : calm, clear >= clear_n ? "clear" : "enough",
                avg, med, peak, g);
            pace_slow = false, nover = nbusy = 0, back_at = now;
            memset(over, 0, sizeof over);
            memset(recent, 0, sizeof recent);
        }
    }
    hold_cadence(hold_30(pace_slow));
    extern volatile bool g_pace_slow;  // (runtime/hle/framepace.c: the game's tries at 60 declined meanwhile)
    g_pace_slow = pace_slow;
}

uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + (uint64_t)((unsigned __int128)PACE_SLOW_NS * 384 / 625);
    return value < min ? min : value;
}
