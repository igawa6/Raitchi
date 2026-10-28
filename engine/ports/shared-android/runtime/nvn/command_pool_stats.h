#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Diagnostic metadata only. Default OFF; no storage/handle retirement. IDs are
 * process-local monotonically increasing pool generations, never guest IDs. */
typedef struct {
    uint64_t generations, finalizes, command_generations, command_finalizes;
    uint64_t adds, clipped_adds, active_pools, active_command_pools;
    uint64_t active_command_bytes, peak_command_bytes, finalized_command_bytes;
    uint64_t handle_finalizes, unknown_events, dropped_events, reports;
    uint64_t destroys, command_destroys;
    bool overflowed;
} NvnCommandPoolStats;
void nvn_command_pool_note_init(const void *pool, uint64_t storage, uint64_t size);
void nvn_command_pool_note_add(const void *pool, uint64_t offset, uint64_t nominal, uint64_t effective);
void nvn_command_pool_note_finalize(const void *pool);
void nvn_command_pool_note_destroy(const void *pool);
void nvn_command_pool_note_handle_finalize(uint64_t handle);
bool nvn_command_pool_stats_snapshot(NvnCommandPoolStats *out);
