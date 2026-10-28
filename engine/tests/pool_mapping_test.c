#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "nvn/nvn.h"
#include "nvn/command_recording.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); abort(); } } while (0)
static unsigned retired;
void rq_call(void (*fn)(void *),void *arg) { CHECK(arg); ++retired; fn(arg); }
/* The extracted production wrapper is tested with the command-lifetime mode
 * explicitly OFF. This controllable OFF seam simulates registry rejection so
 * initialization rollback and no-destroy-before-registration remain covered. */
static bool registration_allowed = true;
static NvnPool *registered_pools[8];
static unsigned registered_count;
static unsigned pool_create_calls, pool_destroy_calls;
bool nvn_command_lifetime_enabled(void) { return false; }
bool nvn_command_pool_created(NvnPool *pool) {
    ++pool_create_calls;
    if (!registration_allowed) return false;
    CHECK(pool && registered_count < sizeof registered_pools / sizeof registered_pools[0]);
    for (unsigned i=0;i<registered_count;i++) CHECK(registered_pools[i]!=pool);
    registered_pools[registered_count++]=pool;
    return true;
}
void nvn_command_pool_destroyed(NvnPool *pool) {
    ++pool_destroy_calls;
    CHECK(pool);
    unsigned i=0;while(i<registered_count&&registered_pools[i]!=pool)++i;
    CHECK(i<registered_count);
    registered_pools[i]=registered_pools[--registered_count];registered_pools[registered_count]=NULL;
}
#include "pool_mapping_production.inc"
static uint8_t memory[2048];
static void api(void) {
    uint64_t virtual_guest=0, physical_guest=0; uint8_t builder[40]={0}; Cpu cpu={0};
    uint64_t size=256;memcpy(builder+24,&size,8);cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=(uintptr_t)builder;hle_nvnMemoryPoolInitialize(&cpu);CHECK(cpu.x[0]==1&&virtual_guest);
    uint64_t storage=(uintptr_t)memory;memcpy(builder+16,&storage,8);size=sizeof memory;memcpy(builder+24,&size,8);cpu.x[0]=(uintptr_t)&physical_guest;cpu.x[1]=(uintptr_t)builder;hle_nvnMemoryPoolInitialize(&cpu);CHECK(cpu.x[0]==1);
    uint64_t raw[10]={(uintptr_t)&physical_guest,100,20,80,0,(uintptr_t)&physical_guest,180,100,40,0};
    cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=2;cpu.x[2]=(uintptr_t)raw;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(cpu.x[0]==1);
    NvnPool *p=(void*)(uintptr_t)virtual_guest;uint64_t address,generation;CHECK(nvn_pool_resolve_span(p,20,120,&address,&generation)&&address==(uintptr_t)memory+100&&generation>1);
    CHECK(!nvn_pool_resolve_span(p,19,1,&address,&generation)&&!address&&!generation);
    raw[0]=0;raw[2]=40;raw[3]=10;raw[5]=(uintptr_t)&physical_guest;raw[6]=UINT64_MAX;cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=2;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(cpu.x[0]==0);CHECK(nvn_pool_resolve_span(p,20,120,&address,&generation)); // failed batch did not unmap its first request
    cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=1;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(cpu.x[0]==1);CHECK(!nvn_pool_resolve_span(p,20,120,&address,&generation));CHECK(nvn_pool_resolve_span(p,50,90,&address,&generation)&&address==(uintptr_t)memory+130);
    cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=UINT32_MAX;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(!cpu.x[0]);cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=4097;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(!cpu.x[0]);cpu.x[0]=(uintptr_t)&virtual_guest;cpu.x[1]=1;cpu.x[2]=UINT64_MAX-10;hle_nvnMemoryPoolMapVirtual(&cpu);CHECK(!cpu.x[0]);
    cpu.x[0]=(uintptr_t)&virtual_guest;hle_nvnMemoryPoolFinalize(&cpu);CHECK(!virtual_guest&&retired==1);cpu.x[0]=(uintptr_t)&physical_guest;hle_nvnMemoryPoolFinalize(&cpu);CHECK(!physical_guest&&retired==2);
    CHECK(!registered_count&&pool_create_calls==2&&pool_destroy_calls==2);
    // Virtual pools allocate mapping metadata before registry admission. A
    // rejected registration must roll it back and must not destroy a pool
    // that was never registered. Sanitized mode runs with leak detection.
    uint64_t rejected_guest=0,virtual_storage=0,virtual_size=256;memcpy(builder+16,&virtual_storage,8);memcpy(builder+24,&virtual_size,8);
    registration_allowed=false;unsigned destroyed_before=pool_destroy_calls;
    cpu=(Cpu){0};cpu.x[0]=(uintptr_t)&rejected_guest;cpu.x[1]=(uintptr_t)builder;
    hle_nvnMemoryPoolInitialize(&cpu);
    CHECK(cpu.x[0]==0&&!rejected_guest&&pool_create_calls==3&&pool_destroy_calls==destroyed_before&&!registered_count);
    registration_allowed=true;
}
static uint32_t rng=7;static uint32_t next(void){rng=rng*1664525+1013904223;return rng;}
static void random_maps(void) {
    NvnPoolMappings *s=nvn_pool_mappings_create();CHECK(s);uint64_t expected[256]={0},generation=0;
    for(unsigned turn=0;turn<3000;++turn){NvnPoolMappingRequest r[3];size_t count=1+next()%3;for(size_t j=0;j<count;++j){uint64_t offset=next()%256,size=1+next()%(256-offset),address=next()%4?(uintptr_t)memory+next()%512:0;r[j]=(NvnPoolMappingRequest){offset,size,address};for(uint64_t k=0;k<size;++k)expected[offset+k]=address?address+k:0;}CHECK(nvn_pool_mappings_apply(s,256,r,count));
      for(unsigned j=0;j<20;++j){uint64_t offset=next()%256,size=1+next()%(256-offset),address=99,g=99;bool valid=expected[offset]!=0;for(uint64_t k=1;k<size;++k)valid&=expected[offset+k]==expected[offset]+k;bool ok=nvn_pool_mappings_resolve(s,256,offset,size,&address,&g);CHECK(ok==valid);CHECK(ok?(address==expected[offset]&&g>generation):(!address&&!g));if(ok)generation=g-1;}
    }
    NvnPoolMappingRequest invalid[2]={{0,16,(uintptr_t)memory},{250,7,(uintptr_t)memory}};CHECK(!nvn_pool_mappings_apply(s,256,invalid,2));for(unsigned k=0;k<256;++k){uint64_t a,g;bool ok=nvn_pool_mappings_resolve(s,256,k,1,&a,&g);CHECK(ok==(expected[k]!=0));if(ok)CHECK(a==expected[k]);}
    invalid[0]=(NvnPoolMappingRequest){0,2,UINT64_MAX};CHECK(!nvn_pool_mappings_apply(s,256,invalid,1));CHECK(!nvn_pool_mappings_apply(s,256,invalid,4097));nvn_pool_mappings_destroy(s);
}
static NvnPoolMappings *concurrent;static _Atomic bool stop;
static void *reader(void *unused){(void)unused;while(!atomic_load(&stop)){uint64_t a,g;if(nvn_pool_mappings_resolve(concurrent,256,0,256,&a,&g))if((a!=(uintptr_t)memory&&a!=(uintptr_t)memory+512)||!g)abort();}return NULL;}
static void concurrency(void){concurrent=nvn_pool_mappings_create();CHECK(concurrent);pthread_t th;CHECK(!pthread_create(&th,NULL,reader,NULL));for(unsigned i=0;i<2000;++i){NvnPoolMappingRequest r={0,256,(uintptr_t)memory+(i&1)*512};CHECK(nvn_pool_mappings_apply(concurrent,256,&r,1));}atomic_store(&stop,true);CHECK(!pthread_join(th,NULL));nvn_pool_mappings_destroy(concurrent);}
int main(void){CHECK(!nvn_command_lifetime_enabled());api();random_maps();concurrency();printf("state=passed checks=%u; command_lifetime=off; actual NVN pool wrappers with simulated registration rejection, transactional interval model and concurrent readers\n",checks);}
