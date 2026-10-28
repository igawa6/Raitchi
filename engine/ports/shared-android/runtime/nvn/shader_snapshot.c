#include "shader_snapshot.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define BUCKETS 256u
struct NvnShaderSnapshot { struct NvnShaderSnapshot *next; uint64_t hash,instructions,constants; uint8_t data[]; };
struct NvnShaderSnapshotStore { pthread_mutex_t lock; uint64_t budget,limit,used; NvnShaderSnapshot *buckets[BUCKETS]; };
NvnShaderSnapshotStore *nvn_shader_snapshots_create(uint64_t budget,uint64_t limit) {
    if (!budget || !limit || limit > SIZE_MAX-sizeof(NvnShaderSnapshot)) return NULL;
    NvnShaderSnapshotStore *s = calloc(1,sizeof *s);
    if (!s) return NULL;
    if (pthread_mutex_init(&s->lock,NULL)) { free(s); return NULL; }
    s->budget=budget;s->limit=limit;return s;
}
void nvn_shader_snapshots_destroy(NvnShaderSnapshotStore *s) {
    if (!s) return;
    for (unsigned i=0;i<BUCKETS;++i) { NvnShaderSnapshot *n=s->buckets[i];while(n){NvnShaderSnapshot *next=n->next;free(n);n=next;} }
    pthread_mutex_destroy(&s->lock);free(s);
}
static uint64_t content_hash(const uint8_t *data,uint64_t size) {
    uint64_t h=UINT64_C(1469598103934665603);
    for(uint64_t i=0;i<size;++i){h^=data[i];h*=UINT64_C(1099511628211);}return h;
}
const NvnShaderSnapshot *nvn_shader_snapshot_capture(NvnShaderSnapshotStore *s,
    uint64_t source,uint64_t instructions,uint64_t constants,NvnShaderSnapshotCopy copy,
    enum NvnShaderSnapshotResult *result) {
    enum NvnShaderSnapshotResult status=NVN_SNAPSHOT_ARGUMENT;
    const NvnShaderSnapshot *found=NULL;
    if (!s || !source || !instructions || !copy || source>UINTPTR_MAX ||
        instructions>UINTPTR_MAX-source || constants>UINTPTR_MAX-source-instructions) goto finish;
    uint64_t size=instructions+constants;
    if(size>s->limit){status=NVN_SNAPSHOT_LIMIT;goto finish;}
    // Serialize the temporary capture too: at most one stage-sized temporary
    // exists, even when callers supply shaders concurrently.
    pthread_mutex_lock(&s->lock);
    NvnShaderSnapshot *candidate=malloc(sizeof *candidate+(size_t)size);
    if(!candidate){status=NVN_SNAPSHOT_ALLOCATION;goto unlock;}
    if(!copy(candidate->data,source,size)){free(candidate);status=NVN_SNAPSHOT_COPY;goto unlock;}
    uint64_t hash=content_hash(candidate->data,size);unsigned bucket=(unsigned)(hash^(hash>>32))&(BUCKETS-1);
    for(NvnShaderSnapshot *n=s->buckets[bucket];n;n=n->next)
        if(n->hash==hash && n->instructions==instructions && n->constants==constants &&
            !memcmp(n->data,candidate->data,(size_t)size)){found=n;free(candidate);status=NVN_SNAPSHOT_OK;goto unlock;}
    uint64_t charged=sizeof *candidate+size;
    if(charged>s->budget-s->used){free(candidate);status=NVN_SNAPSHOT_LIMIT;goto unlock;}
    candidate->hash=hash;candidate->instructions=instructions;candidate->constants=constants;
    candidate->next=s->buckets[bucket];s->buckets[bucket]=candidate;s->used+=charged;found=candidate;status=NVN_SNAPSHOT_OK;
unlock:
    pthread_mutex_unlock(&s->lock);
finish:
    if(result)*result=status;return found;
}
const uint8_t *nvn_shader_snapshot_data(const NvnShaderSnapshot *snapshot){return snapshot?snapshot->data:NULL;}
uint64_t nvn_shader_snapshots_used(NvnShaderSnapshotStore *s){if(!s)return 0;pthread_mutex_lock(&s->lock);uint64_t used=s->used;pthread_mutex_unlock(&s->lock);return used;}
