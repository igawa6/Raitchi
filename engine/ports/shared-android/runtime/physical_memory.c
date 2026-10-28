#include "physical_memory.h"
#include <stdlib.h>
#include <string.h>
#define PAGE 4096u
#define MAX_SYSTEM_RESOURCE UINT32_C(0x1fe00000)
static struct {uint64_t alias,size,pages,mapped,metadata;uint32_t total;unsigned char *bits;} memory;
static int bit(uint64_t p){return (memory.bits[p/8]>>(p%8))&1;}
static void set(uint64_t p,int value){unsigned char mask=(unsigned char)(1u<<(p%8));if(value)memory.bits[p/8]|=mask;else memory.bits[p/8]&=(unsigned char)~mask;}
void physical_memory_reset(void){free(memory.bits);memset(&memory,0,sizeof memory);}
int physical_memory_configure(uint32_t total,uint64_t alias,uint64_t size){
 if(total>MAX_SYSTEM_RESOURCE||(total&(PAGE-1))||!size||(alias&(PAGE-1))||(size&(PAGE-1))||size>UINT64_MAX-alias)return PHYS_SIZE;
 if(memory.total||memory.bits)return PHYS_STATE;
 if(!total)return PHYS_OK;
 uint64_t pages=size/PAGE,bytes=(pages+7)/8,charge=(bytes+PAGE-1)&~(uint64_t)(PAGE-1);
 if(charge>total||bytes>SIZE_MAX)return PHYS_RESOURCE;
 unsigned char *bits=calloc(1,(size_t)bytes);if(!bits)return PHYS_RESOURCE;
 memory.alias=alias;memory.size=size;memory.pages=pages;memory.total=total;memory.bits=bits;memory.metadata=charge;return PHYS_OK;
}
uint64_t physical_memory_system_total(void){return memory.total;}
uint64_t physical_memory_system_used(void){return memory.metadata;}
uint64_t physical_memory_mapped(void){return memory.mapped;}
int physical_memory_query(uint64_t address,uint64_t *base,uint64_t *size,int *mapped){
 if(!memory.bits||address<memory.alias||address-memory.alias>=memory.size)return 0;
 uint64_t p=(address-memory.alias)/PAGE,lo=p,hi=p+1;int state=bit(p);
 while(lo&&bit(lo-1)==state)lo--;
 while(hi<memory.pages&&bit(hi)==state)hi++;
 *base=memory.alias+lo*PAGE;*size=(hi-lo)*PAGE;*mapped=state;return 1;
}
int physical_memory_change(uint64_t address,uint64_t size,int map,uint64_t budget,
 PhysicalConflict conflict,PhysicalBacking backing,void *context){
 if(address&(PAGE-1))return PHYS_ADDRESS;
 if(!size||(size&(PAGE-1)))return PHYS_SIZE;
 if(size>UINT64_MAX-address)return PHYS_REGION;
 if(!memory.total)return PHYS_STATE;
 if(address<memory.alias||address-memory.alias>=memory.size||size>memory.size-(address-memory.alias))return PHYS_REGION;
 if(!backing||(conflict&&conflict(address,size,context)))return PHYS_STATE;
 uint64_t first=(address-memory.alias)/PAGE,count=size/PAGE,changed=0;
 for(uint64_t i=0;i<count;i++)changed+=bit(first+i)!=(map!=0);
 uint64_t bytes=changed*PAGE;
 if(map&&(memory.mapped>budget||bytes>budget-memory.mapped))return PHYS_RESOURCE;
 if(!changed)return PHYS_OK;
 /* One backing operation for the entire validated owned span. It must be
  * failure-atomic; on failure the page inventory/accounting stay unchanged. */
 if(!backing(address,size,map!=0,context))return PHYS_RESOURCE;
 if(map){
  for(uint64_t i=0;i<count;){if(bit(first+i)){i++;continue;}uint64_t begin=i;while(i<count&&!bit(first+i))i++;memset((void*)(uintptr_t)(address+begin*PAGE),0,(size_t)((i-begin)*PAGE));}
 }
 for(uint64_t i=0;i<count;i++)set(first+i,map!=0);
 if(map)memory.mapped+=bytes;else memory.mapped-=bytes;
 return PHYS_OK;
}
