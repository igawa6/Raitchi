#define _GNU_SOURCE
#include "nvn/constant_buffer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;++i)CHECK(p[i]==0);}
int main(void){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);uint8_t *map=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);CHECK(!mprotect(map+page,page,PROT_NONE));uint8_t *source=map+page-20;for(unsigned i=0;i<20;++i)source[i]=(uint8_t)(i+1);
 uint8_t *out=malloc(65536+32);CHECK(out);NvnConstantBufferPlan p;
 for(unsigned mode=0;mode<2;++mode){uint64_t extent=mode?65536:256;memset(out,0xa7,65536+32);CHECK(nvn_constant_buffer_plan((uintptr_t)source-128,128,20,extent,65536,&p));CHECK(p.source_address==(uintptr_t)source&&p.source_size==20&&p.staging_size==extent);CHECK(nvn_constant_buffer_copy(&p,out+16,extent));CHECK(!memcmp(out+16,source,20));zero(out+36,(size_t)extent-20);for(unsigned i=0;i<16;++i)CHECK(out[i]==0xa7&&out[16+extent+i]==0xa7);}
 // The arbitrary code end is in an inaccessible page. Empty declarations do
 // not even perform source-address arithmetic, much less read fallback bytes.
 CHECK(nvn_constant_buffer_plan(UINT64_MAX,UINT64_MAX,0,256,65536,&p));CHECK(!p.source_address&&!p.source_size&&p.staging_size==256);memset(out,0xa7,300);CHECK(nvn_constant_buffer_copy(&p,out+16,256));zero(out+16,256);CHECK(out[15]==0xa7&&out[272]==0xa7);
 CHECK(nvn_constant_buffer_plan(0,0,0,0,65536,&p)&&p.staging_size==16);CHECK(nvn_constant_buffer_copy(&p,out,16));zero(out,16);
 uint8_t many[512];for(unsigned i=0;i<sizeof many;++i)many[i]=(uint8_t)i;
 CHECK(nvn_constant_buffer_plan((uintptr_t)many,0,512,65536,64,&p));CHECK(p.source_size==64&&p.staging_size==64);CHECK(nvn_constant_buffer_copy(&p,out,64)&&!memcmp(out,many,64));
 CHECK(nvn_constant_buffer_plan((uintptr_t)source,0,20,UINT64_MAX,16,&p));CHECK(p.source_size==16&&p.staging_size==16);CHECK(nvn_constant_buffer_copy(&p,out,16)&&!memcmp(out,source,16));
 memset(&p,0xa7,sizeof p);CHECK(!nvn_constant_buffer_plan(UINT64_MAX-4,8,20,256,65536,&p));CHECK(!p.source_address&&!p.source_size&&!p.staging_size);
 CHECK(!nvn_constant_buffer_plan(UINT64_MAX-32,0,64,256,16,&p));CHECK(!nvn_constant_buffer_plan(0,128,20,256,65536,&p));CHECK(!nvn_constant_buffer_plan(1,0,20,256,15,&p));CHECK(!nvn_constant_buffer_plan(1,0,20,256,65536,NULL));
 CHECK(nvn_constant_buffer_plan((uintptr_t)source,0,20,256,65536,&p));memset(out,0x5a,300);CHECK(!nvn_constant_buffer_copy(&p,out,255));for(unsigned i=0;i<300;++i)CHECK(out[i]==0x5a);CHECK(!nvn_constant_buffer_copy(&p,source,256));CHECK(!nvn_constant_buffer_copy(&p,NULL,256));CHECK(!nvn_constant_buffer_copy(NULL,out,256));CHECK(!nvn_constant_buffer_copy(&p,(void *)(uintptr_t)(UINTPTR_MAX-20),256));
 NvnConstantBufferPlan invalid={0,20,256};CHECK(!nvn_constant_buffer_copy(&invalid,out,256));invalid=(NvnConstantBufferPlan){(uintptr_t)source,257,256};CHECK(!nvn_constant_buffer_copy(&invalid,out,256));invalid=(NvnConstantBufferPlan){(uintptr_t)source,1,15};CHECK(!nvn_constant_buffer_copy(&invalid,out,256));invalid=(NvnConstantBufferPlan){UINT64_MAX-1,20,256};CHECK(!nvn_constant_buffer_copy(&invalid,out,256));
 // A short source is copied once; canary bytes immediately after its declared
 // range must not become shader padding (even when they happen to be mapped).
 for(unsigned bytes=0;bytes<=512;++bytes){CHECK(nvn_constant_buffer_plan((uintptr_t)many,0,bytes,512,512,&p));memset(out,0xa7,544);CHECK(nvn_constant_buffer_copy(&p,out+16,512));CHECK(!memcmp(out+16,many,bytes));zero(out+16+bytes,512-bytes);CHECK(out[15]==0xa7&&out[528]==0xa7);}
 free(out);CHECK(!munmap(map,page*2));printf("state=passed checks=%u; declared C1 guard-page reads, padding, cap, overflow and canaries\n",checks);
}
