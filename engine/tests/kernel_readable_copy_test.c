#define _GNU_SOURCE
#include "physical_memory.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
typedef struct {uint64_t addr,size;uint32_t type,perm;} Region;
static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
static Region regions[1024];static int nregions;
static void *watched;static unsigned writes,checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static void *checked_copy(void *dst,const void *src,size_t size) {
 if(dst==watched){CHECK(pthread_mutex_trylock(&memory_lock)==EBUSY);++writes;}
 return memcpy(dst,src,size);
}
#define memcpy checked_copy
#include "kernel_readable_copy_production.h"
#undef memcpy
static int backing(uint64_t a,uint64_t size,int mapped,void *unused){(void)unused;return !mprotect((void*)(uintptr_t)a,(size_t)size,mapped?PROT_READ|PROT_WRITE:PROT_NONE);}
static uint8_t *arena;static size_t page=4096;
static int change(unsigned index,int mapped){return physical_memory_change((uintptr_t)arena+page*index,page,mapped,UINT64_MAX,NULL,backing,NULL);}
static void add(unsigned index,uint32_t state,uint32_t perm){regions[nregions++]=(Region){(uintptr_t)arena+page*index,page,state,perm};}
static void unchanged(const uint8_t *data,size_t size){for(size_t i=0;i<size;i++)CHECK(data[i]==0xa5);}
static void *racer(void *arg){(void)arg;for(int i=0;i<1000;i++){pthread_mutex_lock(&memory_lock);if(change(0,0)||change(0,1))abort();memset(arena,0x5a,page);pthread_mutex_unlock(&memory_lock);}return NULL;}
int main(void){
 CHECK(sysconf(_SC_PAGESIZE)==4096);arena=mmap(NULL,page*8,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(arena!=MAP_FAILED);
 CHECK(physical_memory_configure(16u<<20,(uintptr_t)arena,page*4)==PHYS_OK);
 pthread_mutex_lock(&memory_lock);CHECK(change(0,1)==PHYS_OK);CHECK(change(3,1)==PHYS_OK);pthread_mutex_unlock(&memory_lock);
 memset(arena,0x5a,page);memset(arena+3*page,0x33,page);
 CHECK(!mprotect(arena+2*page,page*4,PROT_READ|PROT_WRITE));memset(arena+2*page,0x22,page);memset(arena+4*page,0x44,page);memset(arena+5*page,0x55,page);
 add(2,5,1);add(4,3,1);add(5,4,3);CHECK(!mprotect(arena+4*page,page,PROT_READ));
 uint8_t *out=malloc(page*4);CHECK(out);watched=out;
 CHECK(kernel_copy_readable(out,(uintptr_t)arena,page));CHECK(writes==1);for(size_t i=0;i<page;i++)CHECK(out[i]==0x5a);
 memset(out,0xa5,page*4);writes=0;CHECK(!kernel_copy_readable(out,(uintptr_t)arena,page*2));CHECK(!writes);unchanged(out,page*4); // readable prefix, free physical hole
 CHECK(kernel_copy_readable(out,(uintptr_t)arena+2*page,page*4));CHECK(writes==1);for(unsigned i=0;i<4;i++)for(size_t j=0;j<page;j++)CHECK(out[i*page+j]==(uint8_t)(0x22+0x11*i)); // foreign+physical+RO+RW
 regions[2].perm=2;memset(out,0xa5,page*4);writes=0;CHECK(!kernel_copy_readable(out,(uintptr_t)arena+4*page,page*2));CHECK(!writes);unchanged(out,page*4);
 regions[2].perm=3;regions[2].type=0x10;CHECK(!kernel_copy_readable(out,(uintptr_t)arena+5*page,1));CHECK(!writes);regions[2].type=0;CHECK(!kernel_copy_readable(out,(uintptr_t)arena+5*page,1));regions[2].type=4;
 CHECK(!kernel_copy_readable(NULL,(uintptr_t)arena,1));CHECK(!kernel_copy_readable(out,0,1));CHECK(!kernel_copy_readable(out,(uintptr_t)arena,0));
 CHECK(!kernel_copy_readable(out,(uintptr_t)arena,KERNEL_READABLE_COPY_MAX+1));CHECK(!kernel_copy_readable(out,UINT64_MAX-4,8));CHECK(!kernel_copy_readable((void*)(UINTPTR_MAX-4),(uintptr_t)arena,8));
 CHECK(!kernel_copy_readable(arena+1,(uintptr_t)arena,8));CHECK(!kernel_copy_readable(out,(uintptr_t)arena+6*page,1));CHECK(!writes);unchanged(out,page*4);
 CHECK(kernel_copy_readable(out,(uintptr_t)arena+4*page-20,20));for(unsigned i=0;i<20;i++)CHECK(out[i]==0x33);
 // Existing mapping lock prevents physical unmap between validation and copy.
 watched=NULL;pthread_t writer;CHECK(!pthread_create(&writer,NULL,racer,NULL));for(int i=0;i<1000;i++){CHECK(kernel_copy_readable(out,(uintptr_t)arena,page));for(size_t j=0;j<page;j++)CHECK(out[j]==0x5a);}CHECK(!pthread_join(writer,NULL));
 physical_memory_reset();nregions=0;free(out);CHECK(!munmap(arena,page*8));
 printf("{\"passed\":true,\"checks\":%u,\"concurrent_mapping_cycles\":1000,\"copy_cap\":%llu}\n",checks,(unsigned long long)KERNEL_READABLE_COPY_MAX);return 0;
}
