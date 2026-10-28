/* Actual production item completion/publication and NVN wrappers, controlled queue/GPU. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "production_fence_types.inc"
typedef struct{uint64_t x[31];}Cpu;
#define NVN(name) static void name(Cpu*c)
#define A(i) (c->x[i])
#define RET(v) (c->x[0]=(uint64_t)(v))
#define HOST(T,g) (*(T**)(uintptr_t)(g))
#define SET_HOST(g,v) (*(void**)(uintptr_t)(g)=(v))
#define LOG(...) ((void)0)
static unsigned checks,submits,waits,mirror_merges,stopped,game_gpu_calls;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
volatile int g_rt_unwind;
static bool is_render,pending,render_on=true;
static uint64_t issued,completed,q_enq,q_done,mappings[32],guest_result,device_result;
static Item queue[32];
static uint64_t now;
static uint64_t g_gpu_wait_ns;
static uint64_t plat_now_ns(void){return ++now;}
static bool rq_on(void){return render_on;}
static uint64_t rq_current_item(void){return is_render?1:0;}
static void rq_watch(const char*w,uint64_t a){(void)w;(void)a;}
static uint64_t renderer_fence_seq(void){CHECK(is_render||!render_on);return issued+(pending?1:0);}
static uint64_t renderer_issued_seq(void){CHECK(is_render);return issued;}
static void renderer_submit_upto(uint64_t seq){
 CHECK(is_render);if(seq>issued){CHECK(pending&&seq==issued+1);issued=seq;pending=false;submits++;}
}
static void renderer_wait_seq(uint64_t seq){
 CHECK(!is_render);waits++;if(seq){CHECK(seq<=issued);if(seq>completed)completed=seq;guest_result=device_result;mirror_merges++;}
}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
void lock_timed(void){CHECK(pthread_mutex_lock(&lock)==0);}
static uint64_t enqueue(const Item*it,const uint64_t*f,int nf){CHECK(!is_render&&!f&&!nf);CHECK(q_enq+1<32);queue[++q_enq]=*it;return q_enq;}
static int prof;
static uint64_t cum_exec;
#define PROF_T0() uint64_t prof_t0_=0
#define PROF_ADD(x) ((void)0)
static uint64_t SDL_GetTicksNS(void){return ++now;}
static void renderer_submit(void){renderer_submit_upto(renderer_fence_seq());}
static void renderer_flush(void);
static void exec_list(uint64_t handle,int depth){CHECK(is_render&&depth==0);if(handle){pending=true;device_result=42;}}
#include "production_backend_execute.inc"
#include "production_item_completion.inc"
#include "production_queue_fence.inc"
static uint64_t rq_wait_item(uint64_t q){
 CHECK(!is_render&&q<=q_enq);if(stopped){g_rt_unwind=1;return 0;}
 is_render=true;
 while(q_done<q){Item*it=&queue[++q_done];if(it->kind==IT_EXEC)backend_execute(it->handle);else if(it->kind==IT_DESTROY){pending=true;device_result=73;} /* controlled alias-preservation recording contract, not full destroy implementation */mappings[q_done]=rq_item_completed_seq(it);}
 is_render=false;return mappings[q];
}
static void renderer_sync(void){renderer_wait_seq(issued);}
static void renderer_flush(void){CHECK(!is_render);game_gpu_calls++;CHECK(false);}
static void rq_flush(void){bool old=is_render;is_render=true;renderer_submit_upto(renderer_fence_seq());is_render=old;renderer_wait_seq(issued);}
static void rq_drain(void){rq_wait_item(q_enq);}
static bool rq_fence_info(uint64_t q,uint64_t*b,bool*u){(void)q;*b=0;*u=false;return false;}
static void rq_fence_times(uint64_t s,uint64_t*t,uint64_t*h){(void)s;*t=*h=0;}
static uint64_t renderer_present_count(void){return 0;}
uint64_t rq_trace_t0;
#include "production_sync.inc"
static void wait_sync(NvnSync**guest){Cpu c={0};c.x[0]=(uintptr_t)guest;c.x[1]=1000000;nvnSyncWait(&c);CHECK(c.x[0]==0);}
static void reset(void){issued=completed=q_enq=q_done=guest_result=device_result=0;submits=waits=mirror_merges=0;pending=is_render=false;g_rt_unwind=0;stopped=0;memset(mappings,0,sizeof mappings);}
int main(int argc,char**argv){
 if(argc==2){
  reset();NvnSync*s=NULL;Item it={.kind=!strcmp(argv[1],"--ordinary-only")?IT_EXEC:IT_DESTROY,.handle=1};uint64_t q=enqueue(&it,NULL,0);
  if(it.kind==IT_EXEC)nvn_sync_set_seq((uintptr_t)&s,q);else{Cpu c={0};c.x[1]=(uintptr_t)&s;nvnQueueFenceSync(&c);}
  wait_sync(&s);CHECK(issued==1&&completed==1&&guest_result==(it.kind==IT_EXEC?42u:73u));free(s);
  printf("{\"state\":\"passed\",\"checks\":%u}\n",checks);return 0;
 }

 reset();NvnSync*s=NULL;
 // QueueFenceSync must enqueue a marker, not sample an item index that can omit pending work.
 Cpu c={0};c.x[1]=(uintptr_t)&s;nvnQueueFenceSync(&c);CHECK(q_enq==1&&queue[1].kind==IT_FENCE&&s->seq==1&&s->signaled);
 wait_sync(&s);CHECK(submits==0&&issued==0&&completed==0);free(s);s=NULL;
 // The actual backend_execute always submits; old and new ordinary item mappings must both complete.
 reset();Item list={.kind=IT_EXEC,.handle=1};uint64_t q=enqueue(&list,NULL,0);nvn_sync_set_seq((uintptr_t)&s,q);wait_sync(&s);
 CHECK(issued==1&&completed==1&&submits==1&&mappings[q]==1&&guest_result==42&&mirror_merges==1);free(s);s=NULL;
 // Model a non-list destruction tail recording a preservation copy without submitting.
 reset();list.kind=IT_DESTROY;enqueue(&list,NULL,0);c=(Cpu){0};c.x[1]=(uintptr_t)&s;nvnQueueFenceSync(&c);CHECK(q_enq==2);
 wait_sync(&s);CHECK(mappings[1]==0&&mappings[2]==1&&issued==1&&completed==1&&guest_result==73);
 // Empty fences do not invent another GPU submission or turn old completion into pending work.
 unsigned old=submits;c=(Cpu){0};c.x[1]=(uintptr_t)&s;nvnQueueFenceSync(&c);wait_sync(&s);CHECK(submits==old&&issued==1&&completed==1);free(s);s=NULL;
 // A later reuse cannot regress when an older list or producer publishes late.
 NvnSync reused={0};s=&reused;nvn_sync_set_seq((uintptr_t)&s,100);nvn_sync_set_seq((uintptr_t)&s,50);CHECK(s->seq==100);
 is_render=true;nvn_sync_set_seq((uintptr_t)&s,7);is_render=false;CHECK(s->seq==100);
 sync_publish(s,200);sync_publish(s,150);CHECK(s->seq==200&&s->signaled);
 // Stop/unwind during translation must fail rather than announce fence success.
 reset();s=NULL;list.kind=IT_EXEC;q=enqueue(&list,NULL,0);nvn_sync_set_seq((uintptr_t)&s,q);stopped=1;c=(Cpu){0};c.x[0]=(uintptr_t)&s;nvnSyncWait(&c);CHECK(c.x[0]==3&&issued==0&&completed==0&&guest_result==0);free(s);
 CHECK(game_gpu_calls==0&&g_gpu_wait_ns>0&&cum_exec==0);
 printf("{\"state\":\"passed\",\"checks\":%u,\"actual_completion_and_wrappers\":true,\"controlled_queue_gpu\":true,\"strict_default\":true,\"finite_timeouts_remain_blocking\":true}\n",checks);
}
