#include "command_lifetime.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE_TAG UINT64_C(0xc17f000000000000)
#define HANDLE_MASK UINT64_C(0xffff000000000000)
#define ENCODED_TAG UINT64_C(0xc200000000000000)
#define ENCODED_MASK UINT64_C(0xff00000000000000)
#define ENCODED_DOMAIN_MAX UINT64_C(0xffffff)
#define ENCODED_OFFSET_MAX (UINT64_C(0xffffffff)*4)
#define HANDLE_SEQUENCE UINT64_C(0x0000ffffffffffff)
typedef struct {uint32_t op,size;uint64_t x[7];uint32_t f[4];} Raw;
_Static_assert(sizeof(Raw)==80&&offsetof(Raw,x)==8,"native HLE record schema");
typedef struct Body Body;
typedef struct Slot Slot;
typedef struct SpanNode SpanNode;
struct SpanNode {NvnClSpan span;Body*owner;SpanNode*left,*right,*parent;int height;};
typedef struct {uint32_t record;Slot*slot;} Edge;
struct Slot {uint64_t domain,offset;NvnClHandle handle;Body*current;size_t edges,alive_bodies,transaction_pins;uint64_t transaction_marker;Slot*previous,*next,*pending_next;};
struct Body {
    NvnClBodyView view;CbaBlock block;SpanNode*spans;size_t nspans;
    Edge*edges;size_t nedges,readers;Slot*slot;bool current,pending;
    Body*retire_next;
};
typedef struct {Slot*slot;Body*body;NvnClLeaseFlags flags;} Choice;
struct NvnClLease {
    NvnClStore*store;Choice*choices;size_t count,capacity;size_t*map;size_t map_capacity;
    size_t references;NvnClLeaseFlags flags;
};
struct NvnClSnapshot {NvnClStore*store;Body*body;};
struct NvnClStore {
    pthread_mutex_t mutex;NvnClConfig config;CbaArena*arena;
    Slot**ids,**keys;size_t map_capacity;Slot*slots;SpanNode*spans;
    NvnClStats stats;uint64_t next_handle;bool destroying;
};
typedef struct {uint32_t records;size_t edges;uint64_t cost;bool calls,deferred,fences;} Parsed;
static void *take(NvnClStore*s,size_t n){void*p=s->config.arena.allocator.allocate(n,s->config.arena.allocator.context);if(p&&((uintptr_t)p&15)){s->config.arena.allocator.release(p,s->config.arena.allocator.context);return NULL;}return p;}
static void give(NvnClStore*s,void*p){if(p)s->config.arena.allocator.release(p,s->config.arena.allocator.context);}
static void *zero(NvnClStore*s,size_t n){void*p=take(s,n);if(p)memset(p,0,n);return p;}
static uint64_t mix(uint64_t x){x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);x^=x>>27;x*=UINT64_C(0x94d049bb133111eb);return x^(x>>31);}
static uint64_t key_hash(uint64_t domain,uint64_t offset){return mix(domain)^((mix(offset)<<27)|(mix(offset)>>37));}
static bool encoded_decode(NvnClHandle h,uint64_t *domain,uint64_t *offset){
 if((h&ENCODED_MASK)!=ENCODED_TAG||!((h>>32)&ENCODED_DOMAIN_MAX))return false;
 if(domain)*domain=(h>>32)&ENCODED_DOMAIN_MAX;if(offset)*offset=(h&UINT64_C(0xffffffff))*4;return true;
}
static NvnClHandle encoded_handle(uint64_t domain,uint64_t offset){return ENCODED_TAG|(domain<<32)|(offset>>2);}
static Slot *by_id(NvnClStore*s,NvnClHandle handle){
    if(!s->map_capacity)return NULL;
    if(s->config.deterministic_handles){if(!encoded_decode(handle,NULL,NULL))return NULL;}
    else if((handle&HANDLE_MASK)!=HANDLE_TAG||!(handle&HANDLE_SEQUENCE))return NULL;
    size_t at=(size_t)mix(handle)&(s->map_capacity-1);
    for(size_t i=0;i<s->map_capacity;++i,at=(at+1)&(s->map_capacity-1)){++s->stats.slot_probes;Slot*q=s->ids[at];if(!q||q->handle==handle)return q;}return NULL;
}
static Slot *by_key(NvnClStore*s,uint64_t domain,uint64_t offset){
    if(!s->map_capacity)return NULL;size_t at=(size_t)key_hash(domain,offset)&(s->map_capacity-1);
    for(size_t i=0;i<s->map_capacity;++i,at=(at+1)&(s->map_capacity-1)){++s->stats.slot_probes;Slot*q=s->keys[at];if(!q||(q->domain==domain&&q->offset==offset))return q;}return NULL;
}
static void insert_slot(Slot**ids,Slot**keys,size_t capacity,Slot*q){
    size_t at=(size_t)mix(q->handle)&(capacity-1);while(ids[at])at=(at+1)&(capacity-1);ids[at]=q;
    at=(size_t)key_hash(q->domain,q->offset)&(capacity-1);while(keys[at])at=(at+1)&(capacity-1);keys[at]=q;
}
/* Remove without tombstones, preserving probe chains through wraparound. */
static void remove_map(NvnClStore*s,Slot**map,Slot*q,bool key){
 size_t mask=s->map_capacity-1,at=(size_t)(key?key_hash(q->domain,q->offset):mix(q->handle))&mask;
 while(map[at]!=q){++s->stats.slot_delete_probes;if(!map[at])abort();at=(at+1)&mask;}
 size_t hole=at;
 for(size_t next=(hole+1)&mask;map[next];next=(next+1)&mask){
  ++s->stats.slot_delete_probes;Slot*item=map[next];size_t home=(size_t)(key?key_hash(item->domain,item->offset):mix(item->handle))&mask;
  if(((next-home)&mask)>=((hole-home)&mask)){map[hole]=item;hole=next;}
 }
 map[hole]=NULL;
}
static void collect_slot(NvnClStore*s,Slot*q){
 if(!s->config.deterministic_handles||s->destroying||!q||q->current||q->alive_bodies||q->edges||q->transaction_pins)return;
 remove_map(s,s->ids,q,false);remove_map(s,s->keys,q,true);
 if(q->previous)q->previous->next=q->next;else s->slots=q->next;
 if(q->next)q->next->previous=q->previous;
 --s->stats.slots;++s->stats.collected_slots;give(s,q);
}
static int height(SpanNode*n){return n?n->height:0;}
static void fix_height(SpanNode*n){int a=height(n->left),b=height(n->right);n->height=1+(a>b?a:b);}
static void replace_node(NvnClStore*s,SpanNode*old,SpanNode*n){
    if(!old->parent)s->spans=n;else if(old->parent->left==old)old->parent->left=n;else old->parent->right=n;
    if(n)n->parent=old->parent;
}
static SpanNode *rotate_left(NvnClStore*s,SpanNode*n){SpanNode*r=n->right;replace_node(s,n,r);n->right=r->left;if(n->right)n->right->parent=n;r->left=n;n->parent=r;fix_height(n);fix_height(r);return r;}
static SpanNode *rotate_right(NvnClStore*s,SpanNode*n){SpanNode*l=n->left;replace_node(s,n,l);n->left=l->right;if(n->left)n->left->parent=n;l->right=n;n->parent=l;fix_height(n);fix_height(l);return l;}
static void rebalance(NvnClStore*s,SpanNode*n){
    while(n){fix_height(n);int balance=height(n->left)-height(n->right);
        if(balance>1){if(height(n->left->left)<height(n->left->right))rotate_left(s,n->left);n=rotate_right(s,n);}
        else if(balance< -1){if(height(n->right->right)<height(n->right->left))rotate_right(s,n->right);n=rotate_left(s,n);}n=n->parent;}
}
static int span_key(uint64_t d,uint64_t b,const SpanNode*n){if(d!=n->span.domain)return d<n->span.domain?-1:1;if(b!=n->span.begin)return b<n->span.begin?-1:1;return 0;}
static void span_insert(NvnClStore*s,SpanNode*n){
    SpanNode*parent=NULL,**at=&s->spans;while(*at){parent=*at;++s->stats.span_probes;at=span_key(n->span.domain,n->span.begin,parent)<0?&parent->left:&parent->right;}
    n->left=n->right=NULL;n->parent=parent;n->height=1;*at=n;++s->stats.span_nodes;rebalance(s,parent);
}
static SpanNode *minimum(SpanNode*n){while(n&&n->left)n=n->left;return n;}
static SpanNode *maximum(SpanNode*n){while(n&&n->right)n=n->right;return n;}
static SpanNode *next_span(SpanNode*n){if(n->right)return minimum(n->right);while(n->parent&&n==n->parent->right)n=n->parent;return n->parent;}
static SpanNode *previous_span(SpanNode*n){if(n->left)return maximum(n->left);while(n->parent&&n==n->parent->left)n=n->parent;return n->parent;}
static void span_remove(NvnClStore*s,SpanNode*n){
    SpanNode*from;
    if(!n->left||!n->right){from=n->parent;replace_node(s,n,n->left?n->left:n->right);}
    else{SpanNode*y=minimum(n->right);if(y->parent!=n){from=y->parent;replace_node(s,y,y->right);y->right=n->right;y->right->parent=y;}else from=y;
        replace_node(s,n,y);y->left=n->left;y->left->parent=y;y->height=n->height;}
    n->left=n->right=n->parent=NULL;n->height=1;--s->stats.span_nodes;rebalance(s,from);
}
static SpanNode *first_overlap(NvnClStore*s,const NvnClSpan*span){
    SpanNode*n=s->spans,*lower=NULL;
    while(n){++s->stats.span_probes;if(span_key(span->domain,span->begin,n)<=0){lower=n;n=n->left;}else n=n->right;}
    SpanNode*before=lower?previous_span(lower):maximum(s->spans);
    if(before&&before->span.domain==span->domain&&before->span.end>span->begin)return before;return lower;
}
static void free_body(NvnClStore*s,Body*b){
    for(size_t i=0;i<b->nedges;++i){Slot*q=b->edges[i].slot;--q->edges;collect_slot(s,q);}
    Slot*owner=b->slot;if(!owner->alive_bodies)abort();--owner->alive_bodies;collect_slot(s,owner);
    --s->stats.alive_bodies;s->stats.alive_bytes-=b->view.bytes;
    if(cba_release(s->arena,&b->block)!=CBA_OK)abort();give(s,b->spans);give(s,b->edges);give(s,b);
}
static void retire_body(NvnClStore*s,Body*b){
    for(size_t i=0;i<b->nspans;++i)span_remove(s,&b->spans[i]);
    b->current=false;if(b->slot->current==b)b->slot->current=NULL;
    --s->stats.current_bodies;s->stats.current_bytes-=b->view.bytes;
    if(!b->readers)free_body(s,b);
}
static void pin_body(NvnClStore*s,Body*b){if(!b->readers)s->stats.pinned_bytes+=b->view.bytes;++b->readers;}
static void unpin_body(NvnClStore*s,Body*b){if(!--b->readers){s->stats.pinned_bytes-=b->view.bytes;if(!b->current)free_body(s,b);}}
static bool schema_valid(NvnClSchema x){
    if(!x.op_count||x.op_count>65536||x.begin>=x.op_count||x.end>=x.op_count||x.jump>=x.op_count||x.call>=x.op_count)return false;
    uint32_t required[]={x.begin,x.end,x.jump,x.call};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(required[i]==required[j])return false;
    uint32_t optional[]={x.fence,x.deferred[0],x.deferred[1],x.deferred[2]};
    for(unsigned i=0;i<4;++i)if(optional[i]!=UINT32_MAX){if(optional[i]>=x.op_count)return false;for(unsigned j=0;j<4;++j)if(optional[i]==required[j])return false;for(unsigned j=0;j<i;++j)if(optional[i]==optional[j])return false;}return true;
}
static NvnClResult parse(NvnClStore*s,const void*data,size_t bytes,Parsed*out){
    Parsed p={0};size_t offset=0;NvnClSchema schema=s->config.schema;
    while(offset<bytes){
        if(sizeof(Raw)>bytes-offset)return NVN_CL_INVALID;if(p.records==s->config.record_budget)return NVN_CL_LIMIT;
        Raw r;memcpy(&r,(const char*)data+offset,sizeof r);
        if(r.op>=schema.op_count||r.size<sizeof r||(r.size&7)||r.size>bytes-offset||r.op==schema.jump)return NVN_CL_INVALID;
        if(!p.records&&(r.op!=schema.begin||r.size!=sizeof r))return NVN_CL_INVALID;
        if(r.op==schema.end&&(r.size!=sizeof r||offset+r.size!=bytes))return NVN_CL_INVALID;
        uint64_t cost=!p.records?0:r.op==schema.end?4:(((uint64_t)r.size+31)/32)*4;
        if(cost>UINT64_MAX-p.cost)return NVN_CL_LIMIT;p.cost+=cost;
        if(r.op==schema.call){
            if(r.x[0]>(r.size-sizeof r)/8)return NVN_CL_INVALID;if(r.x[0]>s->config.max_call_edges-p.edges)return NVN_CL_LIMIT;
            p.edges+=(size_t)r.x[0];p.calls=true;
        }
        p.fences|=r.op==schema.fence;for(unsigned i=0;i<3;++i)p.deferred|=r.op==schema.deferred[i];
        ++p.records;offset+=r.size;
        if(r.op==schema.end){*out=p;return p.records>=2?NVN_CL_OK:NVN_CL_INVALID;}
    }
    return NVN_CL_INVALID;
}
static int compare_spans(const void*a,const void*b){const SpanNode*x=a,*y=b;int c=span_key(x->span.domain,x->span.begin,y);return c;}
static NvnClResult arena_result(CbaResult r){return r==CBA_NOMEM?NVN_CL_NOMEM:r==CBA_LIMIT?NVN_CL_LIMIT:r==CBA_OK?NVN_CL_OK:NVN_CL_INVALID;}
NvnClResult nvn_cl_create(NvnClConfig c,NvnClStore**out){
    if(!out||!c.arena.allocator.allocate||!c.arena.allocator.release||!schema_valid(c.schema)||
       !c.max_slots||c.max_slots>(1u<<24)||!c.max_bodies||c.max_bodies>(1u<<24)||
       !c.max_spans_per_body||c.max_spans_per_body>4096||c.max_call_edges>10000000||
       !c.max_lease_slots||c.max_lease_slots>c.max_slots||c.max_body_bytes<160||c.max_body_bytes>SIZE_MAX||
       !c.record_budget||c.record_budget>10000000)return NVN_CL_INVALID;
    NvnClStore*s=c.arena.allocator.allocate(sizeof*s,c.arena.allocator.context);if(!s)return NVN_CL_NOMEM;
    if((uintptr_t)s&15){c.arena.allocator.release(s,c.arena.allocator.context);return NVN_CL_INVALID;}
    memset(s,0,sizeof*s);s->config=c;
    if(pthread_mutex_init(&s->mutex,NULL)){give(s,s);return NVN_CL_NOMEM;}
    CbaResult result=cba_create(c.arena,&s->arena);
    if(result!=CBA_OK){pthread_mutex_destroy(&s->mutex);give(s,s);return arena_result(result);}
    *out=s;return NVN_CL_OK;
}
NvnClResult nvn_cl_destroy(NvnClStore*s){
    if(!s)return NVN_CL_INVALID;pthread_mutex_lock(&s->mutex);
    if(s->stats.leases||s->stats.snapshots){pthread_mutex_unlock(&s->mutex);return NVN_CL_BUSY;}
    s->destroying=true;
    for(Slot*q=s->slots;q;q=q->next)if(q->current)retire_body(s,q->current);
    if(s->stats.alive_bodies||s->spans||cba_destroy(s->arena)!=CBA_OK)abort();
    while(s->slots){Slot*q=s->slots;s->slots=q->next;give(s,q);}
    give(s,s->ids);give(s,s->keys);pthread_mutex_unlock(&s->mutex);pthread_mutex_destroy(&s->mutex);give(s,s);return NVN_CL_OK;
}
static void discard_body(NvnClStore*s,Body*b){
    if(!b)return;if(b->block.pointer&&cba_release(s->arena,&b->block)!=CBA_OK)abort();
    give(s,b->spans);give(s,b->edges);give(s,b);
}
typedef struct {Slot**map;size_t capacity,count;Slot*head;} Pending;
static Slot *pending_find(const Pending*p,NvnClHandle h){
 if(!p->capacity)return NULL;size_t at=(size_t)mix(h)&(p->capacity-1);
 while(p->map[at]){if(p->map[at]->handle==h)return p->map[at];at=(at+1)&(p->capacity-1);}return NULL;
}
static NvnClResult pending_add(NvnClStore*s,Pending*p,uint64_t domain,uint64_t offset,NvnClHandle h,Slot**out){
 Slot*q=pending_find(p,h);if(q){*out=q;return NVN_CL_OK;}
 if(p->count>=s->config.max_slots-s->stats.slots)return NVN_CL_LIMIT;
 q=zero(s,sizeof*q);if(!q)return NVN_CL_NOMEM;
 if(p->count+1>p->capacity/2){size_t capacity=p->capacity?p->capacity*2:16;
  Slot**map=zero(s,capacity*sizeof*map);if(!map){give(s,q);return NVN_CL_NOMEM;}
  for(Slot*n=p->head;n;n=n->pending_next){size_t at=(size_t)mix(n->handle)&(capacity-1);while(map[at])at=(at+1)&(capacity-1);map[at]=n;}
  give(s,p->map);p->map=map;p->capacity=capacity;
 }
 q->domain=domain;q->offset=offset;q->handle=h;q->pending_next=p->head;p->head=q;++p->count;
 size_t at=(size_t)mix(h)&(p->capacity-1);while(p->map[at])at=(at+1)&(p->capacity-1);p->map[at]=q;*out=q;return NVN_CL_OK;
}
static void pending_discard(NvnClStore*s,Pending*p){while(p->head){Slot*q=p->head;p->head=q->pending_next;give(s,q);}give(s,p->map);}
static NvnClResult prepare_maps(NvnClStore*s,size_t extra,Slot***ids,Slot***keys,size_t*capacity){
 *ids=*keys=NULL;*capacity=s->map_capacity;if(!extra||s->stats.slots+extra<=s->map_capacity/2)return NVN_CL_OK;
 size_t cap=s->map_capacity?s->map_capacity:16;while(s->stats.slots+extra>cap/2){if(cap>SIZE_MAX/2)return NVN_CL_LIMIT;cap*=2;}
 if(cap>SIZE_MAX/sizeof(Slot*))return NVN_CL_LIMIT;
 Slot**a=zero(s,cap*sizeof*a),**b=zero(s,cap*sizeof*b);if(!a||!b){give(s,a);give(s,b);return NVN_CL_NOMEM;}
 for(Slot*q=s->slots;q;q=q->next)insert_slot(a,b,cap,q);*ids=a;*keys=b;*capacity=cap;return NVN_CL_OK;
}
static void mark_victim(Body*b,Body**head){if(b&&!b->pending){b->pending=true;b->retire_next=*head;*head=b;}}
NvnClResult nvn_cl_publish(NvnClStore*s,const NvnClInput*in,NvnClHandle*out){
    if(!s||!in||!out||!in->logical_domain||(in->logical_offset&3)||!in->records||in->bytes<160||
       in->bytes>UINTPTR_MAX-(uintptr_t)in->records||!in->spans||!in->span_count)return NVN_CL_INVALID;
    if(s->config.deterministic_handles&&(in->logical_domain>ENCODED_DOMAIN_MAX||in->logical_offset>ENCODED_OFFSET_MAX))return NVN_CL_INVALID;
    if(in->bytes>s->config.max_body_bytes||in->span_count>s->config.max_spans_per_body)return NVN_CL_LIMIT;
    pthread_mutex_lock(&s->mutex);
    NvnClResult result=NVN_CL_OK;Body*b=NULL;Pending pending={0};Slot**ids=NULL,**keys=NULL;size_t map_capacity=0;
    if(s->stats.alive_bodies==s->config.max_bodies||s->stats.next_generation==UINT64_MAX){result=NVN_CL_LIMIT;goto done;}
    Slot*slot=by_key(s,in->logical_domain,in->logical_offset);
    if(!slot&&(s->stats.slots==s->config.max_slots||(!s->config.deterministic_handles&&s->next_handle==HANDLE_SEQUENCE))){result=NVN_CL_LIMIT;goto done;}
    b=zero(s,sizeof*b);if(!b){result=NVN_CL_NOMEM;goto done;}
    b->nspans=in->span_count;b->spans=zero(s,b->nspans*sizeof*b->spans);if(!b->spans){result=NVN_CL_NOMEM;goto done;}
    uint64_t span_bytes=0;
    for(size_t i=0;i<b->nspans;++i){NvnClSpan p=in->spans[i];
        if(!p.domain||!p.pool_generation||p.end<=p.begin||((p.begin|p.end)&3)||p.end-p.begin>UINT64_MAX-span_bytes){result=NVN_CL_INVALID;goto done;}
        span_bytes+=p.end-p.begin;b->spans[i].span=p;b->spans[i].owner=b;b->spans[i].height=1;
    }
    qsort(b->spans,b->nspans,sizeof*b->spans,compare_spans);
    for(size_t i=1;i<b->nspans;++i)if(b->spans[i-1].span.domain==b->spans[i].span.domain&&b->spans[i-1].span.end>b->spans[i].span.begin){result=NVN_CL_INVALID;goto done;}
    result=arena_result(cba_allocate(s->arena,in->bytes,&b->block));if(result!=NVN_CL_OK)goto done;
    memcpy(b->block.pointer,in->records,in->bytes);Parsed parsed;
    result=parse(s,b->block.pointer,in->bytes,&parsed);if(result!=NVN_CL_OK)goto done;
    if(parsed.cost!=span_bytes){result=NVN_CL_INVALID;goto done;}
    b->view=(NvnClBodyView){b->block.pointer,in->bytes,0,parsed.records,parsed.calls,parsed.deferred,parsed.fences};
    b->nedges=parsed.edges;
    if(b->nedges){b->edges=take(s,b->nedges*sizeof*b->edges);if(!b->edges){result=NVN_CL_NOMEM;goto done;}}
    if(!slot){NvnClHandle h=s->config.deterministic_handles?encoded_handle(in->logical_domain,in->logical_offset):HANDLE_TAG|(s->next_handle+1);
        result=pending_add(s,&pending,in->logical_domain,in->logical_offset,h,&slot);if(result!=NVN_CL_OK)goto done;}
    size_t offset=0,edge=0;uint32_t record=0;
    while(offset<in->bytes){Raw r;memcpy(&r,(char*)b->block.pointer+offset,sizeof r);
        if(r.op==s->config.schema.call)for(uint64_t i=0;i<r.x[0];++i){NvnClHandle h;memcpy(&h,(char*)b->block.pointer+offset+80+i*8,8);Slot*child=by_id(s,h);
            if(!child&&s->config.deterministic_handles){child=pending_find(&pending,h);if(!child){uint64_t domain,logical_offset;
                if(!encoded_decode(h,&domain,&logical_offset)){result=NVN_CL_UNKNOWN_HANDLE;goto done;}
                result=pending_add(s,&pending,domain,logical_offset,h,&child);if(result!=NVN_CL_OK)goto done;}}
            if(!child){result=NVN_CL_UNKNOWN_HANDLE;goto done;}b->edges[edge++]=(Edge){record,child};}
        offset+=r.size;++record;
    }
    result=prepare_maps(s,pending.count,&ids,&keys,&map_capacity);if(result!=NVN_CL_OK)goto done;
    // All fallible work completed. Victim collection uses existing intrusive
    // links; no published index/body is changed before this commit boundary.
    Body*victims=NULL;mark_victim(slot->current,&victims);
    for(size_t i=0;i<b->nspans;++i){NvnClSpan*p=&b->spans[i].span;SpanNode*n=first_overlap(s,p);
        while(n&&n->span.domain==p->domain&&n->span.begin<p->end){++s->stats.span_probes;if(n->span.end>p->begin)mark_victim(n->owner,&victims);n=next_span(n);}}
    if(pending.count){
        if(ids){give(s,s->ids);give(s,s->keys);s->ids=ids;s->keys=keys;s->map_capacity=map_capacity;ids=keys=NULL;}
        while(pending.head){Slot*q=pending.head;pending.head=q->pending_next;q->pending_next=NULL;insert_slot(s->ids,s->keys,s->map_capacity,q);
            q->next=s->slots;if(s->slots)s->slots->previous=q;s->slots=q;++s->stats.slots;}
        if(s->stats.slots>s->stats.peak_slots)s->stats.peak_slots=s->stats.slots;
        if(!s->config.deterministic_handles)++s->next_handle;
    }
    uint64_t transaction=s->stats.next_generation+1;
    slot->transaction_marker=transaction;++slot->transaction_pins;
    for(size_t i=0;i<b->nedges;++i){Slot*child=b->edges[i].slot;
        if(child->transaction_marker!=transaction){child->transaction_marker=transaction;++child->transaction_pins;}}
    b->slot=slot;b->view.generation=++s->stats.next_generation;
    for(size_t i=0;i<b->nedges;++i)++b->edges[i].slot->edges;
    while(victims){Body*old=victims;victims=old->retire_next;old->pending=false;old->retire_next=NULL;retire_body(s,old);}
    slot->current=b;b->current=true;++slot->alive_bodies;
    for(size_t i=0;i<b->nedges;++i){Slot*child=b->edges[i].slot;if(child->transaction_marker==transaction){child->transaction_marker=0;--child->transaction_pins;}}
    if(slot->transaction_marker==transaction){slot->transaction_marker=0;--slot->transaction_pins;}
    for(size_t i=0;i<b->nspans;++i)span_insert(s,&b->spans[i]);
    ++s->stats.alive_bodies;++s->stats.current_bodies;s->stats.alive_bytes+=b->view.bytes;s->stats.current_bytes+=b->view.bytes;
    *out=slot->handle;b=NULL;
done:
    give(s,ids);give(s,keys);pending_discard(s,&pending);discard_body(s,b);pthread_mutex_unlock(&s->mutex);return result;
}
static size_t choice_find(const NvnClLease*l,NvnClHandle handle,uint64_t*probes){
    if(!l->map_capacity)return SIZE_MAX;size_t at=(size_t)mix(handle)&(l->map_capacity-1);
    for(size_t i=0;i<l->map_capacity;++i,at=(at+1)&(l->map_capacity-1)){
        if(probes)++*probes;size_t item=l->map[at];if(!item)return SIZE_MAX;if(l->choices[item-1].slot->handle==handle)return item-1;
    }
    return SIZE_MAX;
}
static NvnClResult grow_choices(NvnClLease*l){
    NvnClStore*s=l->store;if(l->count<l->capacity)return NVN_CL_OK;
    if(l->capacity==s->config.max_lease_slots)return NVN_CL_LIMIT;
    size_t capacity=l->capacity?l->capacity*2:8;if(capacity>s->config.max_lease_slots)capacity=s->config.max_lease_slots;
    if(capacity<l->capacity||capacity>SIZE_MAX/sizeof(Choice))return NVN_CL_LIMIT;
    size_t map_capacity=16;while(map_capacity/2<capacity){if(map_capacity>SIZE_MAX/2)return NVN_CL_LIMIT;map_capacity*=2;}
    if(map_capacity>SIZE_MAX/sizeof(size_t))return NVN_CL_LIMIT;
    Choice*choices=take(s,capacity*sizeof*choices);size_t*map=zero(s,map_capacity*sizeof*map);
    if(!choices||!map){give(s,choices);give(s,map);return NVN_CL_NOMEM;}
    if(l->count)memcpy(choices,l->choices,l->count*sizeof*choices);
    for(size_t i=0;i<l->count;++i){size_t at=(size_t)mix(choices[i].slot->handle)&(map_capacity-1);while(map[at])at=(at+1)&(map_capacity-1);map[at]=i+1;}
    give(s,l->choices);give(s,l->map);l->choices=choices;l->map=map;l->capacity=capacity;l->map_capacity=map_capacity;return NVN_CL_OK;
}
static NvnClResult visit(NvnClLease*l,Slot*q,Slot**path,unsigned depth,uint32_t*budget,NvnClLeaseFlags*out){
    NvnClStore*s=l->store;if(depth>8||!*budget)return NVN_CL_LIMIT;--*budget;
    if(!q||!q->current)return NVN_CL_UNKNOWN_HANDLE;
    for(unsigned i=0;i<depth;++i)if(path[i]==q)return NVN_CL_INVALID;
    size_t at=choice_find(l,q->handle,&s->stats.lease_probes);
    if(at==SIZE_MAX){NvnClResult r=grow_choices(l);if(r!=NVN_CL_OK)return r;
        at=l->count++;l->choices[at]=(Choice){.slot=q,.body=q->current};size_t k=(size_t)mix(q->handle)&(l->map_capacity-1);
        while(l->map[k]){++s->stats.lease_probes;k=(k+1)&(l->map_capacity-1);}l->map[k]=at+1;
    }
    Body*b=l->choices[at].body;NvnClLeaseFlags flags={b->view.direct_deferred,b->view.direct_fences};path[depth]=q;size_t edge=0;
    // Even selected bodies must be traversed on repeated paths: a shallow visit
    // cannot hide a later depth9 path, a cycle or record-budget consumption.
    for(uint32_t record=0;record<b->view.record_count;++record){
        if(!*budget)return NVN_CL_LIMIT;--*budget;
        while(edge<b->nedges&&b->edges[edge].record==record){NvnClLeaseFlags child={0};NvnClResult r=visit(l,b->edges[edge++].slot,path,depth+1,budget,&child);
            if(r!=NVN_CL_OK)return r;flags.deferred|=child.deferred;flags.fences|=child.fences;}
    }
    l->choices[at].flags=flags;*out=flags;return NVN_CL_OK;
}
NvnClResult nvn_cl_acquire(NvnClStore*s,const NvnClHandle*handles,size_t count,NvnClLease**out){
    if(!s||!out||(count&&!handles)||count>s->config.max_lease_slots)return NVN_CL_INVALID;
    pthread_mutex_lock(&s->mutex);NvnClResult result=NVN_CL_OK;NvnClLease*l=zero(s,sizeof*l);
    if(!l){result=NVN_CL_NOMEM;goto done;}l->store=s;l->references=1;
    uint32_t budget=s->config.record_budget;Slot*path[9];
    for(size_t i=0;i<count;++i){NvnClLeaseFlags flags={0};result=visit(l,by_id(s,handles[i]),path,0,&budget,&flags);
        if(result!=NVN_CL_OK)goto done;l->flags.deferred|=flags.deferred;l->flags.fences|=flags.fences;}
    if(s->stats.leases==SIZE_MAX){result=NVN_CL_LIMIT;goto done;}
    for(size_t i=0;i<l->count;++i)if(l->choices[i].body->readers==SIZE_MAX){result=NVN_CL_LIMIT;goto done;}
    for(size_t i=0;i<l->count;++i)pin_body(s,l->choices[i].body);
    ++s->stats.leases;*out=l;l=NULL;
done:
    if(l){give(s,l->choices);give(s,l->map);give(s,l);}pthread_mutex_unlock(&s->mutex);return result;
}
NvnClResult nvn_cl_lease_retain(NvnClLease*l){
    if(!l)return NVN_CL_INVALID;NvnClStore*s=l->store;pthread_mutex_lock(&s->mutex);NvnClResult r=NVN_CL_OK;
    if(l->references==SIZE_MAX)r=NVN_CL_LIMIT;else ++l->references;pthread_mutex_unlock(&s->mutex);return r;
}
NvnClResult nvn_cl_lease_release_completed(NvnClLease*l,bool finished){
    if(!l)return NVN_CL_INVALID;if(!finished)return NVN_CL_BUSY;NvnClStore*s=l->store;pthread_mutex_lock(&s->mutex);
    if(!--l->references){for(size_t i=0;i<l->count;++i)unpin_body(s,l->choices[i].body);--s->stats.leases;give(s,l->choices);give(s,l->map);give(s,l);}
    pthread_mutex_unlock(&s->mutex);return NVN_CL_OK;
}
bool nvn_cl_lease_body(const NvnClLease*l,NvnClHandle handle,NvnClBodyView*out){
    if(!l||!out)return false;size_t at=choice_find(l,handle,NULL);if(at==SIZE_MAX)return false;*out=l->choices[at].body->view;return true;
}
bool nvn_cl_lease_span(const NvnClLease*l,NvnClHandle handle,size_t index,NvnClSpan*out){
    if(!l||!out)return false;size_t at=choice_find(l,handle,NULL);if(at==SIZE_MAX||index>=l->choices[at].body->nspans)return false;*out=l->choices[at].body->spans[index].span;return true;
}
NvnClLeaseFlags nvn_cl_lease_flags(const NvnClLease*l){return l?l->flags:(NvnClLeaseFlags){0};}
bool nvn_cl_lease_handle_flags(const NvnClLease*l,NvnClHandle handle,NvnClLeaseFlags*out){
    if(!l||!out)return false;size_t at=choice_find(l,handle,NULL);if(at==SIZE_MAX)return false;*out=l->choices[at].flags;return true;
}
NvnClResult nvn_cl_snapshot(NvnClStore*s,NvnClHandle handle,NvnClSnapshot**out){
    if(!s||!out)return NVN_CL_INVALID;pthread_mutex_lock(&s->mutex);NvnClResult result=NVN_CL_OK;Slot*q=by_id(s,handle);
    if(!q||!q->current){result=NVN_CL_UNKNOWN_HANDLE;goto done;}
    if(q->current->readers==SIZE_MAX||s->stats.snapshots==SIZE_MAX){result=NVN_CL_LIMIT;goto done;}
    NvnClSnapshot*snap=take(s,sizeof*snap);if(!snap){result=NVN_CL_NOMEM;goto done;}
    snap->store=s;snap->body=q->current;pin_body(s,snap->body);++s->stats.snapshots;*out=snap;
done:
    pthread_mutex_unlock(&s->mutex);return result;
}
NvnClBodyView nvn_cl_snapshot_view(const NvnClSnapshot*snapshot){return snapshot?snapshot->body->view:(NvnClBodyView){0};}
void nvn_cl_snapshot_release(NvnClSnapshot*snapshot){
    if(!snapshot)return;NvnClStore*s=snapshot->store;pthread_mutex_lock(&s->mutex);unpin_body(s,snapshot->body);--s->stats.snapshots;give(s,snapshot);pthread_mutex_unlock(&s->mutex);
}
bool nvn_cl_slot_present(NvnClStore*s,NvnClHandle handle){
    if(!s)return false;pthread_mutex_lock(&s->mutex);Slot*q=by_id(s,handle);bool result=q&&q->current;pthread_mutex_unlock(&s->mutex);return result;
}
NvnClStats nvn_cl_stats(NvnClStore*s){
    NvnClStats result={0};if(!s)return result;pthread_mutex_lock(&s->mutex);result=s->stats;result.arena=cba_stats(s->arena);pthread_mutex_unlock(&s->mutex);return result;
}
void nvn_cl_reset_probes(NvnClStore*s){if(!s)return;pthread_mutex_lock(&s->mutex);s->stats.slot_probes=s->stats.span_probes=s->stats.lease_probes=0;pthread_mutex_unlock(&s->mutex);}

NvnClStats nvn_cl_diagnostic_stats(NvnClStore*s){
 NvnClStats result={0};if(!s)return result;pthread_mutex_lock(&s->mutex);result=s->stats;result.arena=cba_stats(s->arena);
 for(const Slot*q=s->slots;q;q=q->next)if(!q->current){
  if(q->edges&&q->alive_bodies)++result.empty_both;
  else if(q->edges)++result.empty_incoming_only;
  else if(q->alive_bodies)++result.empty_old_body_only;
  else ++result.empty_unreferenced;
 }
 pthread_mutex_unlock(&s->mutex);return result;
}
