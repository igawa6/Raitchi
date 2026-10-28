#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include "nvn/maxwell/shader.h"
#define PROG_TAB 8
#define PIPE_TAB 8
#define LOG(...) fprintf(stderr,__VA_ARGS__)
#define FATAL(...) do{fprintf(stderr,__VA_ARGS__);abort();}while(0)
static _Atomic unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
typedef struct{uint64_t code[2],cmax[2],csize[2],chash;const uint8_t*read_code[2];ShaderKey vkey,fkey;bool headstart;_Atomic int state;}Prog;
typedef struct{Prog*prog;uint64_t flags;}PipeKey;
typedef struct{PipeKey k;_Atomic int state;}Pipe;
static pthread_mutex_t hs_lock=PTHREAD_MUTEX_INITIALIZER;
static struct{uint64_t key;Prog*prog;bool taken;}hs_tab[PROG_TAB];
static struct{uint64_t key;Pipe*pipe;}hs_pipes[PIPE_TAB];
static _Atomic int hs_total,hs_done;
static _Atomic bool hs_started,hs_finished,hs_requested;
static uint64_t SDL_GetTicksNS(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void SDL_Delay(unsigned ms){usleep(ms*1000);}
static void plat_thread_name(const char*n){(void)n;}
uint64_t renderer_present_count(void){return 0;}
static bool headstart_on(void){return true;}
static bool sync_shaders(void){return false;}
static bool hs_prepare_locked(void){return true;}
static _Atomic int modules,pipelines;
static bool fail_module,fail_pipeline;
static void translate_program(void*arg){Prog*p=arg;CHECK(p->cmax[0]==128&&p->csize[0]==4);uint32_t constant;memcpy(&constant,(void*)(uintptr_t)(p->code[0]+128),4);CHECK(constant==0xdeadcafe);atomic_fetch_add(&modules,1);atomic_store(&p->state,fail_module?2:1);}
static void build_pipeline(void*arg){Pipe*p=arg;CHECK(atomic_load(&p->k.prog->state)==1);atomic_fetch_add(&pipelines,1);atomic_store(&p->state,fail_pipeline?2:1);}
typedef struct{void(*fn)(void*);void*arg;unsigned delay;}Job;
static void*job(void*arg){Job*j=arg;SDL_Delay(j->delay);j->fn(j->arg);free(j);return NULL;}
static void queue(void(*fn)(void*),void*arg,unsigned delay){Job*j=malloc(sizeof*j);CHECK(j);*j=(Job){fn,arg,delay};pthread_t t;CHECK(!pthread_create(&t,NULL,job,j));pthread_detach(t);}
static void run_translate_q(void(*fn)(void*),void*arg,bool low){CHECK(low);queue(fn,arg,40);}
static void run_job_q(void(*fn)(void*),void*arg,bool low){CHECK(low);queue(fn,arg,120);}
typedef struct{_Atomic int state;}CsProg;
static CsProg compute_program;
static _Atomic unsigned compute_jobs;
static int hs_cs_state(CsProg*p){return atomic_load(&p->state);}
static void finish_compute(void*arg){CsProg*p=arg;atomic_fetch_add(&compute_jobs,1);atomic_store(&p->state,1);}
static CsProg*test_compute_replay(uint32_t size,const void*bytes,bool*fresh){CHECK(size>sizeof(ShaderKey));CHECK(*(const unsigned char*)bytes==0x5a);*fresh=true;queue(finish_compute,&compute_program,120);return&compute_program;}
/* Portable replay/writer ownership has a separate production integration fixture. */
static void discovery_replay(void) {}
static void discovery_seed_profiles(void) {}
static void discovery_finish(void) {}
#include "headstart_progress_production.h"
static _Atomic bool cancel_requested;
static bool cancelled_fn(void){return atomic_load(&cancel_requested);}
static int callbacks,last_done,last_total;static bool incomplete;
static void progress(int done,int total){CHECK(done>=0&&total>=done);callbacks++;last_done=done;last_total=total;if(total&&done<total)incomplete=true;}
int main(int argc,char**argv){
 CHECK(argc==2);bool empty=!strcmp(argv[1],"empty"),corrupt=!strcmp(argv[1],"corrupt"),cancelled=!strcmp(argv[1],"cancel"),compute=!strcmp(argv[1],"compute");
 fail_module=!strcmp(argv[1],"failmodule");fail_pipeline=!strcmp(argv[1],"failpipe");
 mkdir("cache",0755);FILE*f=fopen("cache/headstart.bin","wb");CHECK(f);CHECK(fwrite(&hs_header,sizeof hs_header,1,f)==1);
 if(!empty&&!compute){uint8_t code[132]={0};uint32_t constant=0xdeadcafe;memcpy(code+128,&constant,4);HsProgHeader h={.cmax={128,0},.csize={4,0}};h.fkey.tex_uint=7;h.chash=prog_content_hash(code,128,4,NULL,0,0,&h.vkey,&h.fkey);if(corrupt)h.fkey.tex_uint++;
 HsRec rec={HS_PROG,sizeof h+sizeof code};CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(&h,sizeof h,1,f)==1);CHECK(fwrite(code,sizeof code,1,f)==1);PipeKey key={.flags=17};memcpy(&key.prog,&h.chash,sizeof h.chash);rec=(HsRec){HS_PIPE,sizeof key};CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(&key,sizeof key,1,f)==1);}
 if(compute){HsCsHeader h={.stage_hash=7,.local={32,1,1},.code_bytes=1};HsRec r={HS_CS,sizeof h+1};CHECK(fwrite(&r,sizeof r,1,f)==1);CHECK(fwrite(&h,sizeof h,1,f)==1);CHECK(fputc(0x5a,f)!=EOF);}
 CHECK(!fclose(f));vk_headstart_begin();SDL_Delay(20);CHECK(!atomic_load(&modules)&&!atomic_load(&pipelines)&&!atomic_load(&compute_jobs));uint64_t before=SDL_GetTicksNS();atomic_store(&cancel_requested,cancelled);vk_headstart_prepare(progress,cancelled_fn);uint64_t duration=SDL_GetTicksNS()-before;
 if(cancelled){CHECK(duration<100000000ull);uint64_t drain_start=SDL_GetTicksNS();vk_headstart_finish();CHECK(atomic_load(&hs_finished));CHECK(SDL_GetTicksNS()-drain_start>=120000000ull);CHECK(atomic_load(&modules)==1&&atomic_load(&pipelines)==1);}
 else if(empty){CHECK(callbacks&&last_total==0&&last_done==0);CHECK(duration<200000000ull);}
 else if(corrupt){CHECK(callbacks&&last_total==1&&last_done==1);CHECK(!atomic_load(&modules)&&!atomic_load(&pipelines));}
 else if(compute){CHECK(callbacks>2&&incomplete);CHECK(last_total==1&&last_done==1);CHECK(atomic_load(&compute_jobs)==1);CHECK(!atomic_load(&modules)&&!atomic_load(&pipelines));CHECK(duration>=90000000ull);}
 else{CHECK(callbacks>2&&incomplete);CHECK(last_total==2&&last_done==2);CHECK(atomic_load(&modules)==1&&atomic_load(&pipelines)==(fail_module?0:1));CHECK(duration>=(fail_module?40000000ull:160000000ull));}
 for(int i=0;i<PROG_TAB;i++)free(hs_tab[i].prog);for(int i=0;i<PIPE_TAB;i++)free(hs_pipes[i].pipe);
 printf("%s: %u readiness assertions passed\n",argv[1],checks);return 0;
}
