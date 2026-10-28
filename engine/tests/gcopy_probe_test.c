/* Real guest-copy cache bodies, controlled transient storage and guest reservation. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef struct{uint64_t buf,off;void*ptr;}VkSpan;
static _Alignas(256) unsigned char guest[131072],transient[1048576];
#define GUEST_LO ((uint64_t)(uintptr_t)guest)
#define GUEST_HI (GUEST_LO+sizeof guest)
static uint64_t pending=1,used;
static unsigned copies,scans,checks,logs;
static char messages[64][512];
static const char*mode,*bypass_mode;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static const char*probe_env(const char*n){return !strcmp(n,"LARECOMP_GCOPY_PROBE")?mode:!strcmp(n,"LARECOMP_GCOPY_BYPASS_SMALL")?bypass_mode:NULL;}
#define getenv probe_env
static void probe_log(const char*fmt,...){CHECK(logs<64);va_list ap;va_start(ap,fmt);CHECK(vsnprintf(messages[logs++],512,fmt,ap)<512);va_end(ap);}
#define LOG(...) probe_log(__VA_ARGS__)
static uint64_t vk_pending_seq(void){return pending;}
static uint64_t gmem_heap_size(void){return 1u<<20;}
static bool gc_scan_needed(void){return true;}
static void gc_scan_pointers(const uint32_t*p,uint64_t n){CHECK(p&&(n<=sizeof guest));scans++;}
static VkSpan vk_transient_copy(const void*p,uint64_t n,uint64_t align){used=(used+align-1)&~(align-1);CHECK(n<=sizeof transient-used);VkSpan s={1,used,transient+used};memcpy(s.ptr,p,(size_t)n);used+=n;copies++;return s;}
#include "gcopy_probe.production.h"
int main(int argc,char**argv){
    CHECK(argc==2||argc==3);mode=argv[1];bypass_mode=argc==3?argv[2]:NULL;bool on=!strcmp(mode,"1");
    for(unsigned i=0;i<sizeof guest;i++)guest[i]=(unsigned char)i;
    if(bypass_mode&&!strcmp(bypass_mode,"1")){
        VkSpan a=guest_copy(GUEST_LO,64),b=guest_copy(GUEST_LO,64);CHECK(copies==2&&a.ptr!=b.ptr);
        guest[7]=0x99;VkSpan c=guest_copy(GUEST_LO,64);CHECK(copies==3&&c.ptr!=b.ptr&&((unsigned char*)c.ptr)[7]==0x99);
        a=guest_copy(GUEST_LO+8192,8193);b=guest_copy(GUEST_LO+8192,8193);CHECK(copies==4&&a.ptr==b.ptr);
        guest[8192+3]=0xa5;c=guest_copy(GUEST_LO+8192,8193);CHECK(copies==4&&a.ptr==c.ptr&&((unsigned char*)c.ptr)[3]==3);
        a=guest_copy(GUEST_LO+32768,8192);b=guest_copy(GUEST_LO+32768,8192);CHECK(copies==6&&a.ptr!=b.ptr);
        CHECK(!logs&&!scans&&!prof_scan_skipped);
        printf("{\"state\":\"passed\",\"checks\":%u,\"small_bypass\":true,\"large_cache_retained\":true}\n",checks);return 0;
    }
    /* Large bootstrap tables cannot consume the camera/model probe budget. */
    unsigned char large_before[65536]={0};
    for(unsigned i=0;i<100;i++)gcopy_probe_hit(GUEST_LO,65536,(VkSpan){.ptr=large_before});
    CHECK(!logs);
    VkSpan a=guest_copy(GUEST_LO,64);CHECK(copies==1&&!logs);
    VkSpan b=guest_copy(GUEST_LO,64);CHECK(copies==1&&!logs&&a.ptr==b.ptr&&a.off==b.off);
    guest[7]=0x99;guest[8]=0x44;unsigned char saved[64];memcpy(saved,guest,64);
    b=guest_copy_ex(GUEST_LO,64,true);CHECK(copies==1&&scans==1);CHECK(a.ptr==b.ptr&&a.off==b.off);
    CHECK(!memcmp(guest,saved,64));CHECK(((unsigned char*)a.ptr)[7]==7&&((unsigned char*)a.ptr)[8]==8);
    CHECK(logs==(unsigned)on);
    if(on){CHECK(strstr(messages[0],"size 64 seq 1 gen 0 first 7 bytes 32"));CHECK(strstr(messages[0],"cached 0708090a"));CHECK(strstr(messages[0],"guest 9944090a"));}
    guest_copy_ex(GUEST_LO,64,true);CHECK(scans==1&&prof_scan_skipped==1&&logs==(unsigned)on);
    unsigned previous=logs;guest_copy_flush();b=guest_copy(GUEST_LO,64);CHECK(copies==2&&b.ptr!=a.ptr&&logs==previous);CHECK(!memcmp(b.ptr,guest,64));
    guest[63]^=0xff;guest_copy(GUEST_LO,64);CHECK(logs==previous+(unsigned)on);
    if(on)CHECK(strstr(messages[logs-1],"gen 1 first 63 bytes 1"));
    pending++;previous=logs;b=guest_copy(GUEST_LO,64);CHECK(copies==3&&logs==previous);CHECK(!memcmp(b.ptr,guest,64));
    /* Invalid guest windows and overflowing/oversized ranges must be rejected
       before touching even this deliberately invalid cached pointer. */
    VkSpan invalid={.ptr=(void*)(uintptr_t)1};
    gcopy_probe_hit(GUEST_LO-1,4,invalid);gcopy_probe_hit(GUEST_HI,4,invalid);
    gcopy_probe_hit(GUEST_HI-4,8,invalid);gcopy_probe_hit(UINT64_MAX-1,8,invalid);
    gcopy_probe_hit(GUEST_LO,UINT64_MAX,invalid);gcopy_probe_hit(GUEST_LO,8193,invalid);
    gcopy_probe_hit(GUEST_LO,0,invalid);gcopy_probe_hit(GUEST_LO,4,(VkSpan){0});CHECK(logs==previous);
    /* Saturation cannot alter the cached GPU copy or source bytes. */
    for(unsigned i=0;i<100;i++){guest[0]=(unsigned char)(i+1);guest_copy(GUEST_LO,64);}
    CHECK(logs==(on?3u:0u));CHECK(copies==3);CHECK(((unsigned char*)b.ptr)[0]==0);CHECK(guest[0]==100);
    for(unsigned i=0;i<100;i++){
        pending++;unsigned old_copies=copies;unsigned char old=guest[0];b=guest_copy(GUEST_LO,64);
        guest[0]^=1;VkSpan c=guest_copy(GUEST_LO,64);CHECK(copies==old_copies+1&&c.ptr==b.ptr);CHECK(((unsigned char*)c.ptr)[0]==old);
    }
    CHECK(logs==(on?64u:0u)&&copies==103&&guest[0]==100);
    printf("{\"state\":\"passed\",\"checks\":%u,\"mismatch_logs\":%u,\"copies\":%u,\"policy_unchanged\":true}\n",checks,logs,copies);return 0;
}
