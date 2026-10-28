/* Actual readiness helper with worker publication and real sleeping waits. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
static _Atomic unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d:%s\n",__LINE__,#x);abort();}}while(0)
static uint64_t stat_wait_ns;
static __thread uint64_t g_blocked_ns;
static _Atomic unsigned delays;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static uint64_t SDL_GetTicksNS(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return(uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static void SDL_Delay(unsigned ms){CHECK(ms==1);atomic_fetch_add(&delays,1);CHECK(!pthread_mutex_lock(&lock));CHECK(!pthread_cond_signal(&condition));CHECK(!pthread_mutex_unlock(&lock));struct timespec t={0,1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
#include "graphics_ready.production.h"
typedef struct{_Atomic int state;int payload;int result;}Build;
static void*publish(void*p){Build*b=p;CHECK(!pthread_mutex_lock(&lock));while(!atomic_load(&delays))CHECK(!pthread_cond_wait(&condition,&lock));CHECK(!pthread_mutex_unlock(&lock));struct timespec t={0,3000000};while(nanosleep(&t,&t)&&errno==EINTR){}b->payload=0x12340000+b->result;atomic_store_explicit(&b->state,b->result,memory_order_release);return NULL;}
int main(void){
 for(int ready=1;ready<=2;ready++){_Atomic int state=ready;uint64_t waited=stat_wait_ns,blocked=g_blocked_ns;unsigned slept=atomic_load(&delays);CHECK(graphics_wait_ready(&state)==ready);CHECK(stat_wait_ns==waited&&g_blocked_ns==blocked&&atomic_load(&delays)==slept);}
 unsigned transitions=0,total_sleeps=0;
 for(int result=1;result<=2;result++)for(int iteration=0;iteration<16;iteration++){
  Build b={.state=0,.payload=0,.result=result};atomic_store(&delays,0);pthread_t thread;CHECK(!pthread_create(&thread,NULL,publish,&b));uint64_t waited=stat_wait_ns,blocked=g_blocked_ns;CHECK(graphics_wait_ready(&b.state)==result);CHECK(b.payload==0x12340000+result);CHECK(!pthread_join(thread,NULL));unsigned sleeps=atomic_load(&delays);CHECK(sleeps>0);CHECK(stat_wait_ns-waited>=1000000&&g_blocked_ns-blocked==stat_wait_ns-waited);total_sleeps+=sleeps;transitions++;
 }
 CHECK(stat_wait_ns==g_blocked_ns);printf("{\"state\":\"passed\",\"checks\":%u,\"threaded_transitions\":%u,\"sleep_calls\":%u,\"blocked_ns\":%llu,\"ready_and_failed_immediate\":true}\n",checks,transitions,total_sleeps,(unsigned long long)g_blocked_ns);return 0;
}
