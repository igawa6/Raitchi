/* Asset-free controlled native userspace/kernel condition-variable handoff.
 * Actual production kernel bodies are inserted by the checker; no guest code. */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <sched.h>
#undef R_OK
#define R_OK 0u
#define R_TIMEOUT 0xEA01u
#define R_CANCELLED 0xEC01u
#define WAITERS_BIT 0x40000000u
enum {WAIT_NONE,WAIT_MUTEX,WAIT_CONDVAR};
typedef struct KThread {uint32_t handle;int prio;pthread_cond_t cond;struct KThread *wnext;int wait_kind;uint64_t wait_addr,wait_mutex,wait_since;uint32_t wait_tag,wait_result;} KThread;
static _Thread_local KThread *t_cur;
static KThread *g_waitlist;
static pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_nothread_cond=PTHREAD_COND_INITIALIZER;
_Thread_local uint64_t g_blocked_ns;
static int g_rt_unwind;
static uint32_t *u32p(uint64_t a){return (uint32_t *)(uintptr_t)a;}
static uint64_t plat_now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void wake(KThread *t){pthread_cond_signal(&t->cond);}
static uint32_t rdw(uint64_t a){return __atomic_load_n(u32p(a),__ATOMIC_SEQ_CST);}
static bool kt_timed_key(uint64_t key,bool add){(void)key;(void)add;return false;}
enum {KT_WAIT,KT_WAIT_END,KT_SIGNAL};
static void kt(uint32_t kind,uint64_t a,uint64_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)kind;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
static void mutex_release(uint64_t);

#include "cv_publication_fragments.h"

static _Atomic int released,observed,predicate;
static uint32_t mutex_word,key_word;
static int seen_key;
static void mutex_release(uint64_t address){
 release_body(address);
 atomic_store_explicit(&released,1,memory_order_release);
 // Controlled preemption after the actual guest-visible unlock, while the
 // consumer still owns g_klock. Producer uses the real userspace CAS path.
 while(!atomic_load_explicit(&observed,memory_order_acquire))sched_yield();
}
static void *producer(void *unused){
 (void)unused;while(!atomic_load_explicit(&released,memory_order_acquire))sched_yield();
 uint32_t expected=0;
 while(!__atomic_compare_exchange_n(&mutex_word,&expected,0x102,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){expected=0;sched_yield();}
 atomic_store(&predicate,1);seen_key=(int)__atomic_load_n(&key_word,__ATOMIC_SEQ_CST);
 atomic_store_explicit(&observed,1,memory_order_release);
 if(seen_key)svc_signal_process_wide_key((uintptr_t)&key_word,1);
 // The signaled waiter may now be queued for this guest mutex. Use the actual
 // handoff routine under the same kernel lock rather than clearing its word.
 pthread_mutex_lock(&g_klock);release_body((uintptr_t)&mutex_word);pthread_mutex_unlock(&g_klock);
 return NULL;
}
int main(void){
 unsigned lost=0,delivered=0;
 for(int fixed=0;fixed<2;fixed++)for(unsigned n=0;n<20;n++){
  atomic_store(&released,0);atomic_store(&observed,0);atomic_store(&predicate,0);seen_key=-1;
  mutex_word=0x101;key_word=0;g_waitlist=NULL;
  KThread waiter={.handle=0x101,.prio=44};pthread_cond_init(&waiter.cond,NULL);t_cur=&waiter;
  pthread_t p;if(pthread_create(&p,NULL,producer,NULL))return 2;
  uint32_t result=(fixed?wait_publish_first:wait_old_mutant)((uintptr_t)&mutex_word,(uintptr_t)&key_word,0x101,fixed?1000000000:10000000);
  pthread_join(p,NULL);pthread_cond_destroy(&waiter.cond);
  if(!atomic_load(&predicate))return 3;
  if(fixed){if(result!=R_OK||seen_key!=1||mutex_word!=0x101)return 4;++delivered;}
  else{if(result!=R_TIMEOUT||seen_key!=0||mutex_word!=0)return 5;++lost;}
 }
 printf("{\"state\":\"passed\",\"original_lost_notifications\":%u,\"publish_first_successful_wakes\":%u,\"production_wait_body_extracted\":true,\"preemption\":\"controlled immediately after actual guest mutex release\"}\n",lost,delivered);return 0;
}
