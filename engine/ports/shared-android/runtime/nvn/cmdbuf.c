/* Shared NVN command recording derived from LAS Recomp Android
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Stage binding derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
/* The eightfold accounting estimate derives from EoWRecomp (MIT),
 * dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505, Copyright(c)2026 Peiikai
 * and EoWRecomp contributors. Shared recorders and fence metadata are retained.
 * Handles point into host shadows; guest chunk bytes remain untouched. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "command_recording.h"
#include "command_pool_stats.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

/* Optional memory-API history only: no per-command/draw hook or logging. This
 * records observations without changing the callback or memory contract. */
#define COMMAND_MEMORY_TRACE_EVENTS 64u
typedef enum { MT_INIT, MT_FINAL, MT_ADD, MT_ADDED, MT_BEGIN, MT_BEGUN, MT_END, MT_ENDED,
               MT_CALLBACK, MT_RETURN, MT_ENABLE, MT_SET_CALLBACK, MT_CONTROL,
               MT_QUERY_SIZE, MT_QUERY_USED, MT_QUERY_FREE } MemoryTraceKind;
typedef struct {
    uint64_t cb, caller, function, data, requested;
    uint32_t op, extra, bytes, cost, attempt, depth;
} MemoryTraceOrigin;
typedef struct MemoryTraceCallback {
    struct MemoryTraceCallback *previous;
    NvnCmdBuf *cb;
    MemoryTraceOrigin origin;
} MemoryTraceCallback;
typedef struct {
    NvnCmdBuf *cb;
    uint64_t instance, last, enabled, callback_thread, recording_thread, chunk[6];
    unsigned callback_depth;
    bool enabled_seen, chunk_seen;
} MemoryTraceState;
typedef struct {
    uint64_t sequence, instance, thread, caller, args[6], chunk[6], callback_thread, recording_thread, enabled;
    NvnCmdBuf state;
    NvnCmdBuf *identity;
    MemoryTraceOrigin origin;
    MemoryTraceKind kind;
    unsigned callback_depth;
    bool enabled_seen, chunk_seen, same_cb, foreign_callback;
} MemoryTraceEvent;
static bool memory_trace_enabled;
static pthread_once_t memory_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t memory_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static MemoryTraceEvent memory_trace_events[COMMAND_MEMORY_TRACE_EVENTS];
static MemoryTraceState memory_trace_states[COMMAND_MEMORY_TRACE_EVENTS];
static uint64_t memory_trace_sequence;
static _Thread_local MemoryTraceCallback *memory_trace_callback;
static void memory_trace_select(void) {
    const char *value = getenv("LARECOMP_TRACE_COMMAND_MEMORY");
    memory_trace_enabled = value && !strcmp(value, "1");
}
/* Called under the trace mutex. Eviction loses only diagnostic history and
 * marks the next observation as a new instance with unknown enable state. */
static MemoryTraceState *memory_trace_state(NvnCmdBuf *cb) {
    MemoryTraceState *oldest = NULL;
    for (size_t i = 0; i < COMMAND_MEMORY_TRACE_EVENTS; ++i) {
        MemoryTraceState *s = &memory_trace_states[i];
        if (s->cb == cb) return s;
        if (!s->cb) oldest = s;
        else if (!s->callback_depth && (!oldest || (oldest->cb && s->last < oldest->last))) oldest = s;
    }
    if (oldest) *oldest = (MemoryTraceState){.cb=cb,.instance=memory_trace_sequence+1};
    return oldest;
}
static void memory_trace(Cpu *c, NvnCmdBuf *cb, MemoryTraceKind kind, const uint64_t args[6]) {
    if (!memory_trace_enabled || !cb) return;
    uint64_t thread = (uintptr_t)pthread_self();
    pthread_mutex_lock(&memory_trace_lock);
    MemoryTraceState *s = memory_trace_state(cb);
    if (s && kind == MT_INIT) *s = (MemoryTraceState){.cb=cb,.instance=memory_trace_sequence+1};
    if (s && kind == MT_ENABLE) { s->enabled_seen=true; s->enabled=args[0]; }
    if (s && kind == MT_ADDED) { s->chunk_seen=true; memcpy(s->chunk,args,sizeof s->chunk); }
    if (s && kind == MT_CALLBACK) { if (!s->callback_depth) s->callback_thread=thread; ++s->callback_depth; }
    if (s && kind == MT_BEGUN) s->recording_thread=thread;
    MemoryTraceEvent *e = &memory_trace_events[memory_trace_sequence % COMMAND_MEMORY_TRACE_EVENTS];
    *e = (MemoryTraceEvent){.sequence=++memory_trace_sequence,.instance=s?s->instance:0,.thread=thread,
        .caller=c->x[30],.state=*cb,.identity=cb,.kind=kind,.callback_thread=s?s->callback_thread:0,
        .callback_depth=s?s->callback_depth:0,.recording_thread=s?s->recording_thread:0,
        .enabled_seen=s&&s->enabled_seen,.chunk_seen=s&&s->chunk_seen,.enabled=s?s->enabled:0,
        .foreign_callback=s&&s->callback_depth&&s->callback_thread!=thread};
    if (args) memcpy(e->args,args,sizeof e->args);
    if (s) memcpy(e->chunk,s->chunk,sizeof e->chunk);
    if (memory_trace_callback) { e->origin=memory_trace_callback->origin; e->same_cb=memory_trace_callback->cb==cb; }
    if (s) {
        s->last=e->sequence;
        if (kind == MT_RETURN && s->callback_depth && !--s->callback_depth) s->callback_thread=0;
        if (kind == MT_ENDED) s->recording_thread=0;
        if (kind == MT_FINAL) s->cb=NULL;
    }
    pthread_mutex_unlock(&memory_trace_lock);
}
static void memory_trace_dump(NvnCmdBuf *cb, const char *reason) {
    if (!memory_trace_enabled) return;
    static const char *const names[]={"init","final","add","added","begin","begun","end","ended",
        "callback","return","enable","set-callback","control","query-size","query-used","query-free"};
    MemoryTraceEvent history[COMMAND_MEMORY_TRACE_EVENTS];
    size_t count=0;
    pthread_mutex_lock(&memory_trace_lock);
    uint64_t instance=0;
    for(size_t i=0;i<COMMAND_MEMORY_TRACE_EVENTS;++i)
        if(memory_trace_states[i].cb==cb)instance=memory_trace_states[i].instance;
    uint64_t first=memory_trace_sequence>COMMAND_MEMORY_TRACE_EVENTS?memory_trace_sequence-COMMAND_MEMORY_TRACE_EVENTS:0;
    for(uint64_t i=first;i<memory_trace_sequence;++i) {
        const MemoryTraceEvent *e=&memory_trace_events[i%COMMAND_MEMORY_TRACE_EVENTS];
        if(e->identity==cb && (!instance || e->instance==instance))history[count++]=*e;
    }
    pthread_mutex_unlock(&memory_trace_lock);
    LOG("[nvn-cmdmem] self-chain reason=%s cb=%llx history=%zu/%u (bounded memory APIs; enable state unknown until setter)\n",
        reason,(unsigned long long)cb->guest,count,COMMAND_MEMORY_TRACE_EVENTS);
    for(size_t i=0;i<count;++i) {
        const MemoryTraceEvent *e=&history[i];const NvnCmdBuf *b=&e->state;
        LOG("[nvn-cmdmem] #%llu %s cb=%llx object=%llx instance=%llu thread=%llx lr=%llx recording=%u start=%llx "
            "guest=%llx nominal=%llu capacity=%llu used=%llu host=%llx hcap=%llu hused=%llu "
            "enabled_seen=%u enabled=%llu recording_thread=%llx active_thread=%llx active_depth=%u foreign=%u\n",
            (unsigned long long)e->sequence,names[e->kind],(unsigned long long)b->guest,(unsigned long long)(uintptr_t)e->identity,
            (unsigned long long)e->instance,(unsigned long long)e->thread,(unsigned long long)e->caller,b->recording,
            (unsigned long long)b->start,(unsigned long long)b->mem,(unsigned long long)b->size,
            (unsigned long long)b->guest_capacity,(unsigned long long)b->guest_used,(unsigned long long)b->host,
            (unsigned long long)b->hcap,(unsigned long long)b->used,e->enabled_seen,(unsigned long long)e->enabled,
            (unsigned long long)e->recording_thread,(unsigned long long)e->callback_thread,e->callback_depth,e->foreign_callback);
        if(e->kind==MT_ADD || e->kind==MT_ADDED || e->kind==MT_CONTROL || e->kind==MT_ENABLE || e->kind==MT_SET_CALLBACK || e->kind>=MT_QUERY_SIZE)
            LOG("[nvn-cmdmem] #%llu args=%llx,%llx,%llx,%llx,%llx,%llx "
                "(add: pool,storage,pool-size,offset,nominal,effective; query: result; others: raw arguments)\n",
                (unsigned long long)e->sequence,(unsigned long long)e->args[0],(unsigned long long)e->args[1],
                (unsigned long long)e->args[2],(unsigned long long)e->args[3],(unsigned long long)e->args[4],(unsigned long long)e->args[5]);
        if(e->origin.depth)
            LOG("[nvn-cmdmem] #%llu origin_cb=%llx same_cb=%u origin_lr=%llx callback=%llx data=%llx "
                "op=%u extra=%u bytes=%u cost=%u requested=%llu attempt=%u depth=%u event=0\n",
                (unsigned long long)e->sequence,(unsigned long long)e->origin.cb,e->same_cb,
                (unsigned long long)e->origin.caller,(unsigned long long)e->origin.function,(unsigned long long)e->origin.data,
                e->origin.op,e->origin.extra,e->origin.bytes,e->origin.cost,(unsigned long long)e->origin.requested,
                e->origin.attempt,e->origin.depth);
        if(e->kind==MT_ADD || e->kind==MT_CALLBACK)
            LOG("[nvn-cmdmem] #%llu current_chunk_known=%u pool=%llx storage=%llx pool_size=%llu offset=%llu nominal=%llu effective=%llu\n",
                (unsigned long long)e->sequence,e->chunk_seen,(unsigned long long)e->chunk[0],(unsigned long long)e->chunk[1],
                (unsigned long long)e->chunk[2],(unsigned long long)e->chunk[3],(unsigned long long)e->chunk[4],(unsigned long long)e->chunk[5]);
    }
}
static void memory_trace_query(Cpu *c, NvnCmdBuf *cb, MemoryTraceKind kind, uint64_t result) {
    if (memory_trace_enabled && memory_trace_callback && memory_trace_callback->cb==cb) {
        const uint64_t args[]={result,0,0,0,0,0};
        memory_trace(c,cb,kind,args);
    }
}
static unsigned memory_trace_accept_reports[2];
static void memory_trace_accepted(const NvnCmdBuf *cb, bool empty, bool prefix, bool lifetime,
                                   uint64_t old_floor, uint64_t old_used, uint64_t old_host,
                                   uint64_t pool, uint64_t offset) {
    if (!memory_trace_enabled || (!empty && !prefix)) return;
    unsigned *reports=&memory_trace_accept_reports[empty?0:1];
    unsigned index=__atomic_load_n(reports,__ATOMIC_RELAXED);
    while(index<8 && !__atomic_compare_exchange_n(reports,&index,index+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    if(index>=8)return;
    LOG("[nvn-cmdmem] accepted-%s cb=%llx mode=%s old_floor=%llx old_used=%llu "
        "new_guest=%llx new_end=%llx pool=%llx offset=%llu nominal=%llu effective=%llu "
        "old_host=%llx new_host=%llx root=%llx\n",empty?"empty":"prefix",
        (unsigned long long)cb->guest,lifetime?"on":"off",(unsigned long long)old_floor,
        (unsigned long long)old_used,(unsigned long long)cb->mem,(unsigned long long)(cb->mem+cb->guest_capacity),
        (unsigned long long)pool,(unsigned long long)offset,(unsigned long long)cb->size,
        (unsigned long long)cb->guest_capacity,(unsigned long long)old_host,(unsigned long long)cb->host,
        (unsigned long long)cb->start);
}

NVN(nvnCommandBufferInitialize) {
    pthread_once(&memory_trace_once,memory_trace_select);
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    memory_trace(c,cb,MT_INIT,NULL);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { NvnCmdBuf *cb=cb_of(A(0)); memory_trace(c,cb,MT_FINAL,NULL); nvn_command_record_finalize(cb); free(cb); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// Logical guest accounting is an HLE estimate, not exact original GPU packet
// encoding: real records cost ceil(host_size/8), rounded to a 4-byte word;
// synthetic Begin costs zero and End one word. Host framing never consumes guest
// bytes. Keep host capacity independent: retained spill blocks cover many tiny
// lists without an unconditional large multiplier for every supplied chunk.
#define CMD_SHADOW 8u
#define CMD_WORD 4u
#define END_BUDGET CMD_WORD
#define HOST_FRAMING (2 * sizeof(CmdRec) + JUMP_RESERVE)
#define SHADOW_BLOCK_LIMIT (UINT64_C(32) * 1024)
#define SHADOW_INITIAL_LIMIT (UINT64_C(4) * 1024)
#define SHADOW_SLAB_CAPACITY (UINT64_C(4) * 1024 * 1024)
typedef struct ShadowSlab {
    struct ShadowSlab *next;
    uint64_t capacity, used;
    _Alignas(16) unsigned char data[];
} ShadowSlab;
_Static_assert(offsetof(ShadowSlab, data)%16 == 0, "shadow slab alignment");
typedef struct { uint64_t guest, size, host; } Shadow;
typedef struct ShadowAllocation {
    void *memory;
    uint64_t capacity, owner_root;
    struct ShadowAllocation *next, *lease_previous, *lease_next;
    bool pinned;
} ShadowAllocation;
typedef struct {
    uint64_t host;
    ShadowAllocation *head, *tail, *cursor, *limit;
} ShadowRoot;
static Shadow *g_shadow;
static size_t g_nshadow, g_shadow_cap;
static ShadowAllocation *g_shadow_allocations;
static ShadowSlab *g_shadow_slabs;
// Sorted starts preserve per-block validation boundaries inside shared slabs.
// Allocation nodes and backing slabs both survive until GPU-idle cleanup.
static ShadowAllocation **g_shadow_index;
static size_t g_shadow_index_count, g_shadow_index_capacity;
static ShadowRoot *g_shadow_roots;
static size_t g_shadow_root_count, g_shadow_root_capacity;
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t bytes, peak, allocations, spills, largest, reused, pinned, slabs, slab_bytes; unsigned reports; } g_shadow_stats;
static const ShadowAllocation *shadow_allocation_locked(uint64_t address);
static bool shadow_contains_header(const ShadowAllocation *a, uint64_t address);
typedef struct {
    const char *reason;
    uint64_t requested, retained, peak, allocations, spills, largest, guests, table_capacity;
} ShadowFailure;

static void shadow_failure_locked(ShadowFailure *failure, const char *reason, uint64_t requested) {
    if (failure) *failure = (ShadowFailure){reason, requested, g_shadow_stats.bytes, g_shadow_stats.peak,
        g_shadow_stats.allocations, g_shadow_stats.spills, g_shadow_stats.largest, g_nshadow, g_shadow_cap};
}
static void shadow_growth_locked(void) {
    // At most12 lines per shadow lifetime:1,2,...2048MiB of retained data.
    if (g_shadow_stats.reports >= 12 ||
        g_shadow_stats.bytes < (UINT64_C(1) << (20+g_shadow_stats.reports))) return;
    do { ++g_shadow_stats.reports; } while (g_shadow_stats.reports < 12 &&
        g_shadow_stats.bytes >= (UINT64_C(1) << (20+g_shadow_stats.reports)));
    LOG("[nvn] command shadows: retained %llu peak %llu bytes, allocations %llu spills %llu largest %llu, "
        "guest starts %llu table entries %llu, reused %llu pinned %llu roots %llu, slabs %llu slab bytes %llu (metadata %llu bytes)\n",
        (unsigned long long)g_shadow_stats.bytes, (unsigned long long)g_shadow_stats.peak,
        (unsigned long long)g_shadow_stats.allocations, (unsigned long long)g_shadow_stats.spills,
        (unsigned long long)g_shadow_stats.largest, (unsigned long long)g_nshadow,
        (unsigned long long)g_shadow_cap,
        (unsigned long long)g_shadow_stats.reused, (unsigned long long)g_shadow_stats.pinned,
        (unsigned long long)g_shadow_root_count,
        (unsigned long long)g_shadow_stats.slabs, (unsigned long long)g_shadow_stats.slab_bytes,
        (unsigned long long)(g_shadow_stats.allocations*sizeof(ShadowAllocation)+g_shadow_cap*sizeof(Shadow)+
                             g_shadow_index_capacity*sizeof(*g_shadow_index)+g_shadow_root_capacity*sizeof(ShadowRoot)));
}

static size_t shadow_root_hash(uint64_t host, size_t capacity) {
    host ^= host >> 33; host *= UINT64_C(0xff51afd7ed558ccd); host ^= host >> 33;
    return (size_t)host & (capacity-1);
}
// Root identity is the exact host Begin address, not the guest chunk or current
// continuation. Table movement never moves command storage or allocation nodes.
static ShadowRoot *shadow_root_locked(uint64_t host, bool create, ShadowFailure *failure) {
    if (!host) return NULL;
    if (g_shadow_root_capacity) {
        size_t at = shadow_root_hash(host, g_shadow_root_capacity);
        while (g_shadow_roots[at].host) {
            if (g_shadow_roots[at].host == host) return &g_shadow_roots[at];
            at = (at+1)&(g_shadow_root_capacity-1);
        }
    }
    if (!create) return NULL;
    if (g_shadow_root_count >= g_shadow_root_capacity/2) {
        size_t capacity = g_shadow_root_capacity ? g_shadow_root_capacity*2 : 1024;
        if (capacity < g_shadow_root_capacity || capacity > SIZE_MAX/sizeof(ShadowRoot)) {
            shadow_failure_locked(failure, "root table size overflow", capacity); return NULL;
        }
        ShadowRoot *next = calloc(capacity, sizeof *next);
        if (!next) { shadow_failure_locked(failure, "root table allocation", capacity*sizeof *next); return NULL; }
        for (size_t i = 0; i < g_shadow_root_capacity; ++i) if (g_shadow_roots[i].host) {
            size_t at = shadow_root_hash(g_shadow_roots[i].host, capacity);
            while (next[at].host) at = (at+1)&(capacity-1);
            next[at] = g_shadow_roots[i];
        }
        free(g_shadow_roots); g_shadow_roots = next; g_shadow_root_capacity = capacity;
    }
    size_t at = shadow_root_hash(host, g_shadow_root_capacity);
    while (g_shadow_roots[at].host) at = (at+1)&(g_shadow_root_capacity-1);
    g_shadow_roots[at].host = host; ++g_shadow_root_count;
    return &g_shadow_roots[at];
}
static bool shadow_begin_root(uint64_t host, ShadowFailure *failure) {
    pthread_mutex_lock(&g_shadow_lock);
    ShadowAllocation *block = (ShadowAllocation *)shadow_allocation_locked(host);
    if ((host & 7) || !shadow_contains_header(block, host)) {
        shadow_failure_locked(failure, "invalid root address", host);
        pthread_mutex_unlock(&g_shadow_lock); return false;
    }
    ShadowRoot *root = shadow_root_locked(host, true, failure);
    if (!root) { pthread_mutex_unlock(&g_shadow_lock); return false; }
    // A separately begun list can live in the unused tail of an earlier list's
    // continuation. Permanently remove that entire block from its old owner's
    // reuse pool, so resetting the parent can never overwrite the child root.
    if (block && block->owner_root && !block->pinned) {
        ShadowRoot *owner = shadow_root_locked(block->owner_root, false, NULL);
        if (!owner) { pthread_mutex_unlock(&g_shadow_lock); return false; }
        if (owner->cursor == block) owner->cursor = block == owner->limit ? NULL : block->lease_next;
        if (owner->limit == block) owner->limit = block->lease_previous;
        if (block->lease_previous) block->lease_previous->lease_next = block->lease_next;
        else owner->head = block->lease_next;
        if (block->lease_next) block->lease_next->lease_previous = block->lease_previous;
        else owner->tail = block->lease_previous;
        block->lease_previous = block->lease_next = NULL;
        block->pinned = true; ++g_shadow_stats.pinned;
    }
    // Existing commands in this exact root are already overwritten by Begin.
    // The client must have finished submissions that refer to that root before
    // rerecording it. No reuse starts sooner than that existing contract.
    root->cursor = root->head; root->limit = root->tail;
    pthread_mutex_unlock(&g_shadow_lock);
    return true;
}

static bool shadow_capacity(uint64_t guest_size, uint64_t *capacity) {
    // Preserve overflow rejection for the complete guest reservation, but
    // commit a modest host block until actual recorded commands need more.
    // Retaining a large guest budget does not require reserving its8x ceiling.
    if (guest_size > (SIZE_MAX - HOST_FRAMING) / CMD_SHADOW) return false;
    uint64_t ceiling = guest_size * CMD_SHADOW + HOST_FRAMING;
    *capacity = ceiling < SHADOW_BLOCK_LIMIT ? ceiling : SHADOW_BLOCK_LIMIT;
    return true;
}
static bool shadow_initial_capacity(uint64_t guest_size, uint64_t *capacity) {
    // Most distinct supplied guest starts need only a short list. Preserve the
    // complete reservation/overflow checks but grow actual records through the
    // unchanged continuation path, instead of retaining an unwritten32KiB root.
    if (!shadow_capacity(guest_size, capacity)) return false;
    if (*capacity > SHADOW_INITIAL_LIMIT) *capacity = SHADOW_INITIAL_LIMIT;
    return true;
}
// Caller holds g_shadow_lock. All generations/continuations survive until GPU
// idle cleanup; never realloc/move storage under an already returned handle.
static uint64_t shadow_allocate_locked(uint64_t capacity, bool spill, ShadowFailure *failure) {
    if (capacity > UINT64_MAX-g_shadow_stats.bytes) {
        shadow_failure_locked(failure, "retained-size overflow", capacity); return 0;
    }
    if (!capacity || capacity > SIZE_MAX - offsetof(ShadowSlab, data)) {
        shadow_failure_locked(failure, "slab size overflow", capacity); return 0;
    }
    // Reserve privately until metadata/index allocation succeeds. Existing slab
    // cursors and published addresses never move, including on failure.
    ShadowSlab *slab = g_shadow_slabs, *fresh = NULL;
    uint64_t offset = slab ? slab->used : 0;
    uint64_t padding = (UINT64_C(0)-offset)&15;
    bool fits = slab && offset <= slab->capacity && padding <= slab->capacity-offset &&
                capacity <= slab->capacity-offset-padding;
    if (fits) offset += padding;
    else {
        uint64_t data_size = capacity > SHADOW_SLAB_CAPACITY ? capacity : SHADOW_SLAB_CAPACITY;
        if (data_size > SIZE_MAX - offsetof(ShadowSlab, data) ||
            data_size+offsetof(ShadowSlab, data) > UINT64_MAX-g_shadow_stats.slab_bytes) {
            shadow_failure_locked(failure, "slab size overflow", data_size); return 0;
        }
        size_t bytes = (size_t)data_size+offsetof(ShadowSlab, data);
        fresh = malloc(bytes);
        if (!fresh) { shadow_failure_locked(failure, "slab allocation", bytes); return 0; }
        fresh->next = g_shadow_slabs; fresh->capacity = data_size; fresh->used = 0;
        slab = fresh; offset = 0;
    }
    void *memory = slab->data+offset;
    ShadowAllocation *allocation = malloc(sizeof *allocation);
    if (!allocation) { free(fresh); shadow_failure_locked(failure, "allocation metadata", sizeof *allocation); return 0; }
    if (g_shadow_index_count == g_shadow_index_capacity) {
        size_t next_capacity = g_shadow_index_capacity ? g_shadow_index_capacity*2 : 1024;
        if (next_capacity < g_shadow_index_capacity || next_capacity > SIZE_MAX/sizeof(*g_shadow_index)) {
            free(allocation); free(fresh); shadow_failure_locked(failure, "allocation index size overflow", next_capacity); return 0;
        }
        ShadowAllocation **next = calloc(next_capacity, sizeof *next);
        if (!next) {
            free(allocation); free(fresh);
            shadow_failure_locked(failure, "allocation index", next_capacity*sizeof(*next)); return 0;
        }
        if (g_shadow_index_count) memcpy(next, g_shadow_index, g_shadow_index_count*sizeof(*next));
        free(g_shadow_index); g_shadow_index = next; g_shadow_index_capacity = next_capacity;
    }
    *allocation = (ShadowAllocation){.memory=memory, .capacity=capacity, .next=g_shadow_allocations};
    size_t lo = 0, hi = g_shadow_index_count;
    while (lo < hi) {
        size_t mid = lo+(hi-lo)/2;
        if ((uintptr_t)g_shadow_index[mid]->memory < (uintptr_t)memory) lo = mid+1;
        else hi = mid;
    }
    memmove(g_shadow_index+lo+1, g_shadow_index+lo, (g_shadow_index_count-lo)*sizeof(*g_shadow_index));
    g_shadow_index[lo] = allocation; ++g_shadow_index_count;
    g_shadow_allocations = allocation;
    slab->used = offset+capacity;
    if (fresh) {
        g_shadow_slabs = fresh; ++g_shadow_stats.slabs;
        g_shadow_stats.slab_bytes += fresh->capacity+offsetof(ShadowSlab, data);
    }
    g_shadow_stats.bytes += capacity;
    if (g_shadow_stats.bytes > g_shadow_stats.peak) g_shadow_stats.peak = g_shadow_stats.bytes;
    if (capacity > g_shadow_stats.largest) g_shadow_stats.largest = capacity;
    ++g_shadow_stats.allocations;
    g_shadow_stats.spills += spill;
    return (uint64_t)(uintptr_t)memory;
}
static uint64_t shadow_continuation(uint64_t *capacity, uint64_t owner_root, ShadowFailure *failure) {
    pthread_mutex_lock(&g_shadow_lock);
    ShadowRoot *root = owner_root ? shadow_root_locked(owner_root, false, NULL) : NULL;
    if (owner_root && !root) {
        shadow_failure_locked(failure, "unregistered command root", *capacity);
        pthread_mutex_unlock(&g_shadow_lock); return 0;
    }
    // Each old block is considered at most once in this recording. The limit
    // captured at Begin excludes newly appended blocks from this lease cycle.
    while (root && root->cursor) {
        ShadowAllocation *block = root->cursor;
        root->cursor = block == root->limit ? NULL : block->lease_next;
        if (block->pinned || block->capacity < *capacity) continue;
        *capacity = block->capacity; ++g_shadow_stats.reused;
        uint64_t host = (uintptr_t)block->memory;
        pthread_mutex_unlock(&g_shadow_lock); return host;
    }
    uint64_t host = shadow_allocate_locked(*capacity, true, failure);
    if (host && root) {
        ShadowAllocation *block = g_shadow_allocations;
        block->owner_root = owner_root; block->lease_previous = root->tail;
        if (root->tail) root->tail->lease_next = block; else root->head = block;
        root->tail = block;
    }
    if (host) shadow_growth_locked();
    pthread_mutex_unlock(&g_shadow_lock);
    return host;
}
static uint64_t shadow_of(uint64_t guest, uint64_t size, ShadowFailure *failure) {
    uint64_t capacity;
    if (!guest || !size || !shadow_initial_capacity(size, &capacity)) return 0;
    pthread_mutex_lock(&g_shadow_lock);
    if (g_nshadow >= g_shadow_cap / 2) {
        size_t ncap = g_shadow_cap ? g_shadow_cap * 2 : 4096;
        if (ncap < g_shadow_cap || ncap > SIZE_MAX / sizeof(Shadow)) {
            shadow_failure_locked(failure, "guest table size overflow", ncap); goto failed;
        }
        Shadow *next = calloc(ncap, sizeof *next);
        if (!next) { shadow_failure_locked(failure, "guest table allocation", ncap*sizeof(Shadow)); goto failed; }
        for (size_t i = 0; i < g_shadow_cap; ++i) if (g_shadow[i].guest) {
            size_t k = (g_shadow[i].guest >> 8) & (ncap - 1);
            while (next[k].guest) k = (k + 1) & (ncap - 1);
            next[k] = g_shadow[i];
        }
        free(g_shadow); g_shadow = next; g_shadow_cap = ncap;
    }
    size_t k = (guest >> 8) & (g_shadow_cap - 1);
    while (g_shadow[k].guest && g_shadow[k].guest != guest) k = (k + 1) & (g_shadow_cap - 1);
    Shadow *entry = &g_shadow[k];
    if (entry->size < size) {
        uint64_t host = shadow_allocate_locked(capacity, false, failure);
        if (!host) goto failed;
        if (!entry->guest) { entry->guest = guest; ++g_nshadow; }
        entry->host = host; entry->size = size;
    }
    uint64_t result = entry->host;
    shadow_growth_locked();
    pthread_mutex_unlock(&g_shadow_lock);
    return result;
failed:
    pthread_mutex_unlock(&g_shadow_lock);
    return 0;
}
void nvn_command_shadow_finish_after_gpu_idle(void) {
    nvn_command_lifetime_finish_after_consumers();
    pthread_mutex_lock(&g_shadow_lock);
    while (g_shadow_allocations) {
        ShadowAllocation *old = g_shadow_allocations;
        g_shadow_allocations = old->next; free(old);
    }
    while (g_shadow_slabs) {
        ShadowSlab *old = g_shadow_slabs;
        g_shadow_slabs = old->next; free(old);
    }
    free(g_shadow); g_shadow = NULL; g_nshadow = g_shadow_cap = 0;
    free(g_shadow_index); g_shadow_index = NULL; g_shadow_index_count = g_shadow_index_capacity = 0;
    free(g_shadow_roots); g_shadow_roots = NULL; g_shadow_root_count = g_shadow_root_capacity = 0;
    memset(&g_shadow_stats, 0, sizeof g_shadow_stats);
    pthread_mutex_unlock(&g_shadow_lock);
}
NVN(nvnCommandBufferAddCommandMemory) {
    // Guest NVN object pointers remain the existing HLE ownership contract;
    // validate nonnull/alignment before dereference, then the actual pool range.
    if (!A(0) || !A(1) || ((A(0) | A(1)) & 7)) FATAL("nvn: invalid command buffer/pool pointer\n");
    NvnCmdBuf *cb = cb_of(A(0)); NvnPool *pool = HOST(NvnPool, A(1));
    uint64_t offset = A(2), size = A(3), capacity;
    if (!cb || !pool || !pool->storage || !size || offset >= pool->size ||
        offset > UINTPTR_MAX - pool->storage || !shadow_initial_capacity(size, &capacity))
        FATAL("nvn: invalid command chunk range/size\n");
    uint64_t mem = pool->storage + offset;
    if ((mem & 3) || (size & 3) || size > UINTPTR_MAX - mem)
        FATAL("nvn: invalid command chunk alignment/endpoint\n");
    // Explicit HLE compatibility policy: some callers hand the API its minimum
    // nominal capacity while their suballocation has a smaller usable tail.
    // Accept the reservation, never the unbacked bytes: budgets, callbacks and
    // host backing are bounded independently by the real memory pool extent.
    // This is not a claim about original proprietary driver's validation rules.
    uint64_t effective = size < pool->size - offset ? size : pool->size - offset;
    if (!effective || (effective & (CMD_WORD - 1)) || !shadow_initial_capacity(effective, &capacity))
        FATAL("nvn: invalid effective command chunk capacity\n");
    if (effective < size) {
        static unsigned reports;
        unsigned index = __atomic_load_n(&reports, __ATOMIC_RELAXED);
        while (index < 8 && !__atomic_compare_exchange_n(&reports, &index, index + 1, false,
                                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (index < 8) LOG("[nvn] command capacity %llu limited to backed pool tail %llu (offset %llu, pool %llu)\n",
            (unsigned long long)size, (unsigned long long)effective,
            (unsigned long long)offset, (unsigned long long)pool->size);
    }
    const uint64_t trace_args[]={A(1),pool->storage,pool->size,offset,size,effective};
    memory_trace(c,cb,MT_ADD,trace_args);
    // Only the synthetic Begin exists: no command payload or earlier chunk can
    // be forgotten just because the current chunk's guest counter is zero.
    bool empty_recording = cb->recording && cb->host && cb->used >= sizeof(CmdRec) &&
        cb->used <= cb->hcap && cb->start >= cb->host &&
        cb->start - cb->host == cb->used - sizeof(CmdRec) &&
        ((const CmdRec *)(uintptr_t)cb->start)->op == CMD_BARRIER &&
        ((const CmdRec *)(uintptr_t)cb->start)->size == sizeof(CmdRec);
    // A ring allocator can recycle a completed prefix of the same reservation
    // while the current list occupies its tail. Require the WHOLE new backed
    // chunk below every span used by this recording, not merely its last chunk.
    bool same_chunk = cb->recording && cb->host && mem == cb->mem;
    bool safe_prefix = same_chunk && cb->recording_guest_floor >= mem &&
        effective <= cb->recording_guest_floor - mem;
    uint64_t old_floor=cb->recording_guest_floor,old_used=cb->guest_used,old_host=cb->host;
    if (same_chunk && !safe_prefix && !empty_recording) {
        memory_trace_dump(cb,"same-guest-chunk");
        FATAL("nvn: recording cannot chain a command chunk to itself\n");
    }
    if (cb->recording && cb->host && (cb->used > cb->hcap || sizeof(CmdRec) > cb->hcap - cb->used))
        FATAL("nvn: command chunk has no chain space\n");
    if(nvn_command_lifetime_enabled()){
        nvn_command_record_add(cb,pool,offset,size,effective);
        if (empty_recording || (cb->recording && mem < cb->recording_guest_floor)) cb->recording_guest_floor=mem;
        nvn_command_pool_note_add(pool,offset,size,effective);
        memory_trace(c,cb,MT_ADDED,trace_args);
        memory_trace_accepted(cb,empty_recording,safe_prefix,true,old_floor,old_used,old_host,trace_args[0],offset);
        return;
    }
    if (empty_recording) {
        // Keep the real Begin/header and its ACTUAL backing capacity. A larger
        // new guest budget can grow host backing through normal spill handling.
        cb->mem=mem;cb->size=size;cb->guest_capacity=effective;cb->guest_used=0;
        cb->recording_guest_floor=mem;
        nvn_command_pool_note_add(pool,offset,size,effective);
        memory_trace(c,cb,MT_ADDED,trace_args);
        memory_trace_accepted(cb,true,false,false,old_floor,old_used,old_host,trace_args[0],offset);
        return;
    }
    ShadowFailure failure = {0};
    // Guest-keyed shadows may contain earlier lists or the active tail. A
    // recycled prefix instead leases an unused block owned by this exact root;
    // the continuation allocator never returns a block twice in one recording.
    uint64_t host = safe_prefix ? shadow_continuation(&capacity,cb->start,&failure) : shadow_of(mem, effective, &failure);
    if (!host) FATAL("nvn: cannot allocate checked command shadow: %s, request %llu; guest offset %llu nominal %llu effective %llu "
        "host capacity %llu; retained %llu peak %llu allocations %llu spills %llu largest %llu guest starts %llu table %llu\n",
        failure.reason ? failure.reason : "invalid range", (unsigned long long)failure.requested,
        (unsigned long long)offset, (unsigned long long)size, (unsigned long long)effective, (unsigned long long)capacity,
        (unsigned long long)failure.retained, (unsigned long long)failure.peak, (unsigned long long)failure.allocations,
        (unsigned long long)failure.spills, (unsigned long long)failure.largest, (unsigned long long)failure.guests,
        (unsigned long long)failure.table_capacity);
    if (cb->recording && cb->host) {
        if ((mem == cb->mem && !safe_prefix) || host == cb->host) {
            memory_trace_dump(cb,"same-host-shadow");
            FATAL("nvn: recording cannot chain a command chunk to itself\n");
        }
        CmdRec *j = raw_rec(cb->host + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = host;
    }
    cb->mem = mem; cb->size = size; cb->guest_capacity = effective; cb->host = host;
    cb->hcap = capacity; cb->used = 0; cb->guest_used = 0;
    if (cb->recording && mem < cb->recording_guest_floor) cb->recording_guest_floor=mem;
    nvn_command_pool_note_add(pool, offset, size, effective);
    memory_trace(c,cb,MT_ADDED,trace_args);
    memory_trace_accepted(cb,false,safe_prefix,false,old_floor,old_used,old_host,trace_args[0],offset);
}
NVN(nvnCommandBufferAddControlMemory) { if(memory_trace_enabled) { const uint64_t args[]={A(1),A(2),0,0,0,0}; memory_trace(c,cb_of(A(0)),MT_CONTROL,args); } }
NVN(nvnCommandBufferGetCommandMemorySize) { NvnCmdBuf *cb=cb_of(A(0)); uint64_t value=cb->size; memory_trace_query(c,cb,MT_QUERY_SIZE,value); RET(value); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { NvnCmdBuf *cb=cb_of(A(0)); uint64_t value=cb->guest_used; memory_trace_query(c,cb,MT_QUERY_USED,value); RET(value); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    // Free means payload budget; retain a word for a terminating End. Used/size
    // are guest units and never vary when host backing spills.
    uint64_t value=cb->guest_used <= cb->guest_capacity && END_BUDGET <= cb->guest_capacity - cb->guest_used ?
        cb->guest_capacity - cb->guest_used - END_BUDGET : 0;
    memory_trace_query(c,cb,MT_QUERY_FREE,value);
    RET(value);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { NvnCmdBuf *cb=cb_of(A(0)); cb->callback=A(1); const uint64_t args[]={A(1),0,0,0,0,0}; memory_trace(c,cb,MT_SET_CALLBACK,args); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { if(memory_trace_enabled) { const uint64_t args[]={A(1),0,0,0,0,0}; memory_trace(c,cb_of(A(0)),MT_ENABLE,args); } }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first.
static CmdRec *rec_budget(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra, bool begin) {
    if(nvn_command_lifetime_enabled())return nvn_command_record_budget(c,cb,op,extra,begin);
    uint64_t wide = (uint64_t)sizeof(CmdRec) + extra + 7;
    if (wide > UINT32_MAX) FATAL("nvn: command record size overflow\n");
    uint32_t size = (uint32_t)(wide & ~UINT64_C(7));
    uint64_t cost = begin ? 0 : op == CMD_END ? END_BUDGET :
        (((uint64_t)size + CMD_SHADOW * CMD_WORD - 1) / (CMD_SHADOW * CMD_WORD)) * CMD_WORD;
    uint64_t required = cost + (op == CMD_END ? 0 : END_BUDGET);
    for (int tries = 0; !cb->host || cb->guest_used > cb->guest_capacity || required > cb->guest_capacity - cb->guest_used; tries++) {
        if (!cb->callback || tries == 2) FATAL("nvn: out of command memory (need %llu guest bytes)\n", (unsigned long long)required);
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        MemoryTraceCallback trace={0};
        if(memory_trace_enabled) {
            trace=(MemoryTraceCallback){.previous=memory_trace_callback,.cb=cb,
                .origin={cb->guest,c->x[30],cb->callback,cb->callback_data,required,op,extra,size,(uint32_t)cost,
                         (uint32_t)tries,memory_trace_callback?memory_trace_callback->origin.depth+1:1}};
            memory_trace_callback=&trace;memory_trace(c,cb,MT_CALLBACK,NULL);
        }
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = required;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        if(memory_trace_enabled) { memory_trace(c,cb,MT_RETURN,NULL);memory_trace_callback=trace.previous; }
        memcpy(c->x, saved, sizeof saved);
    }
    if (cb->used > cb->hcap || JUMP_RESERVE > cb->hcap - cb->used)
        FATAL("nvn: invalid host command shadow cursor\n");
    if (size > cb->hcap - cb->used - JUMP_RESERVE) {
        uint64_t capacity;
        if (!shadow_capacity(cb->guest_capacity - cb->guest_used, &capacity)) FATAL("nvn: command continuation size overflow\n");
        uint64_t minimum = (uint64_t)size + HOST_FRAMING; // the actual record, terminating End and chain space
        if (minimum > SIZE_MAX) FATAL("nvn: command continuation record size overflow\n");
        if (capacity < minimum) capacity = minimum; // one genuinely large inline record, not an unused guest reservation
        ShadowFailure failure = {0};
        uint64_t host = shadow_continuation(&capacity, cb->recording ? cb->start : 0, &failure);
        if (!host) FATAL("nvn: cannot allocate command continuation: %s, request %llu record %u; "
            "retained %llu peak %llu allocations %llu spills %llu largest %llu guest starts %llu table %llu\n",
            failure.reason ? failure.reason : "invalid range", (unsigned long long)failure.requested, size,
            (unsigned long long)failure.retained, (unsigned long long)failure.peak, (unsigned long long)failure.allocations,
            (unsigned long long)failure.spills, (unsigned long long)failure.largest, (unsigned long long)failure.guests,
            (unsigned long long)failure.table_capacity);
        // Begin is not active yet: never append a link to a previous ended list.
        // Mid-list growth keeps its original start/handle and chains only here.
        if (cb->recording) {
            CmdRec *jump = raw_rec(cb->host + cb->used, CMD_JUMP, sizeof(CmdRec));
            jump->x[0] = host;
        }
        cb->host = host; cb->hcap = capacity; cb->used = 0;
    }
    // Reserve/pin the root before overwriting its Begin header. Concurrent
    // overlapping recording still requires client synchronization, as it did
    // before continuation reuse; the pin itself has no write-before-pin gap.
    if (begin) {
        ShadowFailure failure = {0};
        if (!shadow_begin_root(cb->host+cb->used, &failure))
            FATAL("nvn: cannot register command root: %s, request %llu; retained %llu peak %llu allocations %llu spills %llu\n",
                  failure.reason ? failure.reason : "invalid owner", (unsigned long long)failure.requested,
                  (unsigned long long)failure.retained, (unsigned long long)failure.peak,
                  (unsigned long long)failure.allocations, (unsigned long long)failure.spills);
    }
    CmdRec *r = raw_rec(cb->host + cb->used, op, size);
    cb->used += size; cb->guest_used += cost;
    return r;
}
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    return rec_budget(c, cb, op, extra, false);
}
NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    memory_trace(c,cb,MT_BEGIN,NULL);
    if (cb->recording) FATAL("nvn: BeginRecording while already recording\n");
    cb->fences = false;
    cb->deferred = false;
    CmdRec *start = rec_budget(c, cb, CMD_BARRIER, 0, true);
    cb->start = (uint64_t)(uintptr_t)start;
    cb->recording_guest_floor = cb->mem + cb->guest_used; // Begin consumes zero guest bytes.
    cb->recording = true;
    memory_trace(c,cb,MT_BEGUN,NULL);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    memory_trace(c,cb,MT_END,NULL);
    if (!cb->recording) FATAL("nvn: EndRecording without BeginRecording\n");
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    ((CmdRec *)(uintptr_t)cb->start)->x[6] = cb->fences ? CMD_LIST_FENCES : 0;
    ((CmdRec *)(uintptr_t)cb->start)->x[5] = cb->deferred ? CMD_LIST_DEFERRED : 0;
    uint64_t handle=nvn_command_lifetime_enabled()?nvn_command_record_end(cb):cb->start;
    memory_trace(c,cb,MT_ENDED,NULL);
    RET(handle);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// Caller holds g_shadow_lock; only retained host shadows are executable list
// storage. Never inspect an arbitrary guest pointer while detecting late reads.
// Test instrumentation counts comparisons, not wall-clock timing. Production
// expands this away; no counter or contention is added to command submission.
#ifndef NVN_SHADOW_LOOKUP_PROBE
#define NVN_SHADOW_LOOKUP_PROBE() ((void)0)
#endif
static const ShadowAllocation *shadow_allocation_locked(uint64_t address) {
    size_t lo = 0, hi = g_shadow_index_count;
    while (lo < hi) {
        size_t mid = lo+(hi-lo)/2;
        NVN_SHADOW_LOOKUP_PROBE();
        if ((uintptr_t)g_shadow_index[mid]->memory <= address) lo = mid+1;
        else hi = mid;
    }
    return lo ? g_shadow_index[lo-1] : NULL;
}
static bool shadow_contains_header(const ShadowAllocation *a, uint64_t address) {
    if (!a) return false;
    uint64_t base = (uintptr_t)a->memory;
    return address >= base && address-base <= a->capacity && sizeof(CmdRec) <= a->capacity-(address-base);
}
static const CmdRec *shadow_record_locked(uint64_t address, const ShadowAllocation **current) {
    if (address & 7) return NULL;
    // Sequential records usually remain in the same block. Revalidate every
    // header/record extent, resolving the index only on a block transition.
    const ShadowAllocation *a = *current;
    if (!shadow_contains_header(a, address)) a = shadow_allocation_locked(address);
    if (!shadow_contains_header(a, address)) return NULL;
    *current = a;
    const CmdRec *r = (const CmdRec *)(uintptr_t)address;
    if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || (r->size & 7) ||
        r->size > a->capacity-(address-(uintptr_t)a->memory)) return NULL;
    return r;
}
static int command_deferred_locked(uint64_t handle, unsigned depth, uint32_t *budget) {
    if (!*budget) return -1;
    --*budget;
    const ShadowAllocation *current = NULL;
    const CmdRec *first = shadow_record_locked(handle, &current);
    if (!first || first->op != CMD_BARRIER) return -1;
    if (first->x[5] != CMD_LIST_DEFERRED) return 0;
    uint64_t address = handle;
    while (*budget) {
        --*budget;
        const CmdRec *r = shadow_record_locked(address, &current);
        if (!r) return -1;
        switch (r->op) {
        case CMD_END: return 0;
        case CMD_JUMP: address = r->x[0]; continue;
        case CMD_BIND_TEXTURES_DEFERRED:
        case CMD_BIND_UNIFORM_BUFFERS_DEFERRED:
        case CMD_BIND_STORAGE_BUFFERS_DEFERRED: return 1;
        case CMD_CALL:
            if (r->x[0] > (r->size-sizeof(CmdRec))/sizeof(uint64_t)) return -1;
            if (depth < 8) for (uint64_t i = 0; i < r->x[0]; ++i) {
                uint64_t child; memcpy(&child, r->extra+i*sizeof child, sizeof child);
                int result = command_deferred_locked(child, depth+1, budget);
                if (result) return result;
            }
            break;
        default: break;
        }
        address += r->size; // validated wholly inside its retained allocation
    }
    return -1;
}
bool nvn_command_has_deferred(uint64_t handle) {
    if(nvn_command_lifetime_enabled())return nvn_command_flags(handle).deferred;
    uint32_t budget = 10000000; // same global record guard as command replay
    pthread_mutex_lock(&g_shadow_lock);
    int result = command_deferred_locked(handle, 0, &budget);
    pthread_mutex_unlock(&g_shadow_lock);
    if (result < 0) FATAL("[nvn] invalid command shadow while checking deferred bindings\n");
    return result != 0;
}

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;
    r->x[1] = s.x[2];  // stage mask; NULL program unbinds only these stages
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
// The deferred API records the address, not its values. In this HLE the
// render-thread replay consumes the array, in command order. QueueSubmitCommands
// waits for that CPU replay before returning, so the caller may then reuse the
// array while the submitted GPU work remains asynchronous.
static void bind_deferred(Cpu *c, uint32_t op, uint32_t limit, uint32_t stride) {
    Snap s = snap(c);
    int32_t stage = (int32_t)(uint32_t)s.x[1], first = (int32_t)(uint32_t)s.x[2];
    int32_t count = (int32_t)(uint32_t)s.x[3];
    if (!count) return;
    if (stage < 0 || stage >= 6 || first < 0 || count < 0 ||
        (uint32_t)first >= limit || (uint32_t)count > limit-(uint32_t)first ||
        !s.x[4] || s.x[4] > UINTPTR_MAX-(uint64_t)(uint32_t)count*stride)
        FATAL("[nvn] invalid deferred binding range\n");
    CmdRec *r = record(c, op, 0, &s);
    r->x[0] = (uint32_t)stage; r->x[1] = (uint32_t)first;
    r->x[2] = (uint32_t)count; r->x[3] = s.x[4];
    cb_of(s.x[0])->deferred = true;
}
NVN(nvnCommandBufferBindTexturesDeferred) { bind_deferred(c, CMD_BIND_TEXTURES_DEFERRED, 32, 8); }
NVN(nvnCommandBufferBindUniformBuffersDeferred) { bind_deferred(c, CMD_BIND_UNIFORM_BUFFERS_DEFERRED, 18, 16); }
NVN(nvnCommandBufferBindStorageBuffersDeferred) { bind_deferred(c, CMD_BIND_STORAGE_BUFFERS_DEFERRED, 16, 16); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) {
            rt.has_view[i] = 1;
            memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView));
            nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.tex[i], &rt.views[i]);
        }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) {
        rt.has_dview = 1;
        memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView));
        nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.depth, &rt.dview);
    }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
NVN(nvnCommandBufferSetDepthClamp) { (void)c; }
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 0; }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); nvn_texture_note_view((NvnTexture *)(uintptr_t)ct.tex, &ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= GUEST_LO && fp < GUEST(0x8000000000ull) && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); nvn_texture_note_view((NvnTexture *)(uintptr_t)t->tex, &t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    cb_of(A(0))->fences = true;
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
NVN(nvnCommandBufferCallCommands) { if(nvn_command_lifetime_enabled()){nvn_command_record_call(c);return;} cb_of(A(0))->fences = true; cb_of(A(0))->deferred = true; record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferCopyCommands) { if(nvn_command_lifetime_enabled()){nvn_command_record_copy(c);return;} cb_of(A(0))->fences = true; cb_of(A(0))->deferred = true; record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
