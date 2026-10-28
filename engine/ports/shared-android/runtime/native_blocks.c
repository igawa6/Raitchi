#define _GNU_SOURCE
#include "native_blocks.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#ifndef NATIVE_BLOCKS_GLOBAL_PAGE_LIMIT
#define NATIVE_BLOCKS_GLOBAL_PAGE_LIMIT 32768u
#endif
#define OVERFLOW_SLOTS 16u
#define OVERFLOW_CPUS_PER_THREAD 4u
/* Production keeps its immutable global cache. Only standalone test builds
 * override the compile-time page limit; no environment knob changes admission. */
typedef struct NativeBlockFrame {
    Cpu *cpu;
    const void *entry;
    uintptr_t host_sp, scratch16, scratch17;
} NativeBlockFrame;
_Static_assert(offsetof(NativeBlockFrame,cpu)==0 && offsetof(NativeBlockFrame,entry)==8 &&
    offsetof(NativeBlockFrame,host_sp)==16 && offsetof(NativeBlockFrame,scratch16)==24 &&
    offsetof(NativeBlockFrame,scratch17)==32,"native bridge frame");
_Static_assert(offsetof(Cpu,x)==0 && offsetof(Cpu,sp)==256 && offsetof(Cpu,nzcv)==272 &&
    offsetof(Cpu,fpcr)==320 && offsetof(Cpu,fpsr)==328 && offsetof(Cpu,v)==512,"native CPU3 offsets");
typedef struct NativeCode {
    struct NativeCode *next;
    uint64_t hash;
    size_t count,body_offset;
    uint32_t gpr_usage;
    unsigned context_register;
    void *entry;
} NativeCode;
static _Atomic(NativeCode *) table[4096];
static pthread_mutex_t code_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t pages;
static _Atomic int global_full;
typedef struct OverflowSlot {NativeCode code;int valid;} OverflowSlot;
typedef struct OverflowCache {
    Cpu *cpu;
    struct OverflowCache *next;
    size_t page_size;
    unsigned next_slot,busy;
    OverflowSlot slots[OVERFLOW_SLOTS];
} OverflowCache;
static pthread_key_t overflow_key;
static pthread_once_t overflow_once=PTHREAD_ONCE_INIT;
static int overflow_key_valid;
static _Atomic uint64_t overflow_live,overflow_peak,overflow_maps,overflow_rewrites;
static void overflow_destroy(void *opaque) {
    OverflowCache *cache=opaque;
    while(cache) {
        OverflowCache *next=cache->next;
        /* An admitted block has no host callouts. A genuinely abandoned active
         * invocation is never recycled/unmapped; process teardown owns it. */
        if(!cache->busy) {
            for(unsigned i=0;i<OVERFLOW_SLOTS;i++)if(cache->slots[i].code.entry) {
                if(!munmap(cache->slots[i].code.entry,cache->page_size))atomic_fetch_sub_explicit(&overflow_live,1,memory_order_relaxed);
            }
            free(cache);
        }
        cache=next;
    }
}
static void overflow_initialize(void) {overflow_key_valid=pthread_key_create(&overflow_key,overflow_destroy)==0;}
int native_blocks_thread_cleanup(void) {
    if(pthread_once(&overflow_once,overflow_initialize)||!overflow_key_valid)return 1;
    OverflowCache *head=pthread_getspecific(overflow_key);
    for(OverflowCache *cache=head;cache;cache=cache->next)if(cache->busy)return 1;
    if(pthread_setspecific(overflow_key,NULL))return 1;
    overflow_destroy(head);return 0;
}
void native_blocks_cache_stats(NativeBlocksCacheStats *out) {
    if(!out)return;
    pthread_mutex_lock(&code_lock);out->global_pages=pages;pthread_mutex_unlock(&code_lock);
    out->global_page_limit=NATIVE_BLOCKS_GLOBAL_PAGE_LIMIT;out->overflow_slots_per_cpu=OVERFLOW_SLOTS;
    out->overflow_cpu_records_per_thread=OVERFLOW_CPUS_PER_THREAD;
    out->overflow_live_pages=atomic_load_explicit(&overflow_live,memory_order_relaxed);
    out->overflow_peak_pages=atomic_load_explicit(&overflow_peak,memory_order_relaxed);
    out->overflow_mappings=atomic_load_explicit(&overflow_maps,memory_order_relaxed);
    out->overflow_rewrites=atomic_load_explicit(&overflow_rewrites,memory_order_relaxed);
}
static OverflowCache *overflow_for(Cpu *cpu,size_t page) {
    if(pthread_once(&overflow_once,overflow_initialize)||!overflow_key_valid)return NULL;
    OverflowCache *head=pthread_getspecific(overflow_key);unsigned count=0;
    for(OverflowCache *cache=head;cache;cache=cache->next) {if(cache->cpu==cpu)return cache;count++;}
    if(count>=OVERFLOW_CPUS_PER_THREAD)return NULL;
    OverflowCache *created=calloc(1,sizeof *created);if(!created)return NULL;
    created->cpu=cpu;created->page_size=page;created->next=head;
    if(pthread_setspecific(overflow_key,created)){free(created);return NULL;}return created;
}
static uint32_t load64(unsigned reg,unsigned base,size_t offset) {return 0xf9400000u|((uint32_t)(offset/8)<<10)|(base<<5)|reg;}
static uint32_t store64(unsigned reg,unsigned base,size_t offset) {return 0xf9000000u|((uint32_t)(offset/8)<<10)|(base<<5)|reg;}
static uint32_t move64(unsigned to,unsigned from) {return 0xaa0003e0u|(from<<16)|to;}
static int matches(const NativeCode *code,uint64_t hash,const uint32_t *words,size_t count,uint32_t usage,unsigned context) {
    return code->hash==hash&&code->count==count&&code->gpr_usage==usage&&code->context_register==context&&
        !memcmp((const char*)code->entry+code->body_offset,words,count*4);
}
static NativeCode *find(uint64_t hash,const uint32_t *words,size_t count,uint32_t usage,unsigned context) {
    NativeCode *entry=atomic_load_explicit(&table[hash&4095],memory_order_acquire);
    for(;entry;entry=entry->next)if(matches(entry,hash,words,count,usage,context))return entry;
    return NULL;
}
int native_blocks_context_register(uint32_t usage) {
    if(usage&((UINT32_C(1)<<18)|(UINT32_C(1)<<31)))return -1;
    if(!(usage&(UINT32_C(1)<<19)))return 19;
    uint32_t candidates=NGB_CONTEXT_GPR_MASK&~usage;
    for(unsigned reg=0;reg<31;reg++)if(candidates&(UINT32_C(1)<<reg))return (int)reg;
    return -1;
}
/* Mapping must already be RW, and can only be reached by this quiescent owner.
 * Publish valid code only after the single RW -> RX transition succeeds. */
static int emit(void *mapping,size_t page,const uint32_t *words,size_t count,unsigned context,size_t *body_offset) {
    size_t prefix_size=context==19?12:24,tail_size=context==19?0:24,exit_size=(size_t)(native_blocks_exit_end-native_blocks_exit_begin);
    if(prefix_size+count*4+tail_size+exit_size>page)return 1;
    unsigned char *memory=mapping;
    uint32_t prefix[]={load64(17,19,0),load64(16,17,16*8),load64(17,17,17*8),move64(context,19),load64(19,context,offsetof(NativeBlockFrame,cpu)),load64(19,19,offsetof(Cpu,x)+19*8)};
    memcpy(memory,prefix,prefix_size);memcpy(memory+prefix_size,words,count*4);
    unsigned char *tail=memory+prefix_size+count*4;
    if(context!=19) {
        uint32_t restore[]={store64(16,context,offsetof(NativeBlockFrame,scratch16)),load64(16,context,offsetof(NativeBlockFrame,cpu)),store64(19,16,offsetof(Cpu,x)+19*8),move64(19,context),load64(context,16,offsetof(Cpu,x)+context*8),load64(16,19,offsetof(NativeBlockFrame,scratch16))};
        memcpy(tail,restore,sizeof restore);
    }
    memcpy(tail+tail_size,native_blocks_exit_begin,exit_size);
    __builtin___clear_cache((char*)memory,(char*)tail+tail_size+exit_size);
    if(mprotect(memory,page,PROT_READ|PROT_EXEC))return 1;
    *body_offset=prefix_size;return 0;
}
static void describe(NativeCode *code,uint64_t hash,size_t count,size_t offset,uint32_t usage,unsigned context,void *mapping) {
    code->hash=hash;code->count=count;code->body_offset=offset;code->gpr_usage=usage;code->context_register=context;code->entry=mapping;
}
static NativeCode *overflow_code(OverflowCache *cache,uint64_t hash,const uint32_t *words,size_t count,uint32_t usage,unsigned context) {
    if(cache->busy)return NULL;
    for(unsigned i=0;i<OVERFLOW_SLOTS;i++)if(cache->slots[i].valid&&matches(&cache->slots[i].code,hash,words,count,usage,context))return &cache->slots[i].code;
    OverflowSlot *slot=&cache->slots[cache->next_slot++%OVERFLOW_SLOTS];void *mapping=slot->code.entry;int fresh=!mapping;
    if(fresh) {
        mapping=mmap(NULL,cache->page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(mapping==MAP_FAILED)return NULL;
    } else {
        if(mprotect(mapping,cache->page_size,PROT_READ|PROT_WRITE))return NULL;
        slot->valid=0;
    }
    if(fresh) {
        /* Retain even an RX-seal failure as an invalid private RW slot. This
         * keeps allocations bounded and allows safe reuse/exit cleanup. */
        slot->code.entry=mapping;
        atomic_fetch_add_explicit(&overflow_maps,1,memory_order_relaxed);
        uint64_t live=atomic_fetch_add_explicit(&overflow_live,1,memory_order_relaxed)+1,peak=atomic_load_explicit(&overflow_peak,memory_order_relaxed);
        while(live>peak&&!atomic_compare_exchange_weak_explicit(&overflow_peak,&peak,live,memory_order_relaxed,memory_order_relaxed)){}
    }
    size_t offset;
    if(emit(mapping,cache->page_size,words,count,context,&offset))return NULL;
    describe(&slot->code,hash,count,offset,usage,context,mapping);slot->valid=1;
    if(!fresh)atomic_fetch_add_explicit(&overflow_rewrites,1,memory_order_relaxed);
    return &slot->code;
}
int native_blocks_run(Cpu *cpu,const uint32_t *words,size_t count,uint32_t usage) {
    if(!cpu||!words||!count||count>64||!cpu->sp||(cpu->sp&15))return 1;
    int selected=native_blocks_context_register(usage);if(selected<0)return 1;unsigned context=(unsigned)selected;
    uint64_t hash=UINT64_C(1469598103934665603)^count^((uint64_t)usage<<32)^context;
    for(size_t i=0;i<count;i++)hash=(hash^words[i])*UINT64_C(1099511628211);
    NativeCode *entry=find(hash,words,count,usage,context);OverflowCache *overflow=NULL;
    if(!entry) {
        long host_page=sysconf(_SC_PAGESIZE);if(host_page<4096||((unsigned long)host_page&((unsigned long)host_page-1)))return 1;
        size_t page=(size_t)host_page,limit=NATIVE_BLOCKS_GLOBAL_PAGE_LIMIT;
        if(!atomic_load_explicit(&global_full,memory_order_relaxed)) {
        pthread_mutex_lock(&code_lock);entry=find(hash,words,count,usage,context);
        if(!entry&&pages<limit) {
            void *mapping=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);NativeCode *created=calloc(1,sizeof *created);size_t offset;
            if(mapping!=MAP_FAILED&&created&&!emit(mapping,page,words,count,context,&offset)) {
                describe(created,hash,count,offset,usage,context,mapping);created->next=atomic_load_explicit(&table[hash&4095],memory_order_relaxed);atomic_store_explicit(&table[hash&4095],created,memory_order_release);pages++;entry=created;
            } else {if(mapping!=MAP_FAILED)munmap(mapping,page);free(created);}
        }
        if(pages>=limit)atomic_store_explicit(&global_full,1,memory_order_relaxed);
        pthread_mutex_unlock(&code_lock);
        }
        if(!entry) {overflow=overflow_for(cpu,page);if(!overflow||(entry=overflow_code(overflow,hash,words,count,usage,context))==NULL)return 1;}
    }
    if(overflow)overflow->busy=1;
    NativeBlockFrame frame={.cpu=cpu,.entry=entry->entry};native_blocks_enter(&frame);
    if(overflow)overflow->busy=0;
    return 0;
}
