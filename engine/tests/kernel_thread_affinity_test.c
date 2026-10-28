/* Pure guest state and a per-core work-queue regression; no host affinity calls. */
#include "kernel_thread_affinity.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort(); } } while (0)
static void expected(const KernelThreadAffinity *s,int ideal,int current,uint64_t mask) {
    CHECK(s->ideal==ideal && s->current==current && s->mask==mask);
    CHECK(s->current>=0 && s->current<4 && (s->mask&(UINT64_C(1)<<s->current)));
}
static uint32_t change(KernelThreadAffinity *s,int32_t ideal,uint64_t mask,int32_t def,uint64_t allowed) {
    KernelThreadAffinityRequest request;uint32_t rc=kernel_thread_affinity_prepare(&request,ideal,mask,def,allowed);
    return rc?rc:kernel_thread_affinity_apply(s,&request);
}
static void invalid_unchanged(KernelThreadAffinity *s,int32_t ideal,uint64_t mask,uint32_t error) {
    KernelThreadAffinity before=*s;CHECK(change(s,ideal,mask,0,15)==error);CHECK(!memcmp(s,&before,sizeof before));
}
static void semantics(void) {
    KernelThreadAffinity s={99,99,99};
    for(int def=0;def<4;def++)for(int core=0;core<4;core++) {
        CHECK(!kernel_thread_affinity_create(&s,core,def,15));expected(&s,core,core,1u<<core);
        CHECK(!kernel_thread_affinity_create(&s,-2,def,15));expected(&s,def,def,1u<<def);
    }
    const int invalid[]={-1,-3,-4,4,31,63,INT32_MIN,INT32_MAX};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        KernelThreadAffinity before=s;CHECK(kernel_thread_affinity_create(&s,invalid[i],0,15)==KERNEL_AFFINITY_INVALID_CORE);CHECK(!memcmp(&s,&before,sizeof s));
    }
    CHECK(kernel_thread_affinity_create(&s,0,4,15)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_create(&s,0,0,0)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_create(&s,0,0,0x10)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_create(&s,0,0,2)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(!kernel_thread_affinity_create(&s,2,0,15));
    CHECK(!change(&s,-3,12,0,15));expected(&s,2,2,12);
    invalid_unchanged(&s,-3,8,KERNEL_AFFINITY_INVALID_COMBINATION);
    CHECK(!change(&s,-1,3,0,15));expected(&s,-1,1,3); // old core excluded: deterministic highest permitted
    invalid_unchanged(&s,-3,3,KERNEL_AFFINITY_INVALID_COMBINATION); // no concrete retained ideal
    CHECK(!change(&s,-1,15,0,15));expected(&s,-1,1,15); // still permitted, current is stable
    CHECK(!change(&s,3,15,0,15));expected(&s,3,3,15); // explicit preference chooses a permitted logical processor
    CHECK(!change(&s,-2,0,3,15));expected(&s,3,3,8); // default overrides supplied mask
    invalid_unchanged(&s,0,2,KERNEL_AFFINITY_INVALID_COMBINATION);
    invalid_unchanged(&s,-1,0,KERNEL_AFFINITY_INVALID_COMBINATION);
    invalid_unchanged(&s,-1,16,KERNEL_AFFINITY_INVALID_CORE);
    invalid_unchanged(&s,4,1,KERNEL_AFFINITY_INVALID_CORE);
    invalid_unchanged(&s,-4,1,KERNEL_AFFINITY_INVALID_CORE);
    invalid_unchanged(&s,INT32_MIN,15,KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_create(&s,2,1,3)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(!kernel_thread_affinity_create(&s,-2,1,3));expected(&s,1,1,2);
    CHECK(change(&s,1,6,1,3)==KERNEL_AFFINITY_INVALID_CORE);
    KernelThreadAffinityRequest request={7,7},before=request;
    CHECK(kernel_thread_affinity_prepare(&request,0,0,0,15)==KERNEL_AFFINITY_INVALID_COMBINATION);
    CHECK(!memcmp(&request,&before,sizeof before));
    CHECK(kernel_thread_affinity_create(NULL,0,0,15)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_prepare(NULL,0,1,0,15)==KERNEL_AFFINITY_INVALID_CORE);
    CHECK(kernel_thread_affinity_apply(NULL,&request)==KERNEL_AFFINITY_INVALID_COMBINATION);
    CHECK(kernel_thread_affinity_apply(&s,NULL)==KERNEL_AFFINITY_INVALID_COMBINATION);
}
static void exhaustive(void) {
    // Every four-core mask/request preference transition, checking invariants and
    // error atomicity rather than any Android CPU assignment.
    for(int initial=0;initial<4;initial++)for(uint64_t mask=0;mask<32;mask++)for(int ideal=-4;ideal<5;ideal++) {
        KernelThreadAffinity s;CHECK(!kernel_thread_affinity_create(&s,initial,0,15));KernelThreadAffinity old=s;
        uint32_t result=change(&s,ideal,mask,0,15);
        if(result)CHECK(!memcmp(&s,&old,sizeof s));
        else {
            CHECK(s.mask && !(s.mask&~15u) && s.current>=0 && s.current<4);
            CHECK(s.mask&(UINT64_C(1)<<s.current));
            CHECK(s.ideal==-1 || (s.ideal>=0 && s.ideal<4 && (s.mask&(UINT64_C(1)<<s.ideal))));
            if(ideal==-2)CHECK(s.ideal==0 && s.mask==1 && s.current==0);
            if(ideal==-3)CHECK(s.ideal==initial);
        }
    }
}
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static KernelThreadAffinity workers[4];
static unsigned completed[4],wrong_queue;
static void *worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<1000;i++) {
        pthread_mutex_lock(&state_lock);
        // Engines index their guest work queues by GetCurrentProcessorNumber.
        unsigned core=(unsigned)workers[id].current;
        if(core!=id)wrong_queue++;else completed[core]++;
        pthread_mutex_unlock(&state_lock);
    }
    return NULL;
}
static void queue_regression(void) {
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++) {
        CHECK(!kernel_thread_affinity_create(&workers[i],-2,0,15));expected(&workers[i],0,0,1);
        CHECK(!change(&workers[i],(int)i,15,0,15));expected(&workers[i],(int)i,(int)i,15);
        CHECK(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i));
    }
    for(unsigned i=0;i<4;i++)CHECK(!pthread_join(threads[i],NULL));
    CHECK(!wrong_queue);for(unsigned i=0;i<4;i++)CHECK(completed[i]==1000);
}
int main(void) {
    semantics();exhaustive();queue_regression();printf("state=passed checks=%u; logical affinity sentinels/masks/default and4 independent guest work queues\n",checks);return 0;
}
