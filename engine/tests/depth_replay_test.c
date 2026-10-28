/* Asset-free tests of actual request policy and extracted command emitter. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan_core.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x); abort(); } } while (0)
#define DEPTH_REPLAY_POLICY_ONLY
#include "depth_replay.h"
#undef DEPTH_REPLAY_POLICY_ONLY
static struct { bool vids; } g_vk;
typedef struct { unsigned used_streams; } VtxLayout;
typedef struct { VkPipelineLayout layout; } TestProgram;
typedef struct {
    uint32_t known, vb_known;
    VkViewport vp; VkRect2D sc; VkCullModeFlags cull; VkFrontFace ff; VkPrimitiveTopology topo;
    VkBool32 pre,dbe; float db[3]; VkDescriptorSet ds[2]; uint32_t ndyn,dyn[192];
    VkBuffer vb[17],ib; VkDeviceSize vo[17],io; VkIndexType it;
} TestState;
typedef struct {
    struct { VkFormat stencil; TestProgram *prog; } key;
    VkPipeline pipeline; VkImage image, reference_image; VkImageView view, reference_view;
    VkBuffer output[2]; uint32_t width,height;
    VtxLayout vertices;
    TestState recorded;
    bool indexed,captured; uint64_t seq,frame,reference_guest; uint32_t count,instances,first,base_instance; int32_t base_vertex;
} DepthReplayJob;
static DepthReplayJob *expected;
static bool rendering;
static unsigned copy_count,draws,begins,ends,layouts,barriers;
static void vk_full_barrier(VkCommandBuffer cb){CHECK(cb==(VkCommandBuffer)(uintptr_t)9&&!rendering);barriers++;}
static void rec_CmdCopyImageToBuffer(VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer buffer,uint32_t n,const VkBufferImageCopy *copy){
 CHECK(cb==(VkCommandBuffer)(uintptr_t)9&&!rendering&&layout==VK_IMAGE_LAYOUT_GENERAL&&n==1);CHECK(copy->imageExtent.width==expected->width&&copy->imageExtent.height==expected->height&&copy->imageExtent.depth==1);CHECK(copy->imageSubresource.mipLevel==0&&copy->imageSubresource.baseArrayLayer==0&&copy->imageSubresource.layerCount==1);
 if(copy_count==0){CHECK(!begins&&image==expected->reference_image&&buffer==expected->output[1]&&copy->imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);}else{CHECK(copy_count==1&&ends==1&&image==expected->image&&buffer==expected->output[0]&&copy->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);}copy_count++;
}
static void rec_CmdBeginRendering(VkCommandBuffer cb,const VkRenderingInfo *r){CHECK(cb&&(copy_count==1)&&!rendering&&!begins);CHECK(r->renderArea.offset.x==0&&r->renderArea.extent.width==expected->width&&r->renderArea.extent.height==expected->height&&r->layerCount==1);CHECK(r->colorAttachmentCount==1&&r->pColorAttachments->imageView==expected->view);CHECK(r->pColorAttachments->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR&&isinf(r->pColorAttachments->clearValue.color.float32[0])&&r->pColorAttachments->clearValue.color.float32[0]>0);CHECK(r->pDepthAttachment->imageView==expected->reference_view&&r->pDepthAttachment->loadOp==VK_ATTACHMENT_LOAD_OP_LOAD&&r->pDepthAttachment->storeOp==VK_ATTACHMENT_STORE_OP_STORE);CHECK((r->pStencilAttachment!=NULL)==(expected->key.stencil!=0));rendering=true;begins++;}
static void rec_CmdBindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){CHECK(cb&&rendering&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&pipeline==expected->pipeline);}
static void rec_CmdSetViewport(VkCommandBuffer cb,uint32_t first,uint32_t n,const VkViewport*v){CHECK(cb&&rendering&&!first&&n==1&&!memcmp(v,&expected->recorded.vp,sizeof *v));}
static void rec_CmdSetScissor(VkCommandBuffer cb,uint32_t first,uint32_t n,const VkRect2D*v){CHECK(cb&&rendering&&!first&&n==1&&!memcmp(v,&expected->recorded.sc,sizeof *v));}
#define STATE(name,type,expr) static void name(VkCommandBuffer cb,type value){CHECK(cb&&rendering&&(expr));}
STATE(rec_CmdSetCullMode,VkCullModeFlags,value==expected->recorded.cull)
STATE(rec_CmdSetFrontFace,VkFrontFace,value==expected->recorded.ff)
STATE(rec_CmdSetPrimitiveTopology,VkPrimitiveTopology,value==expected->recorded.topo)
STATE(rec_CmdSetPrimitiveRestartEnable,VkBool32,value==expected->recorded.pre)
STATE(rec_CmdSetDepthBiasEnable,VkBool32,value==expected->recorded.dbe)
STATE(rec_CmdSetDepthTestEnable,VkBool32,value==VK_TRUE)
STATE(rec_CmdSetDepthCompareOp,VkCompareOp,value==VK_COMPARE_OP_ALWAYS)
STATE(rec_CmdSetDepthWriteEnable,VkBool32,value==VK_FALSE)
STATE(rec_CmdSetStencilTestEnable,VkBool32,value==VK_FALSE)
static void rec_CmdSetDepthBias(VkCommandBuffer cb,float constant,float clamp,float slope){CHECK(cb&&rendering&&constant==expected->recorded.db[1]&&clamp==expected->recorded.db[2]&&slope==expected->recorded.db[0]);}
static void set_vertex_input(VkCommandBuffer cb,const VtxLayout*v){CHECK(cb&&rendering&&v==&expected->vertices);layouts++;}
static void rec_CmdBindDescriptorSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t n,const VkDescriptorSet*sets,uint32_t ndyn,const uint32_t*dyn){CHECK(cb&&rendering&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&layout==expected->key.prog->layout&&first==0&&n==2);CHECK(!memcmp(sets,expected->recorded.ds,2*sizeof *sets));CHECK(ndyn==expected->recorded.ndyn&&(!ndyn||!memcmp(dyn,expected->recorded.dyn,ndyn*sizeof *dyn)));}
static void rec_CmdBindVertexBuffers(VkCommandBuffer cb,uint32_t binding,uint32_t n,const VkBuffer*b,const VkDeviceSize*off){CHECK(cb&&rendering&&n==1&&binding<17&&(expected->vertices.used_streams&(1u<<binding)));CHECK(*b==expected->recorded.vb[binding]&&*off==expected->recorded.vo[binding]);}
static void rec_CmdBindIndexBuffer(VkCommandBuffer cb,VkBuffer b,VkDeviceSize off,VkIndexType type){CHECK(cb&&rendering&&expected->indexed&&b==expected->recorded.ib&&off==expected->recorded.io&&type==expected->recorded.it);}
static void rec_CmdDrawIndexed(VkCommandBuffer cb,uint32_t count,uint32_t instances,uint32_t first,int32_t base,uint32_t base_instance){CHECK(cb&&rendering&&expected->indexed&&count==expected->count&&instances==expected->instances&&first==0&&base==expected->base_vertex&&base_instance==expected->base_instance);draws++;}
static void rec_CmdDraw(VkCommandBuffer cb,uint32_t count,uint32_t instances,uint32_t first,uint32_t base_instance){CHECK(cb&&rendering&&!expected->indexed&&count==expected->count&&instances==expected->instances&&first==expected->first&&base_instance==expected->base_instance);draws++;}
static void rec_CmdEndRendering(VkCommandBuffer cb){CHECK(cb&&rendering&&draws==1);rendering=false;ends++;}
static void rec_CmdPipelineBarrier2(VkCommandBuffer cb,const VkDependencyInfo*d){CHECK(cb&&!rendering&&copy_count==2&&d->memoryBarrierCount==1);CHECK(d->pMemoryBarriers->srcStageMask==VK_PIPELINE_STAGE_2_TRANSFER_BIT&&d->pMemoryBarriers->srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT&&d->pMemoryBarriers->dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT&&d->pMemoryBarriers->dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);}
#include "depth_replay.emitter.h"
typedef struct { VkImageUsageFlags usage; uint32_t levels,layers,width,height,samples; VkFormat format; VkImage image; } TexBackend;
typedef struct { TexBackend *backend; uint64_t addr; } TestTexture;
static struct { struct { TestTexture *depth; } rt; bool rt_dirty; } st;
static TestState ss;
static bool in_pass;
static VkImageView pass_depth_attachment_view;
static struct { unsigned phase; DepthReplayJob jobs[2]; } depth_replay;
static unsigned wrapper_ends,layout_inits,status_calls;
static void vk_end_pass(void){CHECK(in_pass);in_pass=false;rendering=false;wrapper_ends++;}
static VkCommandBuffer vk_cmd(void){return(VkCommandBuffer)(uintptr_t)9;}
static void vk_image_init_layout(VkCommandBuffer cb,VkImage image,VkImageAspectFlags aspect){CHECK(cb&&!in_pass&&!rendering&&image==expected->image&&aspect==VK_IMAGE_ASPECT_COLOR_BIT);layout_inits++;}
static uint64_t vk_pending_seq(void){return 123;}
static uint64_t vk_frame_counter(void){return 456;}
static void depth_replay_status(const char*status){CHECK(status&&*status);status_calls++;}
#include "depth_replay.capture.h"
int main(void){
 DepthReplayConfig c={0};CHECK(depth_replay_parse("vs=0123456789abcdef fs=fedcba9876543210 ordinal=0 count=2\n",&c));CHECK(c.vs==0x0123456789abcdefull&&c.fs==0xfedcba9876543210ull&&c.count==2);
 const char*invalid[]={"", "vs=-123456789abcdef fs=fedcba9876543210 ordinal=0 count=2", "vs=0123456789abcdef fs=fedcba9876543210 ordinal=2047 count=2", "vs=0123456789abcdef fs=fedcba9876543210 ordinal=0 count=3", "vs=0123456789abcdef fs=fedcba9876543210 ordinal=0 count=0", "vs=0000000000000000 fs=fedcba9876543210 ordinal=0 count=1", "vs=123 fs=fedcba9876543210 ordinal=0 count=1", "vs=0123456789abcdef fs=fedcba9876543210 ordinal=0 count=1 extra"};
 for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)CHECK(!depth_replay_parse(invalid[i],&c));CHECK(!depth_replay_parse(NULL,&c)&&!depth_replay_parse("x",NULL));
 uint64_t bytes;CHECK(depth_replay_extent(1344,756,&bytes)&&bytes==4064256);CHECK(depth_replay_extent(1920,1080,&bytes)&&2*DEPTH_REPLAY_MAX_DRAWS*bytes<=DEPTH_REPLAY_MAX_BYTES);CHECK(!depth_replay_extent(0,1,&bytes));CHECK(!depth_replay_extent(UINT32_MAX,UINT32_MAX,&bytes));CHECK(!depth_replay_extent(2048,1025,&bytes));
 TestProgram program={.layout=(VkPipelineLayout)(uintptr_t)77};
 for(int indexed=0;indexed<2;indexed++)for(int dynamic=0;dynamic<2;dynamic++){
  DepthReplayJob j={0};j.key.prog=&program;j.key.stencil=indexed?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_UNDEFINED;j.pipeline=(VkPipeline)(uintptr_t)31;j.image=(VkImage)(uintptr_t)32;j.reference_image=(VkImage)(uintptr_t)33;j.view=(VkImageView)(uintptr_t)34;j.reference_view=(VkImageView)(uintptr_t)35;j.output[0]=(VkBuffer)(uintptr_t)36;j.output[1]=(VkBuffer)(uintptr_t)37;j.width=1344;j.height=756;j.vertices.used_streams=1u|(1u<<16);j.recorded.vp=(VkViewport){3,756,1344,-756,0,1};j.recorded.sc=(VkRect2D){{2,4},{1000,700}};j.recorded.cull=VK_CULL_MODE_BACK_BIT;j.recorded.ff=VK_FRONT_FACE_CLOCKWISE;j.recorded.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;j.recorded.pre=1;j.recorded.db[0]=2;j.recorded.db[1]=3;j.recorded.db[2]=4;j.recorded.ds[0]=(VkDescriptorSet)(uintptr_t)40;j.recorded.ds[1]=(VkDescriptorSet)(uintptr_t)41;j.recorded.ndyn=3;j.recorded.dyn[0]=256;j.recorded.dyn[1]=512;j.recorded.dyn[2]=768;for(unsigned s=0;s<17;s++){j.recorded.vb[s]=(VkBuffer)(uintptr_t)(50+s);j.recorded.vo[s]=s*256;}j.recorded.ib=(VkBuffer)(uintptr_t)99;j.recorded.io=128;j.recorded.it=VK_INDEX_TYPE_UINT16;j.indexed=indexed;j.count=1698;j.instances=2;j.first=7;j.base_vertex=-4;j.base_instance=3;
  unsigned char original[sizeof j];memcpy(original,&j,sizeof j);expected=&j;g_vk.vids=dynamic;rendering=false;copy_count=draws=begins=ends=layouts=barriers=0;depth_replay_emit((VkCommandBuffer)(uintptr_t)9,&j);CHECK(!rendering&&copy_count==2&&draws==1&&begins==1&&ends==1&&barriers==2&&layouts==(unsigned)dynamic);CHECK(!memcmp(original,&j,sizeof j));
 }
 // The actual wrapper must retire game render state, retain exact bindings,
 // and publish a future readback seq without waiting or touching guest inputs.
 DepthReplayJob*j=&depth_replay.jobs[0];memset(j,0,sizeof *j);j->key.prog=&program;j->width=1344;j->height=756;j->image=(VkImage)(uintptr_t)32;j->view=(VkImageView)(uintptr_t)34;j->pipeline=(VkPipeline)(uintptr_t)31;j->output[0]=(VkBuffer)(uintptr_t)36;j->output[1]=(VkBuffer)(uintptr_t)37;j->vertices.used_streams=1;expected=j;
 ss=(TestState){.known=0xffffffff,.vb_known=0x1ffff,.vp={0,756,1344,-756,0,1},.sc={{0,0},{1344,756}},.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.it=VK_INDEX_TYPE_UINT16};ss.ds[0]=(VkDescriptorSet)(uintptr_t)40;ss.ds[1]=(VkDescriptorSet)(uintptr_t)41;ss.vb[0]=(VkBuffer)(uintptr_t)50;ss.ib=(VkBuffer)(uintptr_t)99;
 TestState original_ss=ss;TexBackend depth={.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.levels=1,.layers=1,.width=1344,.height=756,.samples=1,.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.image=(VkImage)(uintptr_t)33};TestTexture texture={.backend=&depth,.addr=0x1234};st.rt.depth=&texture;pass_depth_attachment_view=(VkImageView)(uintptr_t)35;depth_replay.phase=2;in_pass=rendering=true;g_vk.vids=false;copy_count=draws=begins=ends=barriers=0;
 depth_replay_capture(0,true,624,0,-2,3,1,1);CHECK(wrapper_ends==1&&layout_inits==1&&status_calls==1);CHECK(!in_pass&&!rendering&&st.rt_dirty&&ss.known==0&&ss.vb_known==0);CHECK(!memcmp(&j->recorded,&original_ss,sizeof ss));CHECK(j->captured&&j->seq==123&&j->frame==456&&j->reference_image==depth.image&&j->reference_view==pass_depth_attachment_view);CHECK(j->reference_guest==0x1234&&j->indexed&&j->count==624&&j->base_vertex==-2&&j->base_instance==3&&j->instances==1);
 unsigned before=wrapper_ends;depth_replay_capture(-1,true,1,0,0,0,1,1);depth_replay_capture(0,true,1,0,0,0,1,2);CHECK(wrapper_ends==before);
 j->captured=false;depth.samples=2;depth_replay_capture(0,true,1,0,0,0,1,1);CHECK(!j->captured&&depth_replay.phase==3&&wrapper_ends==before);
 printf("{\"state\":\"passed\",\"checks\":%u,\"actual_emitter_cases\":4,\"actual_capture_state_cases\":4}\n",checks);return 0;
}
