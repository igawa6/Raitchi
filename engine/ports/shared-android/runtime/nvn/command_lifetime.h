#pragma once
#include "command_body_arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NVN_CL_ENCODED_DOMAIN_MAX UINT64_C(0xffffff)
#define NVN_CL_ENCODED_BYTE_OFFSET_MAX (UINT64_C(0xffffffff)*4)
/* Encoded mode: tagC2/domain24/wordOffset32; unknown shape-valid CALL IDs
 * create only metadata placeholders. No body is resolvable until publication. */
typedef struct NvnClStore NvnClStore;
typedef struct NvnClLease NvnClLease;
typedef struct NvnClSnapshot NvnClSnapshot;
typedef uint64_t NvnClHandle;
typedef enum { NVN_CL_OK,NVN_CL_INVALID,NVN_CL_UNKNOWN_HANDLE,NVN_CL_LIMIT,NVN_CL_NOMEM,NVN_CL_BUSY } NvnClResult;
/* Host HLE ABI schema. Integration must statically assert actual CmdRec is80B
 * with op/size at0/4 and x[0] at8; opcode values come from the real enum. */
typedef struct { uint32_t op_count,begin,end,jump,call,fence,deferred[3]; } NvnClSchema;
typedef struct {
    size_t max_slots,max_bodies,max_spans_per_body,max_call_edges,max_lease_slots;
    uint64_t max_body_bytes;
    uint32_t record_budget; /*1..10,000,000; closure rejects depth>8/cycles */
    NvnClSchema schema;
    CbaConfig arena;
    bool deterministic_handles; /* encoded handle + structural slot GC; fixed for this store */
} NvnClConfig;
/* Canonical backing identity, not CPU virtual address inference. Different
 * logical domains may alias the same backing domain/range. Unknown aliases
 * MUST remain separate proof domains, retaining extra data conservatively. */
typedef struct { uint64_t domain,begin,end,pool_generation; } NvnClSpan;
typedef struct {
    uint64_t logical_domain,logical_offset;
    const void *records;size_t bytes;
    const NvnClSpan *spans;size_t span_count;
} NvnClInput;
typedef struct {
    const void *records;size_t bytes;uint64_t generation;
    uint32_t record_count;bool has_calls,direct_deferred,direct_fences;
} NvnClBodyView;
typedef struct { bool deferred,fences; } NvnClLeaseFlags;
typedef struct {
    size_t slots,current_bodies,alive_bodies,span_nodes,leases,snapshots;
    uint64_t current_bytes,alive_bytes,pinned_bytes,next_generation;
    uint64_t slot_probes,span_probes,lease_probes;
    CbaStats arena;
    uint64_t collected_slots,slot_delete_probes;size_t peak_slots;
    size_t empty_incoming_only,empty_old_body_only,empty_both,empty_unreferenced;
} NvnClStats;
NvnClResult nvn_cl_create(NvnClConfig config,NvnClStore **out);
NvnClResult nvn_cl_destroy(NvnClStore *store); /* BUSY with leased/snapshot bytes */
/* Input bytes/spans are trusted caller-owned host storage, not arbitrary guest
 * addresses. Copies full flat CmdRec bytes. No JUMPs; Begin first/End last;
 * record envelopes and CALL payload bounds validated. Other op-specific
 * payload semantics remain the HLE producer's responsibility. Spans must be
 * disjoint, word-aligned, and sum
 * to exact HLE costs (Begin0, End4, otherceil(size/8) rounded4). Transactional
 * publish reuses an exact logical root's slot, invalidates overlapping others,
 * and releases old bodies only when their last reader finishes. */
NvnClResult nvn_cl_publish(NvnClStore *store,const NvnClInput *input,NvnClHandle *out);
/* One coherent CURRENT closure; all later classifier/prescan/replay consumers
 * MUST read these chosen bodies, never re-resolve global current slots. */
NvnClResult nvn_cl_acquire(NvnClStore *store,const NvnClHandle *handles,size_t count,NvnClLease **out);
NvnClResult nvn_cl_lease_retain(NvnClLease *lease);
NvnClResult nvn_cl_lease_release_completed(NvnClLease *lease,bool reader_finished);
bool nvn_cl_lease_body(const NvnClLease *lease,NvnClHandle handle,NvnClBodyView *out);
bool nvn_cl_lease_span(const NvnClLease *lease,NvnClHandle handle,size_t index,NvnClSpan *out);
NvnClLeaseFlags nvn_cl_lease_flags(const NvnClLease *lease);
bool nvn_cl_lease_handle_flags(const NvnClLease *lease,NvnClHandle handle,NvnClLeaseFlags *out);
/* COPY needs only direct bytes pinned. Nested CALL slot IDs stay unchanged;
 * the copied target's later acquire selects their then-current bodies. */
NvnClResult nvn_cl_snapshot(NvnClStore *store,NvnClHandle handle,NvnClSnapshot **out);
NvnClBodyView nvn_cl_snapshot_view(const NvnClSnapshot *snapshot);
void nvn_cl_snapshot_release(NvnClSnapshot *snapshot);
bool nvn_cl_slot_present(NvnClStore *store,NvnClHandle handle);
NvnClStats nvn_cl_stats(NvnClStore *store);
/* Diagnostic O(slots) snapshot; exclusive empty-slot reasons. Ordinary
 * nvn_cl_stats remains O(1). Call only on an explicit low-rate diagnostic path. */
NvnClStats nvn_cl_diagnostic_stats(NvnClStore *store);
void nvn_cl_reset_probes(NvnClStore *store);
/* No Finalize/LRU/time retirement API: normative handle/pool Finalize lifetime
 * remains unproven. Caller owns store lifetime and must serialize destruction
 * against API entry. Normal calls are internally mutex-protected; allocator
 * callbacks must not reenter this store. Environment/mode selection belongs to integration and never changes within
 * an existing store. */
