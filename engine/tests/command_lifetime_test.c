/* Synthetic CURRENT closures, copied CALLs, and structural slot collection. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_lifetime.h"
#include "production_record_schema.h"
_Static_assert(sizeof(CmdRec)==80&&offsetof(CmdRec,x)==8,"production recorder ABI");
typedef struct {size_t live;} Memory;
static void *allocate(size_t n,void*ctx){void*p=malloc(n);if(p)++((Memory*)ctx)->live;return p;}
static void release(void*p,void*ctx){assert(((Memory*)ctx)->live);--((Memory*)ctx)->live;free(p);}
static NvnClStore *create(Memory*m,bool encoded){NvnClStore*s=NULL;NvnClConfig c={.max_slots=64,.max_bodies=64,.max_spans_per_body=8,.max_call_edges=256,.max_lease_slots=64,.max_body_bytes=65536,.record_budget=10000,.schema={CMD_COUNT,CMD_BARRIER,CMD_END,CMD_JUMP,CMD_CALL,CMD_FENCE_SYNC,{CMD_BIND_TEXTURES_DEFERRED,CMD_BIND_UNIFORM_BUFFERS_DEFERRED,CMD_BIND_STORAGE_BUFFERS_DEFERRED}},.arena={8*1024*1024,2,{allocate,release,m}},.deterministic_handles=encoded};assert(nvn_cl_create(c,&s)==NVN_CL_OK);return s;}
typedef struct {_Alignas(16) unsigned char bytes[512];size_t used;uint64_t cost;} Packet;
static CmdRec *emit(Packet*p,uint32_t op,uint32_t extra){uint32_t n=(sizeof(CmdRec)+extra+7)&~7u;assert(n<=sizeof p->bytes-p->used);CmdRec*r=(void*)(p->bytes+p->used);memset(r,0,n);r->op=op;r->size=n;if(p->used)p->cost+=op==CMD_END?4:((n+31)/32)*4;p->used+=n;return r;}
static Packet packet(uint64_t value,NvnClHandle child,bool flags){Packet p={0};emit(&p,CMD_BARRIER,0);CmdRec*r=emit(&p,child?CMD_CALL:CMD_BIND_TEXTURES,8);r->x[0]=child?1:0;memcpy((char*)r+sizeof*r,child?&child:&value,8);if(flags){emit(&p,CMD_BIND_TEXTURES_DEFERRED,0);emit(&p,CMD_FENCE_SYNC,0);}emit(&p,CMD_END,0);return p;}
static NvnClHandle publish(NvnClStore*s,uint64_t offset,uint64_t backing,Packet*p){NvnClSpan span={1,backing,backing+p->cost,1};NvnClInput in={11,offset,p->bytes,p->used,&span,1};NvnClHandle h=0;assert(nvn_cl_publish(s,&in,&h)==NVN_CL_OK);return h;}
static NvnClLease *acquire(NvnClStore*s,NvnClHandle h){NvnClLease*l=NULL;assert(nvn_cl_acquire(s,&h,1,&l)==NVN_CL_OK);return l;}
static uint64_t value(NvnClLease*l,NvnClHandle h){NvnClBodyView v;assert(nvn_cl_lease_body(l,h,&v));uint64_t result;memcpy(&result,(const char*)v.records+2*sizeof(CmdRec),8);return result;}
static void done(NvnClLease*l){assert(nvn_cl_lease_release_completed(l,true)==NVN_CL_OK);}
static void closures(bool encoded){
 Memory m={0};NvnClStore*s=create(&m,encoded);Packet p=packet(10,0,false);NvnClHandle child=publish(s,0,0,&p);p=packet(0,child,false);NvnClHandle parent=publish(s,128,128,&p);NvnClLease*old=acquire(s,parent);assert(value(old,child)==10);
 NvnClSnapshot*snap=NULL;assert(nvn_cl_snapshot(s,parent,&snap)==NVN_CL_OK);NvnClBodyView copied=nvn_cl_snapshot_view(snap);unsigned char snapshot_bytes[512];assert(copied.bytes<=sizeof snapshot_bytes);memcpy(snapshot_bytes,copied.records,copied.bytes);NvnClSpan span={1,256,272,1};NvnClInput in={11,256,copied.records,copied.bytes,&span,1};NvnClHandle copy;assert(nvn_cl_publish(s,&in,&copy)==NVN_CL_OK);
 p=packet(20,0,true);assert(publish(s,0,0,&p)==child);NvnClLease*next=acquire(s,copy);assert(value(next,child)==20&&value(old,child)==10);assert(nvn_cl_lease_flags(next).deferred&&nvn_cl_lease_flags(next).fences&&!nvn_cl_lease_flags(old).deferred);
 p=packet(77,0,false);publish(s,4,4,&p);NvnClLease*sentinel=(void*)1;assert(nvn_cl_acquire(s,&parent,1,&sentinel)==NVN_CL_UNKNOWN_HANDLE&&sentinel==(void*)1);assert(value(old,child)==10&&value(next,child)==20);
 p=packet(30,0,false);assert(publish(s,0,0,&p)==child);NvnClLease*current=acquire(s,copy);assert(value(current,child)==30&&!nvn_cl_lease_flags(current).deferred);
 p=packet(99,0,false);assert(publish(s,128,128,&p)==parent);assert(!memcmp(nvn_cl_snapshot_view(snap).records,snapshot_bytes,copied.bytes));NvnClBodyView copy_view;assert(nvn_cl_lease_body(current,copy,&copy_view)&&!memcmp(copy_view.records,snapshot_bytes,copied.bytes));assert(nvn_cl_destroy(s)==NVN_CL_BUSY);assert(nvn_cl_lease_release_completed(old,false)==NVN_CL_BUSY);assert(nvn_cl_lease_retain(old)==NVN_CL_OK);done(old);assert(value(old,child)==10);done(old);done(next);done(current);nvn_cl_snapshot_release(snap);assert(!nvn_cl_stats(s).pinned_bytes);assert(nvn_cl_destroy(s)==NVN_CL_OK&&!m.live);
}
static void collected_cached_handles(void){
 Memory m={0};NvnClStore*s=create(&m,true);Packet p=packet(1,0,false);NvnClHandle cached=publish(s,0,0,&p);p=packet(2,0,false);publish(s,4,4,&p);assert(!nvn_cl_slot_present(s,cached)&&nvn_cl_stats(s).collected_slots);
 NvnClLease*l=(void*)1;assert(nvn_cl_acquire(s,&cached,1,&l)==NVN_CL_UNKNOWN_HANDLE&&l==(void*)1);p=packet(3,0,false);assert(publish(s,0,0,&p)==cached);l=acquire(s,cached);assert(value(l,cached)==3);done(l);
 for(unsigned i=0;i<2048;i++){p=packet(i,0,false);publish(s,1024+4*i,0,&p);assert(nvn_cl_stats(s).slots<=2&&nvn_cl_stats(s).alive_bodies==1);}
 assert(nvn_cl_stats(s).collected_slots>=2048);assert(nvn_cl_destroy(s)==NVN_CL_OK&&!m.live);
}
int main(void){closures(false);closures(true);collected_cached_handles();puts("CURRENT closures, CALL/COPY snapshots, pinned retirement and cached handles after slot collection passed");}
