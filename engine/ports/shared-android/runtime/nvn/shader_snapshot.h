/* Immutable, bounded stage-byte storage shared across shader-key variants. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct NvnShaderSnapshotStore NvnShaderSnapshotStore;
typedef struct NvnShaderSnapshot NvnShaderSnapshot;
typedef bool (*NvnShaderSnapshotCopy)(void *destination,uint64_t source,uint64_t size);
enum NvnShaderSnapshotResult { NVN_SNAPSHOT_OK, NVN_SNAPSHOT_ARGUMENT,
    NVN_SNAPSHOT_LIMIT, NVN_SNAPSHOT_COPY, NVN_SNAPSHOT_ALLOCATION };
NvnShaderSnapshotStore *nvn_shader_snapshots_create(uint64_t budget,uint64_t stage_limit);
/* Destroy only after every consumer has finished. Entries otherwise remain
 * immutable and stable, including after later captures hit the resource cap. */
void nvn_shader_snapshots_destroy(NvnShaderSnapshotStore *store);
const NvnShaderSnapshot *nvn_shader_snapshot_capture(NvnShaderSnapshotStore *store,
    uint64_t source,uint64_t instructions,uint64_t constants,NvnShaderSnapshotCopy copy,
    enum NvnShaderSnapshotResult *result);
const uint8_t *nvn_shader_snapshot_data(const NvnShaderSnapshot *snapshot);
uint64_t nvn_shader_snapshots_used(NvnShaderSnapshotStore *store);
