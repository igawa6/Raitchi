/* Production compute cache helpers with controlled translator/build queues. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "maxwell/shader.h"
#define LOG(...) ((void)0)
#include "compute_cache.inc"
struct {struct {struct {uint32_t maxComputeWorkGroupSize[3],maxComputeWorkGroupInvocations;} limits;} props;} g_vk={.props.limits={{1024,1024,64},1024}};
typedef struct {_Atomic int state;struct {uint64_t hash;} fp[1];} Prog;
struct CsProg {Prog p;ShaderInfo *si;uint32_t local[3];uint64_t code;};
static ShaderInfo translated_info={.msl="synthetic"};
static struct {uint64_t key,stage_hash;uint32_t code_bytes,constant_bytes,local[3];ShaderKey shader_key;CsProg*cp;} hs_cs_tab[HS_CS_TAB];
static size_t hs_cs_n;
static uint64_t hs_cs_pending_bytes;
static pthread_mutex_t hs_lock=PTHREAD_MUTEX_INITIALIZER;
static int translates,builds,fail_translate;
static void (*queued)(void*);static void*queued_arg;
static bool translate_compute(CsProg*cp,const uint8_t*data,size_t code,size_t constants,uint64_t hash){assert(code==4&&constants==1&&data[0]==1&&data[4]==5&&hash);++translates;if(fail_translate){atomic_store(&cp->p.state,2);return false;}cp->si=&translated_info;return true;}
static void build_compute(void*arg){CsProg*cp=arg;++builds;atomic_store_explicit(&cp->p.state,1,memory_order_release);}
static void run_job_q(void(*fn)(void*),void*arg,bool background){assert(background);fn(arg);}
static void run_translate_q(void(*fn)(void*),void*arg,bool background){assert(background&&!queued);queued=fn;queued_arg=arg;}
#include "compute_replay.inc"
static void pump(void){assert(queued);void(*fn)(void*)=queued;void*arg=queued_arg;queued=NULL;queued_arg=NULL;fn(arg);}
/* Types and routing below are extracted from the current runtime, while
 * preparation, capture, logging and foreground scheduling are controlled. */
#include "compute_stage_types.inc"
#include "shader_snapshot.h"
typedef struct {char kind;uint64_t vs;uint32_t local[3];} ShaderRec;
typedef struct {ShaderRec rec;ShaderKey vkey,fkey;const uint8_t*code[2];uint32_t code_bytes[2],constant_bytes[2];} NvnDiscoveryRecipe;
/* Snapshot/store ownership is tested by the portable runtime fixture. */
static void discovery_record(const NvnDiscoveryRecipe*r,uint64_t content){(void)r;(void)content;assert(false);}
static struct CsEnt {uint64_t addr;uint32_t local[3],code_bytes,constant_bytes;CsProg*cp;int fp;} g_cs_tab[256];
static int g_cs_n,prep_cs_n,record_notes;
#define PREP_GET(tab,key) ((void)(key),(CsProg*)NULL)
static void shader_fp_check(int*fp,const NvnShaderStage*s,const char*kind){(void)fp;(void)s;(void)kind;}
static void cs_remember_use(const NvnShaderStage*s,const uint32_t local[3],CsProg*cp){assert(g_cs_n<256);struct CsEnt*e=&g_cs_tab[g_cs_n++];e->addr=s->code_addr;memcpy(e->local,local,sizeof e->local);e->code_bytes=s->const_offset;e->constant_bytes=s->const_size;e->cp=cp;}
static bool shader_rec_recording(void){return true;}
static void shader_rec_note(const ShaderRec*r){assert(r->kind=='C');++record_notes;}
static bool headstart_on(void){return true;}
static const uint8_t*hs_capture_stage(const NvnShaderStage*s,uint64_t n,uint64_t c,enum NvnShaderSnapshotResult*why){(void)s;(void)n;(void)c;(void)why;return NULL;}
static void hs_cs_record_queue(const HsCsHeader*h,const uint8_t*data){(void)h;(void)data;assert(false);}
static void shader_first_visit(const char*s,uint64_t a,uint64_t b,uint64_t c){(void)s;(void)a;(void)b;(void)c;}
static void save_glsl(const char*s,uint64_t a,const char*kind){(void)s;(void)a;(void)kind;}
static void run_job(void(*fn)(void*),void*arg){fn(arg);}
#include "compute_program.inc"
static void route_program(uint8_t*data,HsCsHeader*h,CsProg*ready){
 _Alignas(4) uint8_t control[0x754]={0};memcpy(control+0x748,h->local,sizeof h->local);NvnProgram np={0};np.stages[5]=(NvnShaderStage){.code_addr=(uintptr_t)data,.control=control,.const_offset=4,.const_size=1};
 g_cs_n=0;int before=translates,notes=record_notes;CsProg*hit=compute_program(&np);assert(hit==ready&&translates==before&&record_notes==notes);g_cs_n=0;
 atomic_store(&ready->p.state,0);CsProg*cold=compute_program(&np);assert(cold&&cold!=ready&&translates==before+1&&record_notes==notes+1&&atomic_load(&cold->p.state)==1);free(cold);g_cs_n=0;
 atomic_store(&ready->p.state,2);cold=compute_program(&np);assert(cold&&cold!=ready&&translates==before+2);free(cold);g_cs_n=0;atomic_store(&ready->p.state,1);
}
static void write_record(const char*path,HsHeader*id,HsCsHeader*h,const uint8_t*data){FILE*f=fopen(path,"wb");assert(f);HsRec r={HS_CS,sizeof*h+5};assert(fwrite(id,sizeof*id,1,f)==1&&fwrite(&r,sizeof r,1,f)==1&&fwrite(h,sizeof*h,1,f)==1&&fwrite(data,5,1,f)==1);assert(!fclose(f));}
static void parser(const char*path,HsCsHeader*h,const uint8_t*data){
 HsHeader id={.magic=HS_MAGIC,.header_size=sizeof id,.pipe_size=32,.shader_key_size=sizeof(ShaderKey)};write_record(path,&id,h,data);FILE*f=hs_cache_open(path,&id);assert(f&&hs_cache_record_count==1&&hs_cs_recorded_find_locked(hs_cs_content_key(h)));assert(!fclose(f));
 f=fopen(path,"r+b");assert(f&&!fseek(f,-1,SEEK_END));long n=ftell(f);assert(n>0&&!ftruncate(fileno(f),n));assert(!fclose(f));f=hs_cache_open(path,&id);assert(f&&hs_cache_record_count==0&&hs_cache_file_bytes==sizeof id);assert(!fclose(f));
 HsCsHeader bad=*h;bad.stage_hash++;write_record(path,&id,&bad,data);f=hs_cache_open(path,&id);assert(f&&!hs_cache_record_count);assert(!fclose(f));
 bad=*h;bad.constant_bytes=UINT32_MAX;write_record(path,&id,&bad,data);f=hs_cache_open(path,&id);assert(f&&!hs_cache_record_count);assert(!fclose(f));
 write_record(path,&id,h,data);id.identity[0]++;f=hs_cache_open(path,&id);assert(f&&!hs_cache_record_count);assert(!fclose(f));assert(!unlink(path));
}
int main(int argc,char**argv){
 assert(argc==2);uint8_t data[5]={1,2,3,4,5};HsCsHeader h={.local={32,1,1},.code_bytes=4,.constant_bytes=1,.key={.glsl=1}};h.stage_hash=shader_code_hash(data,5);uint32_t size=sizeof h+5;
 assert(hs_cs_size_valid(size,&h,data));assert(!hs_cs_size_valid(size-1,&h,data));HsCsHeader bad=h;bad.key.flip_x=true;assert(!hs_cs_size_valid(size,&bad,data));bad=h;bad.local[0]=0;assert(!hs_cs_size_valid(size,&bad,data));
 uint64_t key=hs_cs_content_key(&h);for(int i=0;i<3;i++){bad=h;bad.local[i]++;assert(hs_cs_content_key(&bad)!=key);}bad=h;bad.code_bytes=5;bad.constant_bytes=0;assert(hs_cs_content_key(&bad)!=key);bad=h;bad.key.flip_y=true;assert(hs_cs_content_key(&bad)!=key);parser(argv[1],&h,data);
 bool fresh;CsProg*cp=hs_cs_replay(size,&h,data,&fresh);assert(cp&&fresh&&hs_cs_state(cp)==0&&!translates&&!builds);assert(!hs_cs_take(h.stage_hash,4,1,h.local,&h.key));assert(!hs_cs_replay(size,&h,data,&fresh)&&!fresh);data[0]=9;pump();data[0]=1;assert(translates==1&&builds==1&&!hs_cs_pending_bytes&&hs_cs_state(cp)==1&&hs_cs_take(h.stage_hash,4,1,h.local,&h.key)==cp);
 route_program(data,&h,cp);
 uint32_t local[3]={32,2,1};assert(!hs_cs_take(h.stage_hash,4,1,local,&h.key));assert(!hs_cs_take(h.stage_hash,5,0,h.local,&h.key));ShaderKey other=h.key;other.flip_y=true;assert(!hs_cs_take(h.stage_hash,4,1,h.local,&other));
 bad=h;bad.local[0]=2048;assert(!hs_cs_replay(size,&bad,data,&fresh));bad=h;bad.local[0]=1024;bad.local[1]=2;assert(!hs_cs_replay(size,&bad,data,&fresh));bad=h;bad.local[0]=64;fail_translate=1;CsProg*failed=hs_cs_replay(size,&bad,data,&fresh);assert(failed&&fresh);pump();assert(hs_cs_state(failed)==2&&!hs_cs_take(bad.stage_hash,4,1,bad.local,&bad.key)&&!hs_cs_pending_bytes);
 hs_cs_pending_bytes=HS_CS_PENDING_BYTES;bad=h;bad.local[0]=128;assert(!hs_cs_replay(size,&bad,data,&fresh));hs_cs_pending_bytes=0;for(size_t i=0;i<hs_cs_n;i++)free(hs_cs_tab[i].cp);
 puts("production compute parser, local/key bounds, immutable replay, cold misses and ready-only reuse passed");
}
