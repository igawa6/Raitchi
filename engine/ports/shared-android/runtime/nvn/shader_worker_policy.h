/* Bounded shader compiler capacity; independent of the non-reentrant translator. */
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

static int shader_worker_count(int affinity_cpus, int logical_cpus, bool android,
                               const char *override) {
    int cpus = affinity_cpus > 0 ? affinity_cpus : logical_cpus;
    if (cpus < 1) cpus = 2;
    int count = cpus > 1 ? cpus - 1 : 1;
    int maximum = android ? 8 : 16;
    if (count > maximum) count = maximum;
    if (override && *override) {
        char *end = NULL;
        errno = 0;
        long requested = strtol(override, &end, 10);
        if (!errno && end != override && !*end && requested > 0) {
            count = requested > 16 ? 16 : (int)requested;
        }
    }
    return count;
}

static int shader_background_capacity(int workers, bool preparing) {
    if (workers < 1) workers = 1;
    return preparing || workers < 2 ? workers : 2;
}
