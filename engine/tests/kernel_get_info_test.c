// Host fixture compiles actual production helpers and the SVC register-routing case.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define R_OK 0u
#define R_INVALID_ENUM 0xf001u
#define R_INVALID_HANDLE 0xe401u
#define PSEUDO_PROCESS 0xffff8001u
#define PROCESS_CORE_MASK 15u
#define ALIAS_BASE UINT64_C(0x200000000)
#define ALIAS_SIZE UINT64_C(0x100000000)
#define HEAP_BASE UINT64_C(0x300000000)
#define HEAP_MAX UINT64_C(0x80000000)
#define GUEST_LO UINT64_C(0x8000000)
#define GUEST_HI UINT64_C(0x800000000)
#define STACK_BASE UINT64_C(0x700000000)
#define STACK_SIZE UINT64_C(0x100000)
#define LOG(...) ((void)0)
static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t exception_context_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t heap_size=0x3000,user_exception_context;
static unsigned allocations;
static uint64_t physical_memory_system_total(void){return 0x5000;}
static uint64_t physical_memory_system_used(void){return 0x1000;}
static uint64_t physical_memory_mapped(void){return 0x2000;}
static void *hle_alloc(size_t size){if(size!=4096)abort();++allocations;return (void *)(uintptr_t)0x12345000;}
static struct {uint64_t program_id;} adapter={UINT64_C(0x100000000000123)};
#define title_adapter_selected() (&adapter)
static uint32_t controlled_random(void){return 0x12345678;}
#define arc4random controlled_random
#include "kernel_get_info_bodies.h"
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 uint64_t v=99;
 CHECK(process_info(28,PSEUDO_PROCESS,0,&v)==R_OK&&v==0);
 uint64_t sub_ids[]={1,UINT64_MAX,UINT64_C(0x100000000)};
 uint32_t handles[]={0,1,0xffff8000u,UINT32_MAX,0x100};
 for(unsigned i=0;i<sizeof(sub_ids)/sizeof(*sub_ids);++i){v=99;CHECK(process_info(28,PSEUDO_PROCESS,sub_ids[i],&v)==R_INVALID_ENUM&&v==99);CHECK(process_info(28,0,sub_ids[i],&v)==R_INVALID_ENUM&&v==99);}
 for(unsigned i=0;i<sizeof(handles)/sizeof(*handles);++i){v=99;CHECK(process_info(28,handles[i],0,&v)==R_INVALID_HANDLE&&v==99);}
 const struct{uint32_t id;uint64_t expected;} cases[]={
  {0,PROCESS_CORE_MASK},{1,UINT64_MAX},{2,ALIAS_BASE},{3,ALIAS_SIZE},{4,HEAP_BASE},{5,HEAP_MAX},
  {6,UINT64_C(0xcd500000)},{7,UINT64_C(0x400a000)},{8,0},{9,0},
  {11,UINT64_C(0x1234567812345678)},{12,GUEST_LO},{13,GUEST_HI-GUEST_LO},
  {14,STACK_BASE},{15,STACK_SIZE},{16,0x5000},{17,0x1000},{18,UINT64_C(0x100000000000123)},
  {20,0x12345000},{21,UINT64_C(0xcd4fb000)},{22,UINT64_C(0x4005000)}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){v=99;CHECK(process_info(cases[i].id,0,UINT64_MAX,&v)==R_OK&&v==cases[i].expected);}
 CHECK(allocations==1);CHECK(process_info(20,0,0,&v)==R_OK&&v==0x12345000&&allocations==1);
 v=99;CHECK(!physical_memory_info(28,&v)&&v==99);CHECK(process_info(99,0,0,&v)==R_INVALID_ENUM&&v==99);
 uint64_t x[4]={0,28,PSEUDO_PROCESS,0};dispatch_get_info(x);CHECK(x[0]==R_OK&&x[1]==0&&x[2]==PSEUDO_PROCESS&&x[3]==0);
 x[1]=28;x[2]=0;x[3]=0;dispatch_get_info(x);CHECK(x[0]==R_INVALID_HANDLE&&x[1]==0);
 x[1]=28;x[2]=PSEUDO_PROCESS;x[3]=UINT64_C(0x100000000);dispatch_get_info(x);CHECK(x[0]==R_INVALID_ENUM&&x[1]==0);
 puts("GetInfo selectors, failure outputs, lazy allocation and SVC ABI passed");return 0;
}
