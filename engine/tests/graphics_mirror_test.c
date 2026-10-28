/* Actual mirror backend bodies with a controlled Vulkan command recorder. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <setjmp.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);} }while(0)
typedef uint64_t VkBuffer,VkCommandBuffer,VkDeviceSize,VmaAllocation;
typedef struct {int sType;uint64_t size,usage;} VkBufferCreateInfo;
typedef struct {int usage;uint64_t flags;} VmaAllocationCreateInfo;
typedef struct {void *pMappedData;} VmaAllocationInfo;
typedef struct {uint64_t srcOffset,dstOffset,size;} VkBufferCopy;
typedef struct {VkBuffer buf;uint64_t off;void *ptr;} VkSpan;
typedef struct {bool uses_gmem_store,uses_gmem_map,uses_gmem_readonly;} ShaderInfo;
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 1
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 1
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT 2
#define VK_BUFFER_USAGE_INDEX_BUFFER_BIT 4
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT 8
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 16
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT 32
static uint64_t uniform_forward_write_epoch;
static bool fixture_uniform_forward;
static bool uniform_forward_enabled(void) { (void)uniform_forward_write_epoch; return fixture_uniform_forward; }
#define VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE 1
#define VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT 1
#define VMA_ALLOCATION_CREATE_MAPPED_BIT 2
#define VK_CHECK(x) CHECK((x)==0)
static void mirror_log(const char *format,...) {(void)format;}
#define LOG(...) mirror_log(__VA_ARGS__)
#define GMAP_MAX 32
static _Alignas(16) uint32_t guest[8192];
#define HEAP_BASE ((uint64_t)(uintptr_t)guest)
#define HEAP_MAX (sizeof guest)
static struct {bool host_import;int vma;} g_vk;
static bool maxwell_gmem_readonly,in_pass,mir_graphics_prepared,mir_graphics_preparing;
uint64_t mir_graphics_seq,stat_upload_bytes;
uint32_t gmap_tab[(GMAP_MAX+1)*4];
const char *wait_why;
typedef struct {uint64_t addr,size;} Range;
static struct {Range sbuf[6][16],ubuf[6][18];} st;
static jmp_buf fatal_env;
static bool fatal_expected;
static void fatal(const char *s,...) __attribute__((noreturn));
static void fatal(const char *s,...){if(fatal_expected)longjmp(fatal_env,1);fprintf(stderr,"%s",s);exit(1);}
#define FATAL(...) fatal(__VA_ARGS__)
static unsigned char *mapped;
static unsigned char transient[1<<20];
static uint64_t transient_used,pending=1,done;
static bool submit_on_end;
enum {OP_BARRIER,OP_COPY,OP_DRAW,OP_BEGIN,OP_END};
typedef struct {int kind;VkBuffer src,dst;VkBufferCopy copy;uint64_t read,write;uint32_t value;bool indirect;} Op;
static Op ops[8192];static unsigned nops,draws,readbacks;
static void append(Op op){CHECK(nops<8192);ops[nops++]=op;}
static uint64_t vk_pending_seq(void){return pending;}
static uint64_t vk_completed_seq(void){return done;}
#ifndef MIRROR_WAIT_HOOK
#define MIRROR_WAIT_HOOK() ((void)0)
#endif
static void vk_wait_seq(uint64_t seq){CHECK(seq<pending);MIRROR_WAIT_HOOK();done=seq;}
static VkCommandBuffer vk_cmd(void){return 1;}
static VkCommandBuffer vk_upload_cmd(void){return 2;}
static void vk_end_pass(void){if(in_pass){append((Op){.kind=OP_END});in_pass=false;if(submit_on_end)pending++;}}
static void begin_pass(void){CHECK(!in_pass);append((Op){.kind=OP_BEGIN});in_pass=true;}
static void vk_full_barrier(VkCommandBuffer cb){CHECK(!in_pass||cb==2);append((Op){.kind=OP_BARRIER});}
static int vmaCreateBuffer(int v,const VkBufferCreateInfo *bi,const VmaAllocationCreateInfo *ai,VkBuffer *b,VmaAllocation *a,VmaAllocationInfo *i){
 (void)v;(void)ai;mapped=calloc(1,(size_t)bi->size);CHECK(mapped!=NULL);*b=1;*a=1;i->pMappedData=mapped;return 0;
}
static void vmaFlushAllocation(int v,VmaAllocation a,uint64_t o,uint64_t n){(void)v;(void)a;CHECK(!in_pass);CHECK(o+n<=(64u<<20));}
static void vmaInvalidateAllocation(int v,VmaAllocation a,uint64_t o,uint64_t n){(void)v;(void)a;CHECK(o+n<=(64u<<20));}
static VkSpan vk_transient_copy(const void *data,uint64_t size,uint64_t alignment){
 CHECK(!in_pass);transient_used=(transient_used+alignment-1)&~(alignment-1);CHECK(size<=sizeof transient-transient_used);
 VkSpan sp={2,transient_used,transient+transient_used};memcpy(sp.ptr,data,(size_t)size);transient_used+=size;return sp;
}
static void rec_CmdCopyBuffer(VkCommandBuffer cb,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy *copy){
 CHECK(!in_pass||cb==2);for(unsigned i=0;i<n;i++){append((Op){.kind=OP_COPY,.src=src,.dst=dst,.copy=copy[i]});if(src==1&&dst==1)readbacks++;}
}
#include "graphics_mirror_fragments.h"
static uint64_t offset(int stage,uint64_t address){
 uint32_t *tab=mir_graphics_tables[stage];
 for(unsigned i=0;i<tab[GMAP_MAX*4];i++)if(address-HEAP_BASE>=tab[i*4]&&address-HEAP_BASE-tab[i*4]<tab[i*4+1])
  return (uint64_t)tab[i*4+2]*4+address-HEAP_BASE-tab[i*4];
 CHECK(0);return 0;
}
static void draw_value(int stage,uint64_t address,uint32_t value){
 CHECK(in_pass&&mir_graphics_prepared);append((Op){.kind=OP_DRAW,.write=offset(stage,address),.value=value});draws++;
}
static void draw_read_write(int read_stage,uint64_t read,int write_stage,uint64_t write){
 CHECK(in_pass&&mir_graphics_prepared);append((Op){.kind=OP_DRAW,.read=offset(read_stage,read),.write=offset(write_stage,write),.indirect=true});draws++;
}
static void execute(void){
 for(unsigned i=0;i<nops;i++){
  Op *o=&ops[i];if(o->kind==OP_COPY){unsigned char *src=o->src==1?mapped:transient,*dst=o->dst==1?mapped:transient;memmove(dst+o->copy.dstOffset,src+o->copy.srcOffset,(size_t)o->copy.size);}
  if(o->kind==OP_DRAW){uint32_t value=o->value;if(o->indirect)memcpy(&value,mapped+o->read,4),value++;memcpy(mapped+o->write,&value,4);}
 }
 nops=0;
}
static void cleanup(void){
 CHECK(!in_pass);for(unsigned i=0;i<MIR_SLOTS;i++)if(mir_slot[i].used)mirror_slot_drop(&mir_slot[i]);
 free(mapped);mapped=NULL;mir_ptr=NULL;mir_buf=mir_mem=0;mir_fh=mir_ft=0;mir_head=0;mir_nstored=0;
 memset(mir_snap_memo,0,sizeof mir_snap_memo);memset(&st,0,sizeof st);memset(guest,0,sizeof guest);
 pending=1;done=0;transient_used=0;nops=draws=readbacks=0;mir_graphics_prepared=mir_graphics_preparing=false;submit_on_end=false;
}
static const ShaderInfo writer={true,true,false},reader={false,true,false};
static void bound(int stage,int i,unsigned word,unsigned count){st.sbuf[stage][i].addr=HEAP_BASE+word*4;st.sbuf[stage][i].size=count*4;}
static void submit_merge(void){execute();done=pending;vk_mirror_writeback();pending++;}
static void union_case(void){
 bound(0,0,0,16);bound(1,0,8,16);mirror_prepare_graphics(&writer,&writer);
 CHECK(mir_nstored==1&&mir_graphics_prepared&&!in_pass);CHECK(offset(1,HEAP_BASE+32)==offset(0,HEAP_BASE)+32);
 begin_pass();draw_value(0,HEAP_BASE,7);draw_value(1,HEAP_BASE+32,8);mirror_finish_graphics(1);
 CHECK(!in_pass&&!mir_graphics_prepared&&readbacks==1);submit_merge();CHECK(guest[0]==7&&guest[8]==8);cleanup();
}
static void full_union_case(void){
 for(int i=0;i<16;i++){bound(0,i,(unsigned)i*16,8);bound(1,i,256+(unsigned)i*16,8);}
 mirror_prepare_graphics(&writer,&writer);CHECK(mir_nstored==32);
 CHECK(mir_graphics_tables[0][GMAP_MAX*4]==16&&mir_graphics_tables[1][GMAP_MAX*4]==16);
 begin_pass();for(unsigned i=0;i<16;i++){draw_value(0,HEAP_BASE+i*64,i+1);draw_value(1,HEAP_BASE+1024+i*64,i+101);}
 mirror_finish_graphics(1);CHECK(readbacks==32);submit_merge();for(unsigned i=0;i<16;i++){CHECK(guest[i*16]==i+1);CHECK(guest[256+i*16]==i+101);}cleanup();
}
static void visibility_and_merge(void){
 bound(0,0,0,16);bound(1,0,0,16);
 mirror_prepare_graphics(&reader,&writer);uint64_t initial=offset(0,HEAP_BASE);CHECK(initial==offset(1,HEAP_BASE));
 begin_pass();draw_value(1,HEAP_BASE,5);mirror_finish_graphics(1);
 // Descriptor reuse may skip all per-stage table work: preparation itself
 // must still collect slots and arrange a second draw's readback.
 guest[1]=3;mirror_prepare_graphics(&reader,&writer);CHECK(mir_nstored==1&&offset(0,HEAP_BASE)==initial);
 begin_pass();draw_read_write(0,HEAP_BASE,1,HEAP_BASE+8);mirror_finish_graphics(1);submit_merge();
 CHECK(guest[0]==5&&guest[1]==3&&guest[2]==6); // CPU-only word survives GPU merge
 guest[0]=99;mirror_prepare_graphics(&reader,&writer);begin_pass();draw_read_write(0,HEAP_BASE,1,HEAP_BASE+8);mirror_finish_graphics(1);submit_merge();
 CHECK(guest[0]==99&&guest[1]==3&&guest[2]==100); // CPU changes uploaded before later shader read
 mirror_prepare_graphics(&reader,&writer);begin_pass();draw_value(1,HEAP_BASE+12,44);mirror_finish_graphics(1);
 guest[3]=55;submit_merge();CHECK(guest[3]==55); // CPU edit after the common baseline survives completion
 CHECK(((uint32_t*)mir_stored[0]->base)[3]==44); // next CPU upload can detect the preserved edit
 cleanup();
}
#define REJECT(code) do {unsigned before=draws;fatal_expected=true;if(!setjmp(fatal_env)){code;CHECK(0);}fatal_expected=false;CHECK(draws==before);mir_graphics_preparing=false;}while(0)
static void fail_closed(void){
 bound(0,0,0,8);bound(1,0,16,8);st.sbuf[1][1].addr=HEAP_BASE-4;st.sbuf[1][1].size=4;
 REJECT(mirror_prepare_graphics(&writer,&writer));CHECK(!nops&&!mir_buf);cleanup();
 REJECT(mirror_prepare_graphics(&writer,&reader));CHECK(!nops);cleanup(); // no bound writable range
 ShaderInfo unsupported=writer;unsupported.uses_gmem_readonly=true;
 REJECT(mirror_prepare_graphics(&unsupported,&reader));CHECK(!nops);cleanup();
 bound(1,0,0,8);mirror_prepare_graphics(&reader,&writer);begin_pass();draw_value(1,HEAP_BASE,1);mirror_finish_graphics(1);submit_merge();
 bound(1,0,4,8);REJECT(mirror_prepare_graphics(&reader,&writer));cleanup(); // expanding an existing persistent owner
 bound(1,0,0,8);mirror_prepare_graphics(&reader,&writer);begin_pass();draw_value(1,HEAP_BASE,1);
 submit_on_end=true;REJECT(mirror_finish_graphics(1));CHECK(!in_pass&&readbacks==0);cleanup();
}
static void fast_paths(void){
 begin_pass();unsigned before=nops;mirror_prepare_graphics(&reader,&reader);CHECK(in_pass&&nops==before&&!mir_graphics_prepared);vk_end_pass();cleanup();
 // The compute table still uses the original machinery and readback helper.
 st.sbuf[5][0].addr=HEAP_BASE;st.sbuf[5][0].size=64;uint32_t tab[(GMAP_MAX+1)*4];
 mir_nstored=0;mirror_table(5,true,1,tab);CHECK(mir_nstored==1&&tab[GMAP_MAX*4]==1);
 append((Op){.kind=OP_DRAW,.write=(uint64_t)tab[2]*4,.value=12});mirror_after_dispatch(1);submit_merge();CHECK(guest[0]==12);cleanup();
}
int main(void){union_case();full_union_case();visibility_and_merge();fail_closed();fast_paths();printf("{\"state\":\"passed\",\"checks\":%u,\"two_stage_union\":true,\"same_submission_visibility\":true,\"cpu_gpu_merge\":true,\"pass_copy_barrier_checks\":true}\n",checks);return 0;}
