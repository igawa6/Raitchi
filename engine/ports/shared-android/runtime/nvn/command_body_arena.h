#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct CbaArena CbaArena;
typedef enum { CBA_OK,CBA_INVALID,CBA_LIMIT,CBA_NOMEM,CBA_BUSY } CbaResult;
typedef struct { void *(*allocate)(size_t,void*);void (*release)(void*,void*);void *context; } CbaAllocator;
typedef struct { uint64_t max_reserved_data;size_t max_slabs;CbaAllocator allocator; } CbaConfig;
/* Owned internal token. Neither its pointer nor its address is a guest handle.
 * Serial and slab identity reject stale/double-free tokens after address reuse. */
typedef struct { void *pointer;uint64_t slab_id,allocation_id,requested,offset;uint32_t order; } CbaBlock;
typedef struct { uint64_t live_requested,live_rounded,reserved_data,reserved_total,peak_requested;size_t live_blocks,slabs;uint64_t lookup_probes; } CbaStats;
/* Caller serializes.4MiB buddy slabs, min256B blocks, hidden16B identity prefix;
 * all returned pointers16B aligned. Larger requests use dedicated slabs. */
CbaResult cba_create(CbaConfig config,CbaArena **out);
CbaResult cba_allocate(CbaArena *arena,uint64_t bytes,CbaBlock *out);
CbaResult cba_release(CbaArena *arena,const CbaBlock *block);
CbaResult cba_destroy(CbaArena *arena); /* BUSY with any live block */
CbaStats cba_stats(const CbaArena *arena);
