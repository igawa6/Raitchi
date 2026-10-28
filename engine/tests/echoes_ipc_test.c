/* Asset-free protocol checks: actual shared IPC codec with controlled kernel handles. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static void *allocated[1024];
static unsigned nallocated;
static void *test_calloc(size_t count,size_t size) {
    void *p=calloc(count,size);CHECK(p!=NULL);CHECK(nallocated<1024);allocated[nallocated++]=p;return p;
}
static char *test_strdup(const char *s) {
    size_t n=strlen(s)+1;char *p=test_calloc(n,1);memcpy(p,s,n);return p;
}
static KObject *handles[256];
static uint32_t next_handle=1;
KObject *handle_get(uint32_t handle) { return handle<256?handles[handle]:NULL; }
uint32_t kernel_new_handle(KObject *object) {CHECK(next_handle<256);handles[next_handle]=object;return next_handle++;}
#define calloc test_calloc
#define strdup test_strdup
#include "production_ipc.c"
#undef calloc
#undef strdup

static IpcReq seen;
static uint8_t seen_data[128];
static unsigned calls,closes;
static uint32_t response_size,response_result;
static bool return_handles;
static void closed(Service *s) {(void)s;closes++;}
static void probe_handler(Service *s,IpcReq *req,IpcResp *resp) {
    (void)s;calls++;seen=*req;CHECK(req->data_size<=sizeof seen_data);
    memcpy(seen_data,req->data,req->data_size);
    resp->result=response_result;resp_size(resp,response_size);
    for(uint32_t i=0;i<response_size;i++)resp->data[i]=(uint8_t)(0x80+i);
    if(return_handles) {
        resp->copy_handles[resp->ncopy++]=0x1234;
        resp->move_handles[resp->nmove++]=0x5678;
        ipc_return_object(resp,service_new("returned",probe_handler,NULL));
    }
}
SERVICE("probe", registered_probe) {probe_handler(s,req,resp);}

// Horizon TLS exposes a 0x100-byte IPC command buffer. Use the exact extent so
// ASan catches production accesses past the actual protocol buffer.
static _Alignas(16) uint32_t message[64];
static Cpu cpu;
static void reset(uint32_t type,uint32_t raw_words) {
    memset(message,0,sizeof message);message[0]=type;message[1]=raw_words;
    cpu.tpidrro=(uintptr_t)message;response_size=0;response_result=0;return_handles=false;
}
static uint32_t new_probe(void) {return session_new(service_new("probe",probe_handler,NULL));}
static void hipc_request(uint32_t type,uint32_t command,uint32_t value) {
    reset(type,9);memcpy(message+4,"SFCI",4);message[6]=command;message[8]=value;
}
static void test_hipc(void) {
    uint32_t h=new_probe();hipc_request(4,77,0x11223344);response_size=5;response_result=0x202;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.type==4);CHECK(seen.cmd==77);
    CHECK(seen.data_size==4);CHECK(!memcmp(seen_data,"\x44\x33\x22\x11",4));
    CHECK(message[0]==0);CHECK(message[1]==10);CHECK(!memcmp(message+4,"SFCO",4));CHECK(message[6]==0x202);
    CHECK(!memcmp(message+8,"\x80\x81\x82\x83\x84",5));CHECK(((uint8_t *)(message+8))[5]==0);
    hipc_request(6,9,42);return_handles=true;response_size=4;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.type==6);CHECK(seen.cmd==9);
    CHECK(message[1]==(9u|0x80000000u));CHECK(message[2]==(2u|(2u<<5)));
    CHECK(message[3]==0x1234);CHECK(message[4]==0x5678);CHECK(handle_get(message[5])->type==KOBJ_SESSION);
    CHECK(!memcmp(message+8,"SFCO",4));CHECK(message[12]==0x83828180);
    hipc_request(5,3,0);CHECK(ipc_handle_request(&cpu,h)==0);
    CHECK(message[1]==9);CHECK(!memcmp(message+4,"SFCO",4));CHECK(message[8]==0x8000);
    hipc_request(5,0,0);CHECK(ipc_handle_request(&cpu,h)==0);CHECK(message[8]==1);
    KSession *session=(KSession *)handle_get(h);CHECK(session->domain!=NULL);CHECK(session->domain->objs[1]==session->svc);
    reset(4,13);((uint8_t *)(message+4))[0]=1;message[5]=1;memcpy(message+8,"SFCI",4);message[10]=3;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.cmd==3);CHECK(seen.domain_obj==1);
    CHECK(message[4]==0);CHECK(!memcmp(message+8,"SFCO",4));
    uint32_t close_h=new_probe();Service *service=((KSession *)handle_get(close_h))->svc;service->on_close=closed;
    reset(2,0);unsigned before=calls;CHECK(ipc_handle_request(&cpu,close_h)==0);CHECK(calls==before);CHECK(closes==1);CHECK(service->refs==0);
}
static void test_tipc(void) {
    uint32_t h=new_probe();
    for(unsigned size=0;size<=0x40;size++) {
        reset(36,2);message[2]=0x11223344;message[3]=0x55667788;response_size=size;response_result=0x151;
        CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.type==36);CHECK(seen.cmd==20);CHECK(seen.data_size==8);
        CHECK(!memcmp(seen_data,"\x44\x33\x22\x11\x88\x77\x66\x55",8));
        CHECK(message[0]==36);CHECK(message[1]==1+(size+3)/4);CHECK(message[2]==0x151);
        for(unsigned i=0;i<size;i++)CHECK(((uint8_t *)(message+3))[i]==(uint8_t)(0x80+i));
        for(unsigned i=size;i<((size+3)/4)*4;i++)CHECK(((uint8_t *)(message+3))[i]==0);
        CHECK(memcmp(message+4,"SFCO",4));
    }
    reset(16,1);message[2]=0xfeed;return_handles=true;response_size=5;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.cmd==0);CHECK(seen.data_size==4);
    CHECK(message[0]==16);CHECK(message[1]==(3u|0x80000000u));CHECK(message[2]==(2u|(2u<<5)));
    CHECK(message[3]==0x1234);CHECK(message[4]==0x5678);CHECK(handle_get(message[5])->type==KOBJ_SESSION);
    CHECK(message[6]==0);CHECK(!memcmp(message+7,"\x80\x81\x82\x83\x84",5));

    // sm: GetServiceHandle uses command type17 and its eight-byte name without CMIF alignment/header.
    h=ipc_connect_named_port("sm:");CHECK(h!=0);reset(17,2);memcpy(message+2,"probe",5);
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(message[0]==17);CHECK(message[1]==(1u|0x80000000u));
    CHECK(message[2]==0x20);KSession *created=(KSession *)handle_get(message[3]);CHECK(created!=NULL);
    CHECK(created->obj.type==KOBJ_SESSION);CHECK(!strcmp(created->svc->name,"probe"));CHECK(message[4]==0);

    h=new_probe();Service *service=((KSession *)handle_get(h))->svc;service->on_close=closed;
    reset(15,0);uint32_t original[64];memcpy(original,message,sizeof message);unsigned before=calls;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before);CHECK(closes==2);CHECK(service->refs==0);
    CHECK(!memcmp(original,message,sizeof message));
}
static void encode_buffer(uint32_t *words,uint64_t addr,uint64_t size) {
    words[0]=(uint32_t)size;words[1]=(uint32_t)addr;
    words[2]=(uint32_t)(((addr>>32)&15)<<28)|((uint32_t)((addr>>36)&7)<<2)|((uint32_t)((size>>32)&15)<<24);
}
static void test_tipc_descriptors(void) {
    uint32_t h=new_probe();reset(44,2);message[0]|=(1u<<16)|(1u<<20)|(1u<<24);
    message[1]|=0x80000000u|(3u<<10);message[2]=1u|2u|0x20u;
    message[3]=0x89abcdef;message[4]=0x01234567;message[5]=0x44;message[6]=0x55;
    uint64_t xa=0x6123456789ull,aa=0x523456789aull,ba=0x43456789abull;
    message[7]=(0x4321u<<16)|((uint32_t)((xa>>32)&15)<<12)|((uint32_t)((xa>>36)&7)<<6);message[8]=(uint32_t)xa;
    encode_buffer(message+9,aa,0x2abcdef01ull);encode_buffer(message+12,ba,0x3bcdef012ull);
    message[15]=0xaabbccdd;message[16]=0x11223344;message[17]=0x76543210;message[18]=(0x1234u<<16)|0x4321u;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(seen.cmd==28);CHECK(seen.data_size==8);CHECK(seen.pid==0x0123456789abcdefull);
    CHECK(seen.ncopy==1&&seen.copy_handles[0]==0x44);CHECK(seen.nmove==1&&seen.move_handles[0]==0x55);
    CHECK(seen.nx==1&&seen.x[0].addr==xa&&seen.x[0].size==0x4321);
    CHECK(seen.na==1&&seen.a[0].addr==aa&&seen.a[0].size==0x2abcdef01ull);
    CHECK(seen.nb==1&&seen.b[0].addr==ba&&seen.b[0].size==0x3bcdef012ull);
    CHECK(seen.nc==1&&seen.c[0].addr==0x432176543210ull&&seen.c[0].size==0x1234);
    CHECK(!memcmp(seen_data,"\xdd\xcc\xbb\xaa\x44\x33\x22\x11",8));
    CHECK(message[0]==44);CHECK(message[1]==1);CHECK(message[2]==0);
}
static void test_command_buffer_bounds(void) {
    uint32_t h=new_probe();unsigned before=calls;
    // Exactly fills all 64 words: 2 header + 15 X (30 words) + 10 A (30 words) + 2 raw.
    // Excess wire counts remain consumed while reported counts stay within IpcReq's [8].
    reset(16,2);message[0]|=(15u<<16)|(10u<<20);
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before+1);CHECK(seen.nx==8);CHECK(seen.na==8);
    before=calls;
    // HIPC aligns CMIF to 16 bytes: this places SFCI+command at words 60..62, within the boundary.
    reset(4,0);message[0]|=(15u<<16)|(9u<<20);
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before+1);CHECK(seen.nx==8);CHECK(seen.na==8);
    before=calls;
    // One more three-word descriptor moves that aligned header past the buffer end.
    reset(4,0);message[0]|=(15u<<16)|(10u<<20);
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);
    // Each class of malformed overlong layout is rejected before dispatch.
    reset(16,2);message[0]|=(15u<<16)|(15u<<20)|(15u<<24);
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);
    reset(16,0);message[0]|=15u<<16;message[1]|=0x80000000u;message[2]=1u|(15u<<1)|(15u<<5);
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);
    reset(16,1023);
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);
    reset(16,50);message[1]|=(15u<<10);
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);
    // Domain's embedded size/object count must remain inside the declared raw request area.
    reset(5,0);CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before);
    reset(4,13);uint8_t *domain=(uint8_t *)(message+4);domain[1]=255;domain[2]=0xff;domain[3]=0xff;
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);

    // A valid domain CMIF payload is limited by dsize - its 16-byte SFCI header.
    reset(4,10);domain=(uint8_t *)(message+4);domain[0]=1;domain[2]=20;
    message[5]=1;memcpy(message+8,"SFCI",4);message[10]=0x77;message[12]=0x12345678;
    CHECK(ipc_handle_request(&cpu,h)==0xE401);CHECK(calls==before);  // raw end is 4 bytes short
    reset(4,11);domain=(uint8_t *)(message+4);domain[0]=1;domain[2]=20;
    message[5]=1;memcpy(message+8,"SFCI",4);message[10]=0x77;message[12]=0x12345678;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before+1);CHECK(seen.cmd==0x77);
    CHECK(seen.data_size==4);CHECK(seen_data[0]==0x78&&seen_data[3]==0x12);
    before=calls;

    // Nine wire object IDs fit in the message: consume all, but expose/store only the [8] request capacity.
    reset(4,19);domain=(uint8_t *)(message+4);domain[0]=1;domain[1]=9;domain[2]=16;
    message[5]=1;memcpy(message+8,"SFCI",4);message[10]=0x78;
    for(int i=0;i<9;i++)message[12+i]=0x100+i;
    CHECK(ipc_handle_request(&cpu,h)==0);CHECK(calls==before+1);CHECK(seen.ndomain_in==8);
    for(int i=0;i<8;i++)CHECK(seen.domain_in_objs[i]==0x100u+(unsigned)i);
    CHECK(seen.data_size==0);
}
int main(void) {
    reset(16,0);uint32_t copy[256];memcpy(copy,message,sizeof message);CHECK(ipc_handle_request(&cpu,255)==0xE401);CHECK(!memcmp(copy,message,sizeof message));
    KObject wrong={.type=KOBJ_EVENT};uint32_t h=kernel_new_handle(&wrong);CHECK(ipc_handle_request(&cpu,h)==0xE401);
    test_hipc();test_tipc();test_tipc_descriptors();test_command_buffer_bounds();
    for(unsigned i=0;i<nallocated;i++)free(allocated[i]);
    printf("%u IPC assertions passed (actual codec; controlled kernel handles)\n",checks);return 0;
}
