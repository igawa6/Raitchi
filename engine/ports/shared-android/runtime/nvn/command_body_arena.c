#include "command_body_arena.h"
#include <string.h>

#define SLAB_BYTES (UINT64_C(4)*1024*1024)
#define MIN_SHIFT 8u
#define MAX_ORDER 14u
#define NODE_COUNT ((1u<<(MAX_ORDER+1))-1)
#define DEDICATED_ORDER UINT32_MAX
enum { UNUSED,FREE,SPLIT,USED };
typedef struct Slab Slab;
typedef struct FreeNode { struct FreeNode *previous,*next; Slab *slab; } FreeNode;
typedef struct { uint64_t allocation,requested; } Prefix;
typedef struct { uint64_t id; Slab *slab; } Registry;
struct Slab {
    uint64_t id,capacity;
    bool dedicated;
    unsigned char states[NODE_COUNT];
    _Alignas(16) unsigned char data[];
};
struct CbaArena {
    CbaConfig config;
    Registry *registry;
    size_t registry_capacity;
    FreeNode *bins[MAX_ORDER+1];
    uint64_t next_slab,next_allocation;
    CbaStats stats;
};
_Static_assert(sizeof(Prefix)==16,"hidden identity prefix");
_Static_assert(offsetof(Slab,data)%16==0,"slab alignment");
static void *take(CbaArena*a,size_t bytes){return a->config.allocator.allocate(bytes,a->config.allocator.context);}
static void give(CbaArena*a,void*p){if(p)a->config.allocator.release(p,a->config.allocator.context);}
static size_t hash_id(uint64_t id,size_t mask){id^=id>>33;id*=UINT64_C(0xff51afd7ed558ccd);id^=id>>33;return (size_t)id&mask;}
static Registry *find(CbaArena*a,uint64_t id){
    if(!id)return NULL;size_t at=hash_id(id,a->registry_capacity-1);
    for(size_t i=0;i<a->registry_capacity;++i,at=(at+1)&(a->registry_capacity-1)){
        ++a->stats.lookup_probes;Registry*entry=&a->registry[at];if(!entry->id)return NULL;if(entry->id==id)return entry;
    }
    return NULL;
}
static void insert(CbaArena*a,Slab*s){
    size_t at=hash_id(s->id,a->registry_capacity-1);while(a->registry[at].id)at=(at+1)&(a->registry_capacity-1);
    a->registry[at]=(Registry){s->id,s};
}
static void erase(CbaArena*a,Registry*entry){
    size_t mask=a->registry_capacity-1,hole=(size_t)(entry-a->registry);
    for(size_t at=(hole+1)&mask;a->registry[at].id;at=(at+1)&mask){
        size_t home=hash_id(a->registry[at].id,mask);
        if(((at-home)&mask)>=((at-hole)&mask)){a->registry[hole]=a->registry[at];hole=at;}
    }
    a->registry[hole]=(Registry){0};
}
static uint64_t block_size(unsigned order){return UINT64_C(1)<<(MIN_SHIFT+order);}
static size_t node_index(uint64_t offset,unsigned order){return ((size_t)1<<(MAX_ORDER-order))-1+(size_t)(offset>>(MIN_SHIFT+order));}
static void push(CbaArena*a,Slab*s,uint64_t offset,unsigned order){
    FreeNode*n=(void*)(s->data+offset);n->slab=s;n->previous=NULL;n->next=a->bins[order];
    if(n->next)n->next->previous=n;a->bins[order]=n;
}
static void remove_free(CbaArena*a,FreeNode*n,unsigned order){
    if(n->previous)n->previous->next=n->next;else a->bins[order]=n->next;
    if(n->next)n->next->previous=n->previous;
}
CbaResult cba_create(CbaConfig config,CbaArena**out){
    if(!out||!config.allocator.allocate||!config.allocator.release||!config.max_slabs||!config.max_reserved_data||config.max_slabs>SIZE_MAX/2)return CBA_INVALID;
    size_t capacity=2;
    while(capacity<config.max_slabs*2){if(capacity>SIZE_MAX/2)return CBA_INVALID;capacity*=2;}
    if(capacity>SIZE_MAX/sizeof(Registry))return CBA_INVALID;
    CbaArena*a=config.allocator.allocate(sizeof*a,config.allocator.context);if(!a)return CBA_NOMEM;
    if((uintptr_t)a&15){config.allocator.release(a,config.allocator.context);return CBA_INVALID;}
    memset(a,0,sizeof*a);a->config=config;a->registry_capacity=capacity;
    a->registry=take(a,capacity*sizeof*a->registry);
    if(!a->registry){give(a,a);return CBA_NOMEM;}
    if((uintptr_t)a->registry&15){give(a,a->registry);give(a,a);return CBA_INVALID;}
    memset(a->registry,0,capacity*sizeof*a->registry);*out=a;return CBA_OK;
}
static CbaResult new_slab(CbaArena*a,uint64_t capacity,bool dedicated,Slab**out){
    if(a->stats.slabs==a->config.max_slabs||a->next_slab==UINT64_MAX||a->stats.reserved_data>a->config.max_reserved_data||capacity>a->config.max_reserved_data-a->stats.reserved_data)return CBA_LIMIT;
    if(capacity>SIZE_MAX-offsetof(Slab,data))return CBA_INVALID;
    size_t bytes=offsetof(Slab,data)+(size_t)capacity;
    if(bytes>UINT64_MAX-a->stats.reserved_total)return CBA_LIMIT;
    Slab*s=take(a,bytes);if(!s)return CBA_NOMEM;
    if((uintptr_t)s&15){give(a,s);return CBA_INVALID;}
    memset(s,0,offsetof(Slab,data));s->id=++a->next_slab;s->capacity=capacity;s->dedicated=dedicated;
    s->states[0]=FREE;insert(a,s);++a->stats.slabs;a->stats.reserved_data+=capacity;a->stats.reserved_total+=bytes;*out=s;return CBA_OK;
}
CbaResult cba_allocate(CbaArena*a,uint64_t bytes,CbaBlock*out){
    if(!a||!out||!bytes||bytes>SIZE_MAX-sizeof(Prefix)||bytes>UINT64_MAX-a->stats.live_requested)return CBA_INVALID;
    if(a->next_allocation==UINT64_MAX)return CBA_LIMIT;
    uint64_t needed=bytes+sizeof(Prefix),offset=0,rounded;Slab*s=NULL;unsigned order=0;
    if(needed>SLAB_BYTES){
        if(needed>SIZE_MAX-15)return CBA_INVALID;rounded=(needed+15)&~UINT64_C(15);
        CbaResult r=new_slab(a,rounded,true,&s);if(r!=CBA_OK)return r;order=DEDICATED_ORDER;s->states[0]=USED;
    }else{
        while(block_size(order)<needed)++order;unsigned found=order;
        while(found<=MAX_ORDER&&!a->bins[found])++found;
        if(found>MAX_ORDER){CbaResult r=new_slab(a,SLAB_BYTES,false,&s);if(r!=CBA_OK)return r;push(a,s,0,MAX_ORDER);found=MAX_ORDER;}
        FreeNode*n=a->bins[found];s=n->slab;offset=(uint64_t)((unsigned char*)n-s->data);remove_free(a,n,found);
        size_t index=node_index(offset,found);
        while(found>order){s->states[index]=SPLIT;--found;size_t left=2*index+1,right=left+1;s->states[left]=s->states[right]=FREE;push(a,s,offset+block_size(found),found);index=left;}
        s->states[index]=USED;rounded=block_size(order);
    }
    Prefix*prefix=(void*)(s->data+offset);uint64_t allocation=++a->next_allocation;*prefix=(Prefix){allocation,bytes};
    a->stats.live_requested+=bytes;a->stats.live_rounded+=rounded;++a->stats.live_blocks;
    if(a->stats.live_requested>a->stats.peak_requested)a->stats.peak_requested=a->stats.live_requested;
    *out=(CbaBlock){s->data+offset+sizeof(Prefix),s->id,allocation,bytes,offset,order};return CBA_OK;
}
CbaResult cba_release(CbaArena*a,const CbaBlock*b){
    if(!a||!b||!b->pointer||!b->allocation_id||!b->requested)return CBA_INVALID;
    Registry*entry=find(a,b->slab_id);if(!entry)return CBA_INVALID;Slab*s=entry->slab;uint64_t rounded;size_t index;
    if(s->dedicated){if(b->order!=DEDICATED_ORDER||b->offset)return CBA_INVALID;rounded=s->capacity;index=0;}
    else{
        if(b->order>MAX_ORDER)return CBA_INVALID;rounded=block_size(b->order);
        if((b->offset&(rounded-1))||b->offset>SLAB_BYTES-rounded)return CBA_INVALID;index=node_index(b->offset,b->order);
    }
    if(s->states[index]!=USED||b->requested>rounded-sizeof(Prefix)||b->pointer!=s->data+b->offset+sizeof(Prefix))return CBA_INVALID;
    Prefix*prefix=(void*)(s->data+b->offset);
    if(prefix->allocation!=b->allocation_id||prefix->requested!=b->requested)return CBA_INVALID;
    a->stats.live_requested-=b->requested;a->stats.live_rounded-=rounded;--a->stats.live_blocks;
    s->states[index]=FREE;uint64_t offset=b->offset;unsigned order=b->order;
    if(!s->dedicated){
        while(index){size_t sibling=(index&1)?index+1:index-1;if(s->states[sibling]!=FREE)break;
            uint64_t other=offset^block_size(order);remove_free(a,(FreeNode*)(void*)(s->data+other),order);
            s->states[index]=s->states[sibling]=UNUSED;index=(index-1)/2;s->states[index]=FREE;
            if(other<offset)offset=other;++order;
        }
    }
    if(s->dedicated||!index){
        uint64_t capacity=s->capacity;erase(a,entry);--a->stats.slabs;a->stats.reserved_data-=capacity;a->stats.reserved_total-=capacity+offsetof(Slab,data);give(a,s);
    }else push(a,s,offset,order);
    return CBA_OK;
}
CbaResult cba_destroy(CbaArena*a){
    if(!a)return CBA_INVALID;if(a->stats.live_blocks||a->stats.slabs)return CBA_BUSY;give(a,a->registry);give(a,a);return CBA_OK;
}
CbaStats cba_stats(const CbaArena*a){return a?a->stats:(CbaStats){0};}
