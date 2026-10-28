#define _GNU_SOURCE
#include "native_sdk_fastpath.h"
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NATIVE_SDK_RELOCATION_STANDALONE
#include "runtime/session_layout.h"
#include <stdatomic.h>
#endif

typedef struct MappedRange {uint64_t lo,hi;bool writable;} MappedRange;
typedef struct MappedRanges {MappedRange *items;size_t count,capacity;} MappedRanges;
typedef struct TableView {
    uint64_t table,file,flag,sections,entries,metadata_end;
    uint32_t count;
} TableView;
static uint16_t read16(uint64_t address) {uint16_t v;memcpy(&v,(const void *)(uintptr_t)address,2);return v;}
static uint32_t read32(uint64_t address) {uint32_t v;memcpy(&v,(const void *)(uintptr_t)address,4);return v;}
static uint64_t read64(uint64_t address) {uint64_t v;memcpy(&v,(const void *)(uintptr_t)address,8);return v;}
static int add(uint64_t a,uint64_t b,uint64_t *out) {if(a>UINT64_MAX-b)return 0;*out=a+b;return 1;}
static int overlap(uint64_t lo,uint64_t size,uint64_t other_lo,uint64_t other_hi) {return lo<other_hi&&other_lo<lo+size;}
static enum NativeSdkRelocationStatus maps_read(MappedRanges *maps) {
    FILE *file=fopen("/proc/self/maps","r");if(!file)return NATIVE_SDK_RELOCATION_RANGE;
    char line[2048],protection[5];unsigned long long lo,hi;
    while(fgets(line,sizeof line,file)) {
        if(sscanf(line,"%llx-%llx %4s",&lo,&hi,protection)!=3||protection[0]!='r')continue;
        if(maps->count==maps->capacity) {
            size_t capacity=maps->capacity?maps->capacity*2:128;
            if(capacity>65536) {fclose(file);return NATIVE_SDK_RELOCATION_NO_MEMORY;}
            void *memory=realloc(maps->items,capacity*sizeof *maps->items);
            if(!memory) {fclose(file);return NATIVE_SDK_RELOCATION_NO_MEMORY;}
            maps->items=memory;maps->capacity=capacity;
        }
        maps->items[maps->count++]=(MappedRange){lo,hi,protection[1]=='w'};
    }
    int error=ferror(file);fclose(file);return error?NATIVE_SDK_RELOCATION_RANGE:NATIVE_SDK_RELOCATION_OK;
}
static int mapped(const MappedRanges *maps,uint64_t address,uint64_t size,int writing,const NativeSdkRelocationBounds *bounds) {
    uint64_t end;if(!size||!add(address,size,&end))return 0;
    size_t lo=0,hi=maps->count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(maps->items[mid].hi<=address)lo=mid+1;else hi=mid;}
    while(address<end&&lo<maps->count) {
        const MappedRange *range=&maps->items[lo++];if(range->lo>address||range->hi<=address)return 0;
        uint64_t next=range->hi<end?range->hi:end;
        if(writing&&!range->writable&&(!bounds||bounds->tracked_write_hi<=bounds->tracked_write_lo||
            address<bounds->tracked_write_lo||next>bounds->tracked_write_hi))return 0;
        address=next;
    }
    return address==end;
}
static enum NativeSdkRelocationStatus table_view(const MappedRanges *maps,uint64_t table,const NativeSdkRelocationBounds *bounds,TableView *view) {
    if(!mapped(maps,table,12,0,bounds))return NATIVE_SDK_RELOCATION_RANGE;
    uint32_t offset=read32(table+4);if(table<offset)return NATIVE_SDK_RELOCATION_OVERFLOW;
    view->table=table;view->file=table-offset;
    if(!add(view->file,0x14,&view->flag)||!mapped(maps,view->flag,2,1,bounds))return NATIVE_SDK_RELOCATION_RANGE;
    int32_t count=(int32_t)read32(table+8);view->count=count>0?(uint32_t)count:0;
    if(!add(table,16,&view->sections)||!add(view->sections,(uint64_t)view->count*24,&view->entries))return NATIVE_SDK_RELOCATION_OVERFLOW;
    view->metadata_end=view->entries;
    if(!view->count)return NATIVE_SDK_RELOCATION_OK;
    if(!mapped(maps,view->sections,(uint64_t)view->count*24,0,bounds))return NATIVE_SDK_RELOCATION_RANGE;
    uint64_t largest=0;
    for(uint32_t i=0;i<view->count;i++) {
        uint64_t section=view->sections+(uint64_t)i*24;
        int32_t count_entries=(int32_t)read32(section+20);
        if(count_entries<=0)continue;
        int32_t first=(int32_t)read32(section+16);
        if(first<0)return NATIVE_SDK_RELOCATION_METADATA;
        uint64_t end=(uint64_t)first+(uint32_t)count_entries;
        if(end>INT32_MAX)return NATIVE_SDK_RELOCATION_OVERFLOW;
        if(end>largest)largest=end;
    }
    if(!add(view->entries,largest*8,&view->metadata_end))return NATIVE_SDK_RELOCATION_OVERFLOW;
    if(largest&&!mapped(maps,view->entries,largest*8,0,bounds))return NATIVE_SDK_RELOCATION_RANGE;
    if(overlap(view->flag,2,view->table,view->metadata_end))return NATIVE_SDK_RELOCATION_METADATA;
    return NATIVE_SDK_RELOCATION_OK;
}
static enum NativeSdkRelocationStatus preflight(const MappedRanges *maps,const TableView *view,const NativeSdkRelocationBounds *bounds,NativeSdkRelocationResult *result) {
    result->sections=view->count;
    for(uint32_t s=0;s<view->count;s++) {
        uint64_t section=view->sections+(uint64_t)s*24;
        int32_t count=(int32_t)read32(section+20);if(count<=0)continue;
        uint32_t first=read32(section+16);
        for(uint32_t j=0;j<(uint32_t)count;j++) {
            uint64_t entry=view->entries+((uint64_t)first+j)*8;
            unsigned groups=read16(entry+4),pointers=*(const uint8_t *)(uintptr_t)(entry+6),gap=*(const uint8_t *)(uintptr_t)(entry+7);
            if(!add(result->entries,1,&result->entries))return NATIVE_SDK_RELOCATION_OVERFLOW;
            if(!groups||!pointers)continue;
            uint64_t first_target,span=(uint64_t)(groups-1)*(pointers+gap)*8+(uint64_t)pointers*8,end;
            if(!add(view->file,read32(entry),&first_target)||!add(first_target,span,&end))return NATIVE_SDK_RELOCATION_OVERFLOW;
            int disjoint=!overlap(first_target,span,view->table,view->metadata_end)&&!overlap(first_target,span,view->flag,view->flag+2);
            if(!disjoint||!mapped(maps,first_target,span,1,bounds)) {
                for(unsigned group=0;group<groups;group++) {
                    uint64_t at=first_target+(uint64_t)group*(pointers+gap)*8,bytes=(uint64_t)pointers*8;
                    if(overlap(at,bytes,view->table,view->metadata_end)||overlap(at,bytes,view->flag,view->flag+2))return NATIVE_SDK_RELOCATION_METADATA;
                    if(!mapped(maps,at,bytes,1,bounds))return NATIVE_SDK_RELOCATION_RANGE;
                }
            }
            if(!add(result->groups,groups,&result->groups)||!add(result->pointers,(uint64_t)groups*pointers,&result->pointers))return NATIVE_SDK_RELOCATION_OVERFLOW;
        }
    }
    return NATIVE_SDK_RELOCATION_OK;
}
enum NativeSdkRelocationStatus native_sdk_relocate_table_ex(uint64_t table,const NativeSdkRelocationBounds *bounds,NativeSdkRelocationResult *output) {
    if(!table)return NATIVE_SDK_RELOCATION_ARGUMENT;
    MappedRanges maps={0};TableView view={0};NativeSdkRelocationResult result={0};
    enum NativeSdkRelocationStatus status=maps_read(&maps);
    if(status==NATIVE_SDK_RELOCATION_OK)status=table_view(&maps,table,bounds,&view);
    if(status==NATIVE_SDK_RELOCATION_OK)status=preflight(&maps,&view,bounds,&result);
    free(maps.items);if(status!=NATIVE_SDK_RELOCATION_OK)return status;
    /* Inputs are caller-owned/quiescent, and preflight rejected any fixup that
     * could change metadata or flags while this exact original order runs. */
    for(uint32_t s=0;s<view.count;s++) {
        uint64_t section=view.sections+(uint64_t)s*24,base=read64(section);
        uint64_t addend=base?base-read32(section+8):view.file;
        int32_t count=(int32_t)read32(section+20);if(count<=0)continue;
        uint32_t first=read32(section+16);
        for(uint32_t j=0;j<(uint32_t)count;j++) {
            uint64_t entry=view.entries+((uint64_t)first+j)*8;
            unsigned groups=read16(entry+4),pointers=*(const uint8_t *)(uintptr_t)(entry+6),gap=*(const uint8_t *)(uintptr_t)(entry+7);
            if(!groups||!pointers)continue;
            uint64_t at=view.file+read32(entry);
            for(unsigned group=0;group<groups;group++,at+=(uint64_t)(pointers+gap)*8) {
                for(unsigned pointer=0;pointer<pointers;pointer++) {
                    uint64_t position=at+(uint64_t)pointer*8,value=read64(position);
                    if(value){value+=addend;memcpy((void *)(uintptr_t)position,&value,8);result.fixed_pointers++;}
                    else result.null_pointers++;
                }
            }
        }
    }
    uint16_t flags=read16(view.flag)|1u;memcpy((void *)(uintptr_t)view.flag,&flags,2);
    if(output)*output=result;return NATIVE_SDK_RELOCATION_OK;
}
enum NativeSdkRelocationStatus native_sdk_relocate_table(uint64_t table,NativeSdkRelocationResult *result) {
    return native_sdk_relocate_table_ex(table,NULL,result);
}
#ifndef NATIVE_SDK_RELOCATION_STANDALONE
static void relocate_fast(Cpu *cpu) {
    const SessionLayout *layout=session_layout_current();NativeSdkRelocationBounds bounds={0};
    if(layout) {bounds.tracked_write_lo=layout->regions[SESSION_APP_HEAP].lo;bounds.tracked_write_hi=layout->regions[SESSION_APP_HEAP].hi;}
    NativeSdkRelocationResult result;
    enum NativeSdkRelocationStatus status=native_sdk_relocate_table_ex(cpu->x[0],&bounds,&result);
    if(status!=NATIVE_SDK_RELOCATION_OK) {fprintf(stderr,"[native-sdk] relocation preflight failed: %d\n",status);abort();}
    static _Atomic uint64_t calls;uint64_t call=atomic_fetch_add_explicit(&calls,1,memory_order_relaxed)+1;
    if(call<=8)fprintf(stderr,"[native-sdk] relocation call=%" PRIu64 " sections=%" PRIu64 " entries=%" PRIu64 " groups=%" PRIu64 " pointers=%" PRIu64 " fixed=%" PRIu64 " null=%" PRIu64 "\n",call,result.sections,result.entries,result.groups,result.pointers,result.fixed_pointers,result.null_pointers);
}
GuestFn native_sdk_fastpath(const char *symbol) {
    if(!symbol||strcmp(symbol,"_ZN2nn4util15RelocationTable8RelocateEv"))return NULL;
    const char *enabled=getenv("LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH");
    return enabled&&!strcmp(enabled,"1")?relocate_fast:NULL;
}
#endif
