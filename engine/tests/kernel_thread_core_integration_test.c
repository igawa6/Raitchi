#define _GNU_SOURCE
#include "kernel_thread_affinity.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
enum { KOBJ_THREAD = 1, KOBJ_EVENT };
typedef struct { int type; } KObject;
typedef struct { uint64_t x[31],sp,excl_addr,tpidrro;void *thread; } Cpu;
typedef struct KThread { KObject obj;Cpu cpu;pthread_cond_t cond;KernelThreadAffinity affinity;uint64_t entry,id,tlr;int prio,core;struct KThread *anext; } KThread;
static pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;
static KThread *g_threads;
static uint32_t process_default_core;
static uint64_t next_thread_id=0x50,tls_next=0x100000;
#define PROCESS_CORE_MASK KERNEL_GUEST_CORE_MASK
#define PSEUDO_THREAD 0xffff8000u
#undef R_OK
#define R_OK 0u
#define R_INVALID_HANDLE 0xe401u
#define FATAL(...) abort()
static KObject *objects[4];
static KObject *handle_get(uint32_t h) {if(h==PSEUDO_THREAD)return &t_cur->obj;return h<4?objects[h]:NULL;}
static void aot_cpu_initialize(Cpu *c) {memset(c,0,sizeof *c);}
#include "kernel_thread_core_production.h"
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static void *worker(void *arg) {
 KThread *self=arg;t_cur=self;
 for(unsigned i=0;i<1000;i++) {
  uint32_t r=thread_core_set(PSEUDO_THREAD,(int32_t)(i&3),UINT64_C(1)<<(i&3));if(r)abort();
  int32_t ideal;uint64_t mask;if(thread_core_get(PSEUDO_THREAD,&ideal,&mask)||ideal!=(int32_t)(i&3)||mask!=(UINT64_C(1)<<(i&3))||thread_current_core()!=(i&3))abort();
 }
 return NULL;
}
int main(void) {
 CHECK(kernel_configure_thread_core(4)==-1&&process_default_core==0);
 CHECK(!kernel_configure_thread_core(2));uint32_t result;
 KThread *main=thread_new(1,2,0x1001,44,-2,&result);CHECK(main&&result==0);CHECK(main->affinity.ideal==2&&main->affinity.mask==4&&main->core==2&&main->cpu.sp==0x1000);
 t_cur=main;objects[1]=&main->obj;KObject event={KOBJ_EVENT};objects[2]=&event;
 CHECK(kernel_configure_thread_core(1)==-1&&process_default_core==2);
 int32_t ideal;uint64_t mask;CHECK(!thread_core_get(1,&ideal,&mask)&&ideal==2&&mask==4);CHECK(thread_current_core()==2);
 CHECK(thread_core_get(2,&ideal,&mask)==R_INVALID_HANDLE);CHECK(thread_core_set(2,0,1)==R_INVALID_HANDLE);
 CHECK(thread_core_set(99,0,16)==KERNEL_AFFINITY_INVALID_CORE);CHECK(thread_core_set(99,0,0)==KERNEL_AFFINITY_INVALID_COMBINATION);
 CHECK(!thread_core_set(PSEUDO_THREAD,3,8));CHECK(thread_current_core()==3&&main->core==3);CHECK(!thread_core_get(PSEUDO_THREAD,&ideal,&mask)&&ideal==3&&mask==8);
 CHECK(!thread_core_set(1,1,0xf));CHECK(thread_current_core()==1&&main->core==1);CHECK(!thread_core_get(1,&ideal,&mask)&&ideal==1&&mask==0xf);CHECK(!thread_core_set(1,3,8));
 CHECK(thread_core_set(1,-3,1)==KERNEL_AFFINITY_INVALID_COMBINATION);CHECK(thread_current_core()==3);
 CHECK(!thread_core_set(1,-2,0)&&thread_current_core()==2);CHECK(!thread_core_get(1,&ideal,&mask)&&ideal==2&&mask==4);
 uint64_t id=next_thread_id,tls=tls_next;KThread *head=g_threads;
 for(int i=-3;i<=4;i++)if(i==-3||i==-1||i==4){CHECK(!thread_new(1,2,0x2000,1,i,&result)&&result==KERNEL_AFFINITY_INVALID_CORE);CHECK(next_thread_id==id&&tls_next==tls&&g_threads==head);}
 KThread *other=thread_new(1,2,0x2000,1,0,&result);CHECK(other&&result==0);objects[3]=&other->obj;
 pthread_t a,b;CHECK(!pthread_create(&a,NULL,worker,main));CHECK(!pthread_create(&b,NULL,worker,other));CHECK(!pthread_join(a,NULL));CHECK(!pthread_join(b,NULL));
 t_cur=NULL;CHECK(thread_core_get(PSEUDO_THREAD,&ideal,&mask)==R_INVALID_HANDLE);CHECK(thread_core_set(PSEUDO_THREAD,0,1)==R_INVALID_HANDLE);
 while(g_threads){KThread *next=g_threads->anext;pthread_cond_destroy(&g_threads->cond);free(g_threads);g_threads=next;}
 printf("{\"passed\":true,\"checks\":%u,\"concurrent_self_updates\":2000}\n",checks);return 0;
}
