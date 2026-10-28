/* Exercises verbatim production state/assembly/cache code with controlled Vulkan outcomes. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
typedef struct { VkShaderModule vs, fs, fs_late; VkPipelineLayout layout; } Prog;
static struct { bool vids, divisor, gpl; VkDevice dev; VkPipelineCache pcache; } g_vk;
static VkResult fake_create(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
static void fake_destroy(VkDevice, VkPipeline, const VkAllocationCallbacks*);
static uint64_t SDL_GetTicksNS(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
#define vkCreateGraphicsPipelines fake_create
#define vkDestroyPipeline fake_destroy
#define LOG(...) ((void)0)
#include "gpl.production.h"
static _Atomic unsigned checks, created[4], links, full, destroyed;
static int fail_part = -1;
static bool fail_link;
#define CHECK(x) do { atomic_fetch_add(&checks,1); if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
static VkResult fake_create(VkDevice d, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)d;(void)cache;(void)alloc; CHECK(count==1);
    static _Atomic unsigned handles;
    *out=(VkPipeline)(uintptr_t)(atomic_fetch_add(&handles,1)+1);
    const VkBaseInStructure *chain=info->pNext;
    if(chain&&chain->sType==VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO){
        const VkPipelineCreationFeedbackCreateInfo *feedback=(const void*)chain;
        feedback->pPipelineCreationFeedback->flags=VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT|VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
        chain=chain->pNext;
    }
    if(info->flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR){
        const VkGraphicsPipelineLibraryCreateInfoEXT *lib=(const void*)chain; unsigned part=0;
        while(part<4&&lib->flags!=gpl_parts[part])part++; CHECK(part<4); atomic_fetch_add(&created[part],1);
        CHECK(info->flags==VK_PIPELINE_CREATE_LIBRARY_BIT_KHR);
        CHECK(part==1||part==2 ? info->layout!=VK_NULL_HANDLE : info->layout==VK_NULL_HANDLE);
        for(unsigned i=0;i<info->pDynamicState->dynamicStateCount;i++) CHECK(gpl_dynamic_part(info->pDynamicState->pDynamicStates[i])==part);
        struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
        return (int)part==fail_part ? VK_ERROR_UNKNOWN : VK_SUCCESS;
    }
    if(chain&&chain->sType==VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR){
        const VkPipelineLibraryCreateInfoKHR *link=(const void*)chain; CHECK(link->libraryCount==4);
        for(unsigned i=0;i<4;i++)CHECK(link->pLibraries[i]); CHECK(!info->flags&&info->layout);
        atomic_fetch_add(&links,1); return fail_link ? VK_ERROR_UNKNOWN : VK_SUCCESS;
    }
    CHECK(info->stageCount==2&&info->pRasterizationState&&info->pDepthStencilState&&info->pMultisampleState);
    atomic_fetch_add(&full,1);return VK_SUCCESS;
}
static void fake_destroy(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a){(void)d;(void)a;CHECK(p);atomic_fetch_add(&destroyed,1);}
static void clear(void){
    for(unsigned i=0;i<GPL_BUCKETS;i++){GplEntry *e=gpl_buckets[i];while(e){GplEntry *next=e->next;free(e);e=next;}gpl_buckets[i]=NULL;}
    gpl_count=0;for(unsigned i=0;i<4;i++)atomic_store(&created[i],0);atomic_store(&links,0);atomic_store(&full,0);atomic_store(&destroyed,0);fail_part=-1;fail_link=false;
}
static Prog prog={(VkShaderModule)1,(VkShaderModule)2,(VkShaderModule)3,(VkPipelineLayout)4};
static PipeKey base(void){PipeKey k={0};k.prog=&prog;k.ncolor=1;k.color[0]=VK_FORMAT_R8G8B8A8_UNORM;k.depth=VK_FORMAT_D32_SFLOAT;k.samples=1;k.poly_fill=1;k.topo_class=2;k.attr_mask=1;k.attr_fmt[0]=VK_FORMAT_R32G32B32A32_SFLOAT;k.used_streams=1;k.stride[0]=16;k.write_mask[0]=15;return k;}
static unsigned changed(PipeKey a,PipeKey b){PipeCI ca,cb;pipe_ci_fill(&ca,&a);pipe_ci_fill(&cb,&b);unsigned mask=0;for(unsigned i=0;i<4;i++){GplKey ka,kb;gpl_key_fill(&ka,&a,&ca,i);gpl_key_fill(&kb,&b,&cb,i);if(memcmp(&ka,&kb,sizeof ka))mask|=1u<<i;}return mask;}
static pthread_barrier_t start;
static void *worker(void *opaque){(void)opaque;PipeKey k=base();PipeCI c;pipe_ci_fill(&c,&k);pthread_barrier_wait(&start);VkPipeline out;GplBuildStats stats;CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS);CHECK(out&&stats.attempted&&!stats.fallback&&stats.hits+stats.misses==4);CHECK(stats.link_driver_hit&&stats.library_driver_hits==stats.misses);return NULL;}
int main(void){
    g_vk.gpl=true;PipeKey k=base(),other=k;CHECK(changed(k,other)==0);
    other.topo_class=1;CHECK(changed(k,other)==1);other=k;other.poly_fill=0;CHECK(changed(k,other)==2);
    other=k;other.stride[0]=32;CHECK(changed(k,other)==1);g_vk.vids=true;CHECK(changed(k,other)==0);g_vk.vids=false;
    other=k;other.samples=4;CHECK(changed(k,other)==12);other=k;other.a2c=1;CHECK(changed(k,other)==12);
    other=k;other.write_mask[0]=7;CHECK(changed(k,other)==8);other=k;other.blend_on=1;other.blend[0][1]=2;CHECK(changed(k,other)==8);
    other=k;other.depth=VK_FORMAT_D24_UNORM_S8_UINT;other.stencil=other.depth;CHECK(changed(k,other)==12);
    other=k;other.color[0]=VK_FORMAT_R16G16B16A16_SFLOAT;CHECK(changed(k,other)==12);
    Prog distinct=prog;other=k;other.prog=&distinct;CHECK(changed(k,other)==0);distinct.vs=(VkShaderModule)99;CHECK(changed(k,other)==2);distinct=prog;distinct.fs=(VkShaderModule)99;CHECK(changed(k,other)==4);distinct=prog;distinct.layout=(VkPipelineLayout)99;CHECK(changed(k,other)==6);
    PipeCI c;pipe_ci_fill(&c,&k);unsigned dynamic=0;for(unsigned i=0;i<4;i++){GplCI ci;gpl_ci_fill(&ci,&c,i);dynamic+=ci.dy.dynamicStateCount;if(i==2||i==3)CHECK(ci.pi.pMultisampleState==&c.ms);if(i==2)CHECK(ci.pi.pDepthStencilState==&c.ds);if(i==3)CHECK(ci.pi.pColorBlendState==&c.cb);if(i==1)CHECK(ci.rendering.colorAttachmentCount==0&&!ci.rendering.pColorAttachmentFormats);}CHECK(dynamic==NDYN);
    CHECK(!pthread_barrier_init(&start,NULL,32));pthread_t threads[32];for(unsigned i=0;i<32;i++)CHECK(!pthread_create(&threads[i],NULL,worker,NULL));for(unsigned i=0;i<32;i++)CHECK(!pthread_join(threads[i],NULL));CHECK(!pthread_barrier_destroy(&start));for(unsigned i=0;i<4;i++)CHECK(atomic_load(&created[i])==1);CHECK(atomic_load(&links)==32&&atomic_load(&full)==0&&gpl_count==4);
    VkPipeline out;GplBuildStats stats;other=k;other.write_mask[0]=7;PipeCI changed_ci;pipe_ci_fill(&changed_ci,&other);CHECK(pipe_create_graphics(&other,&changed_ci,&out,&stats)==VK_SUCCESS&&stats.hits==3&&stats.misses==1);CHECK(atomic_load(&created[0])==1&&atomic_load(&created[3])==2);
    clear();GplKey probe_key;PipeKey collide=k;PipeCI collision_ci;unsigned target=0;bool found=false;
    gpl_key_fill(&probe_key,&k,&c,0);target=hash_words(0xcbf29ce484222325ull,&probe_key,sizeof probe_key)&(GPL_BUCKETS-1);
    for(unsigned stride=17;stride<100000;stride++){collide.stride[0]=stride;pipe_ci_fill(&collision_ci,&collide);gpl_key_fill(&probe_key,&collide,&collision_ci,0);if((hash_words(0xcbf29ce484222325ull,&probe_key,sizeof probe_key)&(GPL_BUCKETS-1))==target){found=true;break;}}
    CHECK(found);GplBuildStats collision_stats={0};VkPipeline first,second;CHECK(gpl_library_get(&k,&c,0,&first,&collision_stats)==VK_SUCCESS);CHECK(gpl_library_get(&collide,&collision_ci,0,&second,&collision_stats)==VK_SUCCESS);CHECK(first!=second&&collision_stats.misses==2&&gpl_count==2);
    clear();fail_part=2;for(unsigned i=0;i<2;i++){CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS&&stats.fallback&&out);}CHECK(atomic_load(&created[2])==1&&atomic_load(&full)==2&&atomic_load(&destroyed)==1&&atomic_load(&links)==0);
    clear();fail_link=true;CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS&&stats.fallback&&out);CHECK(atomic_load(&full)==1&&atomic_load(&destroyed)==1);
    clear();gpl_count=GPL_LIMIT;CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS&&stats.fallback);CHECK(atomic_load(&full)==1);gpl_count=0;
    clear();g_vk.gpl=false;CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS&&!stats.attempted&&atomic_load(&full)==1);g_vk.gpl=true;c.pi.flags=VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;CHECK(pipe_create_graphics(&k,&c,&out,&stats)==VK_SUCCESS&&!stats.attempted&&atomic_load(&full)==2);
    clear();printf("%u assertions: exact subset keys/state,32 concurrent builders,one library owner per key,failed-library memoization,link/capacity/full fallback\n",atomic_load(&checks));return 0;
}
