/* Actual uploader guards and render-queue ownership publication. No Vulkan calls. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "nvn/nvn.h"
typedef struct {bool gpu_owned,uploaded,dynamic;uint64_t checked_frame,cpu_write_generation;
 uint64_t source_address,source_generation;uint32_t wgen;int tp;}TexBackend;
static unsigned checks,fatals;static bool expect_fatal;static jmp_buf failed;
#undef FATAL
#define FATAL(...) do{fatals++;if(!expect_fatal){fprintf(stderr,__VA_ARGS__);abort();}longjmp(failed,1);}while(0)
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static unsigned initial_uploads;
static void upload_texture(NvnTexture *t, TexBackend *b) {
    CHECK(!b->gpu_owned&&!__atomic_load_n(&t->gpu_write_owned,__ATOMIC_ACQUIRE));
    initial_uploads++;b->cpu_write_generation=__atomic_load_n(&t->cpu_write_generation,__ATOMIC_ACQUIRE);
}
#include "texture_coherence_bodies.h"
int main(void){
 NvnTexture t={.addr=0x10000,.target=1};TexBackend b={0};uint64_t generation=99;
 FormatInfo rgba={.bw=1,.bh=1,.bpb=4};
 NvnTexture linear={.layout={.fmt=&rgba,.width=3,.height=2,.depth=1,.layers=1,.levels=1},.stride=16};
 uint64_t span=99;CHECK(texture_linear_span(&linear,&span)&&span==32);
 linear.stride=11;CHECK(!texture_linear_span(&linear,&span)&&span==0);
 linear.stride=UINT64_MAX;CHECK(!texture_linear_span(&linear,&span)&&span==0);
 linear.stride=16;linear.layout.levels=2;CHECK(!texture_linear_span(&linear,&span));
 linear.layout.levels=1;linear.layout.layers=2;CHECK(!texture_linear_span(&linear,&span));
 linear.layout.layers=1;linear.layout.depth=2;CHECK(!texture_linear_span(&linear,&span));
 linear.layout.depth=1;linear.layout.height=0;CHECK(!texture_linear_span(&linear,&span));
 FormatInfo blocks={.bw=4,.bh=4,.bpb=16};linear.layout.fmt=&blocks;
 linear.layout.width=5;linear.layout.height=7;linear.stride=32;
 CHECK(texture_linear_span(&linear,&span)&&span==64);
 // A pool remap must defeat the same-frame and clean-page upload caches,
 // even when it replaces bytes at the identical CPU address.
 b.uploaded=true;b.checked_frame=10;b.wgen=7;b.tp=5;
 texture_source_prepare(&b,0x10000,1);
 CHECK(!b.uploaded&&b.checked_frame==UINT64_MAX&&b.wgen==0&&b.tp==0);
 CHECK(b.source_address==0x10000&&b.source_generation==1);
 b.uploaded=true;b.checked_frame=10;b.wgen=9;b.tp=4;
 texture_source_prepare(&b,0x10000,1);
 CHECK(b.uploaded&&b.checked_frame==10&&b.wgen==9&&b.tp==4);
 texture_source_prepare(&b,0x10000,2);
 CHECK(!b.uploaded&&b.checked_frame==UINT64_MAX&&b.wgen==0&&b.tp==0);
 b.uploaded=true;texture_source_prepare(&b,0x20000,2);
 CHECK(!b.uploaded&&b.source_address==0x20000&&b.source_generation==2);
 CHECK(!texture_upload_skip(&t,&b,0,&generation)&&generation==0);
 b.uploaded=true;
 // Exact production guard: explicit writes must bypass both the current-frame
 // memo and every one of the eight static-texture scheduling phases.
 for(uint64_t frame=0;frame<16;frame++){
  b.checked_frame=frame;CHECK(texture_upload_skip(&t,&b,frame,&generation));
  __atomic_store_n(&t.cpu_write_generation,frame*2+1,__ATOMIC_RELEASE);
  CHECK(!texture_upload_skip(&t,&b,frame,&generation));CHECK(generation==frame*2+1);
  b.cpu_write_generation=frame*2+1;CHECK(texture_upload_skip(&t,&b,frame,&generation));
  b.checked_frame=UINT64_MAX;bool phase=((frame+((uintptr_t)&b>>6))&7)!=0;
  CHECK(texture_upload_skip(&t,&b,frame,&generation)==phase);
  __atomic_store_n(&t.cpu_write_generation,frame*2+2,__ATOMIC_RELEASE);
  CHECK(!texture_upload_skip(&t,&b,frame,&generation));b.cpu_write_generation=frame*2+2;
 }
 t.addr=0;CHECK(texture_upload_skip(&t,&b,1,&generation));t.addr=0x10000;
 NvnPool virtual_pool={.size=4096};t.addr=0;t.pool=&virtual_pool;b.uploaded=false;
 CHECK(!texture_upload_skip(&t,&b,1,&generation));t.pool=NULL;t.addr=0x10000;
 b.dynamic=true;b.checked_frame=UINT64_MAX;CHECK(!texture_upload_skip(&t,&b,1,&generation));
 // Consumption allows a later GPU-ownership transfer. Unconsumed writes fail
 // at that ordered transition, before either ownership field is changed.
 __atomic_store_n(&t.cpu_write_generation,100,__ATOMIC_RELEASE);b.cpu_write_generation=99;
 expect_fatal=true;if(!setjmp(failed)){texture_mark_gpu_owned(&t,&b);CHECK(false);}expect_fatal=false;
 CHECK(fatals==1&&!b.gpu_owned&&!__atomic_load_n(&t.gpu_write_owned,__ATOMIC_ACQUIRE));
 texture_prepare_gpu_owned(&t,&b);CHECK(initial_uploads==1&&b.cpu_write_generation==100);
 CHECK(b.gpu_owned&&__atomic_load_n(&t.gpu_write_owned,__ATOMIC_ACQUIRE));
 // Covers a CPU call queued before the render thread published GPU ownership:
 // sampling the GPU-owned image detects that generation rather than dropping it.
 __atomic_store_n(&t.cpu_write_generation,101,__ATOMIC_RELEASE);expect_fatal=true;
 if(!setjmp(failed)){texture_write_gpu_check(&t,&b);CHECK(false);}expect_fatal=false;
 CHECK(fatals==2&&b.cpu_write_generation==100);
 expect_fatal=true;if(!setjmp(failed)){texture_prepare_gpu_owned(&t,&b);CHECK(false);}expect_fatal=false;
 CHECK(fatals==3&&initial_uploads==1&&b.cpu_write_generation==100);
 printf("state=passed checks=%u; actual sameframe/8phase upload guard, generation publication, ordered GPU rejection\n",checks);return 0;
}
