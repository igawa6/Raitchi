/* Actual optional forwarding and actual CPU-diff producer, controlled GPU command ordering. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
typedef uint64_t VkBuffer, VkDeviceSize, VkCommandBuffer;
typedef struct{VkBuffer buffer;uint64_t offset,range;}VkDescriptorBufferInfo;
typedef struct{uint64_t srcOffset,dstOffset,size;}VkBufferCopy;
typedef struct{VkBuffer buf;uint64_t off;void*ptr;}VkSpan;
typedef struct{uint64_t addr,size;}Range;
typedef struct{uint32_t cbuf_mask,cbuf_dynamic,cbuf_size[32];bool uses_gmem_store;}ShaderInfo;
#define MIR_SIZE 65536u
#define MIR_SLOTS 4
#define MIR_RB 4
static VkBuffer mir_buf=1;
static _Alignas(256) unsigned char mirror[MIR_SIZE],transient[131072],guest[4096],base[4096];
static uint64_t used,seq=2,gcopy_gen=1,stat_upload_bytes,uniform_forward_write_epoch;
static unsigned checks,barriers,copies,ends;
static unsigned prior_read_barrier,last_copy_barrier;
static bool prior_uniform_read,copy_needs_visibility;
static const char *wait_why;
static bool mir_graphics_prepared;
static int mir_nstored;
static bool in_pass,uniform_forward_graphics_prepared,expect_fatal;
static jmp_buf jump;
static const char*env_value;
static struct{struct{struct{uint64_t maxUniformBufferRange,minUniformBufferOffsetAlignment;}limits;}props;void*vma;}g_vk;
static unsigned mir_mem;
static uint8_t *mir_ptr=mirror;
static struct{Range ubuf[6][18];}st;
typedef struct{uint64_t lo,hi,off,gpu_seq,last_use,checked_seq;uint8_t*base;uint64_t rb_off[4],rb_seq[4];uint8_t*rb_base[4];bool used;}MirSlot;
static MirSlot mir_slot[MIR_SLOTS];
static MirSlot *mir_stored[32];
static unsigned char rbbase[MIR_RB][4096];
static pthread_mutex_t mir_lock=PTHREAD_MUTEX_INITIALIZER;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d:%s\n",__LINE__,#x);abort();}}while(0)
static void fatal(const char*fmt,...){(void)fmt;if(!expect_fatal)abort();longjmp(jump,1);}
#define FATAL(...) fatal(__VA_ARGS__)
static const char*get_env(const char*n){CHECK(!strcmp(n,"LARECOMP_GPU_UNIFORM_FORWARD"));return env_value;}
#define getenv get_env
static uint64_t vk_pending_seq(void){return seq;}
static VkCommandBuffer vk_cmd(void){return 7;}
static void vk_end_pass(void){in_pass=false;ends++;}
static void vk_full_barrier(VkCommandBuffer cb){CHECK(cb==7&&!in_pass);barriers++;}
static VkSpan vk_transient(uint64_t n,uint64_t align){CHECK(n<=sizeof transient);used=(used+align-1)/align*align;CHECK(n<=sizeof transient-used);VkSpan s={2,used,transient+used};memset(s.ptr,0xcd,n);used+=n;return s;}
static VkSpan vk_transient_copy(const void*p,uint64_t n,uint64_t align){VkSpan s=vk_transient(n,align);memcpy(s.ptr,p,n);return s;}
static void rec_CmdCopyBuffer(VkCommandBuffer cb,VkBuffer src,VkBuffer dst,unsigned n,const VkBufferCopy*c){CHECK(cb==7&&!in_pass&&n==1);CHECK(src==1||src==2);CHECK(dst==1||dst==2);if(dst==1&&prior_uniform_read){CHECK(barriers>prior_read_barrier);prior_uniform_read=false;}last_copy_barrier=barriers;copy_needs_visibility=true;CHECK((c->srcOffset|c->dstOffset|c->size)%4==0);unsigned char*s=src==1?mirror:transient,*d=dst==1?mirror:transient;CHECK(c->srcOffset+c->size<=(src==1?sizeof mirror:sizeof transient));CHECK(c->dstOffset+c->size<=(dst==1?sizeof mirror:sizeof transient));memmove(d+c->dstOffset,s+c->srcOffset,c->size);copies++;}
static uint64_t cbuf_bound_size(const ShaderInfo*si,int k,Range r){uint64_t n=si->cbuf_size[k]?si->cbuf_size[k]:256;if((si->cbuf_dynamic&(1u<<k))&&r.size)n=r.size>65536?65536:r.size;if(n>r.size&&r.size)n=r.size;return n<16?16:n;}
#include "uniform_forward.cpu.h"
static uint64_t vk_completed_seq(void){return seq;}
static void vmaInvalidateAllocation(void*vma,unsigned mem,uint64_t off,uint64_t n){(void)vma;(void)mem;CHECK(off+n<=sizeof mirror);}
#include "uniform_forward.merge.h"
static void vk_wait_seq(uint64_t n){CHECK(n<seq);mirror_merge_locked();}
#include "uniform_forward.readback.h"
#include "uniform_forward.idle.h"
#include "uniform_forward.h"
static uint32_t word(VkDescriptorBufferInfo b,unsigned off){uint32_t v;if(copy_needs_visibility){CHECK(barriers>last_copy_barrier);copy_needs_visibility=false;}CHECK(off+4<=b.range);CHECK(b.buffer==1||b.buffer==2);memcpy(&v,(b.buffer==1?mirror:transient)+b.offset+off,4);return v;}
static void put(unsigned char*p,unsigned off,uint32_t v){memcpy(p+off,&v,4);}
static void init(void){uniform_forward_visible_epoch=UINT64_MAX;uniform_forward_write_epoch++;memset(mir_slot,0,sizeof mir_slot);memset(guest,0,sizeof guest);memset(base,0,sizeof base);memset(mirror,0,sizeof mirror);memset(&st,0,sizeof st);memset(uniform_forward,0,sizeof uniform_forward);used=0;in_pass=false;seq++;gcopy_gen++;mir_slot[0]=(MirSlot){.used=true,.lo=(uint64_t)(uintptr_t)guest,.hi=(uint64_t)(uintptr_t)guest+1024,.off=4096,.base=base,.gpu_seq=seq,.checked_seq=seq};g_vk.props.limits.maxUniformBufferRange=65536;g_vk.props.limits.minUniformBufferOffsetAlignment=256;for(int j=0;j<4;j++){mir_slot[0].rb_off[j]=8192+j*4096;mir_slot[0].rb_base[j]=rbbase[j];}mir_nstored=0;}
#define REJECT(expr) do{expect_fatal=true;if(!setjmp(jump)){expr;CHECK(false);}expect_fatal=false;}while(0)
static unsigned char multi_base[MIR_SLOTS][4096],multi_rb[MIR_SLOTS][MIR_RB][4096];
static void owner_at(unsigned index,unsigned at,unsigned size,unsigned offset){
 MirSlot*m=&mir_slot[index];*m=(MirSlot){.used=true,.lo=(uint64_t)(uintptr_t)guest+at,.hi=(uint64_t)(uintptr_t)guest+at+size,.off=offset,.base=multi_base[index],.gpu_seq=seq,.checked_seq=seq};
 memcpy(m->base,guest+at,size);memcpy(mirror+offset,guest+at,size);
 for(unsigned j=0;j<4;j++){m->rb_off[j]=offset+(j+1)*256;m->rb_base[j]=multi_rb[index][j];}
}
static void composition_cases(void){
 init();memset(mir_slot,0,sizeof mir_slot);for(unsigned i=0;i<sizeof guest;i++)guest[i]=(unsigned char)(i*3+7);
 owner_at(0,128,128,4096);owner_at(1,512,128,16384);put(mirror,4096+20,0xfaceb00c);put(mirror,16384+40,0xabcdef01);
 uint64_t addr=(uint64_t)(uintptr_t)guest+64;unsigned char expected[640];memcpy(expected,guest+64,sizeof expected);memcpy(expected+64,mirror+4096,128);memcpy(expected+448,mirror+16384,128);
 VkDescriptorBufferInfo bi;unsigned before=copies;CHECK(mirror_uniform_info(5,4,addr,640,&bi));CHECK(bi.buffer==2&&!(bi.offset%256)&&bi.range==640);CHECK(copies==before+2);CHECK(!memcmp(transient+bi.offset,expected,640));CHECK(mir_slot[0].last_use==seq&&mir_slot[1].last_use==seq);
 /* A second compute consumer sees a new GPU result, never an old composed cache. */
 uint64_t old_offset=bi.offset;put(mirror,4096+20,0x11223344);CHECK(mirror_uniform_info(5,4,addr,640,&bi));CHECK(bi.offset!=old_offset&&word(bi,84)==0x11223344&&word(bi,488)==0xabcdef01);
 /* Both graphics stages can share the composed camera range and CPU holes. */
 ShaderInfo si={.cbuf_mask=1u<<4};si.cbuf_size[4]=640;st.ubuf[0][1]=st.ubuf[1][1]=(Range){addr,640};in_pass=true;mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&uniform_forward_graphics_prepared);in_pass=true;CHECK(mirror_uniform_info(0,4,addr,640,&bi));CHECK(word(bi,84)==0x11223344&&word(bi,488)==0xabcdef01);CHECK(!memcmp(transient+bi.offset,guest+64,64));CHECK(!memcmp(transient+bi.offset+192,guest+256,256));in_pass=false;
 /* An explicit same-value update overrides one owner, preserving the other and gaps. */
 uint32_t baseline;memcpy(&baseline,guest+148,4);mirror_uniform_explicit_update((uint64_t)(uintptr_t)guest+148,&baseline,4);gcopy_gen++;CHECK(mirror_uniform_info(5,4,addr,640,&bi));CHECK(word(bi,84)==baseline&&word(bi,488)==0xabcdef01);CHECK(!memcmp(transient+bi.offset+576,guest+640,64));
 /* A clipped owner intersection on the left, then untouched CPU tail. */
 put(mirror,4096+112,0x1234abcd);CHECK(uniform_forward_prepare((uint64_t)(uintptr_t)guest+240,64,&bi));CHECK(word(bi,0)==0x1234abcd&&!memcmp(transient+bi.offset+16,guest+256,48));
 /* Invalid overlapping authorities fail before recording any transfer. */
 mir_slot[2]=(MirSlot){.used=true,.lo=(uint64_t)(uintptr_t)guest+192,.hi=(uint64_t)(uintptr_t)guest+224,.off=32768};before=copies;REJECT(uniform_forward_prepare(addr,640,&bi));CHECK(copies==before);mir_slot[2].used=false;
 before=copies;REJECT(uniform_forward_prepare((uint64_t)(uintptr_t)guest+129,128,&bi));CHECK(copies==before);
}
static void cpu_dirty_gaps_preserve_gpu(void){
 init();uint64_t addr=(uint64_t)(uintptr_t)guest;VkDescriptorBufferInfo bi;
 put(mirror,4096+4,99);put(guest,0,7);put(guest,32,8);CHECK(uniform_forward_prepare(addr,64,&bi));
 CHECK(word(bi,0)==7&&word(bi,32)==8&&word(bi,4)==99);
 mirror_merge_locked();CHECK(*(uint32_t*)(guest+4)==99&&*(uint32_t*)(base+4)==99);
}
static void active_pass_reuse(void){
 init();uint64_t addr=(uint64_t)(uintptr_t)guest;ShaderInfo si={.cbuf_mask=1u<<4};si.cbuf_size[4]=768;st.ubuf[0][1]=st.ubuf[1][1]=(Range){addr,1024};put(mirror,4096+528,99);uniform_forward_write_epoch++;
 mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&uniform_forward_graphics_prepared);in_pass=true;unsigned previous_ends=ends,previous_barriers=barriers,previous_copies=copies;VkDescriptorBufferInfo bi;
 for(int i=0;i<3;i++){mirror_uniform_prepare_graphics(&si,&si);CHECK(in_pass&&uniform_forward_graphics_prepared);CHECK(mirror_uniform_info(0,4,addr,768,&bi)&&word(bi,528)==99);CHECK(mirror_uniform_info(1,4,addr,768,&bi));}
 CHECK(ends==previous_ends&&barriers==previous_barriers&&copies==previous_copies&&mir_slot[0].last_use==seq);
 /* A raw CPU edit, including one within the same seq, must end the pass. */
 put(guest,64,7);prior_uniform_read=true;prior_read_barrier=barriers;mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&ends==previous_ends+1);CHECK(mirror_uniform_info(0,4,addr,768,&bi)&&word(bi,64)==7&&word(bi,528)==99);in_pass=true;previous_ends=ends;previous_barriers=barriers;mirror_uniform_prepare_graphics(&si,&si);CHECK(in_pass&&ends==previous_ends&&barriers==previous_barriers);
 /* Even CPU edits outside this UBO but inside its owner invalidate reuse. */
 put(guest,900,9);mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&ends==previous_ends+1&&*(uint32_t*)(mirror+4096+900)==9);
 uint64_t unchanged_seq=seq;
 for(unsigned value=42;value<44;value++){
  in_pass=false;put(mirror,4096+528,value);mir_stored[0]=&mir_slot[0];mir_nstored=1;mirror_after_dispatch(7);CHECK(uniform_forward_visible_epoch!=uniform_forward_write_epoch);in_pass=true;previous_ends=ends;previous_barriers=barriers;mirror_uniform_prepare_graphics(&si,&si);CHECK(seq==unchanged_seq&&!in_pass&&ends==previous_ends+1&&barriers>previous_barriers);CHECK(mirror_uniform_info(1,4,addr,768,&bi)&&word(bi,528)==value);in_pass=true;previous_ends=ends;previous_barriers=barriers;mirror_uniform_prepare_graphics(&si,&si);CHECK(in_pass&&ends==previous_ends&&barriers==previous_barriers);
 }
 /* Partial and unaligned views keep the proven prepass composition path. */
 si.cbuf_size[4]=64;st.ubuf[0][1]=st.ubuf[1][1]=(Range){addr+1000,64};previous_ends=ends;mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&ends==previous_ends+1);CHECK(mirror_uniform_info(0,4,addr+1000,64,&bi)&&bi.buffer==2);in_pass=true;st.ubuf[0][1]=st.ubuf[1][1]=(Range){addr+4,64};previous_ends=ends;mirror_uniform_prepare_graphics(&si,&si);CHECK(!in_pass&&ends==previous_ends+1);CHECK(mirror_uniform_info(0,4,addr+4,64,&bi)&&bi.buffer==2);
}
int main(int argc,char**argv){CHECK(argc==2);env_value=!strcmp(argv[1],"on")?"1":!strcmp(argv[1],"off")?"0":NULL;bool enabled=strcmp(argv[1],"off")!=0;init();VkDescriptorBufferInfo bi={0};uint64_t addr=(uint64_t)(uintptr_t)guest;unsigned before;
 if(!enabled){put(mirror,4096,99);CHECK(!uniform_forward_prepare(addr,64,&bi));mirror_uniform_explicit_update(addr,guest,64);CHECK(word((VkDescriptorBufferInfo){1,4096,64},0)==99);ShaderInfo s={.cbuf_mask=1u<<4};mirror_uniform_prepare_graphics(&s,&s);CHECK(!uniform_forward_graphics_prepared&&!copies&&!barriers);puts("{\"state\":\"passed\",\"explicit_off\":true}");return 0;}
 /* Shader-produced value is authoritative while CPU and baseline are unchanged. */
 put(mirror,4096+528,0x12345678);CHECK(uniform_forward_prepare(addr,768,&bi));CHECK(bi.buffer==1&&bi.offset==4096&&bi.range==768);CHECK(word(bi,528)==0x12345678);CHECK(!memcmp(guest,base,1024));CHECK(mir_slot[0].last_use==seq);CHECK(!mirror_slot_idle(&mir_slot[0],seq-1)&&mirror_slot_idle(&mir_slot[0],seq));
 /* A same-submission CPU edit updates only changed words, preserving GPU matrix. */
 prior_uniform_read=true;prior_read_barrier=barriers;put(guest,64,9);before=copies;CHECK(uniform_forward_prepare(addr,768,&bi));CHECK(copies==before+2&&word(bi,64)==9&&word(bi,528)==0x12345678);CHECK(!prior_uniform_read&&barriers>=prior_read_barrier+2);CHECK(mir_slot[0].checked_seq==seq);
 prior_uniform_read=true;prior_read_barrier=barriers;put(guest,68,11);mir_stored[0]=&mir_slot[1];mir_nstored=1;CHECK(uniform_forward_prepare(addr,768,&bi));CHECK(word(bi,68)==11&&word(bi,528)==0x12345678);CHECK(mir_nstored==1&&mir_stored[0]==&mir_slot[1]);mir_nstored=0;
 /* Exact explicit same-value CPU write overrides an earlier GPU write. */
 unsigned char zero[16]={0};
 /* Model an earlier shader write/readback, then an explicit CPU override. */
 put(mirror,4096+528,99);mir_stored[0]=&mir_slot[0];mir_nstored=1;mirror_after_dispatch(7);
 CHECK(*(uint32_t*)(mirror+mir_slot[0].rb_off[0]+528)==99);
 in_pass=true;before=ends;mirror_uniform_explicit_update(addr+528,zero,4);CHECK(ends==before+1&&!in_pass);CHECK(!memcmp(guest+528,zero,4));CHECK(word(bi,528)==0);mirror_merge_locked();CHECK(*(uint32_t*)(guest+528)==0&&*(uint32_t*)(base+528)==0);put(mirror,4096+532,0xcafebabe);mirror_uniform_explicit_update(addr+528,zero,4);CHECK(word(bi,532)==0xcafebabe);mirror_merge_locked();CHECK(*(uint32_t*)(guest+528)==0&&*(uint32_t*)(guest+532)==0xcafebabe);
 /* Same-pending explicit CPU7 becomes the later GPU42 write's baseline. */
 uint32_t seven=7;mirror_uniform_explicit_update(addr+528,&seven,4);put(guest,528,seven);put(mirror,4096+528,42);mir_stored[0]=&mir_slot[0];mir_nstored=1;mirror_after_dispatch(7);mirror_merge_locked();CHECK(*(uint32_t*)(guest+528)==42&&*(uint32_t*)(base+528)==42);
 /* Unaligned descriptor offsets use GPU->aligned-transient, not CPU source bytes. */
 put(mirror,4096+4,0x99887766);CHECK(uniform_forward_prepare(addr+4,64,&bi));CHECK(bi.buffer==2&&bi.offset%256==0&&word(bi,0)==0x99887766);CHECK(memcmp(transient+bi.offset,guest+4,64));
 /* Repeated compute consumers observe a fresh GPU value in one submission. */
 put(mirror,4096+4,0x11223344);CHECK(mirror_uniform_info(5,4,addr+4,64,&bi));CHECK(word(bi,0)==0x11223344);CHECK(mir_slot[0].last_use==seq);
 ShaderInfo vs={.cbuf_mask=1u<<4},fs=vs;vs.cbuf_size[4]=fs.cbuf_size[4]=768;st.ubuf[0][1]=(Range){addr,1024};st.ubuf[1][1]=(Range){addr,1024};uniform_forward_visible_epoch=UINT64_MAX;in_pass=true;mirror_uniform_prepare_graphics(&vs,&fs);CHECK(!in_pass&&uniform_forward_graphics_prepared);in_pass=true;CHECK(mirror_uniform_info(0,4,addr,768,&bi));CHECK(bi.buffer==1);CHECK(mirror_uniform_info(1,4,addr,768,&bi));in_pass=false;
 /* Generation or seq changes must never bind a stale prepared transient. */
 gcopy_gen++;REJECT(mirror_uniform_info(1,4,addr,768,&bi));gcopy_gen--;seq++;REJECT(mirror_uniform_info(1,4,addr,768,&bi));seq--;
 /* Unowned ranges preserve the CPU fallback; overlap and overflow fail closed. */
 CHECK(!uniform_forward_prepare(addr+2048,64,&bi));put(mirror,4096+1000,0x55667788);CHECK(uniform_forward_prepare(addr+1000,64,&bi));CHECK(bi.buffer==2&&word(bi,0)==0x55667788&&!memcmp(transient+bi.offset+24,guest+1024,40));REJECT(uniform_forward_prepare(UINT64_MAX-7,16,&bi));REJECT(uniform_forward_prepare(addr+1,16,&bi));REJECT(uniform_forward_prepare(addr+4,17,&bi));in_pass=true;REJECT(uniform_forward_prepare(addr,64,&bi));in_pass=false;
 REJECT(mirror_uniform_explicit_update(addr+1,zero,4));REJECT(mirror_uniform_explicit_update(UINT64_MAX-1,zero,4));
 g_vk.props.limits.maxUniformBufferRange=32;CHECK(uniform_forward_prepare(addr,768,&bi)&&bi.range==32);g_vk.props.limits.maxUniformBufferRange=65536;
 /* Independent graphics stages share a camera owner and reset ownership next draw. */
 st.ubuf[0][1]=(Range){addr+2048,64};st.ubuf[1][1]=st.ubuf[0][1];mirror_uniform_prepare_graphics(&vs,&fs);CHECK(!uniform_forward_graphics_prepared);CHECK(!mirror_uniform_info(1,4,addr+2048,64,&bi));
 composition_cases();cpu_dirty_gaps_preserve_gpu();active_pass_reuse();CHECK(barriers>0&&copies>0&&stat_upload_bytes>0);CHECK(wait_why==NULL);printf("{\"state\":\"passed\",\"checks\":%u,\"gpu_copies\":%u,\"barriers\":%u,\"default_enabled\":%s}\n",checks,copies,barriers,env_value ? "false" : "true");return 0;}
