#include "pool_mapping.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define INTERVAL_LIMIT 65536u
typedef struct { uint64_t begin, end, address; } Interval;
struct NvnPoolMappings { pthread_mutex_t lock; Interval *intervals; size_t count; uint64_t generation; };
NvnPoolMappings *nvn_pool_mappings_create(void) {
    NvnPoolMappings *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (pthread_mutex_init(&s->lock, NULL)) { free(s); return NULL; }
    s->generation = 1;
    return s;
}
void nvn_pool_mappings_destroy(NvnPoolMappings *s) {
    if (!s) return;
    free(s->intervals); pthread_mutex_destroy(&s->lock); free(s);
}
static int compare(const void *a, const void *b) {
    const Interval *x = a, *y = b;
    return (x->begin > y->begin) - (x->begin < y->begin);
}
bool nvn_pool_mappings_apply(NvnPoolMappings *s, uint64_t pool_size,
    const NvnPoolMappingRequest *r, size_t count) {
    if (!s || count > NVN_POOL_MAPPING_REQUEST_LIMIT || (count && !r)) return false;
    for (size_t i = 0; i < count; ++i)
        if (!r[i].size || r[i].offset > pool_size || r[i].size > pool_size - r[i].offset ||
            (r[i].address && r[i].size > UINT64_MAX - r[i].address)) return false;
    if (!count) return true;
    pthread_mutex_lock(&s->lock);
    if (s->generation == UINT64_MAX || s->count > INTERVAL_LIMIT - 2 * count) {
        pthread_mutex_unlock(&s->lock); return false;
    }
    size_t cap = s->count + 2 * count;
    Interval *a = malloc(cap * sizeof *a), *b = malloc(cap * sizeof *b);
    if (!a || !b) { free(a); free(b); pthread_mutex_unlock(&s->lock); return false; }
    size_t n = s->count;
    if (n) memcpy(a, s->intervals, n * sizeof *a);
    for (size_t i = 0; i < count; ++i) {
        uint64_t begin = r[i].offset, end = begin + r[i].size;
        size_t out = 0;
        for (size_t j = 0; j < n; ++j) {
            Interval old = a[j];
            if (old.end <= begin || old.begin >= end) { b[out++] = old; continue; }
            if (old.begin < begin) b[out++] = (Interval){old.begin, begin, old.address};
            if (old.end > end) b[out++] = (Interval){end, old.end, old.address + (end - old.begin)};
        }
        if (r[i].address) b[out++] = (Interval){begin, end, r[i].address};
        Interval *swap = a; a = b; b = swap; n = out;
    }
    qsort(a, n, sizeof *a, compare);
    size_t merged = 0;
    for (size_t i = 0; i < n; ++i) {
        if (merged && a[merged-1].end == a[i].begin &&
            a[merged-1].address + (a[merged-1].end - a[merged-1].begin) == a[i].address)
            a[merged-1].end = a[i].end;
        else a[merged++] = a[i];
    }
    free(s->intervals); s->intervals = a; s->count = merged; ++s->generation;
    free(b); pthread_mutex_unlock(&s->lock); return true;
}
bool nvn_pool_mappings_resolve(NvnPoolMappings *s, uint64_t pool_size,
    uint64_t offset, uint64_t size, uint64_t *address, uint64_t *generation) {
    if (address) *address = 0;
    if (generation) *generation = 0;
    if (!s || !address || !size || offset > pool_size || size > pool_size - offset) return false;
    pthread_mutex_lock(&s->lock);
    size_t lo = 0, hi = s->count;
    while (lo < hi) { size_t mid = lo + (hi-lo)/2; if (s->intervals[mid].begin <= offset) lo = mid+1; else hi = mid; }
    bool ok = false;
    if (lo) {
        const Interval *i = &s->intervals[lo-1];
        if (offset < i->end && size <= i->end-offset) {
            *address = i->address + (offset-i->begin);
            if (generation) *generation = s->generation;
            ok = true;
        }
    }
    pthread_mutex_unlock(&s->lock); return ok;
}
