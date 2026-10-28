#pragma once
#include <stddef.h>
#include <stdint.h>
enum PhysicalResult { PHYS_OK=0,PHYS_ADDRESS=0xcc01,PHYS_SIZE=0xca01,PHYS_REGION=0xdc01,PHYS_STATE=0xfa01,PHYS_RESOURCE=0xd001 };
typedef int (*PhysicalConflict)(uint64_t address,uint64_t size,void *context);
/* A recoverable false result must leave bytes and permissions unchanged. The
 * production owned-reservation callback terminates on unproven syscall failure. */
typedef int (*PhysicalBacking)(uint64_t address,uint64_t size,int mapped,void *context);
/* Caller serializes process configuration, mappings and queries. Metadata is
 * charged to system resources; physical pages use the separate process budget. */
int physical_memory_configure(uint32_t system_bytes,uint64_t alias,uint64_t alias_size);
void physical_memory_reset(void);
uint64_t physical_memory_system_total(void);
uint64_t physical_memory_system_used(void);
uint64_t physical_memory_mapped(void);
int physical_memory_change(uint64_t address,uint64_t size,int map,uint64_t physical_budget,
    PhysicalConflict conflict,PhysicalBacking backing,void *context);
/* Returns alias run containing address (including free runs); zero outside. */
int physical_memory_query(uint64_t address,uint64_t *base,uint64_t *size,int *mapped);
