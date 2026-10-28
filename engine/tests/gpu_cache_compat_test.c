/* Asset-free tests of the actual metadata/header/IO and build/game/env helper. */
#include "shared_gpu_cache.h"
#include "common_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned checks, allocations;
void *cache_test_malloc(size_t n) { ++allocations; return malloc(n); }
#define C(x) do { ++checks; if (!(x)) { fprintf(stderr,"%d %s\n",__LINE__,#x); abort(); } } while (0)
static NativeManifest native;
static TitleManifest aot;
static bool native_on = true, aot_on;
const NativeManifest *shared_native_manifest_current(void) { return native_on ? &native : NULL; }
const TitleManifest *shared_manifest_current(void) { return aot_on ? &aot : NULL; }
static void writefile(const char *name,const void *bytes,size_t n) {
    FILE *f=fopen(name,"wb"); C(f); C(fwrite(bytes,1,n,f)==n); C(!fclose(f));
}
static void put32(uint8_t *p,uint32_t v) { for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static SharedGpuDevice device;
static uint8_t blob[256];
static void reject(void) { size_t n=99;bool m=true;void *p=shared_gpu_cache_load("cache.bin","cache.tag",&device,"GPU, vendor 123 device 456, driver 7, cache 00000000000000000000000000000000",&n,&m);C(!p && !n && !m); }
static void accept(bool migration) {
    size_t n=99;bool m=false;void *p=shared_gpu_cache_load("cache.bin","cache.tag",&device,"GPU, vendor 123 device 456, driver 7, cache 00000000000000000000000000000000",&n,&m);
    C(p && n==sizeof blob && m==migration); C(!memcmp(p,blob,n));free(p);
}
int main(int argc,char **argv) {
    C(argc==2); uint8_t id[32];bool ready=strcmp(argv[1],"bootstrap")!=0;
    C(shared_gpu_build_identity(false,id)==ready);C(shared_gpu_build_identity(true,id)==ready);
    if(!ready) { printf("bootstrap: %u checks\n",checks);return 0; }
    native.title_id=1;native.module_count=1;native.program_id=0x1000;
    native.modules[0].module_id=1;native.modules[0].nso_sha256[0]=1;native.modules[0].nso_build_id[0]=2;
    native.modules[0].mapped_size=4096;native.modules[0].text_size=1024;
    uint8_t game[32],other[32];C(shared_gpu_game_identity(game));native.contract_sha256[0]++;
    C(shared_gpu_game_identity(other) && !memcmp(game,other,32));
    native.modules[0].nso_sha256[2]++;C(shared_gpu_game_identity(other) && memcmp(game,other,32));native.modules[0].nso_sha256[2]--;
    native.program_id++;C(shared_gpu_game_identity(other) && memcmp(game,other,32));native.program_id--;
    native.modules[0].canonical_base++;C(shared_gpu_game_identity(other) && memcmp(game,other,32));native.modules[0].canonical_base--;
    native_on=false;C(!shared_gpu_game_identity(other));aot_on=true;aot.title_id=1;aot.profile.module_count=1;
    aot.modules[0].nso_sha256[0]=1;C(shared_gpu_game_identity(game));aot.library_sha256[0]++;
    C(shared_gpu_game_identity(other) && !memcmp(game,other,32));aot.modules[0].nso_build_id[0]++;
    C(shared_gpu_game_identity(other) && memcmp(game,other,32));native_on=true;
    extern char **environ;char **saved=environ;
    char *first[]={"TU_DEBUG=x","MESA_TEST=y",NULL},*second[]={"MESA_TEST=y","TU_DEBUG=x",NULL};
    environ=first;C(shared_gpu_environment(game));environ=second;C(shared_gpu_environment(other));C(!memcmp(game,other,32));
    char *empty[]={"TU_DEBUG=",NULL},*absent[]={NULL};environ=empty;C(shared_gpu_environment(game));environ=absent;C(shared_gpu_environment(other));C(memcmp(game,other,32));
    char *duplicates[]={"TU_DEBUG=first","TU_DEBUG=second",NULL};environ=duplicates;C(!shared_gpu_environment(game));
    char *diag_off[]={"LARECOMP_NO_SCALE=1","LARECOMP_LODMIN=0","TU_DEBUG=x",NULL};
    char *diag_on[]={"LARECOMP_FONT_TRACE=1","LARECOMP_LOG_COMMAND_POOLS=1","LARECOMP_SAMPLING_TRACE=1","TU_DEBUG=x","LARECOMP_NO_SCALE=1","LARECOMP_LODMIN=0",NULL};
    environ=diag_off;C(shared_gpu_environment(game));environ=diag_on;C(shared_gpu_environment(other));C(!memcmp(game,other,32));
    char *diag_values[]={"LARECOMP_FONT_TRACE=0","LARECOMP_LOG_COMMAND_POOLS=0","LARECOMP_SAMPLING_TRACE=0","TU_DEBUG=x","LARECOMP_NO_SCALE=1","LARECOMP_LODMIN=0",NULL};
    environ=diag_values;C(shared_gpu_environment(other));C(!memcmp(game,other,32));
    char *render_change[]={"LARECOMP_SAMPLING_TRACE=1","TU_DEBUG=x","LARECOMP_NO_SCALE=0","LARECOMP_LODMIN=0",NULL};
    environ=render_change;C(shared_gpu_environment(other));C(memcmp(game,other,32));
    char *driver_change[]={"LARECOMP_NO_SCALE=1","LARECOMP_LODMIN=0","TU_DEBUG=y",NULL};
    environ=driver_change;C(shared_gpu_environment(other));C(memcmp(game,other,32));
    char *direct_off[]={"LARECOMP_VK_DIRECT_SPIRV=0",NULL};
    char *direct_on[]={"LARECOMP_VK_DIRECT_SPIRV=1",NULL};
    environ=direct_off;C(shared_gpu_environment(game));environ=direct_on;C(shared_gpu_environment(other));C(memcmp(game,other,32));
    environ=absent;C(shared_gpu_environment(other));C(memcmp(game,other,32));
    char *duplicate_ignored[]={"LARECOMP_FONT_TRACE=0","LARECOMP_FONT_TRACE=1",NULL};
    environ=duplicate_ignored;C(!shared_gpu_environment(game));
    char *duplicate_ignored_active[]={"LARECOMP_SAMPLING_TRACE=1","LARECOMP_NO_SCALE=0","LARECOMP_NO_SCALE=1",NULL};
    environ=duplicate_ignored_active;C(!shared_gpu_environment(game));
    char *bounded[258];char env_names[257][32];
    for(unsigned i=0;i<257;i++){snprintf(env_names[i],sizeof env_names[i],"LARECOMP_FONT_TRACE_%03u=1",i);bounded[i]=env_names[i];}bounded[257]=NULL;
    /* Unknown similarly prefixed flags are not allowlisted; all relevant names count toward the cap. */
    environ=bounded;C(!shared_gpu_environment(game));
    char *two_fifty_six[257];for(unsigned i=0;i<256;i++)two_fifty_six[i]=bounded[i];two_fifty_six[256]=NULL;
    environ=two_fifty_six;C(shared_gpu_environment(game));C(!shared_gpu_environment(NULL));
    environ=saved;
    device=(SharedGpuDevice){.vendor=0x123,.device=0x456,.driver=7,.api=1};
    C(shared_gpu_environment(device.environment));
    for(size_t i=32;i<sizeof blob;i++)blob[i]=(uint8_t)i;
    put32(blob,32);put32(blob+4,1);put32(blob+8,device.vendor);put32(blob+12,device.device);
    SharedGpuCacheMeta meta;C(shared_gpu_cache_metadata(&meta,blob,sizeof blob,&device));
    writefile("cache.bin",blob,sizeof blob);C(shared_gpu_cache_tag_write("cache.tag",&meta));accept(false);
    /* Unrelated source/build identities never enter opaque driver metadata. */
    uint8_t app_code[100]={1};app_code[0]++;(void)app_code;accept(false);
    unsigned before=allocations;
    device.driver++;reject();C(allocations==before);device.driver--;device.api++;reject();device.api--;
    device.vendor++;reject();device.vendor--;device.device++;reject();device.device--;
    device.uuid[7]++;reject();device.uuid[7]--;device.environment[0]++;reject();device.environment[0]--;
    for(size_t i=0;i<sizeof blob;i++) {
        uint8_t copy[sizeof blob];memcpy(copy,blob,sizeof blob);copy[i]^=1;writefile("cache.bin",copy,sizeof copy);reject();
    }
    writefile("cache.bin",blob,sizeof blob);
    for(size_t n=0;n<sizeof blob;n++) { writefile("cache.bin",blob,n);reject(); }
    writefile("cache.bin",blob,sizeof blob);
    for(size_t i=0;i<sizeof meta;i++) { SharedGpuCacheMeta bad=meta;((uint8_t*)&bad)[i]^=1;writefile("cache.tag",&bad,sizeof bad);reject(); }
    for(size_t n=0;n<sizeof meta;n++) { writefile("cache.tag",&meta,n);reject(); }
    const char *legacy="beta_4 translator 012345abcdef cache 1 | GPU, vendor 123 device 456, driver 7, cache 00000000000000000000000000000000";
    writefile("cache.tag",legacy,strlen(legacy));accept(true);
    struct stat legacy_stat; C(!stat("cache.tag",&legacy_stat));C((size_t)legacy_stat.st_size==strlen(legacy));
    /* Mirror successful Vulkan acceptance: only then commit the new sidecar. */
    C(shared_gpu_cache_tag_write("cache.tag",&meta));accept(false);
    FILE *f=fopen("cache.bin","rb");C(f);uint8_t unchanged[sizeof blob];C(fread(unchanged,1,sizeof blob,f)==sizeof blob);C(!fclose(f));C(!memcmp(unchanged,blob,sizeof blob));
    for(size_t i=0;i<strlen(legacy);i++) {
        /* Translator hex may legitimately differ; all other alterations reject. */
        if(i>=strlen("beta_4 translator ") && i<strlen("beta_4 translator ")+12)continue;
        char bad[256];strcpy(bad,legacy);bad[i]^=0x80;writefile("cache.tag",bad,strlen(legacy));reject();
    }
    char badlegacy[256];strcpy(badlegacy,legacy);badlegacy[strlen("beta_4 translator ")]='g';writefile("cache.tag",badlegacy,strlen(legacy));reject();
    /* A killed save leaves mismatched old metadata/new blob; never accepted. */
    C(shared_gpu_cache_tag_write("cache.tag",&meta));blob[100]++;writefile("cache.bin",blob,sizeof blob);reject();blob[100]--;
    writefile("cache.bin",blob,sizeof blob);remove("cache.tag");before=allocations;reject();C(allocations==before);
    C(shared_gpu_cache_tag_write("cache.tag",&meta));
    f=fopen("cache.bin","r+b");C(f);C(!ftruncate(fileno(f),1ull<<30));C(!fclose(f));
    before=allocations;reject();C(allocations==before);writefile("cache.bin",blob,sizeof blob);remove("cache.tag");
    mkdir("cache.tag.tmp",0700);C(!shared_gpu_cache_tag_write("cache.tag",&meta));reject();rmdir("cache.tag.tmp");
    C(shared_gpu_cache_tag_write("cache.tag",&meta));accept(false);
    printf("gpu metadata/header/IO: %u checks\n",checks);
}
