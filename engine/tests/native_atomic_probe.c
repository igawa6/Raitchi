/* Asset-free tests for the actual host-atomic instruction helper. Synthetic
 * Cpu/explicit value monitors only; no Unicorn-private or hardware reservation
 * survives a C call. The source is shared by host and Android diagnostics. */
#define _GNU_SOURCE
#include "native-game-backend/atomic_step.h"
#include "native_atomic_words.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define THREADS 4u
#define INCREMENTS 10000u
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x))failures++;}while(0)
static uint64_t tls_value(void){
#ifdef __aarch64__
uint64_t value;__asm__ volatile("mrs %0,tpidr_el0":"=r"(value));return value;
#else
return (uintptr_t)pthread_self();
#endif
}
static uint64_t x18_value(void){
#ifdef __aarch64__
uint64_t value;__asm__ volatile("mov %0,x18":"=r"(value));return value;
#else
return 0;
#endif
}
static void seed(Cpu *cpu,unsigned salt){
 memset(cpu,0x5a^salt,sizeof *cpu);
 for(unsigned i=0;i<32;i++){cpu->x[i]=UINT64_C(0x13579bdf12340000)^((uint64_t)i<<32)^salt;cpu->v[i]=((__uint128_t)(UINT64_C(0x9988776655443322)^i)<<64)|(UINT64_C(0x1020304050607080)^i);}
 cpu->pc=0x10000;cpu->sp=0x20000;cpu->nzcv=0xa0000000;cpu->fpcr=0x01400000;cpu->fpsr=0x10;cpu->thread=NULL;
}
static uint64_t low_value(const void *memory,unsigned bytes){uint64_t value=0;memcpy(&value,memory,bytes);return value;}
static void sequential_cases(unsigned *exclusive_cases,unsigned *failed_store_cases,unsigned *ordered_count){
 Cpu *cpu=aligned_alloc(16,sizeof *cpu),*expected=aligned_alloc(16,sizeof *expected);CHECK(cpu&&expected);if(!cpu||!expected){free(cpu);free(expected);return;}
 for(unsigned c=0;c<ATOMIC_CASE_COUNT;c++){
  const AtomicCase *test=&atomic_cases[c];_Alignas(16) unsigned char memory[16],wanted[16];
  for(unsigned i=0;i<16;i++)memory[i]=(unsigned char)(0x20+i);memcpy(wanted,memory,16);
  unsigned part=test->bytes/(test->pair?2:1);uint64_t low=low_value(memory,part),high=test->pair?low_value(memory+part,part):0;
  seed(cpu,c);cpu->x[2]=(uintptr_t)memory;memcpy(expected,cpu,sizeof *cpu);NgbAtomicMonitor monitor={0};
  expected->pc=0x10004;expected->x[0]=low;if(test->pair)expected->x[3]=high;
  CHECK(ngb_atomic_step(cpu,test->load,0x10000,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));CHECK(monitor.valid&&monitor.address==(uintptr_t)memory&&monitor.width==test->bytes);
  cpu->x[0]=low+3;cpu->x[3]=high+5;memcpy(expected,cpu,sizeof *cpu);expected->x[1]=0;expected->pc=0x10008;
  memcpy(wanted,&cpu->x[0],part);if(test->pair)memcpy(wanted+part,&cpu->x[3],part);
  CHECK(ngb_atomic_step(cpu,test->store,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));CHECK(!memcmp(memory,wanted,16));CHECK(!monitor.valid);(*exclusive_cases)++;
  /* A changed-value interfering writer must survive a failed exclusive store.
   * This is the deterministic failure Unicorn's nonparallel lowering violated. */
  seed(cpu,c);cpu->x[2]=(uintptr_t)memory;monitor=(NgbAtomicMonitor){0};
  CHECK(ngb_atomic_step(cpu,test->load,0x10000,&monitor)==1);memory[0]^=0x40;memcpy(wanted,memory,16);cpu->x[0]+=7;cpu->x[3]+=9;memcpy(expected,cpu,sizeof *cpu);expected->x[1]=1;expected->pc=0x10008;
  CHECK(ngb_atomic_step(cpu,test->store,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));CHECK(!memcmp(memory,wanted,16));CHECK(!monitor.valid);(*failed_store_cases)++;
  /* Explicit host transition invalidates an otherwise matching reservation. */
  CHECK(ngb_atomic_step(cpu,test->load,0x10000,&monitor)==1);ngb_atomic_monitor_clear(&monitor);memcpy(wanted,memory,16);
  CHECK(ngb_atomic_step(cpu,test->store,0x10004,&monitor)==1);CHECK(cpu->x[1]==1&&!memcmp(memory,wanted,16));
 }
 for(unsigned c=0;c<ORDERED_CASE_COUNT;c++){
  const OrderedCase *test=&ordered_cases[c];_Alignas(16) unsigned char memory[16],wanted[16];memset(memory,0xa5,16);memcpy(wanted,memory,16);
  seed(cpu,c);cpu->x[2]=(uintptr_t)memory;cpu->x[0]=UINT64_C(0x1020304050607080);memcpy(expected,cpu,sizeof *cpu);expected->pc=0x10004;memcpy(wanted,&cpu->x[0],test->bytes);NgbAtomicMonitor monitor={0};
  CHECK(ngb_atomic_step(cpu,test->store,0x10000,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&!memcmp(memory,wanted,16));
  expected->x[0]=low_value(memory,test->bytes);expected->pc=0x10008;
  CHECK(ngb_atomic_step(cpu,test->load,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));(*ordered_count)++;
 }
 _Alignas(16) uint64_t memory[2]={42,99};NgbAtomicMonitor monitor={0};
 seed(cpu,0);cpu->sp=(uintptr_t)memory;memcpy(expected,cpu,sizeof *cpu);expected->x[0]=42;expected->pc=0x10004;
 CHECK(ngb_atomic_step(cpu,ATOMIC_SP_LOAD,0x10000,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));cpu->x[0]=43;memcpy(expected,cpu,sizeof *cpu);expected->x[1]=0;expected->pc=0x10008;
 CHECK(ngb_atomic_step(cpu,ATOMIC_SP_STORE,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&memory[0]==43&&memory[1]==99);
 seed(cpu,1);cpu->x[2]=(uintptr_t)memory;memcpy(expected,cpu,sizeof *cpu);expected->pc=0x10004;
 CHECK(ngb_atomic_step(cpu,ATOMIC_ZR_LOAD,0x10000,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));cpu->x[0]=0;expected->x[0]=0;expected->pc=0x10008;
 CHECK(ngb_atomic_step(cpu,ATOMIC_ZR_STORE,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&memory[0]==0&&memory[1]==99);
 memory[0]=7;seed(cpu,2);cpu->x[2]=(uintptr_t)memory;
 CHECK(ngb_atomic_step(cpu,ATOMIC_64_LOAD,0x10000,&monitor)==1);memcpy(expected,cpu,sizeof *cpu);expected->x[1]=0;expected->pc=0x10008;
 CHECK(ngb_atomic_step(cpu,ATOMIC_ZR_VALUE_STORE,0x10004,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&memory[0]==0&&memory[1]==99);
 seed(cpu,2);cpu->x[2]=(uintptr_t)memory;CHECK(ngb_atomic_step(cpu,ATOMIC_64_LOAD,0x10000,&monitor)==1);memcpy(expected,cpu,sizeof *cpu);expected->pc=0x10008;
 CHECK(ngb_atomic_step(cpu,ATOMIC_CLEAR,0x10004,&monitor)==1);CHECK(!monitor.valid&&!memcmp(cpu,expected,sizeof *cpu));
 memcpy(expected,cpu,sizeof *cpu);NgbAtomicMonitor before=monitor;CHECK(ngb_atomic_step(cpu,ATOMIC_NOP,0x10008,&monitor)==0);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&!memcmp(&monitor,&before,sizeof monitor));
 cpu->x[2]=(uintptr_t)memory+1;memcpy(expected,cpu,sizeof *cpu);before=monitor;CHECK(ngb_atomic_step(cpu,ATOMIC_64_LOAD,0x10008,&monitor)==-1);CHECK(!memcmp(cpu,expected,sizeof *cpu)&&!memcmp(&monitor,&before,sizeof monitor));
 free(cpu);free(expected);
}
typedef struct Worker {
 unsigned index,pair,commits,retries,failed;_Atomic unsigned *ready,*start;void *memory;
} Worker;
static void *contention_worker(void *argument){
 Worker *worker=argument;Cpu *cpu=aligned_alloc(16,sizeof *cpu),*before=aligned_alloc(16,sizeof *before);
 if(!cpu||!before){worker->failed=1;goto ready;}
 seed(cpu,worker->index);cpu->x[2]=(uintptr_t)worker->memory;memcpy(before,cpu,sizeof *cpu);
ready:
 atomic_fetch_add(worker->ready,1);while(!atomic_load(worker->start)){}
 if(!cpu||!before)goto done;
 uint64_t tls=tls_value(),x18=x18_value(),fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();NgbAtomicMonitor monitor={0};
 for(unsigned i=0;i<INCREMENTS;i++){
  unsigned attempts=0;
  do {
   if(++attempts>1000000){worker->failed++;goto checked;}
   uint32_t load=worker->pair?ATOMIC_PAIR_LOAD:ATOMIC_64_LOAD,store=worker->pair?ATOMIC_PAIR_STORE:ATOMIC_64_STORE;
   if(ngb_atomic_step(cpu,load,0x10000,&monitor)!=1){worker->failed++;goto checked;}
   if(worker->pair&&cpu->x[3]!=3*cpu->x[0]){worker->failed++;goto checked;}
   cpu->x[0]++;if(worker->pair)cpu->x[3]+=3;
   if(ngb_atomic_step(cpu,store,0x10004,&monitor)!=1){worker->failed++;goto checked;}
   if(cpu->x[1])worker->retries++;
  }while(cpu->x[1]);
  worker->commits++;
 }
checked:
 before->pc=cpu->pc;before->x[0]=cpu->x[0];before->x[1]=cpu->x[1];if(worker->pair)before->x[3]=cpu->x[3];
 worker->failed+=memcmp(before,cpu,sizeof *cpu)!=0||monitor.valid||tls_value()!=tls||x18_value()!=x18||rt_get_fpcr()!=fpcr||rt_get_fpsr()!=fpsr;
done:
 free(cpu);free(before);return NULL;
}
static unsigned contention(unsigned pair,unsigned *retries){
 _Alignas(16) uint64_t memory[2]={0,0};Worker workers[THREADS]={0};pthread_t threads[THREADS];_Atomic unsigned ready=0,start=0;unsigned created=0,commits=0;
 for(unsigned i=0;i<THREADS;i++){workers[i]=(Worker){.index=i,.pair=pair,.memory=memory,.ready=&ready,.start=&start};if(pthread_create(&threads[i],NULL,contention_worker,&workers[i])){failures++;break;}created++;}
 while(atomic_load(&ready)!=created){}atomic_store(&start,1);
 for(unsigned i=0;i<created;i++){if(pthread_join(threads[i],NULL))abort();commits+=workers[i].commits;*retries+=workers[i].retries;CHECK(!workers[i].failed);}
 CHECK(created==THREADS);CHECK(commits==THREADS*INCREMENTS);CHECK(memory[0]==THREADS*INCREMENTS);CHECK(memory[1]==(pair?3*THREADS*INCREMENTS:0));return commits;
}
#ifdef __aarch64__
#define ATOMIC_AARCH64_HOST_CHECKS "true"
#else
#define ATOMIC_AARCH64_HOST_CHECKS "false"
#endif
__attribute__((visibility("default"))) int native_atomic_probe(char *output,size_t capacity){
 if(!output||capacity<4096)return -1;checks=0;failures=0;unsigned exclusive=0,failed_stores=0,ordered=0,retries64=0,retries128=0;
 uint64_t tls=tls_value(),x18=x18_value(),fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();sequential_cases(&exclusive,&failed_stores,&ordered);unsigned commits64=contention(0,&retries64),commits128=contention(1,&retries128);
 CHECK(tls_value()==tls&&x18_value()==x18&&rt_get_fpcr()==fpcr&&rt_get_fpsr()==fpsr);
 snprintf(output,capacity,"{\"schema\":1,\"state\":\"%s\",\"checks\":%u,\"failures\":%u,\"exclusive_width_order_cases\":%u,\"failed_stxr_no_writeback_cases\":%u,\"ordered_load_store_cases\":%u,\"contention_threads\":%u,\"increments_per_thread\":%u,\"increments64\":%u,\"increments128_pair\":%u,\"retries64\":%u,\"retries128\":%u,\"host_tls_x18_fp_checked\":%s,\"full_cpu_integrity_checked\":true,\"synthetic_cpu_only\":true,\"unicorn_monitor_used\":false}",failures?"failed":"passed",checks,failures,exclusive,failed_stores,ordered,THREADS,INCREMENTS,commits64,commits128,retries64,retries128,ATOMIC_AARCH64_HOST_CHECKS);return failures!=0;
}
#ifdef NATIVE_ATOMIC_HOST_TEST
int main(void){char output[4096];int status=native_atomic_probe(output,sizeof output);puts(output);return status;}
#endif
