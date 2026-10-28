#define _GNU_SOURCE
#include "constant_buffer.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
typedef uint64_t VkBuffer;
typedef struct {VkBuffer buf;uint64_t off;void *ptr;} VkSpan;
typedef struct {VkBuffer buffer;uint64_t offset,range;} VkDescriptorBufferInfo;
typedef struct {uint64_t code_addr;uint32_t const_offset,const_size;} NvnShaderStage;
static struct {struct {struct {uint32_t maxUniformBufferRange;} limits;} props;} g_vk;
static uint64_t pending_seq=1,gcopy_gen;
static unsigned allocations,checks;
static struct {uint8_t *raw;uint64_t size;} allocated[64];
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
#define FATAL(...) abort()
#define LOG(...) ((void)0)
static uint64_t vk_pending_seq(void){return pending_seq;}
static VkSpan vk_transient(uint64_t size,uint64_t alignment){
 CHECK(alignment==256&&allocations<64&&size<=65536);unsigned index=allocations++;
 allocated[index].raw=malloc((size_t)size+16);CHECK(allocated[index].raw);allocated[index].size=size;
 memset(allocated[index].raw,0xa5,(size_t)size+16);
 return (VkSpan){index+1,4096ull*(index+1),allocated[index].raw+8};
}
static VkDescriptorBufferInfo ubo_span(VkSpan span,uint64_t size){return (VkDescriptorBufferInfo){span.buf,span.off,size};}
#include "compiler_constants_production.h"
static uint8_t *bytes(VkDescriptorBufferInfo d){CHECK(d.buffer&&d.buffer<=allocations);CHECK(d.offset==4096*d.buffer);return allocated[d.buffer-1].raw+8;}
static void contents(VkDescriptorBufferInfo d,const uint8_t *source,size_t n){uint8_t *p=bytes(d);CHECK(d.range>=n);CHECK(!memcmp(p,source,n));for(uint64_t i=n;i<d.range;i++)CHECK(p[i]==0);}
static void canaries(void){for(unsigned i=0;i<allocations;i++)for(unsigned j=0;j<8;j++){CHECK(allocated[i].raw[j]==0xa5);CHECK(allocated[i].raw[8+allocated[i].size+j]==0xa5);}}
int main(void){
 unsetenv("LARECOMP_LOG_CBUF");size_t page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page>=256);
 uint8_t *mapping=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);CHECK(!mprotect(mapping+page,page,PROT_NONE));
 uint8_t *source=mapping+page-20;for(unsigned i=0;i<20;i++)source[i]=(uint8_t)(i+1);
 NvnShaderStage shader={(uintptr_t)source-256,256,20};g_vk.props.limits.maxUniformBufferRange=65536;
 VkDescriptorBufferInfo first=compiler_constants_span(&shader,256);CHECK(first.range==256);contents(first,source,20);unsigned before=allocations;
 // A cache hit must not allocate or touch the source, even if it disappeared.
 CHECK(!mprotect(mapping,page,PROT_NONE));VkDescriptorBufferInfo hit=compiler_constants_span(&shader,256);CHECK(hit.buffer==first.buffer&&allocations==before);CHECK(!mprotect(mapping,page,PROT_READ|PROT_WRITE));
 source[0]=77;++gcopy_gen;VkDescriptorBufferInfo gen=compiler_constants_span(&shader,256);CHECK(gen.buffer!=first.buffer&&allocations==before+1);contents(gen,source,20);
 before=allocations;++pending_seq;source[1]=88;VkDescriptorBufferInfo seq=compiler_constants_span(&shader,256);CHECK(seq.buffer!=gen.buffer&&allocations==before+1);contents(seq,source,20);
 before=allocations;shader.const_size=16;VkDescriptorBufferInfo shorter=compiler_constants_span(&shader,256);CHECK(allocations==before+1&&shorter.buffer!=seq.buffer);contents(shorter,source,16);
 before=allocations;VkDescriptorBufferInfo wider=compiler_constants_span(&shader,512);CHECK(allocations==before+1&&wider.range==512);contents(wider,source,16);
 before=allocations;shader.code_addr-=4;VkDescriptorBufferInfo moved=compiler_constants_span(&shader,512);CHECK(allocations==before+1&&moved.buffer!=wider.buffer);contents(moved,source-4,16);
 // Device range changes affect the actual staging key, not just the descriptor.
 before=allocations;g_vk.props.limits.maxUniformBufferRange=128;VkDescriptorBufferInfo capped=compiler_constants_span(&shader,512);CHECK(capped.range==128&&allocations==before+1);contents(capped,source-4,16);
 // Empty declaration must not even form code+offset (both are deliberately bad).
 shader=(NvnShaderStage){UINT64_MAX,UINT32_MAX,0};before=allocations;VkDescriptorBufferInfo zero=compiler_constants_span(&shader,256);CHECK(zero.range==128&&allocations==before+1);uint8_t dummy=0;contents(zero,&dummy,0);
 shader.code_addr=1;shader.const_offset=1;before=allocations;VkDescriptorBufferInfo samezero=compiler_constants_span(&shader,256);CHECK(samezero.buffer==zero.buffer&&allocations==before); // same empty plan
 g_vk.props.limits.maxUniformBufferRange=65536;shader=(NvnShaderStage){(uintptr_t)source-256,256,20};VkDescriptorBufferInfo large=compiler_constants_span(&shader,65536);CHECK(large.range==65536);contents(large,source,20);
 // Cap the readable source too when the declaration exceeds the device range.
 uint8_t local[80];memset(local,0x37,sizeof local);shader=(NvnShaderStage){(uintptr_t)local,0,80};g_vk.props.limits.maxUniformBufferRange=32;VkDescriptorBufferInfo clipped=compiler_constants_span(&shader,256);CHECK(clipped.range==32);contents(clipped,local,32);
 pid_t child=fork();CHECK(child>=0);if(!child){g_vk.props.limits.maxUniformBufferRange=15;(void)compiler_constants_span(&shader,256);_exit(2);}int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
 canaries();for(unsigned i=0;i<allocations;i++)free(allocated[i].raw);CHECK(!munmap(mapping,page*2));
 printf("{\"passed\":true,\"checks\":%u,\"allocations\":%u,\"guard_page_declared_bytes\":20}\n",checks,allocations);return 0;
}
