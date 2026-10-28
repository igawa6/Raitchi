/* Asset-free host probes. Includes extracted live production functions generated
 * by tools/check_shader_cache_identity.py. GPU capabilities and shaderc are test
 * doubles; persistent-file operations and SHA256 use production implementations. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define VK_UUID_SIZE 16
#define VK_API_VERSION_1_1 ((1u<<22)|(1u<<12))
#define VK_API_VERSION_1_2 ((1u<<22)|(2u<<12))
#define VK_API_VERSION_1_3 ((1u<<22)|(3u<<12))
enum { STAGE_VERTEX, STAGE_FRAGMENT, STAGE_COMPUTE=5 };
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
#define LOG(...) ((void)0)
static struct {
    bool vids, divisor, host_import, ssbo_chunks, bda, gpl_probe, gpl, custom_border, lean_usage;
    uint32_t api;
    struct {
        uint32_t vendorID, deviceID, driverVersion, apiVersion;
        uint8_t pipelineCacheUUID[16];
        struct { uint32_t maxUniformBufferRange, maxStorageBufferRange; } limits;
    } props;
} g_vk;
static int maxwell_shader_int64, maxwell_gmem_readonly, maxwell_frag_subgroups,
    maxwell_h2f_bits, maxwell_gmem_mirror, maxwell_gmem_zero, maxwell_gfind_cache,
    maxwell_half_native, maxwell_precise_pos, maxwell_scalar_cbuf,
    maxwell_ieee_minmax, maxwell_robust_cbuf;
static uint32_t maxwell_vote_stages;
static int maxwell_storage_image_write, maxwell_clip_distance;
static bool maxwell_static_stacks, maxwell_brx_table, gc_lazy, maxwell_gmem_fs_miss;
static uint64_t heap_base=0x1240000000ull, heap_size=0xd0000000ull;
#define HEAP_BASE heap_base
#define HEAP_MAX heap_size
static uint32_t chunk_shift, key_size=736;
static uint32_t gmem_chunk_shift(void) { return chunk_shift; }
static uint32_t pipekey_size(void) { return key_size; }
#include "shader_key_test_types.inc"
static bool headstart_enabled=true;
static bool headstart_on(void) { return headstart_enabled; }
static bool path_available=true;
static unsigned path_calls;
bool shared_sha256_bytes(const void*,size_t,uint8_t[32]);
bool shared_sha256_file(const char*,uint8_t[32]);
static uint8_t game_identity[32], compiler_identity[32];
static bool shared_gpu_build_identity(bool compiler,uint8_t out[32]) {
    if(compiler) { if(!path_available)return false;memcpy(out,compiler_identity,32);return true; }
    ++path_calls;
    return path_available && shared_sha256_file("runtime.fixture",out);
}
static bool shared_gpu_game_identity(uint8_t out[32]) { memcpy(out,game_identity,32);return true; }

/* shaderc ABI doubles: record real production option selection; return a tiny
 * syntactically cacheable blob, not executable SPIR-V or a shader compiler test. */
typedef void *shaderc_compiler_t;
typedef void *shaderc_compile_options_t;
typedef void *shaderc_compilation_result_t;
enum { shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1=11,
       shaderc_env_version_vulkan_1_2, shaderc_env_version_vulkan_1_3,
       shaderc_optimization_level_zero=0, shaderc_optimization_level_performance=1,
       shaderc_compilation_status_success=0 };
static unsigned compiles;
static int selected_opt, selected_env;
static int kind_of(int stage) { return stage; }
static shaderc_compiler_t shaderc_compiler_initialize(void) { return (void*)1; }
static shaderc_compile_options_t shaderc_compile_options_initialize(void) { return (void*)1; }
static void shaderc_compile_options_set_target_env(void *o,int env,int ver) { (void)o;(void)env;selected_env=ver; }
static void shaderc_compile_options_set_optimization_level(void *o,int opt) { (void)o;selected_opt=opt; }
static shaderc_compilation_result_t shaderc_compile_into_spv(void*c,const char*s,size_t n,int k,const char*name,const char*entry,void*o) {
    (void)c;(void)s;(void)n;(void)k;(void)name;(void)entry;(void)o;++compiles;return(void*)1;
}
static void shaderc_compile_options_release(void*o) { (void)o; }
static int shaderc_result_get_compilation_status(void*r) { (void)r;return 0; }
static const char *shaderc_result_get_error_message(void*r) { (void)r;return ""; }
static uint32_t fake_spv[]={0x07230203,0x10000,0,2,0,0};
static size_t shaderc_result_get_length(void*r) { (void)r;return sizeof fake_spv; }
static const char *shaderc_result_get_bytes(void*r) { (void)r;return (const char*)fake_spv; }
static void shaderc_result_release(void*r) { (void)r; }
static unsigned no_contracts;
static uint32_t *no_contraction(uint32_t*p,size_t*n) { (void)n;++no_contracts;return p; }
#include "shader_cache_production.inc"

static void write_bytes(const char *path,const void *p,size_t n) {
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(p,1,n,f)==n);CHECK(!fclose(f));
}
static long file_size(const char *path) { struct stat s;CHECK(!stat(path,&s));return s.st_size; }
static void verify_empty(const char *path,const HsHeader *h) {
    FILE*f=hs_cache_open(path,h);CHECK(f);CHECK(ftell(f)==sizeof *h);CHECK(!fclose(f));
    CHECK(file_size(path)==sizeof *h);
}
static void same_header(const HsHeader *a,const HsHeader *b) { CHECK(!memcmp(a,b,sizeof *a)); }
static HsHeader fresh(void) { HsHeader h;CHECK(hs_header_make(&h));return h; }
#define CHANGED(field) do { ++(field); HsHeader changed=fresh(); CHECK(memcmp(&base,&changed,sizeof base)); --(field); } while(0)
#define TOGGLED(field) do { (field)=!(field); HsHeader changed=fresh(); CHECK(memcmp(&base,&changed,sizeof base)); (field)=!(field); } while(0)
static void *append_thread(void *unused) { (void)unused;uint8_t payload[736]={0};for(int i=0;i<20;++i)hs_write(HS_PIPE,payload,key_size,NULL,0,NULL,0);return NULL; }
static void headstart_tests(void) {
    write_bytes("runtime.fixture","synthetic implementation",24);
    HsHeader base=fresh(), h=fresh();same_header(&base,&h);
    TOGGLED(g_vk.vids);TOGGLED(g_vk.divisor);TOGGLED(g_vk.host_import);
    TOGGLED(g_vk.ssbo_chunks);TOGGLED(g_vk.bda);TOGGLED(g_vk.gpl_probe);
    TOGGLED(g_vk.gpl);
    TOGGLED(g_vk.custom_border);TOGGLED(g_vk.lean_usage);
    CHANGED(maxwell_shader_int64);CHANGED(maxwell_gmem_readonly);CHANGED(maxwell_frag_subgroups);
    CHANGED(maxwell_h2f_bits);CHANGED(maxwell_gmem_mirror);CHANGED(maxwell_gmem_zero);
    CHANGED(maxwell_gfind_cache);CHANGED(maxwell_half_native);CHANGED(maxwell_precise_pos);
    CHANGED(maxwell_scalar_cbuf);CHANGED(maxwell_ieee_minmax);CHANGED(maxwell_robust_cbuf);
    TOGGLED(maxwell_static_stacks);TOGGLED(maxwell_brx_table);TOGGLED(gc_lazy);TOGGLED(maxwell_gmem_fs_miss);
    CHANGED(chunk_shift);CHANGED(heap_base);CHANGED(heap_size);CHANGED(g_vk.api);
    CHANGED(g_vk.props.vendorID);CHANGED(g_vk.props.deviceID);CHANGED(g_vk.props.driverVersion);
    CHANGED(g_vk.props.apiVersion);CHANGED(g_vk.props.pipelineCacheUUID[7]);
    CHANGED(g_vk.props.limits.maxUniformBufferRange);CHANGED(g_vk.props.limits.maxStorageBufferRange);
    CHANGED(key_size);CHANGED(game_identity[3]);
    CHANGED(maxwell_vote_stages);CHANGED(maxwell_storage_image_write);CHANGED(maxwell_clip_distance);
    write_bytes("runtime.fixture","different implementation",24);h=fresh();CHECK(memcmp(&base,&h,sizeof h));
    write_bytes("runtime.fixture","synthetic implementation",24);
    CHECK(!setenv("LARECOMP_CACHE_TEST","",1));h=fresh();CHECK(memcmp(&base,&h,sizeof h));
    CHECK(!setenv("LARECOMP_CACHE_TEST","0",1));HsHeader zero=fresh();CHECK(memcmp(&zero,&h,sizeof h));
    CHECK(!setenv("TU_DEBUG","nolrz",1));h=fresh();CHECK(memcmp(&zero,&h,sizeof h));
    CHECK(!unsetenv("LARECOMP_CACHE_TEST"));CHECK(!unsetenv("TU_DEBUG"));h=fresh();same_header(&base,&h);
    extern char **environ;
    char *order1[]={"LARECOMP_TEST_A=one","LARECOMP_TEST_B=two",NULL};
    char *order2[]={"LARECOMP_TEST_B=two","LARECOMP_TEST_A=one",NULL};
    char **saved=environ;environ=order1;h=fresh();environ=order2;HsHeader reordered=fresh();same_header(&h,&reordered);environ=saved;
    verify_empty("headstart.test",&base);
    uint8_t payload[736]={0};
    FILE*f=hs_cache_open("headstart.test",&base);CHECK(f);HsRec rec={HS_PIPE,sizeof payload};
    CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(payload,sizeof payload,1,f)==1);CHECK(!fclose(f));
    f=hs_cache_open("headstart.test",&base);CHECK(f);CHECK(ftell(f)==sizeof base+sizeof rec+sizeof payload);CHECK(!fclose(f));
    // Same-size key-mode identity mismatch must discard old records.
    TOGGLED(g_vk.vids);g_vk.vids=true;h=fresh();verify_empty("headstart.test",&h);g_vk.vids=false;
    for(size_t i=0;i<sizeof base;++i) { h=base;((uint8_t*)&h)[i]^=0x80;write_bytes("headstart.test",&h,sizeof h);verify_empty("headstart.test",&base); }
    for(size_t i=0;i<sizeof base;++i) { write_bytes("headstart.test",&base,i);verify_empty("headstart.test",&base); }
    uint8_t record[sizeof(HsRec)+sizeof payload];memcpy(record,&rec,sizeof rec);memcpy(record+sizeof rec,payload,sizeof payload);
    for(size_t i=1;i<sizeof record;++i) {
        f=fopen("headstart.test","wb");CHECK(f);CHECK(fwrite(&base,sizeof base,1,f)==1);CHECK(fwrite(record,1,i,f)==i);CHECK(!fclose(f));
        verify_empty("headstart.test",&base);
    }
    rec=(HsRec){99,0};f=fopen("headstart.test","ab");CHECK(f);CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(!fclose(f));verify_empty("headstart.test",&base);
    // Complete program records also validate their internal spans, before replay.
    HsProgHeader program={.chash=1,.cmax={8,8}};
    rec=(HsRec){HS_PROG,sizeof program+16};
    f=fopen("headstart.test","wb");CHECK(f);CHECK(fwrite(&base,sizeof base,1,f)==1);
    CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(&program,sizeof program,1,f)==1);
    CHECK(fwrite(payload,16,1,f)==1);CHECK(!fclose(f));
    f=hs_cache_open("headstart.test",&base);CHECK(f);CHECK(ftell(f)==sizeof base+sizeof rec+rec.size);CHECK(!fclose(f));
    uint64_t bad[][2]={{UINT64_MAX,1},{1,UINT64_MAX},{UINT64_MAX,UINT64_MAX},
                       {17,0},{8,9},{8,7},{0,16},{16,1}};
    for(size_t i=0;i<sizeof bad/sizeof bad[0];++i) {
        program.cmax[0]=bad[i][0];program.cmax[1]=bad[i][1];
        CHECK(!hs_program_size_valid(rec.size,&program));
        f=fopen("headstart.test","wb");CHECK(f);CHECK(fwrite(&base,sizeof base,1,f)==1);
        CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(&program,sizeof program,1,f)==1);
        CHECK(fwrite(payload,16,1,f)==1);CHECK(!fclose(f));verify_empty("headstart.test",&base);
    }
    // The exact legacy overflow: sizeof(header)+UINT64_MAX+1 == sizeof(header).
    program.cmax[0]=UINT64_MAX;program.cmax[1]=1;rec.size=sizeof program;
    CHECK((uint64_t)sizeof program+program.cmax[0]+program.cmax[1]==rec.size);
    CHECK(!hs_program_size_valid(rec.size,&program));
    f=fopen("headstart.test","wb");CHECK(f);CHECK(fwrite(&base,sizeof base,1,f)==1);
    CHECK(fwrite(&rec,sizeof rec,1,f)==1);CHECK(fwrite(&program,sizeof program,1,f)==1);CHECK(!fclose(f));verify_empty("headstart.test",&base);
    program.cmax[0]=8;program.cmax[1]=0;CHECK(hs_program_size_valid(sizeof program+8,&program));
    CHECK(!hs_program_size_valid(sizeof program-1,&program));
    program.cmax[0]=(uint64_t)UINT32_MAX-sizeof program;CHECK(hs_program_size_valid(UINT32_MAX,&program));
    ++program.cmax[0];CHECK(!hs_program_size_valid(UINT32_MAX,&program));
    // LHS5 has four independent spans: constants must fit their own stage and
    // cannot borrow bytes from another stage or wrap the record length.
    program=(HsProgHeader){.chash=1,.cmax={8,8},.csize={4,4}};
    CHECK(hs_program_size_valid(sizeof program+24,&program));
    CHECK(!hs_program_size_valid(sizeof program+23,&program));
    for(int stage=0;stage<2;stage++) {
        program.csize[stage]=UINT64_MAX;
        CHECK(!hs_program_size_valid(sizeof program+24,&program));
        program.csize[stage]=4;
        program.cmax[stage]=0;
        CHECK(!hs_program_size_valid(sizeof program+16,&program));
        program.cmax[stage]=8;
    }
    uint32_t legacy[]={0x3253484c,736};write_bytes("headstart.test",legacy,sizeof legacy);verify_empty("headstart.test",&base);
    // Writer-first validation, one source hash, no record interleaving, unchanged saves.
    mkdir("cache",0755);write_bytes("cache/headstart.bin",legacy,sizeof legacy);write_bytes("save.fixture","save untouched",14);
    unsigned before=path_calls;pthread_t threads[4];
    for(int i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,append_thread,NULL));
    for(int i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    CHECK(path_calls==before+1);CHECK(hs_out);CHECK(!fclose(hs_out));hs_out=NULL;
    f=hs_cache_open("cache/headstart.bin",&base);CHECK(f);CHECK(ftell(f)==sizeof base+80*(sizeof(HsRec)+sizeof payload));CHECK(!fclose(f));
    CHECK(file_size("save.fixture")==14);
    hs_prepared=false;path_available=false;before=path_calls;
    hs_write(HS_PIPE,payload,sizeof payload,NULL,0,NULL,0);hs_write(HS_PIPE,payload,sizeof payload,NULL,0,NULL,0);
    CHECK(path_calls==before+1);CHECK(!hs_out);
    printf("headstart: %u checks\n",checks);
}
static uint64_t compile_shader(int stage,bool optimized) {
    const char *src="public synthetic GLSL fixture";uint64_t h=src_hash(src,stage);
    size_t n=0;unsigned before=compiles;
    uint32_t*p=vk_glsl_to_spirv(src,stage,"test",&n);CHECK(p && n==6);free(p);
    CHECK(compiles==before+1);CHECK(selected_opt==(int)optimized);
    p=vk_glsl_to_spirv(src,stage,"test",&n);CHECK(p && n==6);free(p);CHECK(compiles==before+1);
    return h;
}
static void spirv_tests(const char *mode) {
    unsetenv("LARECOMP_VK_OPT");unsetenv("LARECOMP_VK_OPT_FS");unsetenv("LARECOMP_VK_NO_CONTRACT");unsetenv("LARECOMP_VK_NO_SPIRV_CACHE");
    bool fs=!strcmp(mode,"fs"), nc=!strcmp(mode,"nc");
    if(fs)setenv("LARECOMP_VK_OPT_FS","1",1);
    if(nc)setenv("LARECOMP_VK_NO_CONTRACT","0",1);
    g_vk.api=VK_API_VERSION_1_3;
    uint64_t before_compiler=src_hash("compiler fixture",STAGE_VERTEX);
    ++compiler_identity[2];CHECK(src_hash("compiler fixture",STAGE_VERTEX)!=before_compiler);--compiler_identity[2];
    path_available=false;CHECK(!src_hash("compiler fixture",STAGE_VERTEX));path_available=true;
    uint64_t vertex=compile_shader(STAGE_VERTEX,false), fragment=compile_shader(STAGE_FRAGMENT,fs);
    CHECK(vertex!=fragment);CHECK(no_contracts==!nc);
    CHECK(src_hash("another source",STAGE_VERTEX)!=vertex);
    setenv("LARECOMP_VK_OPT","0",1);uint64_t optimized=compile_shader(STAGE_VERTEX,true);CHECK(optimized!=vertex);
    CHECK(src_hash("public synthetic GLSL fixture",STAGE_FRAGMENT)==fragment || !fs);
    setenv("LARECOMP_VK_OPT","",1);CHECK(src_hash("public synthetic GLSL fixture",STAGE_VERTEX)==optimized);
    setenv("LARECOMP_VK_OPT","1",1);CHECK(src_hash("public synthetic GLSL fixture",STAGE_VERTEX)==optimized);
    unsetenv("LARECOMP_VK_OPT");CHECK(src_hash("public synthetic GLSL fixture",STAGE_VERTEX)==vertex);
    g_vk.api=VK_API_VERSION_1_2;uint64_t old=compile_shader(STAGE_VERTEX,false);CHECK(old!=vertex);CHECK(selected_env==shaderc_env_version_vulkan_1_2);
    g_vk.api=VK_API_VERSION_1_1;CHECK(compile_shader(STAGE_VERTEX,false)!=old);CHECK(selected_env==shaderc_env_version_vulkan_1_1);
    printf("spirv-%s: %u checks vertex=%016llx\n",mode,checks,(unsigned long long)vertex);
}
int main(int argc,char**argv) {
    CHECK(argc==2);if(!strcmp(argv[1],"headstart"))headstart_tests();else spirv_tests(argv[1]);return 0;
}
