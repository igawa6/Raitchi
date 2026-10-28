#define _GNU_SOURCE
#include "physical_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned checks,backing_calls;static int fail_backing;static uint64_t foreign;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int conflict(uint64_t a,uint64_t n,void*c){(void)c;return foreign&&a<foreign+4096&&foreign<a+n;}
static int backing(uint64_t a,uint64_t n,int map,void*c){(void)c;backing_calls++;if(fail_backing){fail_backing=0;return 0;}if(map)return !mprotect((void*)(uintptr_t)a,n,PROT_READ|PROT_WRITE);return mmap((void*)(uintptr_t)a,n,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void*)(uintptr_t)a;}
static int change(uint64_t a,uint64_t n,int map,uint64_t budget){return physical_memory_change(a,n,map,budget,conflict,backing,NULL);}
int main(void){
 size_t bytes=32u<<20;unsigned char*p=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);uint64_t base=(uintptr_t)p;
 CHECK(physical_memory_configure(1,base,bytes)==PHYS_SIZE);CHECK(physical_memory_configure(0x20000000,base,bytes)==PHYS_SIZE);
 CHECK(physical_memory_configure(0,base,bytes)==PHYS_OK);CHECK(change(base,4096,1,bytes)==PHYS_STATE);
 CHECK(physical_memory_configure(0x4000,base,bytes)==PHYS_OK);CHECK(physical_memory_system_total()==0x4000&&physical_memory_system_used()==4096);
 CHECK(change(base+1,4096,1,bytes)==PHYS_ADDRESS);CHECK(change(base,0,1,bytes)==PHYS_SIZE);CHECK(change(base,4097,1,bytes)==PHYS_SIZE);
 CHECK(change(UINT64_MAX-4095,8192,1,bytes)==PHYS_REGION);CHECK(change(base-4096,4096,1,bytes)==PHYS_REGION);CHECK(change(base+bytes,4096,1,bytes)==PHYS_REGION);
 foreign=base+4096;CHECK(change(base,8192,1,bytes)==PHYS_STATE&&backing_calls==0);foreign=0;
 fail_backing=1;CHECK(change(base,8192,1,bytes)==PHYS_RESOURCE&&physical_memory_mapped()==0);
 CHECK(change(base,16384,1,bytes)==PHYS_OK);CHECK(physical_memory_mapped()==16384);memset(p,0x5a,16384);
 CHECK(change(base+4096,16384,1,bytes)==PHYS_OK);for(unsigned i=0;i<16384;i++)CHECK(p[i]==0x5a);for(unsigned i=16384;i<20480;i++)CHECK(p[i]==0);
 CHECK(physical_memory_mapped()==20480);CHECK(change(base+32768,4096,1,20480)==PHYS_RESOURCE);CHECK(physical_memory_mapped()==20480);
 uint64_t qbase,qsize;int mapped;CHECK(physical_memory_query(base+5000,&qbase,&qsize,&mapped)&&mapped&&qbase==base&&qsize==20480);
 fail_backing=1;CHECK(change(base+4096,8192,0,bytes)==PHYS_RESOURCE&&physical_memory_mapped()==20480&&p[4096]==0x5a);
 CHECK(change(base+4096,8192,0,bytes)==PHYS_OK&&physical_memory_mapped()==12288);CHECK(p[0]==0x5a&&p[12288]==0x5a);
 CHECK(physical_memory_query(base+5000,&qbase,&qsize,&mapped)&&!mapped&&qbase==base+4096&&qsize==8192);
 CHECK(change(base+4096,8192,1,bytes)==PHYS_OK);for(unsigned i=4096;i<12288;i++)CHECK(p[i]==0);
 CHECK(change(base,bytes,0,bytes)==PHYS_OK&&physical_memory_mapped()==0);CHECK(change(base,bytes,0,bytes)==PHYS_OK);
 /* NPDM resource metadata is not the user physical mapping budget. */
 CHECK(change(base,20u<<20,1,bytes)==PHYS_OK&&physical_memory_mapped()==(20u<<20));CHECK(physical_memory_system_used()==4096);
 physical_memory_reset();CHECK(!physical_memory_system_total()&&!physical_memory_system_used()&&!physical_memory_mapped());CHECK(!physical_memory_query(base,&qbase,&qsize,&mapped));
 CHECK(!munmap(p,bytes));printf("state=passed checks=%u; real backing/partial unmap/replay; controlled failure-atomic callback rollback; no guest execution\n",checks);return 0;
}
