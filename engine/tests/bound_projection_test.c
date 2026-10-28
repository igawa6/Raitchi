/* Actual production staging capture + actual guest-copy cache, with synthetic owned memory. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
typedef struct { uint64_t buf,off; void *ptr; } VkSpan;
static _Alignas(256) unsigned char guest[131072], transient[1048576];
#define GUEST_LO ((uint64_t)(uintptr_t)guest)
#define GUEST_HI (GUEST_LO+sizeof guest)
static uint64_t pending=1,used,frame=17,g_cur_fs_hash;
static unsigned copies,checks;
static bool active;
static struct { struct { struct { uint64_t maxUniformBufferRange; } limits; } props; } g_vk;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();} } while(0)
#define LOG(...) ((void)0)
static uint64_t vk_pending_seq(void){return pending;}
static uint64_t vk_frame_counter(void){return frame;}
static uint64_t gmem_heap_size(void){return 1u<<20;}
static bool gc_scan_needed(void){return true;}
static void gc_scan_pointers(const uint32_t*p,uint64_t n){CHECK(p&&n<=sizeof guest);}
static VkSpan vk_transient_copy(const void*p,uint64_t n,uint64_t align){used=(used+align-1)&~(align-1);CHECK(n<=sizeof transient-used);VkSpan s={1,used,transient+used};memcpy(s.ptr,p,n);used+=n;copies++;return s;}
#include "bound_projection.cache.h"
static bool material_probe_bound_enabled(void){return active;}
#include "bound_projection.production.h"
static void name(char*p,size_t n,unsigned f,int stage,int k,bool meta){snprintf(p,n,"bound-projection-%u-%d-%d.bin%s",f,stage,k,meta?".json":"");}
static void verify(unsigned f,int stage,int k,const void*bytes,size_t n){char p[128];name(p,sizeof p,f,stage,k,false);FILE*fp=fopen(p,"rb");CHECK(fp);unsigned char b[8193];CHECK(fread(b,1,sizeof b,fp)==n);CHECK(feof(fp));CHECK(!memcmp(b,bytes,n));CHECK(!fclose(fp));}
static void metadata(unsigned f,int stage,int k,const char*needle){char p[128],b[2048];name(p,sizeof p,f,stage,k,true);FILE*fp=fopen(p,"rb");CHECK(fp);size_t n=fread(b,1,sizeof b-1,fp);b[n]=0;CHECK(strstr(b,needle));CHECK(!fclose(fp));}
static bool exists(unsigned f,int stage,int k){char p[128];name(p,sizeof p,f,stage,k,false);return !access(p,F_OK);}
int main(void){char dir[]="/tmp/bound-projection-XXXXXX";CHECK(mkdtemp(dir));CHECK(!chdir(dir));g_vk.props.limits.maxUniformBufferRange=65536;
 for(size_t i=0;i<sizeof guest;i++)guest[i]=(unsigned char)(i*13+3);
 VkSpan old=guest_copy(GUEST_LO,1136);CHECK(copies==1);unsigned char saved[1136];memcpy(saved,old.ptr,sizeof saved);memset(guest,0xa5,sizeof saved);VkSpan hit=guest_copy(GUEST_LO,1136);CHECK(copies==1&&hit.ptr==old.ptr&&memcmp(hit.ptr,guest,1136));
 g_cur_fs_hash=UINT64_C(0x44b584f0d922d49d);material_bound_projection(1,4,hit,1136,GUEST_LO,false);CHECK(!exists(17,1,4));
 active=true;g_cur_fs_hash++;material_bound_projection(1,4,hit,1136,GUEST_LO,false);CHECK(!exists(17,1,4));g_cur_fs_hash--;material_bound_projection(0,4,hit,1136,GUEST_LO,false);CHECK(!exists(17,0,4));
 material_bound_projection(1,4,hit,1136,GUEST_LO,false);verify(17,1,4,saved,1136);metadata(17,1,4,"\"source\":\"exact-bound-staging\"");metadata(17,1,4,"\"range\":1136");metadata(17,1,4,"\"unbound_zero\":false");
 memset(hit.ptr,0xb1,1136);material_bound_projection(1,4,hit,1136,GUEST_LO,false);verify(17,1,4,saved,1136);CHECK(!memcmp(guest,(unsigned char[1]){0xa5},1));
 unsigned char zero[8192]={0};material_bound_projection(0,12,(VkSpan){2,0,NULL},256,0,true);verify(17,0,12,zero,256);metadata(17,0,12,"\"unbound_zero\":true");
 VkSpan large=guest_copy(GUEST_LO+16384,9000);material_bound_projection(1,7,large,9000,GUEST_LO+16384,false);verify(17,1,7,large.ptr,8192);metadata(17,1,7,"\"truncated\":true");metadata(17,1,7,"\"range\":9000");
 frame++;guest_copy_flush();VkSpan fresh=guest_copy(GUEST_LO,1136);CHECK(copies==3&&fresh.ptr!=hit.ptr);material_bound_projection(1,4,fresh,1136,GUEST_LO,false);verify(18,1,4,guest,1136);metadata(18,1,4,"\"frame\":18");
 g_vk.props.limits.maxUniformBufferRange=32;material_bound_projection(1,7,fresh,1136,GUEST_LO,false);verify(18,1,7,guest,32);metadata(18,1,7,"\"range\":32");
 frame++;material_bound_projection(1,4,(VkSpan){1,0,NULL},32,GUEST_LO,false);CHECK(!exists(19,1,4));metadata(19,1,4,"\"status\":\"invalid-or-unmapped-span\"");
 material_bound_projection(1,7,(VkSpan){1,0,(void*)(uintptr_t)(UINTPTR_MAX-15)},32,0,false);CHECK(!exists(19,1,7));metadata(19,1,7,"\"captured_bytes\":0");
 material_bound_projection(0,12,(VkSpan){1,0,guest},0,GUEST_LO,false);CHECK(!exists(19,0,12));metadata(19,0,12,"\"range\":0");
 frame++;material_bound_projection(0,12,(VkSpan){1,0,NULL},9000,0,true);verify(20,0,12,zero,32);
 active=false;frame++;material_bound_projection(1,4,fresh,32,GUEST_LO,false);CHECK(!exists(21,1,4));
 printf("{\"state\":\"passed\",\"checks\":%u,\"cache_copies\":%u,\"source_unchanged\":true}\n",checks,copies);
 CHECK(!chdir("/tmp"));char cmd[256];snprintf(cmd,sizeof cmd,"rm -rf -- %s",dir);CHECK(!system(cmd));return 0;
}
