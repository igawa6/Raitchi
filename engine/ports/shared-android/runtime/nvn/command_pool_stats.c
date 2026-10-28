#include "command_pool_stats.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define POOL_SLOTS 4096u
#define REPORT_LIMIT 64u
typedef struct {
    uintptr_t key;
    uint64_t generation, storage, size;
    unsigned char state;
    bool command, finalizing;
} Pool;
static Pool *pools;
static NvnCommandPoolStats stats;
static uint64_t next_generation;
static unsigned events;
static _Atomic int enabled = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool on(void) {
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value >= 0) return value != 0;
    const char *setting = getenv("LARECOMP_LOG_COMMAND_POOLS");
    value = setting && *setting && strtol(setting, NULL, 10) != 0;
    atomic_store_explicit(&enabled, value, memory_order_relaxed);
    return value != 0;
}
static void add(uint64_t *value, uint64_t amount) {
    if (amount > UINT64_MAX-*value) { *value = UINT64_MAX; stats.overflowed = true; }
    else *value += amount;
}
static size_t hash(uintptr_t key) {
    uint64_t value = key; value ^= value >> 33; value *= UINT64_C(0xff51afd7ed558ccd); value ^= value >> 33;
    return (size_t)value & (POOL_SLOTS-1);
}
static Pool *find(uintptr_t key, bool insert) {
    if (!pools || !key) return NULL;
    Pool *vacant = NULL;
    size_t at = hash(key);
    for (unsigned i = 0; i < POOL_SLOTS; ++i, at = (at+1)&(POOL_SLOTS-1)) {
        Pool *entry = &pools[at];
        if (entry->state == 1 && entry->key == key) return entry;
        if (entry->state != 1 && !vacant) vacant = entry;
        if (!entry->state) break;
    }
    return insert ? vacant : NULL;
}
static void report(const char *event, const Pool *pool, uint64_t offset, uint64_t nominal, uint64_t effective, uint64_t handle) {
    if (stats.reports >= REPORT_LIMIT) return;
    ++stats.reports;
    fprintf(stderr, "[command-pools] %s generation %llu storage %llx size %llu offset %llu nominal %llu effective %llu handle %llx; "
        "pools %llu finalized %llu command %llu command-finalized %llu active-command %llu active-bytes %llu peak-bytes %llu "
        "finalized-bytes %llu destroyed %llu command-destroyed %llu adds %llu clipped %llu handle-finalizes %llu unknown %llu dropped %llu overflow %u\n",
        event, (unsigned long long)(pool ? pool->generation : 0), (unsigned long long)(pool ? pool->storage : 0),
        (unsigned long long)(pool ? pool->size : 0), (unsigned long long)offset, (unsigned long long)nominal,
        (unsigned long long)effective, (unsigned long long)handle, (unsigned long long)stats.generations,
        (unsigned long long)stats.finalizes, (unsigned long long)stats.command_generations,
        (unsigned long long)stats.command_finalizes, (unsigned long long)stats.active_command_pools,
        (unsigned long long)stats.active_command_bytes, (unsigned long long)stats.peak_command_bytes,
        (unsigned long long)stats.finalized_command_bytes, (unsigned long long)stats.destroys,
        (unsigned long long)stats.command_destroys, (unsigned long long)stats.adds,
        (unsigned long long)stats.clipped_adds, (unsigned long long)stats.handle_finalizes,
        (unsigned long long)stats.unknown_events, (unsigned long long)stats.dropped_events, stats.overflowed);
}
void nvn_command_pool_note_init(const void *pool, uint64_t storage, uint64_t size) {
    if (!on()) return;
    pthread_mutex_lock(&lock);
    if (!pools) pools = calloc(POOL_SLOTS, sizeof *pools);
    Pool *entry = find((uintptr_t)pool, true);
    if (!entry || next_generation == UINT64_MAX) {
        add(&stats.dropped_events, 1); report("init-dropped", NULL, 0, 0, 0, 0);
    } else if (entry->state == 1) {
        // No guessed finalize: preserve the original generation and flag a
        // missing/unexpected lifecycle event instead of inventing evidence.
        add(&stats.unknown_events, 1);
    } else {
        *entry = (Pool){.key=(uintptr_t)pool, .generation=++next_generation, .storage=storage, .size=size, .state=1};
        add(&stats.generations, 1); add(&stats.active_pools, 1);
    }
    pthread_mutex_unlock(&lock);
}
void nvn_command_pool_note_add(const void *pool, uint64_t offset, uint64_t nominal, uint64_t effective) {
    if (!on()) return;
    pthread_mutex_lock(&lock);
    add(&stats.adds, 1);
    if (effective < nominal) add(&stats.clipped_adds, 1);
    Pool *entry = find((uintptr_t)pool, false);
    if (!entry) add(&stats.unknown_events, 1);
    else if (!entry->command) {
        entry->command = true; add(&stats.command_generations, 1); add(&stats.active_command_pools, 1);
        add(&stats.active_command_bytes, entry->size);
        if (stats.active_command_bytes > stats.peak_command_bytes) stats.peak_command_bytes = stats.active_command_bytes;
        if (events++ < 24) report("command-first-use", entry, offset, nominal, effective, 0);
    }
    if (stats.adds && !(stats.adds&(stats.adds-1))) report("summary", entry, offset, nominal, effective, 0);
    pthread_mutex_unlock(&lock);
}
void nvn_command_pool_note_finalize(const void *pool) {
    if (!on()) return;
    pthread_mutex_lock(&lock);
    Pool *entry = find((uintptr_t)pool, false);
    if (!entry || entry->finalizing) add(&stats.unknown_events, 1);
    else {
        entry->finalizing = true; add(&stats.finalizes, 1);
        if (entry->command) {
            add(&stats.command_finalizes, 1);
            if (events++ < 24) report("command-finalize-request", entry, 0, 0, 0, 0);
        }
    }
    pthread_mutex_unlock(&lock);
}
void nvn_command_pool_note_destroy(const void *pool) {
    if (!on()) return;
    pthread_mutex_lock(&lock);
    Pool *entry = find((uintptr_t)pool, false);
    if (!entry) add(&stats.unknown_events, 1);
    else {
        add(&stats.destroys, 1); --stats.active_pools;
        if (entry->command) {
            add(&stats.command_destroys, 1); --stats.active_command_pools;
            stats.active_command_bytes = stats.active_command_bytes >= entry->size ? stats.active_command_bytes-entry->size : 0;
            add(&stats.finalized_command_bytes, entry->size);
            if (events++ < 24) report("command-finalize-complete", entry, 0, 0, 0, 0);
        }
        entry->state = 2;
    }
    pthread_mutex_unlock(&lock);
}
void nvn_command_pool_note_handle_finalize(uint64_t handle) {
    if (!on()) return;
    pthread_mutex_lock(&lock);
    add(&stats.handle_finalizes, 1);
    if (stats.handle_finalizes <= 8 || !(stats.handle_finalizes&(stats.handle_finalizes-1))) report("handle-finalize", NULL, 0, 0, 0, handle);
    pthread_mutex_unlock(&lock);
}
bool nvn_command_pool_stats_snapshot(NvnCommandPoolStats *out) {
    if (!out || !on()) return false;
    pthread_mutex_lock(&lock); *out = stats; pthread_mutex_unlock(&lock);
    return true;
}
