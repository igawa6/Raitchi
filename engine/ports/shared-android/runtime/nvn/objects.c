/* Shared NVN objects derived from LAS Recomp Android
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Present timing derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * Compact sampler builder and nonzero Z-cull metadata derived from EoWRecomp
 * dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505, Copyright (c) 2026 Peiikai
 * and EoWRecomp contributors. Host descriptor/queued lifetime logic retained.
 * MIT notices: native/runtime/LICENSE-LASRecomp, LICENSE-LetsGo and
 * native/adapters/echoes/LICENSE. */
// NVN objects, builders, and state objects (everything except command recording).
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel.h"
#include "adapters/title_adapter.h"
#include "nvn.h"
#include "command_recording.h"
#include "../platform.h"
#include "rqueue.h"
#include "command_pool_stats.h"

void renderer_sync(void);

static uint64_t next_debug_id = 1;

// ================================================================== device
// NVNdeviceInfo -> value
static uint32_t device_integer(uint32_t which) {
    /* ABI negotiation is adapter metadata, not a claim of complete NVN API
     * conformance. Unknown procedure invocations retain diagnostic traps. */
    const TitleAdapter *adapter=title_adapter_selected();
    if(adapter&&adapter->nvn_api_major) {
        if(which==0)return adapter->nvn_api_major;
        if(which==1)return adapter->nvn_api_minor;
    }
    static const uint32_t v[] = {
        [0] = 53, [1] = 207,           // API version
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 5, [48] = 1, [49] = 15,
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 0x1000,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
    };
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) {
        once = true;
        rq_hang_watch_start();  // (before anything of the renderer can hang: rqueue.c)
        backend_init();
        extern void shader_prep_boot(const char *romfs);
        shader_prep_boot(g_romfs_dir);  // (LARECOMP_SHADER_PREP=1, tests: what the start screen's setup does)
    }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) { *(int32_t *)AP(2) = (int32_t)device_integer((uint32_t)A(1)); }
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) {  // counter.timestamp: Switch GPU ticks (614.4 MHz, as the counters are written)
    RET((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384));
}
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { nvn_command_pool_note_handle_finalize(A(1)); }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
NVN(nvnQueueSubmitCommands) {
    if(nvn_command_lifetime_enabled()){nvn_command_submit(c);return;}
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    bool rq = rq_on();
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < n; i++) {
        if (rq) {
            bool deferred = nvn_command_has_deferred(handles[i]);
            uint64_t item = rq_submit(handles[i]);
            // Deferred arrays are filled after recording and consumed during
            // submission. Finish their CPU replay before the caller can reuse
            // them; GPU execution remains asynchronous. A submit-time snapshot
            // can recover this CPU overlap later without changing that lifetime.
            if (deferred) rq_wait_item(item);
        } else backend_execute(handles[i]);
    }
    g_submit_ns += plat_now_ns() - t0;
}
__thread uint64_t g_submit_ns, g_gpu_wait_ns;
NVN(nvnQueueFlush) { (void)c; }
NVN(nvnQueueFinish) {
    (void)c;
    if (rq_on()) rq_drain();
    uint64_t t0 = plat_now_ns();
    rq_watch("QueueFinish: the GPU", 0);
    renderer_sync();
    rq_watch(NULL, 0);
    g_gpu_wait_ns += plat_now_ns() - t0;
}

static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
// A fence's seq: the render thread's item number when it's on (rqueue.c), else the backend's submission number.
static void sync_publish(NvnSync *s, uint64_t seq) {
    // Reusing a sync in a later queued list must not be undone by an older
    // fence's delayed publication on another producer thread.
    uint64_t old = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    while (old < seq && !__atomic_compare_exchange_n(&s->seq, &old, seq, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    __atomic_store_n(&s->signaled, 1, __ATOMIC_RELEASE);
}
NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    sync_publish(s, rq_on() ? rq_fence_seq() : renderer_fence_seq());
}
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) {
    // Prescan already published the item's queue number. A translated older
    // list must never replace a later reuse with a backend GPU-sequence value.
    if (rq_current_item()) return;
    sync_publish(sync_of(guest), seq);
}
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) { rq_free(HOST(NvnSync, A(0))); SET_HOST(A(0), 0); }  // (freed after the queued lists that may refer to it)
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = getenv("LARECOMP_SYNC_FLUSH") ? 1 : 0;
    if (getenv("LARECOMP_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // LARECOMP_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = getenv("LARECOMP_SYNC_ALL") ? 1 : 0;
    // Strict completion is the default: GPU writes must reach guest memory
    // before success, not a frame later. LARECOMP_SYNC_LAG=1 is retained only
    // as an explicitly unsafe research override for historical measurements.
    static int lag = -1;
    if (lag < 0) {
        const char *e = getenv("LARECOMP_SYNC_LAG");
        lag = e && atoi(e) != 0;
        if (lag) LOG("[nvn] unsafe research override: SYNC_LAG=1 may return before current GPU writes complete\n");
    }
    uint64_t t0 = 0;
    if (flush) { if (rq_on()) rq_flush(); else { t0 = plat_now_ns(); renderer_flush(); } }
    else if (whole) { if (rq_on()) rq_drain(); t0 = plat_now_ns(); rq_watch("SyncWait (all): the GPU", 0); renderer_sync(); }
    else {  // only the submission that holds this fence
        uint64_t seq = __atomic_load_n(&sync_of(A(0))->seq, __ATOMIC_ACQUIRE), q = seq;
        static int stats = -1;  // (LARECOMP_LOG_PACE: which fences the game waits on, and how long)
        if (stats < 0) stats = getenv("LARECOMP_LOG_PACE") != NULL;
        uint64_t behind = 0, w0 = 0;
        bool cur = false, untranslated = false;
        if (seq && rq_on()) {
            if (stats) { cur = rq_fence_info(seq, &behind, &untranslated); w0 = plat_now_ns(); }
            seq = rq_wait_item(seq);  // translated and submitted; then its GPU work
            if (g_rt_unwind) { RET(3); return; } // stopped translation is not successful completion
        }
        if (lag && seq) {
            static uint64_t prev;
            extern void renderer_submit_upto(uint64_t seq);
            renderer_submit_upto(seq);  // (copies of what it reads taken: the game may rewrite it)
            uint64_t w = prev && prev < seq ? prev : 0;
            if (seq > prev) prev = seq;
            seq = w;
        }
        t0 = plat_now_ns();
        rq_watch("SyncWait: the GPU, submission", seq);
        renderer_wait_seq(seq);
        static long tr_at = -1;  // (debug, rqueue.c LARECOMP_RQ_TRACE) the game thread's fence waits in those frames
        if (tr_at < 0) tr_at = getenv("LARECOMP_RQ_TRACE") ? atol(getenv("LARECOMP_RQ_TRACE")) : 0;
        extern uint64_t renderer_present_count(void);
        if (tr_at && renderer_present_count() >= (uint64_t)tr_at && renderer_present_count() < (uint64_t)tr_at + 4)
        {
            extern uint64_t rq_trace_t0;
            LOG("[rq-trace] game frame %llu: fence wait item %llu (gpu %llu) from %.3f, translation %.3f ms, GPU %.3f ms\n",
                (unsigned long long)renderer_present_count(), (unsigned long long)q, (unsigned long long)seq,
                ((w0 ? w0 : t0) - rq_trace_t0) / 1e6, w0 ? (t0 - w0) / 1e6 : 0.0, (plat_now_ns() - t0) / 1e6);
        }
        if (stats && q && rq_on()) {
            static struct { uint64_t n, untranslated, behind, rq_ns, gpu_ns, tr_ns, head_ns; } st[2];
            static uint64_t total;
            uint64_t t1 = plat_now_ns(), tr, head;
            extern void rq_fence_times(uint64_t wait_start, uint64_t *translate_ns, uint64_t *head_ns);
            rq_fence_times(w0, &tr, &head);
            st[cur].n++, st[cur].untranslated += untranslated, st[cur].behind += behind;
            st[cur].rq_ns += t0 - w0, st[cur].gpu_ns += t1 - t0, st[cur].tr_ns += tr, st[cur].head_ns += head;
            if (++total % 600 == 0) {
                for (int k = 1; k >= 0; k--)
                    if (st[k].n)
                        LOG("[rq] fence waits (%s frame): %llu, not translated yet %llu, avg %.1f items queued after it, "
                            "avg wait for translation %.3f ms, for the GPU %.3f ms; its list: queued to translated %.3f ms, "
                            "translated %.3f ms before the wait\n", k ? "this" : "an earlier",
                            (unsigned long long)st[k].n, (unsigned long long)st[k].untranslated, (double)st[k].behind / st[k].n,
                            st[k].rq_ns / 1e6 / st[k].n, st[k].gpu_ns / 1e6 / st[k].n, st[k].tr_ns / 1e6 / st[k].n,
                            st[k].head_ns / 1e6 / st[k].n);
                memset(st, 0, sizeof st);
            }
        }
    }
    rq_watch(NULL, 0);
    if (t0) g_gpu_wait_ns += plat_now_ns() - t0;
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(*(uint32_t *)(uintptr_t)event_addr(A(0))); }
NVN(nvnEventSignal) { *(uint32_t *)(uintptr_t)event_addr(A(0)) = (uint32_t)A(2); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    if (!p) { RET(0); return; }
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    if (!p->storage && !(p->mappings = nvn_pool_mappings_create())) { free(p); RET(0); return; }
    if (getenv("LARECOMP_LOG_HEAP"))
        fprintf(stderr, "[nvn] pool %llx size %llx flags %x\n", (unsigned long long)p->storage, (unsigned long long)p->size, p->flags);
    if(!nvn_command_pool_created(p)){nvn_pool_mappings_destroy(p->mappings);free(p);RET(0);return;}
    nvn_command_pool_note_init(p, p->storage, p->size);
    SET_HOST(A(0), p);
    RET(1);
}
static void pool_destroy(void *arg) {
    NvnPool *p = arg;
    if (p) { nvn_command_pool_note_destroy(p); nvn_command_pool_destroyed(p); nvn_pool_mappings_destroy(p->mappings); free(p); }
}
NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
    if (p) nvn_command_pool_note_finalize(p);
    SET_HOST(A(0), 0);
    // Mapping metadata stays alive until earlier queued texture reads finish.
    if (p) rq_call(pool_destroy, p);
}
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetBufferAddress) { RET(HOST(NvnPool, A(0))->storage); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
bool nvn_pool_resolve_span(const NvnPool *p, uint64_t offset, uint64_t size,
    uint64_t *address, uint64_t *generation) {
    if (address) *address = 0;
    if (generation) *generation = 0;
    if (!p || !address || !size || offset > p->size || size > p->size-offset) return false;
    if (!p->storage) return nvn_pool_mappings_resolve(p->mappings, p->size, offset, size, address, generation);
    if (offset > UINT64_MAX-p->storage || size > UINT64_MAX-(p->storage+offset)) return false;
    *address = p->storage+offset;
    return true;
}
NVN(nvnMemoryPoolMapVirtual) {
    NvnPool *p = HOST(NvnPool, A(0));
    int32_t count = (int32_t)(uint32_t)A(1);
    // NVNmappingRequest is 0x28 bytes: physical pool, signed physical/virtual
    // offsets, size, storage class. Class does not change CPU address mapping.
    if (!p || p->storage || !p->mappings || count < 0 ||
        (uint32_t)count > NVN_POOL_MAPPING_REQUEST_LIMIT ||
        (count && (!A(2) || A(2) > UINTPTR_MAX-(uint64_t)count*40))) { RET(0); return; }
    NvnPoolMappingRequest *requests = count ? calloc((size_t)count, sizeof *requests) : NULL;
    if (count && !requests) { RET(0); return; }
    bool ok = true;
    for (int32_t i = 0; i < count; ++i) {
        uint64_t raw[5]; memcpy(raw, (const uint8_t *)AP(2)+(size_t)i*40, sizeof raw);
        requests[i].offset = raw[2]; requests[i].size = raw[3];
        if ((int64_t)raw[1] < 0 || (int64_t)raw[2] < 0) { ok = false; break; }
        if (raw[0]) {
            NvnPool *physical = HOST(NvnPool, raw[0]);
            if (!physical || !physical->storage || !nvn_pool_resolve_span(physical, raw[1], raw[3],
                &requests[i].address, NULL)) { ok = false; break; }
        }
    }
    if (ok) ok = nvn_pool_mappings_apply(p->mappings, p->size, requests, (size_t)count);
    free(requests); RET(ok);
}

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
// Z-cull storage: games allocate it (Echoes of Wisdom: its depth buffer's, in a 256-byte-granule allocator that
// breaks on 0) and pass it to Save/RestoreZCullData, which are no-ops here - the host GPU culls by itself. Its size
// only has to be a sane non-zero amount for the dimensions: 8 bytes per 16x16 pixel tile, 4 KB aligned.
static uint64_t zcull_size(uint64_t w, uint64_t h) {
    if (!w) w = 1;
    if (!h) h = 1;
    return (((w + 15) / 16) * ((h + 15) / 16) * 8 + 0xFFF) & ~0xFFFull;
}
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(zcull_size(TB(uint32_t, 0x10), TB(uint32_t, 0x14))); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures (BNTX) were swizzled offline with their own block height, which can differ
    // from the driver default: NVNpackagedTextureLayout's low word is log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
    if (pl && getenv("LARECOMP_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %016llx fmt %#x %ux%u lv %u -> default bh %u\n", pl, G(uint32_t, b, 0x20), t->width, t->height, t->levels, t->block_height_log2);
    }
}

// Keep this historical alignment-query policy separate from row-layout/size:
// LINEAR_RENDER_TARGET's required allocation alignment is not established here.
static bool is_linear(uint64_t b) { return (G(uint32_t, b, 8) & 0x10) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10) {  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    }
    TexLayout t = {0};
    builder_layout(b, &t);
    if (nvn_texture_row_linear(G(uint32_t, b, 8))) {
        uint64_t size;
        return nvn_texture_row_span(&t, G(uint64_t, b, 0x30), &size) ? size : 0;
    }
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? 32 : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    // Virtual offsets are GPU addresses, never CPU pointers. Their current
    // physical backing is resolved by CPU readers/writers at use time.
    t->addr = t->pool && t->pool->storage && t->pool_offset <= UINT64_MAX-t->pool->storage
        ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = next_debug_id++;
    return t;
}

NVN(nvnTextureInitialize) {
    NvnTexture *t = texture_from_builder(A(1));
    uint64_t row_size;
    if (t->target != 10 && nvn_texture_row_linear(t->flags) && !nvn_texture_row_span(&t->layout, t->stride, &row_size)) {
        free(t); SET_HOST(A(0), NULL); RET(0); return;
    }
    if (t->flags & 2)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", t, t->width, t->height,
            t->format, t->flags, t->target, t->addr, t->stride);
    SET_HOST(A(0), t);
    RET(1);
}
static void pool_entries_forget(NvnTexture *t);
// The end of a finalized texture: its texture pool slots emptied (pool_entries_forget), the backend's copy, the host
// object. With the render thread, it runs there in queue order (rq_destroy_texture): lists queued before the
// finalize still sample it through its slots, as they did when they were translated at submit; lists queued after
// it see the slots empty, as without the render thread. (The NvnTexture stays allocated until then, so no other
// texture can take its address while a slot still names it.)
void nvn_texture_destroy(NvnTexture *t) {
    pool_entries_forget(t);
    backend_texture_destroyed(t);
    free(t);
}
NVN(nvnTextureFinalize) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t) {
        if (rq_on()) rq_destroy_texture(t);  // after the queued lists that refer to it
        else nvn_texture_destroy(t);
    }
    SET_HOST(A(0), 0);
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TEX->swizzle[i]; }
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(0); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) {
    NvnTexture *t = TEX;
    if (t->target == 10) { RET((uint64_t)t->width * t->layout.fmt->bpb); return; }
    uint64_t size;
    RET(nvn_texture_row_linear(t->flags) ? (nvn_texture_row_span(&t->layout, t->stride, &size) ? size : 0) : tegra_total_size(&t->layout));
}
NVN(nvnTextureGetDebugID) { RET(TEX->debug_id); }
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(zcull_size(TEX->width, TEX->height)); }
NVN(nvnTextureCompare) { RET(A(0) == A(1)); }
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views: our opaque 16-byte value, shared by every intercepted API.
// Check before narrowing; invalid calls must not mutate an adjacent guest object.
static uint32_t texture_view_value(uint64_t value, uint32_t maximum, const char *field) {
    if (value > maximum) FATAL("[nvn] TextureView %s out of range: %llu (max %u)\n",
                              field, (unsigned long long)value, maximum);
    return (uint32_t)value;
}
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, sizeof(NvnTextureView)); }
// Zero counts retain the established "all remaining" convention. Unknown
// negative/oversized values fail explicitly instead of wrapping the old u16s.
NVN(nvnTextureViewSetLevels) {
    uint32_t base = texture_view_value(A(1), UINT16_MAX, "base level");
    uint32_t count = texture_view_value(A(2), UINT16_MAX, "level count");
    NvnTextureView *v = AP(0); v->setmask |= 1; v->base_level = base; v->num_levels = count;
}
NVN(nvnTextureViewSetLayers) {
    uint32_t base = texture_view_value(A(1), UINT16_MAX, "base layer");
    uint32_t count = texture_view_value(A(2), UINT16_MAX, "layer count");
    NvnTextureView *v = AP(0); v->setmask |= 2; v->min_layer = base; v->num_layers = count;
}
NVN(nvnTextureViewSetFormat) {
    // Keep the full 8-bit NVN format namespace; backend support is checked when
    // used, just as for texture formats. Do not alias a larger invalid enum.
    uint32_t format = texture_view_value(A(1), UINT8_MAX, "format");
    NvnTextureView *v = AP(0); v->setmask |= 4; v->format = format;
}
NVN(nvnTextureViewSetSwizzle) {
    uint8_t swizzle[4];
    for (unsigned i = 0; i < 4; ++i) swizzle[i] = texture_view_value(A(1 + i), 5, "swizzle");
    NvnTextureView *v = AP(0); v->setmask |= 8; memcpy(v->swizzle, swizzle, sizeof swizzle);
}
NVN(nvnTextureViewSetDepthStencilMode) {
    uint32_t mode = texture_view_value(A(1), 1, "depth/stencil mode");
    NvnTextureView *v = AP(0); v->setmask |= 16; v->ds_mode = mode;
}
NVN(nvnTextureViewSetTarget) {
    uint32_t target = texture_view_value(A(1), 10, "target");
    NvnTextureView *v = AP(0); v->setmask |= 32; v->target = target;
}
NVN(nvnTextureViewGetLevels) {
    const NvnTextureView *v = AP(0); *(int32_t *)AP(1) = v->base_level;
    *(int32_t *)AP(2) = v->num_levels; RET(!!(v->setmask & 1));
}
NVN(nvnTextureViewGetLayers) {
    const NvnTextureView *v = AP(0); *(int32_t *)AP(1) = v->min_layer;
    *(int32_t *)AP(2) = v->num_layers; RET(!!(v->setmask & 2));
}
NVN(nvnTextureViewGetFormat) {
    const NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->format; RET(!!(v->setmask & 4));
}
NVN(nvnTextureViewGetSwizzle) {
    const NvnTextureView *v = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = v->swizzle[i];
    RET(!!(v->setmask & 8));
}
NVN(nvnTextureViewGetDepthStencilMode) {
    const NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->ds_mode; RET(!!(v->setmask & 16));
}
NVN(nvnTextureViewGetTarget) {
    const NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->target; RET(!!(v->setmask & 32));
}
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), sizeof(NvnTextureView)) == 0); }

// NVNcopyRegion ABI: six signed 32-bit texel coordinates/extents.
typedef struct { int32_t x, y, z, width, height, depth; } NvnCopyRegion;
_Static_assert(sizeof(NvnCopyRegion) == 24, "NVNcopyRegion ABI");

static enum TexWriteResult texture_write_texels(NvnTexture *t, const NvnTextureView *view,
    const NvnCopyRegion *input, const uint8_t *data, int64_t row_stride, int64_t image_stride) {
    if (!t || !input || !data || !t->pool || !t->layout.fmt || row_stride < 0 || image_stride < 0)
        return TEX_WRITE_ARGUMENT;
    if (__atomic_load_n(&t->gpu_write_owned, __ATOMIC_ACQUIRE)) return TEX_WRITE_GPU_OWNED;
    // Packed D24 / depth-stencil CPU transfers need conversion that the
    // existing Vulkan upload path does not provide; never acknowledge stale data.
    if (!nvn_format_known(t->format) || t->format == 0x33 || t->format == 0x35 || t->format == 0x36 || t->samples > 1 || t->target == 5 || t->target == 6 ||
        t->target > 10 || (view && (view->setmask & 4) && view->format != t->format) ||
        (view && (view->setmask & 32) && view->target != t->target)) return TEX_WRITE_FORMAT;
    if (input->x < 0 || input->y < 0 || input->z < 0 ||
        input->width <= 0 || input->height <= 0 || input->depth <= 0) return TEX_WRITE_BOUNDS;
    uint32_t level = view && (view->setmask & 1) ? view->base_level : 0;
    uint32_t base_layer = view && (view->setmask & 2) ? view->min_layer : 0;
    uint32_t layer_count = view && (view->setmask & 2) ? view->num_layers : 0;
    if (level >= t->layout.levels || (view && (view->setmask & 1) && view->num_levels &&
        view->num_levels > t->layout.levels - level)) return TEX_WRITE_BOUNDS;
    TexWriteRegion region = {(uint32_t)input->x, (uint32_t)input->y, (uint32_t)input->z,
                             (uint32_t)input->width, (uint32_t)input->height, (uint32_t)input->depth};
    if (t->target == 3) { // NVN 1D arrays carry layer coordinates in y/height.
        if (region.z || region.depth != 1) return TEX_WRITE_BOUNDS;
        region.z = region.y; region.depth = region.height; region.y = 0; region.height = 1;
        // Source layers are rows for a 1D array; image stride applies to z,
        // which is always one here. Normalize that row stride into our slices.
        image_stride = row_stride;
    }
    if (t->target == 2) { // A 3D copy's z is a slice; array-layer views are unsupported.
        if (base_layer || layer_count) return TEX_WRITE_FORMAT;
    } else {
        uint32_t layers = t->layout.layers ? t->layout.layers : 1;
        if (base_layer >= layers || (layer_count && layer_count > layers - base_layer)) return TEX_WRITE_BOUNDS;
        uint32_t available = layer_count ? layer_count : layers - base_layer;
        if (region.z >= available || region.depth > available - region.z) return TEX_WRITE_BOUNDS;
        region.z += base_layer;
    }
    if (!t->layout.fmt->bh) return TEX_WRITE_FORMAT;
    bool linear = nvn_texture_row_linear(t->flags) || t->target == 10;
    uint64_t pitch = t->target == 10 ? (uint64_t)t->layout.width * t->layout.fmt->bpb : t->stride;
    uint64_t rows = ((uint64_t)t->layout.height + t->layout.fmt->bh - 1) / t->layout.fmt->bh;
    if (linear && rows && pitch > UINT64_MAX / rows) return TEX_WRITE_STORAGE;
    uint64_t storage = linear ? pitch * rows : tegra_total_size(&t->layout), address;
    if (t->target != 10 && linear && !nvn_texture_row_span(&t->layout, pitch, &storage)) return TEX_WRITE_STORAGE;
    if (!nvn_pool_resolve_span(t->pool, t->pool_offset, storage, &address, NULL)) return TEX_WRITE_STORAGE;
    // CPU stores use the same page-write tracking fault path as the existing
    // whole-level upload; do not dirty the rest of a potentially shared pool.
    enum TexWriteResult result = tegra_write_region(&t->layout, level, linear, pitch,
        (uint8_t *)(uintptr_t)address, storage, &region, data, (uint64_t)row_stride, (uint64_t)image_stride);
    if (result == TEX_WRITE_OK) {
        __atomic_add_fetch(&t->cpu_write_generation, 1, __ATOMIC_RELEASE);
        backend_pool_written(address, storage);
    }
    return result;
}
static void texture_write_call(Cpu *c, bool strided) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    enum TexWriteResult result = texture_write_texels(t, AP(1), AP(2), AP(3),
        strided ? (int64_t)A(4) : 0, strided ? (int64_t)A(5) : 0);
    if (result != TEX_WRITE_OK) {
        static const char *const reasons[] = {"ok", "invalid argument", "region/view bounds",
            "unsupported format/view/target", "invalid source/destination stride", "storage outside texture pool",
            "CPU write to GPU-owned contents requires unsupported region coherence"};
        FATAL("[nvn] WriteTexels rejected: %s; target %u format %#x\n", reasons[result],
              t ? t->target : 0, t ? t->format : 0);
    }
    if (getenv("LARECOMP_LOG_TEX_WRITE")) {
        const NvnCopyRegion *r = AP(2);
        LOG("[nvn] WriteTexels: target %u format %#x texture %ux%ux%u region %d,%d,%d %dx%dx%d%s\n",
            t->target, t->format, t->width, t->height, t->depth,
            r->x, r->y, r->z, r->width, r->height, r->depth, strided ? " strided" : "");
    }
}
NVN(nvnTextureWriteTexels) { texture_write_call(c, false); }
NVN(nvnTextureWriteTexelsStrided) { texture_write_call(c, true); }
NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\n"); (void)c; }
NVN(nvnTextureReadTexelsStrided) { (void)c; }

// ================================================================== samplers
// SamplerBuilder (guest memory): only 0x40 bytes are guaranteed - Echoes of Wisdom keeps them on its stack, right
// under the saved frame pointer and return address (an NvnSamplerDesc stored here overwrote them). So the builder
// holds a packed copy (48 bytes), turned into an NvnSamplerDesc by sampler_desc().
typedef struct {
    uint64_t device;
    uint8_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
} SamplerBuilder;
_Static_assert(sizeof(SamplerBuilder) <= 0x40, "NVNsamplerBuilder: 0x40 bytes in guest memory");
#define SB ((SamplerBuilder *)AP(0))
static NvnSamplerDesc sampler_desc(const SamplerBuilder *b) {
    NvnSamplerDesc d = {0};
    d.min_filter = b->min_filter, d.mag_filter = b->mag_filter;
    for (int i = 0; i < 3; i++) d.wrap[i] = b->wrap[i];
    d.compare_mode = b->compare_mode, d.compare_func = b->compare_func, d.reduction = b->reduction;
    d.lod_min = b->lod_min, d.lod_max = b->lod_max, d.lod_bias = b->lod_bias, d.max_aniso = b->max_aniso;
    memcpy(d.border, b->border, sizeof d.border);
    return d;
}
NVN(nvnSamplerBuilderSetDevice) { SB->device = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    SamplerBuilder *b = SB;
    uint64_t dev = b->device;
    memset(b, 0, sizeof *b);
    b->device = dev;
    b->wrap[0] = b->wrap[1] = b->wrap[2] = 1;  // REPEAT
    b->lod_max = 1000.0f, b->max_aniso = 1.0f;
    b->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SB->min_filter = (uint8_t)A(1); SB->mag_filter = (uint8_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SB->wrap[0] = (uint8_t)A(1); SB->wrap[1] = (uint8_t)A(2); SB->wrap[2] = (uint8_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { SB->lod_min = AF(0); SB->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SB->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SB->compare_mode = (uint8_t)A(1); SB->compare_func = (uint8_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SB->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SB->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SB->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SB->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SB->reduction = (uint8_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(SB->device); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SB->min_filter; *(uint32_t *)AP(2) = SB->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SB->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SB->lod_min; *(float *)AP(2) = SB->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SB->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SB->compare_mode; *(uint32_t *)AP(2) = SB->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SB->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SB->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SB->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SB->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SB->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

NVN(nvnSamplerInitialize) {
    NvnSamplerDesc *d = malloc(sizeof *d);
    *d = sampler_desc((const SamplerBuilder *)AP(1));
    SET_HOST(A(0), d);
    RET(1);
}
NVN(nvnSamplerFinalize) { rq_free(HOST(NvnSamplerDesc, A(0))); SET_HOST(A(0), 0); }  // (freed after the queued lists that may refer to it)
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) { RET(memcmp(HOST(NvnSamplerDesc, A(0)), HOST(NvnSamplerDesc, A(1)), sizeof(NvnSamplerDesc)) == 0); }
NVN(nvnSamplerGetDebugID) { RET(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];
// Texture pool registrations (the game's threads) and the emptying of a finalized texture's slots (the render thread
// when it's on: nvn_texture_destroy) both change entries and pool_refs: this lock keeps them from crossing. (Reads by
// the translation take no lock: the game doesn't change a slot that queued work still uses before its fence, as on
// the Switch, and the emptying runs on the translating thread itself.)
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
RT_ON_RESET(desc_pools_reset) { memset(g_texture_pools, 0, sizeof g_texture_pools); memset(g_sampler_pools, 0, sizeof g_sampler_pools); }

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg) {
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = count > 0 ? calloc((size_t)count, entry) : NULL;
    if (count > 0 && !d->entries) FATAL("[nvn] descriptor pool allocation failed (count %d)\n", count);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    pthread_mutex_lock(&g_pool_lock);  // (pool_entries_forget walks the registry)
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    pthread_mutex_unlock(&g_pool_lock);
    return d;
}

NVN(nvnTexturePoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools);
    RET(1);
}
NVN(nvnTexturePoolFinalize) { (void)c; }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    pthread_mutex_lock(&g_pool_lock);
    if (e->tex && e->tex->pool_refs) e->tex->pool_refs--;
    e->tex = HOST(NvnTexture, A(2));
    if (e->tex) e->tex->pool_refs++;
    e->dead_format = 0;
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
    if (e->has_view) nvn_texture_note_view(e->tex, &e->view);
    pthread_mutex_unlock(&g_pool_lock);
}

// A finalized texture can stay registered: the game frees a room's textures on a level / room change and leaves
// their pool slots as they are until it registers something else there, and a draw can still name such a slot.
// Entries held its NvnTexture pointer, so the next texture allocated at the same address (another format) got the
// old entry's view: a BC4 / BC5 / BC1 texture viewed as RGBA8 or the reverse, a Metal assertion that stopped the
// game (about 1 in 6 long dungeon runs). Those entries are emptied (sampled as nothing, like an unregistered slot).
static void pool_entries_forget(NvnTexture *t) {
    pthread_mutex_lock(&g_pool_lock);
    if (!t->pool_refs) { pthread_mutex_unlock(&g_pool_lock); return; }
    int n = 0;
    for (int p = 0; p < 64 && t->pool_refs; p++) {
        NvnDescPool *d = g_texture_pools[p];
        if (!d) continue;
        NvnTexEntry *ents = (NvnTexEntry *)d->entries;
        for (int i = 0; i < d->count && t->pool_refs; i++)
            if (ents[i].tex == t) {
                ents[i].tex = NULL;
                ents[i].has_view = false;
                ents[i].dead_format = t->format + 1;
                t->pool_refs--;
                n++;
            }
    }
    pthread_mutex_unlock(&g_pool_lock);
    static int nlog;
    if (n && nlog++ < 8)
        LOG("[nvn] texture %ux%u fmt %#x finalized while in %d texture pool slot(s): emptied%s\n", t->width, t->height,
            t->format, n, nlog == 8 ? " (further ones not logged)" : "");
}
void nvn_dead_slot_sampled(const NvnTexEntry *e, uint32_t slot) {
    static int nlog;
    if (nlog < 8) {
        nlog++;
        LOG("[nvn] draw samples texture pool slot %u, whose texture (fmt %#x) was finalized: nothing bound%s\n", slot,
            e->dead_format - 1, nlog == 8 ? " (further ones not logged)" : "");
    }
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = *HOST(NvnSamplerDesc, A(2));
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = sampler_desc((const SamplerBuilder *)AP(2));
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { rq_free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); }  // (freed after the queued lists that may refer to it)
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
// Opt-in metadata only. A declared section length never authorizes a memory
// read: copy each requested range through the kernel's mapping/permission proof.
static void log_compute_control(const NvnShaderStage *stage) {
    const char *enabled = getenv("LARECOMP_LOG_SKIPS");
    const char *memory_trace = getenv("LARECOMP_TRACE_COMMAND_MEMORY");
    if ((!enabled || !atoi(enabled)) && (!memory_trace || strcmp(memory_trace, "1"))) return;
    static unsigned reports;
    unsigned index = __atomic_load_n(&reports, __ATOMIC_RELAXED);
    while (index < 16 && !__atomic_compare_exchange_n(&reports, &index, index + 1, false,
                                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (index >= 16) return;
    uint64_t address = (uintptr_t)stage->control;
    uint32_t header[9];
    if (!kernel_copy_readable(header, address, sizeof header)) {
        LOG("[nvn] compute-control #%u control=%llx const_offset=%u const_size=%u metadata=unreadable\n",
            index + 1, (unsigned long long)address, stage->const_offset, stage->const_size);
        return;
    }
    uint32_t magic = header[0], length = header[8], compute[4];
    bool readable = magic == 0x98761234u && length >= 0x758u &&
        address <= UINTPTR_MAX - 0x748u && kernel_copy_readable(compute, address + 0x748u, sizeof compute);
    if (readable) {
        LOG("[nvn] compute-control #%u control=%llx magic=%08x length=%u local=%u,%u,%u shared_bytes=%u const_offset=%u const_size=%u\n",
            index + 1, (unsigned long long)address, magic, length, compute[0], compute[1], compute[2], compute[3],
            stage->const_offset, stage->const_size);
    } else {
        LOG("[nvn] compute-control #%u control=%llx const_offset=%u const_size=%u metadata=unavailable\n",
            index + 1, (unsigned long long)address, stage->const_offset, stage->const_size);
    }
}
static void set_shaders(Cpu *c, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(uint64_t *)(data + i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (uint64_t *)(data + i * stride + 8);
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = *(uint32_t *)(uintptr_t)(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (*(uint32_t *)(uintptr_t)code != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = *(const uint32_t *)(control + 0x700);
        p->stages[stage].const_size = *(const uint32_t *)(control + 0x6FC);
        if (stage == 5) log_compute_control(&p->stages[stage]);
        p->stage_mask |= 1u << stage;
    }
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
NVN(nvnWindowSetPresentInterval) {  // (the game's 60 / 30 fps mode: 1 / 2; LARECOMP_LOG_INTERVAL=1 logs each change)
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_LOG_INTERVAL") != NULL;
    if (log && WIN->interval != (int)A(1)) LOG("[nvn] present interval %d -> %d\n", WIN->interval, (int)A(1));
    WIN->interval = (int)A(1);
    // the game's mode for frame generation (vulkan/framegen.c: in-between frames in its 30) and the fps line. Set here
    // as well as in runtime/hle/framepace.c rt_frame_steps: that read sits in the part of the game's frame controller
    // its own dynamic resolution runs, and with that off (tools/patches.py GAME_DYNRES_SITE) it never ran - the mode
    // stayed 60 and frame generation never started (1.1.0, issue #7)
    extern uint32_t g_game_interval;
    if (A(1) >= 1 && A(1) <= 4) g_game_interval = (uint32_t)A(1);
}
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
// The window's swap interval, as NVN keeps it: a frame goes out no sooner than `interval` vsyncs after the one before
// (on the Switch the next AcquireTexture waits for that). The game itself only waits for the next vsync, so without
// this a frame of its 30 fps mode (two world steps) that took one vsync went out at once - double speed. A VI timer
// existing does not prove the game waits on its event. Honor interval 1 too;
// an already elapsed guest vsync satisfies the interval without another wait.
// LARECOMP_PRESENT_INTERVAL=0: not kept.
extern __thread uint64_t g_blocked_ns;  // kernel.c (idle for the frame pacer)
static void keep_interval(int interval) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_PRESENT_INTERVAL") || atoi(getenv("LARECOMP_PRESENT_INTERVAL")) != 0;
    extern volatile uint64_t g_vsync_count;
    extern void vi_wait_vsync(uint64_t count);
    static uint64_t last, next;
    uint64_t current = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);
    if (on && !current) {
        // Some games pace solely through NVN and never create a VI event.
        // Honor their window interval without depending on that optional timer.
        if (interval <= 0) { next = 0; return; }
        uint64_t period = (uint64_t)interval * 16666667ull;
        uint64_t now = plat_now_ns();
        if (!next || now > next + 2 * period) next = now;
        else {
            plat_sleep_until_ns(next);
            g_blocked_ns += plat_now_ns() - now;
        }
        next += period;
        return;
    }
    if (on && last && interval > 0 && current < last + (uint64_t)interval) {
        uint64_t t0 = plat_now_ns();
        vi_wait_vsync(last + (uint64_t)interval);
        g_blocked_ns += plat_now_ns() - t0;
    }
    last = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);
}
NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    keep_interval(w->interval);
    extern void widescreen_present_hook(void);  // (re-runs the game's render-resolution setup on this thread when needed)
    widescreen_present_hook();
    if (i >= 0 && i < w->num) backend_present(HOST(NvnTexture, w->textures[i]));
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
