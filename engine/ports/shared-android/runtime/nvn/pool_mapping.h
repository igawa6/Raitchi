/* Bounded CPU span resolution for NVN virtual memory pools. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NVN_POOL_MAPPING_REQUEST_LIMIT 4096u
typedef struct NvnPoolMappings NvnPoolMappings;
typedef struct { uint64_t offset, size, address; } NvnPoolMappingRequest;
NvnPoolMappings *nvn_pool_mappings_create(void);
void nvn_pool_mappings_destroy(NvnPoolMappings *state);
/* An address of zero unmaps. Requests apply in order as one transaction;
 * allocation/validation failure leaves every prior interval unchanged. */
bool nvn_pool_mappings_apply(NvnPoolMappings *state, uint64_t pool_size,
    const NvnPoolMappingRequest *requests, size_t count);
/* Complete span must be mapped to contiguous CPU bytes. Never returns a
 * partial prefix or a virtual offset masquerading as a CPU pointer. */
bool nvn_pool_mappings_resolve(NvnPoolMappings *state, uint64_t pool_size,
    uint64_t offset, uint64_t size, uint64_t *address, uint64_t *generation);
