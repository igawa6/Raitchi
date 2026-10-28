/* Real production queue/mode/wrapper, synthetic compiler work; no GPU/device. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn/shader_worker_policy.h"
#ifndef FATAL
#define FATAL(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#endif
#include "worker_queue_actual.inc"
static _Atomic unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define MAX_TASKS 32
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cond=PTHREAD_COND_INITIALIZER;
static struct {int order[MAX_TASKS],started,done,active,max_active;bool release[MAX_TASKS],seen[MAX_TASKS];} events;
typedef struct{int id;bool hold;} Task;
static struct timespec deadline(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=5;return t;}
static void await_count(int *counter,int want){struct timespec t=deadline();pthread_mutex_lock(&event_lock);while(*counter<want){int status=pthread_cond_timedwait(&event_cond,&event_lock,&t);CHECK(!status);}pthread_mutex_unlock(&event_lock);}
static void controlled_job(void*ctx){Task*t=ctx;pthread_mutex_lock(&event_lock);CHECK(t->id>=0&&t->id<MAX_TASKS&&!events.seen[t->id]);events.seen[t->id]=true;events.order[events.started++]=t->id;events.active++;if(events.active>events.max_active)events.max_active=events.active;pthread_cond_broadcast(&event_cond);while(t->hold&&!events.release[t->id])pthread_cond_wait(&event_cond,&event_lock);events.active--;events.done++;pthread_cond_broadcast(&event_cond);pthread_mutex_unlock(&event_lock);}
static void release_task(int id){pthread_mutex_lock(&event_lock);events.release[id]=true;pthread_cond_broadcast(&event_cond);pthread_mutex_unlock(&event_lock);}
static void release_all(void){pthread_mutex_lock(&event_lock);for(int i=0;i<MAX_TASKS;i++)events.release[i]=true;pthread_cond_broadcast(&event_cond);pthread_mutex_unlock(&event_lock);}
static void await_background(int count){struct timespec limit=deadline();for(;;){pthread_mutex_lock(&g_jobs.lock);int now=g_jobs.bg_running;pthread_mutex_unlock(&g_jobs.lock);if(now==count)return;struct timespec t;clock_gettime(CLOCK_REALTIME,&t);CHECK(t.tv_sec<limit.tv_sec||(t.tv_sec==limit.tv_sec&&t.tv_nsec<limit.tv_nsec));struct timespec nap={0,1000000};nanosleep(&nap,NULL);}}
static void *test_worker(void*arg){(void)arg;shader_job_queue_run(&g_jobs);return NULL;}
static void test_noop_start(void){}
static bool sync_shaders(void){return false;}
static void start_workers(void){CHECK(false);}
#include "worker_run_job_actual.inc"
static void stop_worker(void*owned_job){free(owned_job);pthread_exit(NULL);}
static void pool_start(pthread_t*threads,int n){pthread_mutex_lock(&event_lock);memset(&events,0,sizeof events);pthread_mutex_unlock(&event_lock);pthread_mutex_lock(&g_jobs.lock);CHECK(!g_jobs.head&&!g_jobs.bhead&&!g_jobs.bg_running);shader_workers=n;pthread_mutex_unlock(&g_jobs.lock);vk_shader_preparation_mode(true);for(int i=0;i<n;i++)CHECK(!pthread_create(&threads[i],NULL,test_worker,NULL));}
static void pool_finish(pthread_t*threads,int n){
 /* Test-only teardown for production's immortal workers. A foreground sentinel
  * owns/free its queue node and exits instead of returning to queue_run. */
 pthread_mutex_lock(&g_jobs.lock);CHECK(!g_jobs.head&&!g_jobs.bhead);
 for(int i=0;i<n;i++){Job*j=malloc(sizeof*j);CHECK(j);*j=(Job){stop_worker,j,NULL};if(g_jobs.tail)g_jobs.tail->next=j;else g_jobs.head=j;g_jobs.tail=j;}pthread_cond_broadcast(&g_jobs.cond);pthread_mutex_unlock(&g_jobs.lock);
 for(int i=0;i<n;i++)CHECK(!pthread_join(threads[i],NULL));pthread_mutex_lock(&g_jobs.lock);CHECK(!g_jobs.head&&!g_jobs.bhead&&!g_jobs.bg_running);pthread_mutex_unlock(&g_jobs.lock);
}
static void loading_to_gameplay(void){
 pthread_t workers[7];Task jobs[16];for(int i=0;i<16;i++)jobs[i]=(Task){i,true};jobs[13].hold=false;jobs[14].hold=false;pool_start(workers,7);
 for(int i=0;i<7;i++)queue_push(&g_jobs,controlled_job,&jobs[i],true);await_count(&events.started,7);await_background(7);CHECK(events.max_active==7);
 for(int i=7;i<13;i++)queue_push(&g_jobs,controlled_job,&jobs[i],true);
 vk_shader_preparation_mode(false);pthread_mutex_lock(&g_jobs.lock);CHECK(!shader_preparing&&g_jobs.bg_max==2&&g_jobs.bg_running==7);pthread_mutex_unlock(&g_jobs.lock);
 for(int i=0;i<4;i++)release_task(i);await_count(&events.done,4);await_background(3);
 /* Exercise an idle worker while background work is blocked above the cap. */
 queue_push(&g_jobs,controlled_job,&jobs[13],false);await_count(&events.done,5);pthread_mutex_lock(&event_lock);CHECK(events.started==8&&events.seen[13]&&!events.seen[7]);pthread_mutex_unlock(&event_lock);
 release_task(4);await_count(&events.done,6);await_background(2);queue_push(&g_jobs,controlled_job,&jobs[14],false);await_count(&events.done,7);pthread_mutex_lock(&event_lock);CHECK(events.started==9&&events.seen[14]&&!events.seen[7]);pthread_mutex_unlock(&event_lock);
 release_task(5);await_count(&events.started,10);await_background(2);pthread_mutex_lock(&event_lock);CHECK(events.seen[7]&&!events.seen[8]);pthread_mutex_unlock(&event_lock);
 release_task(7);await_count(&events.started,11);await_background(2);pthread_mutex_lock(&event_lock);CHECK(events.seen[8]&&!events.seen[9]);pthread_mutex_unlock(&event_lock);
 release_all();await_count(&events.done,15);await_background(0);pool_finish(workers,7);
}
static void single_worker_priority(void){
 pthread_t worker;Task jobs[]={{0,true},{1,true},{2,true}};pool_start(&worker,1);queue_push(&g_jobs,controlled_job,&jobs[0],true);await_count(&events.started,1);
 queue_push(&g_jobs,controlled_job,&jobs[1],true);queue_push(&g_jobs,controlled_job,&jobs[2],false);
 pthread_mutex_lock(&g_jobs.lock);CHECK(g_jobs.bhead&&g_jobs.head&&g_jobs.bhead->arg==&jobs[1]&&g_jobs.head->arg==&jobs[2]);pthread_mutex_unlock(&g_jobs.lock);
 vk_shader_preparation_mode(false);CHECK(shader_worker_count(1,1,true,NULL)==1);release_task(0);await_count(&events.started,2);pthread_mutex_lock(&event_lock);CHECK(events.order[1]==2&&!events.seen[1]);pthread_mutex_unlock(&event_lock);
 release_task(2);await_count(&events.started,3);pthread_mutex_lock(&event_lock);CHECK(events.order[2]==1);pthread_mutex_unlock(&event_lock);release_task(1);await_count(&events.done,3);await_background(0);pool_finish(&worker,1);
}
/* Production translate_program wrapper below. Only GPU translation/compilation
 * and optional debug-file writers are stubs; real run_job_q/class queues run. */
typedef struct{const char*msl;} TestShader;
typedef struct{TestShader*vi,*fi;uint64_t code[2];uint32_t cmax[2];bool headstart,translation_ok;Task task;} Prog;
static int save_calls;
static bool translate_program_now(Prog*p){return p->translation_ok;}
static void save_glsl(const char*s,uint64_t code,const char*stage){CHECK(s&&stage&&code);save_calls++;}
static void save_blob(const void*p,uint32_t size,uint64_t code,const char*stage){CHECK(p&&size&&code&&stage);save_calls++;}
static const void*prog_code(Prog*p,int stage){return &p->code[stage];}
static void compile_program(void*arg){Prog*p=arg;controlled_job(&p->task);}
#include "worker_translate_actual.inc"
static void program_priority_routing(void){
 pthread_t worker;Task blocker={0,true};TestShader vs={"syntheticvs"},fs={"syntheticfs"};Prog bg={&vs,&fs,{1,2},{8,8},true,true,{1,true}},fg={&vs,&fs,{1,2},{8,8},false,true,{2,true}},bad={&vs,&fs,{1,2},{8,8},true,false,{3,true}};
 pool_start(&worker,1);queue_push(&g_jobs,controlled_job,&blocker,true);await_count(&events.started,1);
 save_calls=0;translate_program(&bg);translate_program(&fg);translate_program(&bad);CHECK(save_calls==8);
 pthread_mutex_lock(&g_jobs.lock);CHECK(g_jobs.bhead&&g_jobs.head&&g_jobs.bhead->fn==compile_program&&g_jobs.head->fn==compile_program);CHECK(g_jobs.bhead->arg==&bg&&g_jobs.head->arg==&fg&&!g_jobs.bhead->next&&!g_jobs.head->next);pthread_mutex_unlock(&g_jobs.lock);
 release_task(0);await_count(&events.started,2);pthread_mutex_lock(&event_lock);CHECK(events.order[1]==2&&!events.seen[1]&&!events.seen[3]);pthread_mutex_unlock(&event_lock);
 release_task(2);await_count(&events.started,3);pthread_mutex_lock(&event_lock);CHECK(events.order[2]==1);pthread_mutex_unlock(&event_lock);release_task(1);await_count(&events.done,3);await_background(0);pool_finish(&worker,1);
}
int main(void){CHECK(g_tjobs.bg_max==1);CHECK(shader_worker_count(8,8,true,NULL)==7);CHECK(!pthread_once(&g_jobs.once,test_noop_start));loading_to_gameplay();single_worker_priority();program_priority_routing();printf("{\"state\":\"passed\",\"assertions\":%u,\"loading_background_peak\":7,\"gameplay_new_background_cap\":2,\"single_worker_liveness\":true,\"real_translate_wrapper_priority\":true,\"scenarios\":3}\n",checks);return 0;}
