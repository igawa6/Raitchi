#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"line%d: %s\n",__LINE__,#v); abort(); } } while(0)
static _Atomic uint64_t locks, allocations;
static void *counted_calloc(size_t n,size_t size){atomic_fetch_add(&allocations,1);return calloc(n,size);}
static int counted_lock(pthread_mutex_t *mutex){atomic_fetch_add(&locks,1);return pthread_mutex_lock(mutex);}
#define calloc counted_calloc
#define pthread_mutex_lock counted_lock
#include "production_command_pool_stats.c"
#undef pthread_mutex_lock
#undef calloc
static NvnCommandPoolStats snapshot(void){NvnCommandPoolStats out={0};CHECK(nvn_command_pool_stats_snapshot(&out));return out;}
static void *worker(void *argument){
    uintptr_t base=((uintptr_t)argument+1)*UINT64_C(0x1000000);
    for(unsigned i=0;i<1000;++i){
        void *key=(void*)(base+i*16);nvn_command_pool_note_init(key,base+0x10000,4096);
        nvn_command_pool_note_add(key,0,512,512);nvn_command_pool_note_finalize(key);nvn_command_pool_note_destroy(key);
    }
    return NULL;
}
int main(int argc,char**argv){
    CHECK(argc==2);
    if(!strcmp(argv[1],"off")){
        unsetenv("LARECOMP_LOG_COMMAND_POOLS");
        for(unsigned i=0;i<100000;++i){nvn_command_pool_note_init((void*)16,32,4096);nvn_command_pool_note_add((void*)16,0,512,512);nvn_command_pool_note_finalize((void*)16);nvn_command_pool_note_destroy((void*)16);nvn_command_pool_note_handle_finalize(17);}
        NvnCommandPoolStats out,before;memset(&out,0xa5,sizeof out);before=out;
        CHECK(!nvn_command_pool_stats_snapshot(&out)&&!memcmp(&out,&before,sizeof out));CHECK(!atomic_load(&locks)&&!atomic_load(&allocations)&&!pools);
    }else{
        CHECK(!setenv("LARECOMP_LOG_COMMAND_POOLS","1",1));
        void*key=(void*)0x10000;nvn_command_pool_note_init(key,0x120000,4096);NvnCommandPoolStats s=snapshot();CHECK(s.generations==1&&s.active_pools==1&&!s.command_generations);
        uint64_t first_generation=find((uintptr_t)key,false)->generation;
        nvn_command_pool_note_add(key,4000,512,96);s=snapshot();CHECK(s.adds==1&&s.clipped_adds==1&&s.command_generations==1&&s.active_command_bytes==4096);
        nvn_command_pool_note_finalize(key);s=snapshot();CHECK(s.finalizes==1&&s.command_finalizes==1&&!s.destroys&&s.active_command_pools==1);
        nvn_command_pool_note_init(key,0x130000,8192);CHECK(snapshot().generations==1&&snapshot().unknown_events==1);
        nvn_command_pool_note_destroy(key);s=snapshot();CHECK(s.destroys==1&&s.command_destroys==1&&!s.active_pools&&!s.active_command_bytes&&s.finalized_command_bytes==4096);
        nvn_command_pool_note_init(key,0x130000,8192);CHECK(find((uintptr_t)key,false)->generation>first_generation);nvn_command_pool_note_add(key,0,512,512);
        nvn_command_pool_note_handle_finalize(UINT64_C(0xabcdef1234));s=snapshot();CHECK(s.handle_finalizes==1&&s.active_command_pools==1&&s.command_finalizes==1);
        nvn_command_pool_note_finalize(key);nvn_command_pool_note_destroy(key);
        NvnCommandPoolStats before=snapshot();pthread_t threads[4];
        for(uintptr_t i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,worker,(void*)i));
        for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
        s=snapshot();CHECK(s.generations-before.generations==4000&&s.command_generations-before.command_generations==4000&&s.finalizes-before.finalizes==4000&&s.destroys-before.destroys==4000);
        CHECK(!s.active_pools&&!s.active_command_pools&&!s.active_command_bytes&&s.unknown_events==before.unknown_events&&!s.dropped_events);
        before=s;
        for(unsigned i=0;i<POOL_SLOTS;++i){void*p=(void*)(uintptr_t)(UINT64_C(0x50000000)+i*16);nvn_command_pool_note_init(p,0x123000,16);nvn_command_pool_note_add(p,0,16,16);}
        s=snapshot();CHECK(s.active_pools==POOL_SLOTS&&s.active_command_pools==POOL_SLOTS&&s.active_command_bytes==POOL_SLOTS*16);
        nvn_command_pool_note_init((void*)UINT64_C(0x90000000),0,16);CHECK(snapshot().dropped_events==1);
        for(unsigned i=0;i<POOL_SLOTS;++i){void*p=(void*)(uintptr_t)(UINT64_C(0x50000000)+i*16);nvn_command_pool_note_finalize(p);nvn_command_pool_note_destroy(p);}
        s=snapshot();CHECK(s.generations-before.generations==POOL_SLOTS&&s.command_finalizes-before.command_finalizes==POOL_SLOTS&&s.command_destroys-before.command_destroys==POOL_SLOTS&&!s.active_pools&&!s.active_command_bytes);
        CHECK(s.reports<=REPORT_LIMIT&&atomic_load(&allocations)==1&&atomic_load(&locks)>0);
        nvn_command_pool_note_init(key,0,16);stats.active_command_bytes=UINT64_MAX-1;nvn_command_pool_note_add(key,0,16,16);CHECK(snapshot().overflowed&&snapshot().active_command_bytes==UINT64_MAX);
        nvn_command_pool_note_finalize(key);nvn_command_pool_note_destroy(key);
        free(pools);pools=NULL;
    }
    printf("{\"state\":\"passed\",\"checks\":%u,\"mode\":\"%s\",\"locks\":%llu,\"allocations\":%llu}\n",checks,argv[1],(unsigned long long)atomic_load(&locks),(unsigned long long)atomic_load(&allocations));return 0;
}
