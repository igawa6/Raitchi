/* Experimental command storage bridge. All allocation is bounded and all
 * body ownership is explicit; no time/LRU collection or Finalize invalidation.
 * Recording the same CB concurrently remains invalid client usage. */
#include "command_recording.h"
#include "../kernel.h"
#include "../platform.h"
#include "rqueue.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
_Static_assert(sizeof(CmdRec) == 80 && offsetof(CmdRec, op) == 0 && offsetof(CmdRec, size) == 4 &&
                   offsetof(CmdRec, x) == 8 && offsetof(CmdRec, f) == 64 && offsetof(CmdRec, extra) == 80,
               "command core ABI");
#define JUMP_RESERVE (sizeof(CmdRec) + 8)
#define HOST_FRAMING (2 * sizeof(CmdRec) + JUMP_RESERVE)
static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t bytes) {
    CmdRec *r = (void *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = bytes;
    return r;
}
typedef struct {
    uint64_t x[8];
    uint32_t f[4];
} CommandSnap;
static CommandSnap command_snap(Cpu *c) {
    CommandSnap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (unsigned i = 0; i < 4; ++i)
        memcpy(s.f + i, c->v + i, 4);
    return s;
}
#define SCRATCH_BLOCK 32768u
#define SCRATCH_LIMIT (32u * 1024u * 1024u)
typedef struct Scratch {
    struct Scratch *next;
    size_t capacity, used;
    _Alignas(16) unsigned char data[];
} Scratch;
typedef struct {
    NvnPool *pool;
    uint64_t generation, logical, backing, backing_base;
} Proof;
typedef struct {
    Scratch *head, *current, *tail;
    size_t capacity;
    Proof proof;
    uint64_t chunk_offset, root_domain, root_offset;
    bool guest;
    NvnClSpan *spans;
    size_t nspans, span_capacity;
} Recording;
static NvnClStore *nvn_command_store;
static bool nvn_command_mode, lifetime_log;
static void report_lifetime(void);
static _Thread_local NvnClLease *nvn_command_reader;

static _Atomic size_t scratch_capacity;
/* Process selection and store construction are serialized once. */
static pthread_once_t mode_once = PTHREAD_ONCE_INIT;
static void *allocate_body(size_t n, void *context) {
    (void)context;
    return malloc(n);
}
static void release_body(void *p, void *context) {
    (void)context;
    free(p);
}
static void select_mode(void) {
    const char *e = getenv("LARECOMP_COMMAND_LIFETIME");
    if (!e || strcmp(e, "1"))
        return;
#if (!defined(__ANDROID__) || !defined(VK_USE_PLATFORM_ANDROID_KHR)) && !defined(NVN_COMMAND_LIFETIME_HOST_TEST)
    FATAL("[nvn] command lifetime requires Android Vulkan reader routing\n");
#endif
    NvnClConfig cfg = {
        .max_slots = 262144,
        .deterministic_handles = true,
        .max_bodies = 65536,
        .max_spans_per_body = 4096,
        .max_call_edges = 1048576,
        .max_lease_slots = 65536,
        .max_body_bytes = 16 * 1024 * 1024,
        .record_budget = 10000000,
        .schema = {CMD_COUNT,
                   CMD_BARRIER,
                   CMD_END,
                   CMD_JUMP,
                   CMD_CALL,
                   CMD_FENCE_SYNC,
                   {CMD_BIND_TEXTURES_DEFERRED, CMD_BIND_UNIFORM_BUFFERS_DEFERRED, CMD_BIND_STORAGE_BUFFERS_DEFERRED}},
        .arena = {UINT64_C(1024) * 1024 * 1024, 4096, {allocate_body, release_body, NULL}}};
    NvnClResult result = nvn_cl_create(cfg, &nvn_command_store);
    if (result != NVN_CL_OK)
        FATAL("[nvn] command lifetime store initialization failed %u\n", result);
    const char *log = getenv("LARECOMP_LOG_COMMAND_LIFETIME");
    lifetime_log = log && !strcmp(log, "1");
    nvn_command_mode = true;
    LOG("[nvn] experimental command lifetime enabled (pool-local alias proof; slot cap262144, body cap65536, arena "
        "cap1GiB; encoded handles)\n");
}
bool nvn_command_lifetime_enabled(void) {
    pthread_once(&mode_once, select_mode);
    return nvn_command_mode;
}
/* Hash-indexed active-pool metadata. UIDs never repeat, even when malloc or
 * guest storage addresses repeat. Closing a pool does not retire its bodies. */
typedef struct PoolEntry {
    NvnPool *pool;
    uint64_t uid;
    struct PoolEntry *next;
} PoolEntry;
#define POOL_BUCKETS 4096u
#define POOL_LIMIT 65536u
static PoolEntry *pool_registry[POOL_BUCKETS];
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pool_uid;
static size_t pool_count;
static size_t pool_bucket(NvnPool *p) {
    uint64_t x = (uintptr_t)p >> 4;
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    return (size_t)(x ^ (x >> 27)) & (POOL_BUCKETS - 1);
}
bool nvn_command_pool_created(NvnPool *p) {
    if (!nvn_command_lifetime_enabled())
        return true;
    if (!p)
        return false;
    PoolEntry *fresh = malloc(sizeof *fresh);
    if (!fresh)
        return false;
    pthread_mutex_lock(&pool_lock);
    size_t b = pool_bucket(p);
    for (PoolEntry *q = pool_registry[b]; q; q = q->next)
        if (q->pool == p) {
            pthread_mutex_unlock(&pool_lock);
            free(fresh);
            return false;
        }
    if (pool_count == POOL_LIMIT || pool_uid == NVN_CL_ENCODED_DOMAIN_MAX) {
        pthread_mutex_unlock(&pool_lock);
        free(fresh);
        return false;
    }
    *fresh = (PoolEntry){p, ++pool_uid, pool_registry[b]};
    pool_registry[b] = fresh;
    ++pool_count;
    pthread_mutex_unlock(&pool_lock);
    return true;
}
void nvn_command_pool_destroyed(NvnPool *p) {
    if (!nvn_command_lifetime_enabled() || !p)
        return;
    pthread_mutex_lock(&pool_lock);
    PoolEntry **at = &pool_registry[pool_bucket(p)];
    while (*at && (*at)->pool != p)
        at = &(*at)->next;
    PoolEntry *old = *at;
    if (old) {
        *at = old->next;
        --pool_count;
    }
    pthread_mutex_unlock(&pool_lock);
    free(old);
}
static Proof proof_of(NvnPool *p) {
    Proof proof = {0};
    pthread_mutex_lock(&pool_lock);
    for (PoolEntry *q = pool_registry[pool_bucket(p)]; q; q = q->next)
        if (q->pool == p) {
            proof = (Proof){p, q->uid, q->uid, q->uid, 0};
            break;
        }
    pthread_mutex_unlock(&pool_lock);
    if (!proof.generation)
        FATAL("[nvn] unregistered command pool\n");
    return proof;
}
static Recording *recording(NvnCmdBuf *cb) {
    Recording *r = cb->experimental;
    if (!r) {
        r = calloc(1, sizeof *r);
        if (!r)
            FATAL("scratch metadata\n");
        cb->experimental = r;
    }
    return r;
}
void nvn_command_record_add(NvnCmdBuf *cb, NvnPool *pool, uint64_t offset, uint64_t nominal, uint64_t effective) {
    Recording *r = recording(cb);
    r->proof = proof_of(pool);
    r->chunk_offset = offset;
    r->guest = true;
    cb->mem = pool->storage + offset;
    cb->size = nominal;
    cb->guest_capacity = effective;
    cb->guest_used = 0;
    if (cb->recording && !r->nspans) {
        /* Empty Begin has no logical storage span. Rebinding before the first
         * payload selects the new pool/root; a later chunk's reset counter is
         * insufficient because earlier spans remain in nspans. */
        r->root_domain=r->proof.logical;
        r->root_offset=offset;
    }
    /* Deliberately leave scratch host/used/start alone across a guest callback. */
}
static Scratch *scratch_next(Recording *r, size_t need) {
    Scratch *s = r->current ? r->current->next : r->head;
    while (s && s->capacity < need)
        s = s->next;
    if (!s) {
        size_t capacity = need > SCRATCH_BLOCK ? need : SCRATCH_BLOCK;
        if (capacity > SCRATCH_LIMIT - r->capacity || capacity > SIZE_MAX - sizeof *s)
            FATAL("scratch resource limit\n");
        s = malloc(sizeof *s + capacity);
        if (!s)
            FATAL("scratch allocation\n");
        s->next = NULL;
        s->capacity = capacity;
        s->used = 0;
        if (r->tail)
            r->tail->next = s;
        else
            r->head = s;
        r->tail = s;
        r->capacity += capacity;
        atomic_fetch_add(&scratch_capacity, capacity);
    }
    return s;
}
static bool merge_span(Recording *r, uint64_t begin) {
    return r->nspans && r->spans[r->nspans - 1].domain == r->proof.backing &&
           r->spans[r->nspans - 1].pool_generation == r->proof.generation && r->spans[r->nspans - 1].end == begin;
}
static void prepare_span(Recording *r, uint64_t begin, uint64_t bytes) {
    if (!bytes || merge_span(r, begin))
        return;
    if (r->nspans == r->span_capacity) {
        size_t n = r->span_capacity ? r->span_capacity * 2 : 8;
        if (n > 4096)
            FATAL("span resource limit\n");
        NvnClSpan *p = malloc(n * sizeof *p);
        if (!p)
            FATAL("span allocation\n");
        if (r->nspans)
            memcpy(p, r->spans, r->nspans * sizeof *p);
        free(r->spans);
        r->spans = p;
        r->span_capacity = n;
    }
}
static void append_span(Recording *r, uint64_t begin, uint64_t bytes) {
    if (!bytes)
        return;
    NvnClSpan span = {r->proof.backing, begin, begin + bytes, r->proof.generation};
    if (merge_span(r, begin)) {
        r->spans[r->nspans - 1].end = span.end;
        return;
    }
    if (r->nspans >= r->span_capacity)
        FATAL("command span capacity\n");
    r->spans[r->nspans++] = span;
}
CmdRec *nvn_command_record_budget(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra, bool begin) {
    Recording *r = recording(cb);
    uint64_t wide = (uint64_t)sizeof(CmdRec) + extra + 7;
    if (wide > UINT32_MAX)
        FATAL("record overflow\n");
    uint32_t size = (uint32_t)(wide & ~UINT64_C(7));
    uint64_t cost = begin           ? 0
                    : op == CMD_END ? 4
                                    : (((uint64_t)size + 31) / 32) * 4,
             required = cost + (op == CMD_END ? 0 : 4);
    for (unsigned tries = 0;
         !r->guest || cb->guest_used > cb->guest_capacity || required > cb->guest_capacity - cb->guest_used; ++tries) {
        if (!cb->callback || tries == 2)
            FATAL("guest command budget\n");
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;
        c->x[2] = required;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    if (begin) {
        for (Scratch *s = r->head; s; s = s->next)
            s->used = 0;
        r->current = NULL;
        r->nspans = 0;
        r->root_domain = r->proof.logical;
        r->root_offset = r->chunk_offset + cb->guest_used;
    }
    uint64_t canonical = r->proof.backing_base + r->chunk_offset + cb->guest_used;
    if (canonical > UINT64_MAX - cost)
        FATAL("canonical span overflow\n");
    prepare_span(r, canonical, cost); // callback context proof selected before scratch movement
    Scratch *old = r->current;
    if (!old || old->used > old->capacity || size + JUMP_RESERVE > old->capacity - old->used) {
        Scratch *next = scratch_next(r, (size_t)size + HOST_FRAMING);
        next->used = 0;
        if (old && cb->recording) {
            if (old->used > old->capacity || sizeof(CmdRec) > old->capacity - old->used)
                FATAL("scratch chain bounds\n");
            raw_rec((uintptr_t)old->data + old->used, CMD_JUMP, sizeof(CmdRec))->x[0] = (uintptr_t)next->data;
            old->used += sizeof(CmdRec);
        }
        r->current = next;
    }
    Scratch *s = r->current;
    append_span(r, canonical, cost);
    CmdRec *record = raw_rec((uintptr_t)s->data + s->used, op, size);
    s->used += size;
    cb->host = (uintptr_t)s->data;
    cb->hcap = s->capacity;
    cb->used = s->used;
    cb->guest_used += cost;
    return record;
}
static Scratch *scratch_contains(Recording *r, uint64_t at) {
    for (Scratch *s = r->head; s; s = s->next) {
        uint64_t base = (uintptr_t)s->data;
        if (at >= base && at - base <= s->used && sizeof(CmdRec) <= s->used - (at - base))
            return s;
    }
    return NULL;
}
static size_t flatten(Recording *r, uint64_t start, void *out) {
    uint64_t at = start;
    size_t bytes = 0;
    Scratch *s = NULL;
    for (unsigned guard = 0; guard < 10000000; ++guard) {
        if (!s || at < (uintptr_t)s->data || at - (uintptr_t)s->data > s->used ||
            sizeof(CmdRec) > s->used - (at - (uintptr_t)s->data))
            s = scratch_contains(r, at);
        if (!s)
            FATAL("scratch header bounds\n");
        CmdRec *h = (void *)(uintptr_t)at;
        uint64_t offset = at - (uintptr_t)s->data;
        if (h->op >= CMD_COUNT || h->size < 80 || (h->size & 7) || h->size > s->used - offset)
            FATAL("scratch record bounds\n");
        if (h->op == CMD_JUMP) {
            at = h->x[0];
            s = NULL;
            continue;
        }
        if (h->size > 16 * 1024 * 1024 - bytes)
            FATAL("sealed body limit\n");
        if (out)
            memcpy((char *)out + bytes, h, h->size);
        bytes += h->size;
        if (h->op == CMD_END)
            return bytes;
        at += h->size;
    }
    FATAL("scratch record guard\n");
}
uint64_t nvn_command_record_end(NvnCmdBuf *cb) {
    Recording *r = recording(cb);
    size_t bytes = flatten(r, cb->start, NULL);
    void *data = malloc(bytes);
    if (!data)
        FATAL("seal allocation\n");
    if (flatten(r, cb->start, data) != bytes)
        FATAL("command seal size changed\n");
    NvnClInput input = {r->root_domain, r->root_offset, data, bytes, r->spans, r->nspans};
    NvnClHandle handle = 0;
    NvnClResult result = nvn_cl_publish(nvn_command_store, &input, &handle);
    free(data);
    if (result != NVN_CL_OK)
        FATAL("seal rejected %u\n", result);
    report_lifetime();
    return handle;
}
void nvn_command_record_finalize(NvnCmdBuf *cb) {
    Recording *r = cb ? cb->experimental : NULL;
    if (!r)
        return;
    while (r->head) {
        Scratch *s = r->head;
        r->head = s->next;
        atomic_fetch_sub(&scratch_capacity, s->capacity);
        free(s);
    }
    free(r->spans);
    free(r);
    cb->experimental = NULL;
}
static NvnClHandle *checked_handles(Cpu *c, size_t *out) {
    int32_t count = (int32_t)c->x[1];
    if (count < 0)
        FATAL("negative handle count\n");
    uint64_t bytes = (uint64_t)(uint32_t)count * 8;
    if (bytes > 16 * 1024 * 1024 || bytes > UINT32_MAX - 80 - 7)
        FATAL("handle count resource/record overflow\n");
    *out = (size_t)count;
    if (!count)
        return NULL;
    NvnClHandle *handles = malloc((size_t)bytes);
    if (!handles)
        FATAL("handle array allocation\n");
    if (!kernel_copy_readable(handles, c->x[2], bytes)) {
        free(handles);
        FATAL("unreadable handle array\n");
    }
    return handles;
}
void nvn_command_record_call(Cpu *c) {
    CommandSnap saved = command_snap(c);
    size_t count;
    NvnClHandle *handles = checked_handles(c, &count);
    NvnCmdBuf *cb = HOST(NvnCmdBuf, saved.x[0]);
    cb->fences = cb->deferred = true;
    CmdRec *r = nvn_command_record_budget(c, cb, CMD_CALL, (uint32_t)(count * 8), false);
    memcpy(r->x, saved.x + 1, sizeof r->x);
    memcpy(r->f, saved.f, sizeof r->f);
    r->x[0] = count;
    if (count)
        memcpy(r->extra, handles, count * 8);
    free(handles);
}
void nvn_command_record_copy(Cpu *c) {
    size_t count;
    NvnClHandle *handles = checked_handles(c, &count);
    NvnClSnapshot **snapshots = count ? calloc(count, sizeof *snapshots) : NULL;
    if (count && !snapshots) {
        free(handles);
        FATAL("copy snapshots allocation\n");
    }
    for (size_t i = 0; i < count; ++i) {
        NvnClResult result = nvn_cl_snapshot(nvn_command_store, handles[i], &snapshots[i]);
        if (result != NVN_CL_OK) {
            for (size_t j = 0; j < i; ++j)
                nvn_cl_snapshot_release(snapshots[j]);
            free(snapshots);
            free(handles);
            FATAL("copy source rejected\n");
        }
    }
    NvnCmdBuf *cb = HOST(NvnCmdBuf, c->x[0]);
    for (size_t i = 0; i < count; ++i) {
        NvnClBodyView v = nvn_cl_snapshot_view(snapshots[i]);
        size_t at = 0;
        while (at < v.bytes) {
            const CmdRec *source = (const void *)((const char *)v.records + at);
            if (at && source->op != CMD_END) {
                CmdRec *target = nvn_command_record_budget(c, cb, source->op, source->size - sizeof(CmdRec), false);
                memcpy(target, source, source->size);
                if (source->op == CMD_CALL) {
                    cb->fences = cb->deferred = true;
                }
                if (source->op == CMD_FENCE_SYNC)
                    cb->fences = true;
                if (source->op == CMD_BIND_TEXTURES_DEFERRED || source->op == CMD_BIND_UNIFORM_BUFFERS_DEFERRED ||
                    source->op == CMD_BIND_STORAGE_BUFFERS_DEFERRED)
                    cb->deferred = true;
            }
            at += source->size;
        }
        nvn_cl_snapshot_release(snapshots[i]);
    }
    free(snapshots);
    free(handles);
}
NvnClLease *nvn_command_reader_enter(NvnClLease *l) {
    NvnClLease *previous = nvn_command_reader;
    nvn_command_reader = l;
    return previous;
}
void nvn_command_reader_leave(NvnClLease *previous) { nvn_command_reader = previous; }
uint64_t nvn_command_resolve(uint64_t handle) {
    if (!nvn_command_lifetime_enabled())
        return handle;
    NvnClBodyView v;
    if (!nvn_command_reader || !nvn_cl_lease_body(nvn_command_reader, handle, &v))
        FATAL("read outside coherent lease\n");
    return (uintptr_t)v.records;
}
NvnClLeaseFlags nvn_command_flags(uint64_t handle) {
    NvnClLeaseFlags flags = {0};
    if (!nvn_command_reader || !nvn_cl_lease_handle_flags(nvn_command_reader, handle, &flags))
        FATAL("flags outside coherent lease\n");
    return flags;
}

NvnClLease *nvn_command_reader_current(void) { return nvn_command_reader; }
void nvn_command_submit(Cpu *c) {
    uint64_t started = plat_now_ns();
    size_t count = 0;
    NvnClHandle *handles = checked_handles(c, &count);
    if (!count) {
        free(handles);
        return;
    }
    NvnClLease *lease = NULL;
    NvnClResult result = nvn_cl_acquire(nvn_command_store, handles, count, &lease);
    if (result != NVN_CL_OK) {
        free(handles);
        FATAL("[nvn] command closure rejected %u\n", result);
    }
    NvnClLease *previous = nvn_command_reader_enter(lease);
    bool queued = rq_on();
    for (size_t i = 0; i < count; ++i) {
        NvnClLeaseFlags flags = nvn_command_flags(handles[i]);
        if (queued) {
            uint64_t item = rq_submit_leased(handles[i], lease);
            if (!item)
                break;
            if (flags.deferred)
                rq_wait_item(item);
        } else
            backend_execute(handles[i]);
    }
    nvn_command_reader_leave(previous);
    if (nvn_cl_lease_release_completed(lease, true) != NVN_CL_OK)
        FATAL("[nvn] submit lease release failed\n");
    free(handles);
    g_submit_ns += plat_now_ns() - started;
    report_lifetime();
}
void nvn_command_lifetime_finish_after_consumers(void) {
    if (!nvn_command_lifetime_enabled() || !nvn_command_store)
        return;
    if (!rq_command_readers_stopped()) {
        LOG("[nvn] retaining command bodies: render stop unacknowledged\n");
        return;
    }
    NvnClResult result = nvn_cl_destroy(nvn_command_store);
    if (result != NVN_CL_OK) {
        LOG("[nvn] retaining command bodies: readers still active (%u)\n", result);
        return;
    }
    nvn_command_store = NULL;
    pthread_mutex_lock(&pool_lock);
    for (size_t b = 0; b < POOL_BUCKETS; ++b)
        while (pool_registry[b]) {
            PoolEntry *q = pool_registry[b];
            pool_registry[b] = q->next;
            free(q);
        }
    pool_count = 0;
    pthread_mutex_unlock(&pool_lock);
}

/* Diagnostics are opt-in, metadata only, at most one line per two seconds.
 * The snapshot is observational: body and pool counters use separate locks. */
bool nvn_command_lifetime_stats(NvnCommandLifetimeStats *out) {
    if (!out || !nvn_command_lifetime_enabled() || !nvn_command_store)
        return false;
    NvnCommandLifetimeStats value = {.bodies = nvn_cl_diagnostic_stats(nvn_command_store),
                                     .scratch_bytes = atomic_load(&scratch_capacity)};
    pthread_mutex_lock(&pool_lock);
    value.active_pools = pool_count;
    pthread_mutex_unlock(&pool_lock);
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long long total = 0, resident = 0;
    if (file) {
        if (fscanf(file, "%llu %llu", &total, &resident) == 2) {
            long page = sysconf(_SC_PAGESIZE);
            if (page > 0 && resident <= UINT64_MAX / (uint64_t)page)
                value.rss_bytes = resident * (uint64_t)page;
        }
        fclose(file);
    }
    *out = value;
    return true;
}
static void report_lifetime(void) {
    if (!lifetime_log)
        return;
    static _Atomic uint64_t last_report;
    uint64_t now = plat_now_ns(), old = atomic_load_explicit(&last_report, memory_order_relaxed);
    if (old && (now < old || now - old < UINT64_C(2000000000)))
        return;
    if (!atomic_compare_exchange_strong_explicit(&last_report, &old, now, memory_order_relaxed, memory_order_relaxed))
        return;
    NvnCommandLifetimeStats s;
    if (!nvn_command_lifetime_stats(&s))
        return;
    LOG("[command-life] slots %zu empty-slots %zu current %zu alive %zu leases %zu snapshots %zu current-bytes %llu "
        "alive-bytes %llu pinned-bytes %llu arena-reserved %llu arena-live %llu scratch %llu pools %zu rss %llu collected %llu peak-slots %zu "
        "empty-edge %zu empty-old %zu empty-both %zu empty-unreferenced %zu\n",
        s.bodies.slots, s.bodies.slots - s.bodies.current_bodies, s.bodies.current_bodies, s.bodies.alive_bodies,
        s.bodies.leases, s.bodies.snapshots, (unsigned long long)s.bodies.current_bytes,
        (unsigned long long)s.bodies.alive_bytes, (unsigned long long)s.bodies.pinned_bytes,
        (unsigned long long)s.bodies.arena.reserved_total, (unsigned long long)s.bodies.arena.live_rounded,
        (unsigned long long)s.scratch_bytes, s.active_pools, (unsigned long long)s.rss_bytes, (unsigned long long)s.bodies.collected_slots, s.bodies.peak_slots,
        s.bodies.empty_incoming_only, s.bodies.empty_old_body_only, s.bodies.empty_both, s.bodies.empty_unreferenced);
}
