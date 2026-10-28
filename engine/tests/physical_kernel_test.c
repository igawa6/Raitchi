#define _GNU_SOURCE
#include "physical_memory.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
static uint64_t alias_base,alias_size;
#define ALIAS_BASE alias_base
#define ALIAS_SIZE alias_size
#define HEAP_BASE (alias_base+alias_size+4096)
#define HEAP_MAX (32u<<20)
#define R_OUT_OF_RESOURCE 0xd001u
#undef R_OK
#define R_OK 0u
#define FATAL(...) do{fprintf(stderr,__VA_ARGS__);abort();}while(0)
static void plat_commit(uint64_t a,uint64_t n){(void)a;(void)n;}
static int inject_failure;
static int real_mprotect(void *address,size_t size,int prot){return mprotect(address,size,prot);}
static void *real_mmap(void*a,size_t size,int prot,int flags,int fd,off_t off){return mmap(a,size,prot,flags,fd,off);}
static int checked_mprotect(void*a,size_t n,int prot){if(inject_failure==1)return -1;return real_mprotect(a,n,prot);}
static void *checked_mmap(void*a,size_t n,int prot,int flags,int fd,off_t off){if(inject_failure==2)return MAP_FAILED;return real_mmap(a,n,prot,flags,fd,off);}
#define mprotect checked_mprotect
#define mmap checked_mmap
#include "physical_kernel_bodies.h"
#undef mmap
#undef mprotect
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int writable(uint64_t address){
 FILE*f=fopen("/proc/self/maps","r");if(!f)abort();char line[512],mode[5];unsigned long long lo,hi;int result=-1;
 while(fgets(line,sizeof line,f))if(sscanf(line,"%llx-%llx %4s",&lo,&hi,mode)==3&&address>=lo&&address<hi){result=mode[1]=='w';break;}
 fclose(f);return result;
}
int main(void){
 alias_size=32u<<20;void*p=mmap(NULL,alias_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);alias_base=(uintptr_t)p;
 CHECK(writable(alias_base)==1);CHECK(!kernel_configure_system_resources(16u<<20));CHECK(writable(alias_base)==0);CHECK(physical_memory_system_total()==(16u<<20));
 uint64_t baseline=UINT64_C(0xcd500000)-UINT64_C(0x4000000)-(16u<<20);CHECK(physical_budget()==baseline);heap_size=65536;CHECK(physical_budget()==baseline-65536);heap_size=UINT64_C(0xcd500000);CHECK(!physical_budget());heap_size=0;
 region_add(alias_base+8192,4096,6,3);CHECK(physical_memory_change(alias_base,16384,1,physical_budget(),physical_conflict,physical_backing,NULL)==PHYS_STATE);
 uint64_t info[5];query_memory(alias_base,(uintptr_t)info);CHECK(info[0]==alias_base&&info[1]==8192&&info[2]==0);
 query_memory(alias_base+8192,(uintptr_t)info);CHECK(info[0]==alias_base+8192&&info[1]==4096&&info[2]==6);
 CHECK(physical_memory_change(alias_base,8192,1,physical_budget(),physical_conflict,physical_backing,NULL)==PHYS_OK);
 CHECK(physical_owned_overlap(alias_base,4096)&&!physical_owned_overlap(alias_base+16384,4096));
 query_memory(alias_base+4096,(uintptr_t)info);CHECK(info[0]==alias_base&&info[1]==8192&&info[2]==5&&info[3]==3);
 query_memory(alias_base+16384,(uintptr_t)info);CHECK(info[0]==alias_base+12288&&info[1]==alias_size-12288&&info[2]==0);
 uint64_t value;CHECK(physical_memory_info(6,&value)&&value==UINT64_C(0xcd500000));CHECK(physical_memory_info(21,&value)&&value==UINT64_C(0xcd500000)-(16u<<20));CHECK(physical_memory_info(16,&value)&&value==(16u<<20));CHECK(physical_memory_info(17,&value)&&value==4096);CHECK(physical_memory_info(7,&value)&&value==UINT64_C(0x4000000)+(16u<<20)+8192);CHECK(physical_memory_info(22,&value)&&value==UINT64_C(0x4000000)+8192);
 uint64_t addresses[4],sizes[4];CHECK(kernel_data_regions(addresses,sizes,4)==2);
 CHECK(physical_memory_change(alias_base+4096,4096,0,physical_budget(),physical_conflict,physical_backing,NULL)==PHYS_OK);
 query_memory(alias_base+4096,(uintptr_t)info);CHECK(info[0]==alias_base+4096&&info[1]==4096&&info[2]==0);
 CHECK(physical_memory_change(alias_base+4096,4096,1,physical_budget(),physical_conflict,physical_backing,NULL)==PHYS_OK);
 region_remove(alias_base+8192);query_memory(alias_base+16384,(uintptr_t)info);CHECK(info[0]==alias_base+8192&&info[1]==alias_size-8192&&info[2]==0);
 CHECK(physical_memory_change(alias_base,alias_size,0,physical_budget(),physical_conflict,physical_backing,NULL)==PHYS_OK);
 CHECK(prepare_alias_write(alias_base+4096,4096));((unsigned char*)p)[4096]=0x44;CHECK(((unsigned char*)p)[4096]==0x44);CHECK(!prepare_alias_write(alias_base+1,4096));
 CHECK(!prepare_alias_write(alias_base-4096,8192));CHECK(!prepare_alias_write(alias_base+alias_size-4096,8192));CHECK(!prepare_alias_write(UINT64_MAX-4095,8192));
 for(int failure=1;failure<=2;failure++){
  pid_t child=fork();CHECK(child>=0);if(!child){inject_failure=failure;physical_backing(alias_base,4096,failure==1,NULL);_exit(99);}
  int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
 }
 /* First heap region cannot publish into a full region table. */
 int old_count=nregions;Region saved[1024];memcpy(saved,regions,sizeof saved);nregions=1024;
 for(int i=0;i<1024;i++)regions[i]=(Region){UINT64_C(0x100000)+i*4096ull,4096,4,3};
 uint64_t output=UINT64_C(0xdeadbeef),old_heap=heap_size;
 CHECK(set_heap_size(4096,&output)==R_OUT_OF_RESOURCE&&output==UINT64_C(0xdeadbeef)&&heap_size==old_heap&&nregions==1024);
 regions[0].addr=HEAP_BASE;heap_size=8192;
 CHECK(set_heap_size(4096,&output)==R_OK&&output==HEAP_BASE&&heap_size==4096&&nregions==1024);
 heap_size=old_heap;memcpy(regions,saved,sizeof saved);nregions=old_count;
 reset_physical_memory();CHECK(!physical_memory_system_total());CHECK(!kernel_configure_system_resources(0));CHECK(writable(alias_base+8192)==1);
 /* All alias pages return to initial RW, including a previously released page. */
 ((unsigned char*)p)[8192]=0x66;CHECK(((unsigned char*)p)[8192]==0x66);CHECK(prepare_alias_write(alias_base,4096));
 physical_memory_reset();CHECK(!munmap(p,alias_size));printf("state=passed checks=%u; actual kernel region/query/backing/resource helpers\n",checks);return 0;
}
