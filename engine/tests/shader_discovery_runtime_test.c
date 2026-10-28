/* Live discovery/replay/key derivation with real store/SHA; synthetic inputs only. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include "shader_discovery.h"
#include "shader_preparation.h"
/* The source/profile bridge has separate actual-code synthetic RomFS tests.
 * This gate isolates replay/writer scheduling and driver-facing ownership. */
static const char *rt_data_root(void) { return "."; }
const char *g_romfs_dir = "";
bool shader_preparation_seed_catalog(NvnDiscoveryStore *store, const char *romfs,
    const char *profiles, const volatile int *cancel, const ShaderProfileCatalogReader *reader,
    ShaderPreparationStats *stats) {
    (void)store; (void)romfs; (void)profiles; (void)cancel; (void)reader;
    memset(stats, 0, sizeof *stats); return true;
}
/* SDL asset loading has separate catalog bounds tests. This replay harness
 * supplies filesystem IO with the same exact-read/seek contract. */
typedef FILE SDL_IOStream;
typedef int64_t Sint64;
#define SDL_IO_SEEK_SET SEEK_SET
static SDL_IOStream *SDL_IOFromFile(const char *p, const char *m) { return fopen(p, m); }
static Sint64 SDL_GetIOSize(SDL_IOStream *s) {
    struct stat st; return fstat(fileno(s), &st) ? -1 : (Sint64)st.st_size;
}
static bool SDL_CloseIO(SDL_IOStream *s) { return !fclose(s); }
static Sint64 SDL_SeekIO(SDL_IOStream *s, Sint64 o, int w) { return fseeko(s, o, w) ? -1 : ftello(s); }
static size_t SDL_ReadIO(SDL_IOStream *s, void *b, size_t n) { return fread(b, 1, n, s); }
bool shader_preparation_export(NvnDiscoveryStore *store, const char *profiles) {
    (void)store; (void)profiles; return true;
}
static _Atomic unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define LOG(...) fprintf(stderr,__VA_ARGS__)
#define FATAL(...) do { fprintf(stderr,__VA_ARGS__); abort(); } while (0)
#define KERNEL_READABLE_COPY_MAX (16u*1024u*1024u)
#define HEAP_BASE 0x100000000ull
#define HEAP_MAX 0x10000000ull
#define PROG_TAB 8
#define PIPE_TAB 8
#define HS_CS_TAB 8
#define HS_CS_MAX_BYTES KERNEL_READABLE_COPY_MAX
#ifdef REAL_WORKERS
#define HS_CS_PENDING_BYTES 80u /* reduced synthetic budget exercises retry without huge shader fixtures */
#else
#define HS_CS_PENDING_BYTES (32u*1024u*1024u)
#endif
#define HS_CS_DEDUP_TAB 131072
#define HS_CACHE_MAX_RECORDS 65536
#define HS_CACHE_MAX_FILE_BYTES (UINT64_C(256)*1024*1024)
#define HS_MAGIC 0x3753484c
#define HS_PROG 1
#define HS_PIPE 2
#define HS_CS 3
#define ZERO_BINDING 16
#define VK_FORMAT_UNDEFINED 0
#define VK_FORMAT_R32G32B32A32_SFLOAT 109
#define VK_IMAGE_ASPECT_DEPTH_BIT 2
#define VK_IMAGE_ASPECT_STENCIL_BIT 4
typedef uint32_t VkFormat;
typedef uint32_t VkImageAspectFlags;
typedef struct {
    ShaderInfo *vi,*fi;
    uint64_t code[2],cmax[2],csize[2]; const uint8_t *read_code[2];
    ShaderKey vkey,fkey; uint64_t chash; bool headstart,vs_uses_vid;
    _Atomic int state; ShaderFp fp[2];
} Prog;
#include "discovery_runtime_types.inc"
static pthread_mutex_t hs_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t translate_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t key; Prog *prog; bool taken; } hs_tab[PROG_TAB];
static struct { uint64_t key; Pipe *pipe; } hs_pipes[PIPE_TAB];
static struct { uint64_t key,stage_hash; uint32_t code_bytes,constant_bytes,local[3]; ShaderKey shader_key; CsProg *cp; } hs_cs_tab[HS_CS_TAB];
static size_t hs_cs_n; static uint64_t hs_cs_pending_bytes;
static uint64_t hs_cs_recorded[HS_CS_DEDUP_TAB];
static size_t hs_cs_recorded_n;
static FILE *hs_out;
static HsHeader hs_header;
static bool hs_prepared, strict_loaded;
static uint64_t hs_cache_file_bytes;
static uint32_t hs_cache_record_count;
static _Atomic int hs_total,hs_done;
static _Atomic bool hs_started,hs_requested,hs_finished;
static struct { bool vids,divisor; struct { struct { uint32_t maxComputeWorkGroupSize[3],maxComputeWorkGroupInvocations; } limits; } props; } g_vk;
static _Atomic unsigned translate_calls,pipeline_calls,compute_calls,delays;
static bool fail_program,fail_module,fail_pipeline,fail_compute;
static uint32_t translated_attr=4,translated_out=0xf0,translated_varyings=0x20;
static uint32_t observed_fmask,observed_vuint,observed_fuint;
static bool headstart_enabled=true;
static uint8_t game_identity[32]={1,2,3};
static bool shared_gpu_game_identity(uint8_t out[32]) { memcpy(out,game_identity,32); return true; }
bool shared_sha256_bytes(const void *,size_t,uint8_t[32]);
static bool headstart_on(void) { return headstart_enabled; }
static bool sync_shaders(void) { return false; }
static void plat_thread_name(const char *name) { (void)name; }
static uint32_t gmem_chunk_shift(void) { return 0; }
uint64_t maxwell_gmem_base,maxwell_gmem_size;
uint32_t maxwell_gmem_chunk_shift;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
static bool hs_header_make(HsHeader *out) {
    *out=(HsHeader){.magic=HS_MAGIC,.header_size=sizeof *out,.pipe_size=sizeof(PipeKey),.shader_key_size=sizeof(ShaderKey)};
    memset(out->identity,0x5a,sizeof out->identity);return true;
}
static void *mapped; static size_t mapped_bytes;
static void *tracked_mmap(void *address,size_t size,int prot,int flags,int fd,off_t offset) {
    void *result=mmap(address,size,prot,flags,fd,offset);CHECK(result!=MAP_FAILED&&!mapped);mapped=result;mapped_bytes=size;return result;
}
uint64_t renderer_present_count(void) { return 0; }
static uint64_t SDL_GetTicksNS(void) {
#ifdef REAL_WORKERS
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
#else
    return (uint64_t)atomic_load(&delays)*1000000;
#endif
}
#ifdef REAL_WORKERS
static _Atomic unsigned active_builders,max_builders,active_modules,max_modules,active_translations,max_translations,pipeline_overlap;
static void note_max(_Atomic unsigned *maximum,unsigned now) { unsigned old=atomic_load(maximum);while(old<now&&!atomic_compare_exchange_weak(maximum,&old,now)){} }
static void builder_enter(void) { note_max(&max_builders,atomic_fetch_add(&active_builders,1)+1); }
static void builder_leave(void) { atomic_fetch_sub(&active_builders,1); }
#endif
ShaderInfo *maxwell_translate_sized(const uint8_t *bytes,size_t n,size_t constants,int stage,const ShaderKey *key) {
    CHECK(bytes && n>=128 && constants==8);
#ifdef REAL_WORKERS
    usleep(6000);
#endif
    ShaderInfo *s=calloc(1,sizeof *s); CHECK(s);
    if (stage==STAGE_VERTEX) {
        observed_vuint=key->tex_uint; s->attr_mask=translated_attr; s->out_mask=translated_varyings;
    } else {
        CHECK(stage==STAGE_FRAGMENT); observed_fmask=key->vs_out_mask; observed_fuint=key->tex_uint; s->out_mask=translated_out;
    }
    if (!fail_program) s->msl=strdup("synthetic current translator");
    return s;
}
/* Direct compiler selection has its own actual-code/GPU differential gates.
 * This fixture isolates recipe reconstruction and replay scheduling. */
static ShaderInfo *maxwell_compile_sized(const uint8_t *bytes, size_t n, size_t constants,
    int stage, const ShaderKey *key) {
    return maxwell_translate_sized(bytes, n, constants, stage, key);
}
static bool shader_info_succeeded(const ShaderInfo *info) { return info && info->msl; }
static VkFormat vertex_format(uint32_t format) { return 1000+format; }
static VkFormat vertex_format_supported(VkFormat format) { return format+100; }
static VkFormat vk_format(uint32_t format) { return 2000+format; }
static VkFormat vk_image_format(uint32_t format,VkImageAspectFlags *aspects) {
    if (aspects) *aspects=format==99 ? VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT : 0;
    return 2000+format;
}
static bool renderer_format_is_depth(uint32_t format) { return format==99; }
static int rt_kind(uint32_t format) { return format==11; }
static Pipe *prep_pipe_get(uint64_t hash) { (void)hash; return NULL; }
#define PREP_GET(table,hash) prep_pipe_get(hash)
static unsigned npending;
#ifdef REAL_WORKERS
#ifndef FATAL
#define FATAL(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#endif
#include "discovery_runtime_queue.inc"
static unsigned builder_count=3;
static void *real_queue_worker(void *q) { shader_job_queue_run(q);return NULL; }
static void real_start_translator(void) { pthread_t t;CHECK(!pthread_create(&t,NULL,real_queue_worker,&g_tjobs));CHECK(!pthread_detach(t)); }
static void real_start_builders(void) { for(unsigned i=0;i<builder_count;i++){pthread_t t;CHECK(!pthread_create(&t,NULL,real_queue_worker,&g_jobs));CHECK(!pthread_detach(t));} }
static void SDL_Delay(unsigned ms) { atomic_fetch_add(&delays,1);usleep(ms*1000); }
static void run_translate_q(void (*fn)(void *),void *arg,bool low) { CHECK(!pthread_once(&g_tjobs.once,real_start_translator));queue_push(&g_tjobs,fn,arg,low); }
static void run_job_q(void (*fn)(void *),void *arg,bool low) { CHECK(!pthread_once(&g_jobs.once,real_start_builders));queue_push(&g_jobs,fn,arg,low); }
static void real_compile_program(void *arg) {
    Prog *p=arg;builder_enter();note_max(&max_modules,atomic_fetch_add(&active_modules,1)+1);usleep(40000);
    atomic_fetch_sub(&active_modules,1);builder_leave();atomic_store(&p->state,fail_module?2:1);
}
static void real_build_compute(void *arg) { CsProg *cp=arg;builder_enter();usleep(15000);builder_leave();atomic_store(&cp->p.state,fail_compute?2:1); }
#else
typedef struct { void (*fn)(void *); void *arg; unsigned ready; } Pending;
static Pending pending[32];
static pthread_mutex_t pending_lock=PTHREAD_MUTEX_INITIALIZER;
static void schedule(void (*fn)(void *),void *arg) {
    pthread_mutex_lock(&pending_lock); CHECK(npending<32); pending[npending++]=(Pending){fn,arg,atomic_load(&delays)+3}; pthread_mutex_unlock(&pending_lock);
}
static void SDL_Delay(unsigned ms) {
    (void)ms; unsigned now=atomic_fetch_add(&delays,1)+1; Pending next={0};
    pthread_mutex_lock(&pending_lock);
    for (unsigned i=0;i<npending;i++) if (pending[i].ready<=now) { next=pending[i]; pending[i]=pending[--npending]; break; }
    pthread_mutex_unlock(&pending_lock);
    if (next.fn) next.fn(next.arg); else usleep(100);
}
static void run_translate_q(void (*fn)(void *),void *arg,bool low) { CHECK(low); schedule(fn,arg); }
static void run_job_q(void (*fn)(void *),void *arg,bool low) { CHECK(low); schedule(fn,arg); }
#endif
static void translate_program(void *arg);
static void build_pipeline(void *arg) {
    Pipe *p=arg; CHECK(atomic_load(&p->k.prog->state)==1); pipeline_calls++;
#ifdef REAL_WORKERS
    builder_enter();if(atomic_load(&active_modules))atomic_fetch_add(&pipeline_overlap,1);usleep(10000);builder_leave();
#endif
    atomic_store(&p->state,fail_pipeline?2:1);
}
static void hs_cs_translate_job(void *arg) {
    HsCsTranslate *j=arg; CHECK(j->snapshot && j->code_bytes>0 && j->snapshot[0]==0x10);
    compute_calls++;
#ifdef REAL_WORKERS
    note_max(&max_translations,atomic_fetch_add(&active_translations,1)+1);usleep(20000);atomic_fetch_sub(&active_translations,1);
    CsProg *cp=j->cp;
#else
    atomic_store(&j->cp->p.state,fail_compute?2:1);
#endif
    pthread_mutex_lock(&hs_lock); hs_cs_pending_bytes-=j->code_bytes+j->constant_bytes; pthread_mutex_unlock(&hs_lock);
    free(j->snapshot); free(j);
#ifdef REAL_WORKERS
    run_job_q(real_build_compute,cp,true);
#endif
}
static void discovery_replay(void);
static void discovery_finish(void);
static pthread_t replay_thread;
static bool replay_thread_started;
static void *test_replay_thread(void *unused) { (void)unused; discovery_replay(); atomic_store(&hs_finished,true); return NULL; }
static void vk_headstart_begin(void) {
    if (atomic_exchange(&hs_started,true)) return;
    CHECK(!pthread_create(&replay_thread,NULL,test_replay_thread,NULL)); replay_thread_started=true;
}
#include "discovery_runtime_functions.inc"
static void translate_program(void *arg) {
    Prog *p=arg; translate_calls++;
#ifdef REAL_WORKERS
    note_max(&max_translations,atomic_fetch_add(&active_translations,1)+1);
    bool ok=translate_program_now(p);atomic_fetch_sub(&active_translations,1);
    if(ok)run_job_q(real_compile_program,p,p->headstart);
#else
    if (translate_program_now(p)) atomic_store(&p->state,fail_module?2:1);
#endif
}
static uint8_t vs[136],fs[136],cs[40];
static NvnDiscoveryRecipe graphics(bool depth_only) {
    memset(vs,0x11,sizeof vs); memset(fs,0x22,sizeof fs);
    NvnDiscoveryRecipe r={0}; r.rec.kind='G'; r.rec.vs=shader_code_hash(vs,sizeof vs);r.rec.fs=depth_only?0:shader_code_hash(fs,sizeof fs);
    r.rec.nattrib=3;r.rec.nstream=3;r.rec.ncolor=depth_only?0:2;r.rec.samples=1;r.rec.depth=99;r.rec.topo=2;r.rec.poly_fill=1;
    r.rec.attrib[0]=4;r.rec.attrib[2]=6|(2<<8)|(12<<13);r.rec.stream[2][0]=32;r.rec.stream[2][1]=3;
    r.rec.color[0]=10;r.rec.color[1]=11;r.rec.cview[0]=12;r.rec.channel_mask=0xff;r.rec.blend_on=3;
    r.vkey.glsl=1;r.vkey.tex_uint=2;r.fkey.glsl=1;r.fkey.tex_uint=4;r.fkey.rt_kind[1]=1;
    r.code[0]=vs;r.code_bytes[0]=128;r.constant_bytes[0]=8;
    if (!depth_only) { r.code[1]=fs;r.code_bytes[1]=128;r.constant_bytes[1]=8; }
    return r;
}
static NvnDiscoveryRecipe compute(void) {
    memset(cs,0,sizeof cs);cs[0]=0x10;NvnDiscoveryRecipe r={0};r.rec.kind='C';r.rec.vs=shader_code_hash(cs,sizeof cs);
    r.rec.local[0]=8;r.rec.local[1]=2;r.rec.local[2]=1;r.vkey.glsl=1;r.code[0]=cs;r.code_bytes[0]=32;r.constant_bytes[0]=8;return r;
}
static void seed(const NvnDiscoveryRecipe *r) {
    CHECK(discovery_open_locked());CHECK(nvn_discovery_append(&discovery_store,r)==NVN_DISCOVERY_APPEND_APPENDED);
}
static void free_objects(void) {
    CHECK(npending==0);
    for (unsigned i=0;i<PIPE_TAB;i++) free(hs_pipes[i].pipe);
    for (unsigned i=0;i<PROG_TAB;i++) if (hs_tab[i].prog) {
        Prog *p=hs_tab[i].prog;
        if(p->vi){free(p->vi->msl);free(p->vi);}if(p->fi){free(p->fi->msl);free(p->fi);}
        if(!strict_loaded)free((void*)p->read_code[0]);free(p);
    }
    for(size_t i=0;i<hs_cs_n;i++) free(hs_cs_tab[i].cp);
    memset(hs_tab,0,sizeof hs_tab);memset(hs_pipes,0,sizeof hs_pipes);memset(hs_cs_tab,0,sizeof hs_cs_tab);hs_cs_n=0;
    if(mapped){CHECK(!munmap(mapped,mapped_bytes));mapped=NULL;}
}
static void cleanup(void) {
    if (replay_thread_started) CHECK(!pthread_join(replay_thread,NULL));
    free_objects();
    if(hs_out){CHECK(!fclose(hs_out));hs_out=NULL;}
    nvn_discovery_close(&discovery_store);
}
// Inspect bytes emitted by the real strict writer, not a recording-call counter.
static void strict_records(unsigned programs,unsigned pipes,unsigned computes) {
    FILE *file=fopen("cache/headstart.bin","rb");
    if(!file){CHECK(!programs&&!pipes&&!computes);return;}
    HsHeader header;CHECK(fread(&header,sizeof header,1,file)==1);CHECK(!memcmp(&header,&hs_header,sizeof header));
    unsigned counts[4]={0};HsRec rec;
    while(fread(&rec,sizeof rec,1,file)==1){
        CHECK(rec.type>=HS_PROG&&rec.type<=HS_CS);counts[rec.type]++;
        uint8_t *payload=malloc(rec.size);CHECK(payload);CHECK(fread(payload,1,rec.size,file)==rec.size);
        if(rec.type==HS_PROG){
            HsProgHeader h;CHECK(rec.size>=sizeof h);memcpy(&h,payload,sizeof h);CHECK(hs_program_size_valid(rec.size,&h));
            CHECK(h.cmax[0]==128&&h.csize[0]==8&&h.vkey.tex_uint==2&&h.fkey.tex_uint==4&&!h.fkey.vs_out_mask);
            CHECK(!memcmp(payload+sizeof h,vs,sizeof vs));
            if(h.cmax[1])CHECK(h.cmax[1]==128&&h.csize[1]==8&&!memcmp(payload+sizeof h+sizeof vs,fs,sizeof fs));
            CHECK(h.chash==prog_content_hash(payload+sizeof h,h.cmax[0],h.csize[0],h.cmax[1]?payload+sizeof h+h.cmax[0]+h.csize[0]:NULL,h.cmax[1],h.csize[1],&h.vkey,&h.fkey));
        }else if(rec.type==HS_PIPE){
            CHECK(rec.size==sizeof(PipeKey));PipeKey k;memcpy(&k,payload,sizeof k);
            CHECK(k.attr_mask==4&&k.attr_bind[2]==2&&k.attr_off[2]==12);
            CHECK(!k.write_mask[0]&&(k.ncolor==0||k.write_mask[1]==15));
            uint64_t content;memcpy(&content,&k.prog,sizeof content);bool found=false;
            for(unsigned i=0;i<PROG_TAB;i++)if(hs_tab[i].prog&&hs_tab[i].prog->chash==content)found=true;
            CHECK(content&&found);
        }else{
            HsCsHeader h;CHECK(rec.size>=sizeof h);memcpy(&h,payload,sizeof h);CHECK(hs_cs_size_valid(rec.size,&h,payload+sizeof h));
            CHECK(h.code_bytes==32&&h.constant_bytes==8&&h.local[0]==8&&h.local[1]==2&&h.local[2]==1);
            CHECK(!memcmp(payload+sizeof h,cs,sizeof cs));
        }
        free(payload);
    }
    CHECK(feof(file)&&!ferror(file));CHECK(!fclose(file));CHECK(counts[HS_PROG]==programs&&counts[HS_PIPE]==pipes&&counts[HS_CS]==computes);
}
static void progress(int done,int total) { CHECK(done>=0 && total>=done); }
static bool cancel_now(void) { return true; }
static void *finish_job(void *unused) { (void)unused; discovery_finish(); return NULL; }
#ifdef REAL_WORKERS
#include "shader_discovery_parallel_test.inc"
#endif
int main(int argc,char **argv) {
    CHECK(argc==3);CHECK(!chdir(argv[2]));
    g_vk.props.limits.maxComputeWorkGroupSize[0]=32;g_vk.props.limits.maxComputeWorkGroupSize[1]=32;g_vk.props.limits.maxComputeWorkGroupSize[2]=32;g_vk.props.limits.maxComputeWorkGroupInvocations=128;
    const char *mode=argv[1];
#ifdef REAL_WORKERS
    return parallel_test(mode);
#endif
    NvnDiscoveryRecipe r=graphics(!strcmp(mode,"depth"));
    if (!strcmp(mode,"strict_roundtrip")) {
        seed(&r);r.rec.samples=2;seed(&r);NvnDiscoveryRecipe c=compute();seed(&c);
        discovery_replay();CHECK(translate_calls==1&&pipeline_calls==2&&compute_calls==1);
        strict_records(1,2,1);CHECK(hs_cache_record_count==4);uint64_t bytes=hs_cache_file_bytes;
        discovery_replay();strict_records(1,2,1);CHECK(hs_cache_file_bytes==bytes&&translate_calls==1&&pipeline_calls==2&&compute_calls==1);
        CHECK(discovery_commit_locked());nvn_discovery_close(&discovery_store);discovery_store_tried=false;discovery_store_ok=false;
        free_objects();discovery_retained_bytes=0;hs_cs_pending_bytes=0;translate_calls=0;pipeline_calls=0;compute_calls=0;hs_total=0;hs_done=0;
        CHECK(!fclose(hs_out));hs_out=NULL;hs_prepared=false;hs_requested=true;hs_finished=false;strict_loaded=true;
        // Actual strict reader runs before its portable tail; only the four strict jobs may execute.
        CHECK(!headstart_thread(NULL));CHECK(hs_finished&&hs_total==4&&hs_done==4);
        CHECK(translate_calls==1&&pipeline_calls==2&&compute_calls==1);
        CHECK(hs_cache_record_count==4&&hs_cache_file_bytes==bytes);strict_records(1,2,1);
        discovery_replay();CHECK(translate_calls==1&&pipeline_calls==2&&compute_calls==1&&hs_cache_file_bytes==bytes);
    } else if (!strcmp(mode,"unproved")) {
        uint8_t code[144]={0};uint64_t brx=UINT64_C(0xe250000000000000);memcpy(code+136,&brx,8);
        r=graphics(true);r.code[0]=code;r.code_bytes[0]=sizeof code;r.constant_bytes[0]=0;r.rec.vs=shader_code_hash(code,sizeof code);
        seed(&r);discovery_replay();CHECK(!translate_calls&&!pipeline_calls&&!compute_calls);strict_records(0,0,0);
        Prog bad={.read_code={code,NULL},.cmax={sizeof code,0},.chash=9,.headstart=true};hs_record_program_closure(&bad);strict_records(0,0,0);
    } else if (!strcmp(mode,"compute_policy")) {
        r=compute();CHECK(discovery_recipe_replayable(&r));
        uint64_t brx=UINT64_C(0xe250000000000000);memcpy(cs+8,&brx,8);r.constant_bytes[0]=0;
        CHECK(!discovery_recipe_replayable(&r));r.constant_bytes[0]=8;CHECK(discovery_recipe_replayable(&r));
        uint8_t sdk[0x120]={0};uint32_t magic=0x12345679;memcpy(sdk,&magic,4);
        r.code[0]=sdk;r.code_bytes[0]=0xf8;r.constant_bytes[0]=0;CHECK(!discovery_recipe_replayable(&r));
        r.code_bytes[0]=sizeof sdk;CHECK(discovery_recipe_replayable(&r));memcpy(sdk+0x108,&brx,8);CHECK(!discovery_recipe_replayable(&r));
        r.code_bytes[0]=0x118;r.constant_bytes[0]=8;CHECK(discovery_recipe_replayable(&r));
    } else if (!strcmp(mode,"queue") || !strcmp(mode,"concurrent_finish")) {
        uint8_t original[sizeof vs];memcpy(original,vs,sizeof vs);
        discovery_record(&r,123);discovery_record(&r,123);memset(vs,0xcc,sizeof vs);memset(fs,0xdd,sizeof fs);
        if(!strcmp(mode,"concurrent_finish")) {
            pthread_t a,b;CHECK(!pthread_create(&a,NULL,finish_job,NULL));CHECK(!pthread_create(&b,NULL,finish_job,NULL));CHECK(!pthread_join(a,NULL));CHECK(!pthread_join(b,NULL));
        } else discovery_finish();
        CHECK(discovery_joined&&discovery_pending==0&&discovery_pending_bytes==0);
        CHECK(nvn_discovery_count(&discovery_store)==1);NvnDiscoveryRecipe saved={0};CHECK(nvn_discovery_read_at(&discovery_store,0,&saved));
        CHECK(!memcmp(saved.code[0],original,sizeof original));nvn_discovery_recipe_release(&saved);
        discovery_record(&r,456);CHECK(discovery_pending==0);discovery_finish();
    } else if (!strcmp(mode,"queue_failure")) {
        CHECK(!mkdir("cache",0700));CHECK(!mkdir("cache/shader_discovery.bin",0700));
        discovery_record(&r,123);discovery_finish();CHECK(discovery_joined&&!discovery_pending&&!discovery_pending_bytes&&!discovery_store_ok);
    } else if (!strcmp(mode,"queue_caps")) {
        discovery_stopping=true;discovery_record(&r,123);CHECK(!discovery_pending&&!discovery_started);discovery_stopping=false;
        discovery_pending=DISCOVERY_JOBS;discovery_record(&r,123);CHECK(!discovery_started);discovery_pending=0;
        discovery_pending_bytes=DISCOVERY_BYTES;discovery_record(&r,123);CHECK(!discovery_started);discovery_pending_bytes=0;
    } else if (!strcmp(mode,"table_full")) {
        for(unsigned i=0;i<PROG_TAB;i++){hs_tab[i].key=i+1;hs_tab[i].prog=calloc(1,sizeof(Prog));}
        bool fresh;CHECK(!prepare_program_exact(&r,&fresh));CHECK(!fresh&&!discovery_retained_bytes);
    } else if (!strcmp(mode,"retained_cap")) {
        discovery_retained_bytes=DISCOVERY_BYTES;bool fresh;CHECK(!prepare_program_exact(&r,&fresh));CHECK(!fresh);
    } else if (!strcmp(mode,"strict_reuse")) {
        bool fresh;Prog *p=prepare_program_exact(&r,&fresh);CHECK(p&&fresh);translate_program(p);
        Pipe *pipe=calloc(1,sizeof *pipe);CHECK(pipe);prepare_pipe_key(&pipe->k,p,&r.rec);atomic_store(&pipe->state,1);
        CHECK(hs_pipe_put(pipe_key_hash(&pipe->k),pipe,&fresh)==pipe&&fresh);
        seed(&r);discovery_replay();CHECK(translate_calls==1&&!pipeline_calls);
        CHECK(prepare_program_exact(&r,&fresh)==p&&!fresh);
        strict_records(0,0,0);
    } else {
        bool compute_mode=!strcmp(mode,"compute")||!strcmp(mode,"compute_limit")||!strcmp(mode,"fail_compute");
        if(compute_mode){r=compute();if(!strcmp(mode,"compute_limit"))r.rec.local[0]=64;}
        seed(&r);
        if(!strcmp(mode,"fail_program"))fail_program=true;
        if(!strcmp(mode,"fail_module"))fail_module=true;
        if(!strcmp(mode,"fail_pipeline"))fail_pipeline=true;
        if(!strcmp(mode,"fail_compute"))fail_compute=true;
        if(!strcmp(mode,"cancel")) {
            vk_headstart_prepare(progress,cancel_now);CHECK(hs_requested);
            vk_headstart_finish();CHECK(hs_finished&&discovery_stopping);
        } else discovery_replay();
        if(compute_mode) {
            CHECK(compute_calls==(!strcmp(mode,"compute_limit")?0:1));CHECK(!translate_calls&&!pipeline_calls);
            if(compute_calls){CHECK(hs_done==hs_total&&hs_cs_n==1);discovery_replay();CHECK(compute_calls==1);}
            strict_records(0,0,compute_calls&&!fail_compute?1:0);
        } else {
            CHECK(translate_calls==1);
            if(fail_program||fail_module){CHECK(!pipeline_calls);strict_records(0,0,0);}
            else {
                CHECK(pipeline_calls==1&&hs_done==hs_total);
                Prog *p=NULL;Pipe *pp=NULL;for(unsigned i=0;i<PROG_TAB;i++)if(hs_tab[i].prog)p=hs_tab[i].prog;
                for(unsigned i=0;i<PIPE_TAB;i++)if(hs_pipes[i].pipe)pp=hs_pipes[i].pipe;
                CHECK(p&&pp&&pp->k.prog==p&&pp->k.attr_mask==4);
                CHECK(p->headstart);
                CHECK(p->read_code[0]&&p->read_code[0][0]==0x11&&p->read_code[0][135]==0x11);
                CHECK(pp->k.attr_fmt[2]==1106&&pp->k.attr_bind[2]==2&&pp->k.attr_off[2]==12);
                CHECK(pp->k.divisor[2]==1); // current device does not support higher divisors
                CHECK(pp->k.depth==2099&&pp->k.stencil==2099);
                if(!strcmp(mode,"depth")){CHECK(!p->code[1]&&pp->k.ncolor==0&&pp->k.color[0]==0);}
                else {CHECK(observed_fmask==translated_varyings&&observed_vuint==2&&observed_fuint==4);CHECK(!pp->k.write_mask[0]&&pp->k.write_mask[1]==15);CHECK(pp->k.color[0]==2012);}
                unsigned old=atomic_load(&translate_calls);discovery_replay();CHECK(translate_calls==old&&pipeline_calls==1);
                strict_records(1,fail_pipeline?0:1,0);
                PipeKey changed;uint32_t old_attr=p->vi->attr_mask,old_out=p->fi->out_mask;
                p->vi->attr_mask=1;p->fi->out_mask=15;prepare_pipe_key(&changed,p,&r.rec);CHECK(changed.attr_mask==1);
                if(strcmp(mode,"depth"))CHECK(changed.write_mask[0]==15&&!changed.write_mask[1]);
                p->vi->attr_mask=old_attr;p->fi->out_mask=old_out;
                g_vk.vids=true;prepare_pipe_key(&changed,p,&r.rec);CHECK(changed.attr_mask==4&&!changed.used_streams&&!changed.attr_fmt[2]);
            }
        }
    }
    discovery_finish();cleanup();printf("{\"assertions\":%u}\n",atomic_load(&checks));
}
