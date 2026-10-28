#pragma once
/* Experimental Android Vulkan command ownership. Disabled unless the process
 * starts with LARECOMP_COMMAND_LIFETIME=1. No runtime mode changes. */
#include "command_lifetime.h"
#include "nvn.h"
bool nvn_command_lifetime_enabled(void);
/* Initialize before exposing a pool; destroy only in its ordered destructor.
 * Pool-local UID domains intentionally do not infer physical aliases. */
bool nvn_command_pool_created(NvnPool *pool);
void nvn_command_pool_destroyed(NvnPool *pool);
void nvn_command_record_add(NvnCmdBuf *, NvnPool *, uint64_t, uint64_t, uint64_t);
CmdRec *nvn_command_record_budget(Cpu *, NvnCmdBuf *, uint32_t, uint32_t, bool);
uint64_t nvn_command_record_end(NvnCmdBuf *);
void nvn_command_record_finalize(NvnCmdBuf *);
void nvn_command_record_call(Cpu *);
void nvn_command_record_copy(Cpu *);
void nvn_command_submit(Cpu *);
/* Reader scope is stack-like TLS. Resolve and flags require an acquired lease
 * in experimental mode. Nested calls never re-read a global CURRENT slot. */
NvnClLease *nvn_command_reader_enter(NvnClLease *);
void nvn_command_reader_leave(NvnClLease *);
NvnClLease *nvn_command_reader_current(void);
uint64_t nvn_command_resolve(uint64_t);
NvnClLeaseFlags nvn_command_flags(uint64_t);
/* Only called after ALL guest threads have joined. A timed-out render stop
 * retains the entire store. CPU completion, not GPU idle, owns body references. */
void nvn_command_lifetime_finish_after_consumers(void);

typedef struct {
    NvnClStats bodies;
    uint64_t scratch_bytes, rss_bytes;
    size_t active_pools;
} NvnCommandLifetimeStats;
/* No work/locks when OFF. Not callable concurrently with final destruction. */
bool nvn_command_lifetime_stats(NvnCommandLifetimeStats *out);
