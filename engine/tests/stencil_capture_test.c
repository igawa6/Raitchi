/* Actual debug readback/writer; GPU/VMA are controlled, file bytes are real. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include "production_types.inc"
#include "nvn/maxwell/shader.h"
typedef struct {ShaderInfo *vi,*fi;} Prog;
static unsigned naliases,nstale;
static int nfeedback;
static NvnTexture *feedback_tex[4],*test_sampled,*test_alias,*test_depth0,*test_depth2;
static TexBackend *feedback_image[4];
static NvnTexture *shader_texture_object(int stage,ShaderTexture*t){(void)stage;if(test_depth0&&t->handle_offset==0x20)return test_depth0;if(test_depth2&&t->handle_offset==0x30)return test_depth2;return test_sampled;}
static NvnTexture *alias_read(NvnTexture*t){return test_alias?test_alias:t;}
static TexBackend *vk_tex(NvnTexture*t,bool render){(void)render;return t->backend;}
static char captured_log[32768];
static unsigned captured_log_n;
static void capture_log(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(captured_log+captured_log_n,sizeof captured_log-captured_log_n,fmt,ap);va_end(ap);if(n>0){unsigned remain=(unsigned)sizeof captured_log-captured_log_n;captured_log_n+=(unsigned)n<remain?(unsigned)n:remain-1;}}
#define LOG(...) capture_log(__VA_ARGS__)
static unsigned checks,allocations,destroys,copies,barriers,invalidations,ended;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static struct {void*vma;}g_vk;
static bool g_debug_frame,fail_alloc,fail_invalidate;
static uint64_t g_cur_fs_hash=0x1234,frame=77,pending=5,completed,producer_frame=9000;
static struct{bool rt_dirty;struct{int ncolor;NvnTexture *color[2],*depth;}rt;struct{uint64_t addr,size;}ubuf[6][18];}st;
#include "production_mirror_types.inc"
static VkBuffer mir_buf;static pthread_mutex_t mir_lock=PTHREAD_MUTEX_INITIALIZER;static MirSlot mir_slot[MIR_SLOTS];
typedef struct{unsigned usage,flags;VkMemoryPropertyFlags requiredFlags,preferredFlags;}VmaAllocationCreateInfo;
typedef struct{void*pMappedData;}VmaAllocationInfo;
#define VMA_MEMORY_USAGE_AUTO_PREFER_HOST 1
#define VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT 2
#define VMA_ALLOCATION_CREATE_MAPPED_BIT 4
static struct Allocation{void*ptr;uint64_t n,seq;}alloc[64];
static VkResult vmaCreateBuffer(void*a,const VkBufferCreateInfo*bi,const VmaAllocationCreateInfo*ai,VkBuffer*buf,VmaAllocation*mem,VmaAllocationInfo*info){
 (void)a;CHECK(bi->usage==VK_BUFFER_USAGE_TRANSFER_DST_BIT);CHECK(ai->requiredFlags==VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT&&ai->preferredFlags==VK_MEMORY_PROPERTY_HOST_CACHED_BIT);allocations++;
 if(fail_alloc)return VK_ERROR_OUT_OF_HOST_MEMORY;unsigned i=0;while(i<64&&alloc[i].ptr)i++;CHECK(i<64);alloc[i]=(struct Allocation){malloc(bi->size),bi->size,pending};CHECK(alloc[i].ptr);memset(alloc[i].ptr,0xcc,bi->size);info->pMappedData=alloc[i].ptr;*buf=(VkBuffer)(uintptr_t)(i+1);*mem=&alloc[i];return VK_SUCCESS;
}
static void vmaDestroyBuffer(void*a,VkBuffer b,VmaAllocation m){(void)a;struct Allocation*p=m;CHECK(p==&alloc[(uintptr_t)b-1]&&p->ptr&&p->seq<=completed);destroys++;free(p->ptr);p->ptr=NULL;}
static VkResult vmaInvalidateAllocation(void*a,VmaAllocation m,uint64_t off,uint64_t n){(void)a;struct Allocation*p=m;CHECK(p->ptr&&p->seq<=completed&&off==0&&n==p->n);invalidations++;return fail_invalidate?VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;}
static uint64_t vk_pending_seq(void){return pending;}
static uint64_t vk_completed_seq(void){return completed;}
static void complete(uint64_t seq){completed=seq;for(unsigned j=0;j<64;j++)if(alloc[j].ptr&&alloc[j].seq<=completed)for(uint64_t i=0;i<alloc[j].n;i++)((uint8_t*)alloc[j].ptr)[i]=(uint8_t)i;}
static VkCommandBuffer vk_cmd(void){return (VkCommandBuffer)(uintptr_t)9;}
static void vk_full_barrier(VkCommandBuffer cb){CHECK(cb==vk_cmd());barriers++;}
static void rec_CmdCopyImageToBuffer(VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer buf,unsigned n,const VkBufferImageCopy*c){
 CHECK(cb==vk_cmd()&&image&&layout==VK_IMAGE_LAYOUT_GENERAL&&buf&&n==1);CHECK((c->imageSubresource.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT||c->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT||c->imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT)&&c->imageSubresource.mipLevel==0&&c->imageSubresource.baseArrayLayer==0&&c->imageSubresource.layerCount==1);
 CHECK(c->bufferOffset==0&&c->bufferRowLength==0&&c->bufferImageHeight==0&&c->imageExtent.depth==1);CHECK((uint64_t)c->imageExtent.width*c->imageExtent.height*(c->imageSubresource.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT?1:4)==alloc[(uintptr_t)buf-1].n);copies++;
}
static void rec_CmdCopyBuffer(VkCommandBuffer cb,VkBuffer src,VkBuffer dst,unsigned count,const VkBufferCopy*copy){
 CHECK(cb==vk_cmd()&&src==mir_buf&&dst&&count==1);CHECK(copy->srcOffset==(48ull<<20)+256&&copy->dstOffset==0&&copy->size==768);CHECK(alloc[(uintptr_t)dst-1].n==768);copies++;
}
static void rec_CmdPipelineBarrier2(VkCommandBuffer cb,const VkDependencyInfo*d){
 CHECK(cb==vk_cmd()&&d->memoryBarrierCount==1);const VkMemoryBarrier2*b=d->pMemoryBarriers;
 CHECK(b->srcStageMask==VK_PIPELINE_STAGE_2_TRANSFER_BIT&&b->srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT&&b->dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT&&b->dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);barriers++;
}
static uint64_t renderer_present_count(void){return producer_frame;}
static uint64_t vk_frame_counter(void){return frame;}
static void vk_end_pass(void){ended++;}
#include "production_stencil_capture.inc"
static void request(void){FILE*f=fopen("stencil-probe.request","w");CHECK(f);fclose(f);}
int main(int argc,char**argv){
 CHECK(argc==2);CHECK(chdir(argv[1])==0);CHECK(renderer_present_count()==9000&&vk_frame_counter()==77);
 TexBackend b={.image=(VkImage)(uintptr_t)0x111,.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.width=64,.height=4,.depth=1,.layers=1,.levels=1,.samples=1,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
 CHECK(debug_stencil_shape(&b));TexBackend bad=b;bad.samples=4;CHECK(!debug_stencil_shape(&bad));bad=b;bad.layers=2;CHECK(!debug_stencil_shape(&bad));bad=b;bad.levels=2;CHECK(!debug_stencil_shape(&bad));bad=b;bad.depth=2;CHECK(!debug_stencil_shape(&bad));bad=b;bad.width=0;CHECK(!debug_stencil_shape(&bad));bad=b;bad.width=1<<24;bad.height=2;CHECK(!debug_stencil_shape(&bad));bad=b;bad.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;CHECK(!debug_stencil_shape(&bad));bad=b;bad.usage=0;CHECK(!debug_stencil_shape(&bad));CHECK(!debug_stencil_shape(NULL));
 debug_save_stencil(&b,"known.pgm",77,0x111,0x222,0x333,"source",false);CHECK(allocations==1&&copies==1&&barriers==2&&invalidations==0&&destroys==0&&pending==5);
 CHECK(access("known.pgm",F_OK)!=0);complete(4);stencil_probe_retire();CHECK(invalidations==0&&destroys==0&&stencil_probe_bytes==256);
 complete(5);stencil_probe_retire();CHECK(invalidations==1&&destroys==1&&stencil_probe_bytes==0&&access("known.pgm",F_OK)==0);
 fail_alloc=true;debug_save_stencil(&b,"fail-alloc.pgm",77,1,2,3,"source",false);CHECK(allocations==2&&copies==1&&destroys==1);fail_alloc=false;
 pending=6;fail_invalidate=true;debug_save_stencil(&b,"fail-invalidate.pgm",77,1,2,3,"source",false);CHECK(copies==2&&destroys==1);complete(6);stencil_probe_retire();CHECK(destroys==2&&stencil_probe_bytes==0);fail_invalidate=false;
 NvnTexture t={.addr=0x444};TexBackend dst=b;dst.image=(VkImage)(uintptr_t)0x222;unsigned count=allocations;
 debug_stencil_pair(&t,&b,&dst,true);CHECK(allocations==count&&ended==0);request();g_debug_frame=true;stencil_probe_poll();CHECK(!stencil_probe_active&&access("stencil-probe.request",F_OK)==0);
 frame=78;g_debug_frame=false;pending=10;stencil_probe_poll();CHECK(stencil_probe_active&&access("stencil-probe.request",F_OK)!=0);
 for(unsigned i=0;i<8;i++){producer_frame+=3;debug_stencil_pair(&t,&b,&dst,true);}
 CHECK(producer_frame==9024&&stencil_probe_pairs==8&&stencil_probe_frame==78);
 CHECK(allocations==count+16&&ended==8&&st.rt_dirty&&stencil_probe_bytes==4096&&pending==10);count=allocations;debug_stencil_pair(&t,&b,&dst,true);CHECK(allocations==count&&ended==8);
 unsigned old_destroys=destroys;request();frame=79;complete(9);stencil_probe_poll();CHECK(destroys==old_destroys&&!stencil_probe_active&&access("stencil-probe.request",F_OK)==0);
 complete(10);frame=80;stencil_probe_poll();CHECK(destroys==old_destroys+16&&stencil_probe_bytes==0&&stencil_probe_active&&access("stencil-probe.request",F_OK)!=0);
 stencil_probe_captured=STENCIL_PROBE_BYTES-256;debug_stencil_pair(&t,&b,&dst,false);CHECK(allocations==count);stencil_probe_captured=0;pending=11;debug_stencil_pair(&t,&b,&dst,false);CHECK(allocations==count+2&&ended==9&&pending==11);
 complete(11);frame=81;stencil_probe_poll();CHECK(stencil_probe_bytes==0&&!stencil_probe_active&&destroys==allocations-1);
 // Nonattachment raw-stencil consumers must capture the actual bound image,
 // distinct shader consumers, aliases and a selected feedback snapshot.
 request();frame=82;pending=12;stencil_probe_poll();
 ShaderInfo vi={0},fi={0};Prog pr={&vi,&fi};fi.ntex=1;fi.tex[0].handle_offset=0x20;
 t.backend=&b;test_sampled=&t;count=allocations;stencil_probe_consumers(&pr);CHECK(allocations==count);
 fi.tex[0].is_uint=true;producer_frame+=100;stencil_probe_consumers(&pr);CHECK(allocations==count+1&&pending==12&&stencil_probe_sampled==1&&stencil_probe_stats.queued==1&&stencil_probe_frame==82&&producer_frame==9124);
 stencil_probe_consumers(&pr);CHECK(allocations==count+1);g_cur_fs_hash++;stencil_probe_consumers(&pr);CHECK(allocations==count+2);
 nfeedback=1;feedback_tex[0]=&t;feedback_image[0]=&dst;stencil_probe_consumers(&pr);CHECK(allocations==count+3);nfeedback=0;
 NvnTexture alias={.backend=&dst};naliases=1;test_alias=&alias;g_cur_fs_hash++;stencil_probe_consumers(&pr);CHECK(allocations==count+4);CHECK(stencil_probe_stats.uint_candidates==5&&stencil_probe_stats.queued==4);naliases=0;test_alias=NULL;
 complete(11);unsigned prev=destroys;stencil_probe_retire();CHECK(destroys==prev);complete(12);frame=83;stencil_probe_poll();CHECK(destroys==prev+4&&stencil_probe_bytes==0);
 CHECK(strstr(captured_log,"frame 82 ended: uint_candidates 5 shape_rejected 0 queued 4 alloc_failed 0"));
 CHECK(strstr(captured_log,"pending_seq 12 completed_seq 12"));
 // Projection input captures precede execution; output captures are deferred
 // until the actual draw completion hook, without ending the submission.
 request();frame=84;pending=13;stencil_probe_poll();
 TexBackend color=b;color.aspect=VK_IMAGE_ASPECT_COLOR_BIT;color.format=VK_FORMAT_R8G8B8A8_UNORM;color.image=(VkImage)(uintptr_t)0xaaa;
 TexBackend rt0=color,rt1=color;rt0.image=(VkImage)(uintptr_t)0xbbb;rt1.image=(VkImage)(uintptr_t)0xccc;rt1.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
 NvnTexture input={.backend=&color,.addr=0x101},out0={.backend=&rt0,.addr=0x102},out1={.backend=&rt1,.addr=0x103};
 st.rt.ncolor=2;st.rt.color[0]=&out0;st.rt.color[1]=&out1;test_sampled=&input;fi.ntex=4;
 TexBackend d24=b,da=b;d24.image=(VkImage)(uintptr_t)0xddd;d24.format=VK_FORMAT_D24_UNORM_S8_UINT;da.image=(VkImage)(uintptr_t)0xeee;da.format=VK_FORMAT_D32_SFLOAT;
 NvnTexture depth0={.backend=&b,.addr=0x104},depth2={.backend=&d24,.addr=0x105},depthrt={.backend=&da,.addr=0x106};test_depth0=&depth0;test_depth2=&depth2;st.rt.depth=&depthrt;
 fi.tex[0].handle_offset=0x20;fi.tex[2].handle_offset=0x30;fi.tex[3].handle_offset=0x38;
 mir_buf=(VkBuffer)(uintptr_t)99;st.ubuf[1][1].addr=0x1100;st.ubuf[1][1].size=1136;
 mir_slot[25]=(MirSlot){.used=true,.lo=0x1000,.hi=0x2000,.off=(48ull<<20),.gpu_seq=8,.last_use=12};MirSlot owner_before=mir_slot[25];count=allocations;
 g_cur_fs_hash=0;projection_probe_before_draw(&pr);projection_probe_after_draw();CHECK(allocations==count);
 g_cur_fs_hash=0x44b584f0d922d49dull;projection_probe_before_draw(&pr);CHECK(allocations==count+5&&projection_probe_pre&&!projection_probe_post&&pending==13);CHECK(mir_slot[25].last_use==13&&owner_before.last_use==12);owner_before.last_use=13;CHECK(!memcmp(&owner_before,&mir_slot[25],sizeof owner_before));
 projection_probe_before_draw(&pr);CHECK(allocations==count+5);complete(12);stencil_probe_retire();CHECK(access("build/projection_84_input-t3.bin",F_OK)!=0);
 projection_probe_after_draw();CHECK(allocations==count+7&&projection_probe_post&&pending==13&&projection_probe_color_bytes==6144&&stencil_probe_stats.queued==7);
 projection_probe_after_draw();CHECK(allocations==count+7);complete(13);frame=85;stencil_probe_poll();CHECK(stencil_probe_bytes==0);
 color.format=VK_FORMAT_A2B10G10R10_UNORM_PACK32;CHECK(!projection_color_shape(&color));CHECK(!projection_color_queue(&color,0,"bad-format"));
 CHECK(projection_depth_shape(&b)&&projection_depth_shape(&d24)&&projection_depth_shape(&da));TexBackend d16=b;d16.format=VK_FORMAT_D16_UNORM;CHECK(!projection_depth_shape(&d16)&&!projection_plane_queue(&d16,0,"bad-depth",true));
 color.format=VK_FORMAT_R8G8B8A8_UNORM;color.samples=4;CHECK(!projection_color_shape(&color));color.samples=1;color.width=4096;color.height=4096;CHECK(!projection_color_shape(&color));
 request();frame=86;pending=14;stencil_probe_poll();count=allocations;st.ubuf[1][1].addr=0x9000;projection_probe_mirror_cb4();CHECK(allocations==count&&strstr(captured_log,"CB4 mirror unowned frame 86"));projection_probe_mirror_cb4();CHECK(allocations==count);
 printf("{\"state\":\"passed\",\"checks\":%u,\"actual_writer\":true,\"controlled_vulkan\":true,\"async_completion_gate\":true,\"no_submit_or_wait\":true}\n",checks);
}
