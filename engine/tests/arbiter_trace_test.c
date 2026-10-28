#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <errno.h>
typedef struct KThread {
    struct { uint64_t x[31], sp; } cpu;
    uint64_t id, wait_addr, wait_since;
    int prio, wait_kind;
    uint32_t wait_result;
    pthread_cond_t cond;
    struct KThread *wnext;
} KThread;
enum { WAIT_NONE, WAIT_ARBITER };
#undef R_OK
#define R_OK 0u
#define R_TIMEOUT 0xea01u
#define R_CANCELLED 0xec01u
#define R_INVALID_STATE 0xfa01u
#define R_INVALID_ENUM 0xf001u
static pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
static __thread KThread *t_cur;
static __thread uint64_t g_blocked_ns;
static KThread *g_waitlist;
static int g_rt_unwind, ktrace_on = 1;
static uint64_t plat_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint32_t *u32p(uint64_t a) { return (uint32_t *)(uintptr_t)a; }
static unsigned probe_reads;
static long counted_read(long nr,pid_t pid,struct iovec *local,unsigned long nl,struct iovec *remote,unsigned long nrmt,unsigned long flags) {
 ++probe_reads;return syscall(nr,pid,local,nl,remote,nrmt,flags);
}
#define syscall counted_read
#include "arbiter_trace_production.h"
#undef syscall
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static uint32_t word = 1;
static void init(KThread *t,uint64_t id) { memset(t,0,sizeof *t);t->id=id;t->cpu.x[30]=0x100001;t->cpu.x[29]=0x200002;t->cpu.sp=0x300003;pthread_cond_init(&t->cond,NULL); }
static void *waiter(void *arg) {t_cur=arg;return (void *)(uintptr_t)svc_wait_for_address((uintptr_t)&word,2,1,-1);}
static void *signaller(void *arg) {KThread t;init(&t,(uintptr_t)arg);t_cur=&t;for(int i=0;i<2000;i++)svc_signal_to_address((uintptr_t)&word,0,0,1);pthread_cond_destroy(&t.cond);return NULL;}
int main(void) {
 KThread self;init(&self,50);t_cur=&self;setenv("LARECOMP_ARB_TRACE","1",1);unsetenv("LARECOMP_ARB_TRACE_FROM");unsetenv("LARECOMP_ARB_TRACE_TO");arb_trace_reset();
 CHECK(sizeof(ArbTrace)==104);CHECK(!arb_trace_word(1,&word));
 CHECK(svc_wait_for_address((uintptr_t)&word,2,0,-1)==R_INVALID_STATE);
 CHECK(arb_trace_count==2&&arb_trace[1].result==R_INVALID_STATE&&arb_trace[0].event==AT_WAIT_BEGIN);
 CHECK(arb_trace[0].lr==self.cpu.x[30]&&arb_trace[0].fp==self.cpu.x[29]&&arb_trace[0].sp==self.cpu.sp);
 CHECK(svc_wait_for_address((uintptr_t)&word,99,1,-1)==R_INVALID_ENUM);
 CHECK(arb_trace[3].result==R_INVALID_ENUM);
 uint64_t before=arb_trace_count;CHECK(svc_wait_for_address((uintptr_t)&word,2,1,0)==R_TIMEOUT);
 CHECK(arb_trace_count==before&&arb_trace_polls==1);
 __atomic_store_n(&arb_trace_filter,(uintptr_t)&word,__ATOMIC_RELAXED);
 CHECK(svc_wait_for_address((uintptr_t)&word,2,1,0)==R_TIMEOUT);
 CHECK(arb_trace_count==before+2&&arb_trace[before+1].result==R_TIMEOUT);
 CHECK(svc_signal_to_address(1,0,0,0)==R_OK);CHECK(arb_trace_filtered==1);
 __atomic_store_n(&arb_trace_filter,0,__ATOMIC_RELAXED);
 CHECK(svc_signal_to_address(1,0,0,0)==R_OK);CHECK(!arb_trace[arb_trace_count-1].valid_words);
 CHECK(svc_signal_to_address((uintptr_t)&word,1,0,1)==R_INVALID_STATE);
 CHECK(arb_trace[arb_trace_count-1].before==1&&arb_trace[arb_trace_count-1].after==1);
 CHECK(arb_trace[arb_trace_count-1].result==R_INVALID_STATE);
 KThread wt;init(&wt,51);pthread_t worker;CHECK(!pthread_create(&worker,NULL,waiter,&wt));
 bool queued=false;for(int i=0;i<1000&&!queued;i++){pthread_mutex_lock(&g_klock);queued=g_waitlist==&wt;pthread_mutex_unlock(&g_klock);if(!queued)usleep(1000);}CHECK(queued);
 CHECK(svc_signal_to_address((uintptr_t)&word,1,1,1)==R_OK);void *result;CHECK(!pthread_join(worker,&result)&&!result);
 bool saw=false;for(uint64_t i=0;i<arb_trace_count;i++)if(arb_trace[i].event==AT_SIGNAL&&arb_trace[i].matched==1){CHECK(arb_trace[i].woken==1&&arb_trace[i].before==1&&arb_trace[i].after==2);saw=true;}CHECK(saw);
 CHECK(!g_waitlist);pthread_cond_destroy(&wt.cond);
 CHECK(svc_wait_for_address((uintptr_t)&word,2,2,2000000)==R_TIMEOUT);CHECK(arb_trace[arb_trace_count-1].result==R_TIMEOUT);CHECK(g_blocked_ns>0);
 arb_trace_reset();CHECK(!arb_trace_count&&!arb_trace_overflow&&!arb_trace_polls&&!arb_trace_filter);
 pthread_t threads[8];for(uintptr_t i=0;i<8;i++)CHECK(!pthread_create(&threads[i],NULL,signaller,(void*)(i+100)));
 for(int i=0;i<8;i++)CHECK(!pthread_join(threads[i],NULL));CHECK(arb_trace_count==8192&&arb_trace_overflow==16000-8192);
 for(unsigned i=0;i<ARB_TRACE_MAX;i++){CHECK(arb_trace[i].sequence==i+1);CHECK(arb_trace[i].event==AT_SIGNAL);CHECK(arb_trace[i].address==(uintptr_t)&word);}
 uint64_t first=arb_trace[0].thread_id;CHECK(svc_signal_to_address((uintptr_t)&word,0,0,0)==R_OK);CHECK(arb_trace[0].thread_id==first&&arb_trace_overflow==16001-8192);
 arb_trace_reset();ktrace_on=0;unsigned reads=probe_reads;CHECK(svc_signal_to_address((uintptr_t)&word,0,0,0)==R_OK);CHECK(!arb_trace_count&&probe_reads==reads);
 ktrace_on=1;unsetenv("LARECOMP_ARB_TRACE");arb_trace_reset();reads=probe_reads;
 CHECK(svc_signal_to_address((uintptr_t)&word,0,0,0)==R_OK);CHECK(svc_wait_for_address((uintptr_t)&word,2,0,-1)==R_INVALID_STATE);
 CHECK(!arb_trace_count&&!arb_trace_overflow&&!arb_trace_polls&&!arb_trace_filtered&&probe_reads==reads);
 setenv("LARECOMP_ARB_TRACE","1",1);setenv("LARECOMP_ARB_TRACE_FROM","1000",1);setenv("LARECOMP_ARB_TRACE_TO","0x2000",1);arb_trace_reset();
 CHECK(!arb_trace_accept(0xfff,false,0));CHECK(arb_trace_accept(0x1000,false,0));CHECK(arb_trace_accept(0x1fff,false,0));CHECK(!arb_trace_accept(0x2000,false,0));
 CHECK(arb_trace_from==0x1000&&arb_trace_to==0x2000&&!arb_trace_config_error);
 setenv("LARECOMP_ARB_TRACE_FROM","bad!",1);CHECK(arb_trace_accept(0x1001,false,0)); // parsed once
 __atomic_store_n(&arb_trace_filter,0x3000,__ATOMIC_RELAXED);CHECK(arb_trace_accept(0x3000,true,0));CHECK(!arb_trace_accept(0x1000,false,0));
 const char *bad[]={"", "0x", "-1", "+1", " 1000", "1000 ", "xyz", "10000000000000000", "2000", "3000"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++) {setenv("LARECOMP_ARB_TRACE_FROM",bad[i],1);arb_trace_reset();reads=probe_reads;CHECK(svc_signal_to_address((uintptr_t)&word,0,0,0)==R_OK);CHECK(arb_trace_config_error==1&&!arb_trace_enabled&&!arb_trace_count&&probe_reads==reads);}
 unsetenv("LARECOMP_ARB_TRACE_FROM");arb_trace_reset();CHECK(!arb_trace_accept(0x1000,false,0)&&arb_trace_config_error);
 setenv("LARECOMP_ARB_TRACE_FROM","0",1);setenv("LARECOMP_ARB_TRACE_TO","0x0",1);arb_trace_reset();errno=EDOM;CHECK(arb_trace_accept(0x3000,false,0));CHECK(!arb_trace_from&&!arb_trace_to&&!arb_trace_config_error&&errno==EDOM);
 unsetenv("LARECOMP_ARB_TRACE_FROM");unsetenv("LARECOMP_ARB_TRACE_TO");arb_trace_reset();CHECK(!arb_trace_from&&!arb_trace_to&&!arb_trace_filter&&!arb_trace_config_error);
 pthread_cond_destroy(&self.cond);printf("{\"passed\":true,\"checks\":%u,\"record_bytes\":%zu}\n",checks,sizeof(ArbTrace));return 0;
}
