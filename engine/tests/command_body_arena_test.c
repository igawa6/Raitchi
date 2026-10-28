/* Synthetic ownership tests against the production allocator API. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_body_arena.h"
#define SLAB_BYTES (UINT64_C(4) * 1024 * 1024)
typedef struct {size_t live,calls,fail;} Memory;
static void *allocate(size_t n,void *context){Memory*m=context;++m->calls;if(m->fail&&!--m->fail)return NULL;void*p=malloc(n);if(p)++m->live;return p;}
static void release(void*p,void*context){Memory*m=context;assert(m->live);--m->live;free(p);}
int main(void){
 Memory m={0};CbaArena*a=NULL;CbaConfig c={3*SLAB_BYTES,3,{allocate,release,&m}};assert(cba_create(c,&a)==CBA_OK);
 CbaBlock pin={0},blocks[256];assert(cba_allocate(a,128,&pin)==CBA_OK);memset(pin.pointer,0x39,128);
 for(unsigned i=0;i<256;i++){assert(cba_allocate(a,100,&blocks[i])==CBA_OK);assert(!((uintptr_t)blocks[i].pointer&15));memset(blocks[i].pointer,i,100);}
 assert(cba_stats(a).slabs==1&&cba_destroy(a)==CBA_BUSY);
 CbaBlock stale=blocks[37];assert(cba_release(a,&blocks[37])==CBA_OK);assert(cba_release(a,&stale)==CBA_INVALID);
 assert(cba_allocate(a,100,&blocks[37])==CBA_OK);assert(blocks[37].allocation_id!=stale.allocation_id);memset(blocks[37].pointer,37,100);assert(cba_release(a,&stale)==CBA_INVALID);
 for(unsigned i=0;i<256;i++){unsigned char*p=blocks[i].pointer;assert(p[0]==(unsigned char)i&&p[99]==(unsigned char)i);assert(cba_release(a,&blocks[i])==CBA_OK);}
 size_t calls=m.calls;for(unsigned i=0;i<2048;i++){CbaBlock b;assert(cba_allocate(a,1+i%2048,&b)==CBA_OK);memset(b.pointer,0x27,(size_t)b.requested);assert(cba_release(a,&b)==CBA_OK);assert(((unsigned char*)pin.pointer)[127]==0x39);}
 assert(m.calls==calls&&cba_stats(a).live_blocks==1);
 CbaBlock big;assert(cba_allocate(a,SLAB_BYTES+17,&big)==CBA_OK);memset(big.pointer,0x81,(size_t)big.requested);assert(cba_stats(a).slabs==2);assert(cba_release(a,&big)==CBA_OK);
 CbaStats before=cba_stats(a);m.fail=1;CbaBlock unchanged={.pointer=(void*)1},out=unchanged;assert(cba_allocate(a,SLAB_BYTES+17,&out)==CBA_NOMEM);assert(!memcmp(&out,&unchanged,sizeof out));assert(cba_stats(a).live_requested==before.live_requested);m.fail=0;
 assert(cba_allocate(a,UINT64_MAX,&out)==CBA_INVALID);assert(cba_release(a,&pin)==CBA_OK);assert(!cba_stats(a).live_blocks&&!cba_stats(a).reserved_data);assert(cba_destroy(a)==CBA_OK);assert(!m.live);
 puts("allocator packing, stale tokens, preserved bytes, bounded reuse and rollback passed");
}
