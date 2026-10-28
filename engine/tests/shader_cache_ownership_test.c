/* Actual extracted production cache helpers, controlled object payloads/queues.
 * Proves one publishing/queue owner under simultaneous foreground/replay misses;
 * does not execute shaderc, Vulkan or a real driver. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PROG_TAB 64
#define PIPE_TAB 128
typedef struct {uint64_t chash;unsigned marker;} Prog;
typedef struct {unsigned marker;} Pipe;
static pthread_mutex_t hs_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {uint64_t key;Prog*prog;bool taken;} hs_tab[PROG_TAB];
static struct {uint64_t key;Pipe*pipe;} hs_pipes[PIPE_TAB];
#include "ownership.production.h"
static _Atomic unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static pthread_barrier_t ready;
static _Atomic unsigned jobs,freed;
static void*winner[32];
static void*worker(void*opaque){
 unsigned i=(unsigned)(uintptr_t)opaque;bool fresh;
 // All callers observe a miss first, exactly the old race's interleaving.
 if(i&1){CHECK(!hs_pipe_take(123));Pipe*p=calloc(1,sizeof*p);p->marker=i;pthread_barrier_wait(&ready);Pipe*w=hs_pipe_put(123,p,&fresh);if(w!=p){free(p);atomic_fetch_add(&freed,1);}if(fresh)atomic_fetch_add(&jobs,1);winner[i]=w;}
 else {CHECK(!hs_take(456));Prog*p=calloc(1,sizeof*p);p->chash=456;p->marker=i;pthread_barrier_wait(&ready);Prog*w=hs_add(p,i%4==0,&fresh);if(w!=p){free(p);atomic_fetch_add(&freed,1);}if(fresh)atomic_fetch_add(&jobs,1);winner[i]=w;}
 return NULL;
}
int main(void){
 for(unsigned repeat=0;repeat<50;repeat++){
  memset(hs_tab,0,sizeof hs_tab);memset(hs_pipes,0,sizeof hs_pipes);atomic_store(&jobs,0);atomic_store(&freed,0);CHECK(!pthread_barrier_init(&ready,NULL,32));pthread_t threads[32];
  for(unsigned i=0;i<32;i++)CHECK(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i));
  for(unsigned i=0;i<32;i++)CHECK(!pthread_join(threads[i],NULL));
  CHECK(atomic_load(&jobs)==2);CHECK(atomic_load(&freed)==30);
  for(unsigned i=0;i<32;i++)CHECK(winner[i]==winner[i&1]);
  CHECK(hs_take(456)==winner[0]);CHECK(hs_pipe_take(123)==winner[1]);free(winner[0]);free(winner[1]);CHECK(!pthread_barrier_destroy(&ready));
 }
 // Bucket collisions do not merge different content; a repeated key keeps the winner.
 memset(hs_tab,0,sizeof hs_tab);memset(hs_pipes,0,sizeof hs_pipes);
 Prog a={.chash=1},b={.chash=65},loser={.chash=1};Pipe c={0},d={0},pl={0};bool fresh;
 CHECK(hs_add(&a,false,&fresh)==&a&&fresh);CHECK(hs_add(&b,false,&fresh)==&b&&fresh);CHECK(hs_add(&loser,true,&fresh)==&a&&!fresh);CHECK(hs_take(65)==&b);
 CHECK(hs_pipe_put(1,&c,&fresh)==&c&&fresh);CHECK(hs_pipe_put(129,&d,&fresh)==&d&&fresh);CHECK(hs_pipe_put(1,&pl,&fresh)==&c&&!fresh);CHECK(hs_pipe_take(129)==&d);
 // Full auxiliary tables retain prior bounded foreground fallback behavior.
 for(unsigned i=0;i<PROG_TAB;i++)hs_tab[i].key=i+1;Prog full={.chash=9999};CHECK(hs_add(&full,true,&fresh)==&full&&fresh);
 for(unsigned i=0;i<PIPE_TAB;i++)hs_pipes[i].key=i+1;CHECK(hs_pipe_put(9999,&pl,&fresh)==NULL&&!fresh);
 printf("%u ownership assertions passed; 1600 concurrent callers,100 unique queue owners\n",checks);return 0;
}
