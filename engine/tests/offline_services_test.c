/* Actual registered service chains and applet storage, synthetic IPC only. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "hle.h"
#include "ipc.h"
#include "online_guard.h"
#include "title_adapter.h"
static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;
static KEvent events[256];static unsigned nevents;
static KObject *handles[2048];static unsigned handles_closed;
KObject *handle_get(uint32_t handle){return handle<2048?handles[handle]:NULL;}
uint32_t kernel_new_handle(KObject *object){for(unsigned i=100;i<2048;i++)if(!handles[i]){handles[i]=object;object->refs++;return i;}abort();}
static void handle_close(uint32_t handle){KObject *object=handle_get(handle);if(object){handles[handle]=NULL;object->refs--;handles_closed++;}}
#include "production_kernel_close.inc"
KEvent *kernel_create_event_pair(uint32_t *w,uint32_t *r){CHECK(nevents<256);KEvent *event=&events[nevents++];event->type=KOBJ_EVENT;*w=kernel_new_handle(event);*r=kernel_new_handle(event);return event;}
void kernel_signal(KObject *event){CHECK(pthread_mutex_trylock(&g_klock)!=0);event->signaled=true;}
const char *settings_language(void){return "en-US";}
const TitleAdapter *title_adapter_selected(void){static TitleAdapter a={"1.0.0"};return &a;}
#include "production_ipc.c"
extern const uint8_t g_user_id[16];
static IpcResp call(Service *s,uint32_t cmd,const void *data,uint32_t bytes){CHECK(s&&s->handler);IpcReq req={.cmd=cmd,.data=data,.data_size=bytes};IpcResp resp={0};s->handler(s,&req,&resp);return resp;}
static uint64_t u64(const IpcResp *r){uint64_t v;CHECK(r->data_size>=8);memcpy(&v,r->data,8);return v;}
static uint32_t u32(const IpcResp *r){uint32_t v;CHECK(r->data_size>=4);memcpy(&v,r->data,4);return v;}
static Service *object(IpcResp r){CHECK(!r.result&&r.nobjects==1&&r.objects[0]);return r.objects[0];}
static void release(Service *s){if(s->on_close)s->on_close(s);if(s->state&&!strcmp(s->name,"IStorage")){struct {void *bytes;uint64_t size;}*st=s->state;free(st->bytes);free(st);}free((void*)s->name);free(s);}
static void failed(IpcResp r,uint32_t result){CHECK(r.result==result&&!r.nobjects&&!r.ncopy&&!r.nmove&&!r.data_size);}
static void close_copies(IpcResp r){for(int i=0;i<r.ncopy;i++){KObject *object=handle_get(r.copy_handles[i]);CHECK(object);kernel_close_service_handle(r.copy_handles[i],object);}}
static unsigned open_handles(void){unsigned n=0;for(unsigned i=0;i<2048;i++)n+=handles[i]!=NULL;return n;}
static void network_tests(void){
    const char *names[]={"nifm:u","nifm:s","nifm:a"};
    for(unsigned i=0;i<3;i++){
        Service *root=service_lookup(names[i]);Service *general=object(call(root,i==0?4:5,NULL,0));
        IpcResp r=call(general,1,NULL,0);CHECK(!r.result&&u64(&r)==1);
        unsigned queries[]={17,20,21,22,44};for(unsigned j=0;j<5;j++){r=call(general,queries[j],NULL,0);CHECK(!r.result&&r.data_size==1&&!r.data[0]);}
        r=call(general,18,NULL,0);CHECK(!r.result&&r.data_size==3&&!r.data[0]&&!r.data[1]&&!r.data[2]);
        failed(call(general,12,NULL,0),RAITCHI_OFFLINE_NETWORK_DISABLED); // no host IP disclosure
        failed(call(general,0xffff,NULL,0),RAITCHI_OFFLINE_NETWORK_DISABLED);
        Service *request=object(call(general,4,NULL,0));
        r=call(request,0,NULL,0);CHECK(!r.result&&u32(&r)==1);
        failed(call(request,1,NULL,0),RAITCHI_OFFLINE_NETWORK_DISABLED);
        r=call(request,2,NULL,0);CHECK(!r.result&&r.ncopy==2&&r.copy_handles[0]&&r.copy_handles[1]&&r.copy_handles[0]!=r.copy_handles[1]);
        unsigned first=nevents-2;CHECK(!events[first].signaled&&!events[first+1].signaled);
        IpcResp again=call(request,2,NULL,0);CHECK(again.copy_handles[0]!=r.copy_handles[0]&&again.copy_handles[1]!=r.copy_handles[1]);
        CHECK(handle_get(again.copy_handles[0])==handle_get(r.copy_handles[0])&&handle_get(again.copy_handles[1])==handle_get(r.copy_handles[1]));
        close_copies(r);close_copies(again);
        r=call(request,4,NULL,0);CHECK(!r.result&&events[first].signaled&&events[first+1].signaled);
        failed(call(request,1,NULL,0),RAITCHI_OFFLINE_NETWORK_DISABLED);
        r=call(request,0,NULL,0);CHECK(!r.result&&u32(&r)!=3);
        r=call(request,3,NULL,0);CHECK(!r.result);
        failed(call(request,21,NULL,0),RAITCHI_OFFLINE_NETWORK_DISABLED);
        failed(call(request,0xffff,NULL,0),RAITCHI_OFFLINE_UNSUPPORTED);
        release(request);
        request=object(call(general,4,NULL,0));CHECK(!call(request,4,NULL,0).result);
        r=call(request,2,NULL,0);CHECK(r.ncopy==2&&events[nevents-1].signaled&&events[nevents-2].signaled);
        close_copies(r);release(request);release(general);release(root);
        CHECK(!open_handles());
    }
    // Repeated request closure returns owned handles. Guest copies remain
    // independently valid until the guest closes them; recycled slots must
    // never be mistaken for the service's old base handles.
    Service *root=service_lookup("nifm:u"),*general=object(call(root,5,NULL,0));
    for(unsigned i=0;i<32;i++){
        unsigned before=open_handles();Service *request=object(call(general,4,NULL,0));IpcResp copy=call(request,2,NULL,0);CHECK(copy.ncopy==2&&open_handles()==before+6);
        KObject *first=handle_get(copy.copy_handles[0]);release(request);CHECK(open_handles()==before+2&&handle_get(copy.copy_handles[0])==first);
        close_copies(copy);CHECK(open_handles()==before);
    }
    uint32_t old=kernel_new_handle(&events[0]);kernel_close_service_handle(old,&events[0]);uint32_t reused=kernel_new_handle(&events[1]);CHECK(old==reused);
    kernel_close_service_handle(old,&events[0]);CHECK(handle_get(reused)==&events[1]);kernel_close_service_handle(reused,&events[1]);CHECK(!open_handles());
    release(general);release(root);
}
static void account_tests(void){
    Service *account=service_lookup("acc:u0");IpcResp r=call(account,0,NULL,0);CHECK(!r.result&&u32(&r)==1);
    r=call(account,4,NULL,0);CHECK(!r.result&&r.data_size==16&&!memcmp(r.data,g_user_id,16));
    uint8_t required=0;r=call(account,51,&required,1);CHECK(!r.result&&r.data_size==16&&!memcmp(r.data,g_user_id,16));
    required=1;failed(call(account,51,&required,1),RAITCHI_OFFLINE_ACCOUNT_REQUEST);failed(call(account,52,&required,1),RAITCHI_OFFLINE_ACCOUNT_REQUEST);
    failed(call(account,51,NULL,0),MAKE_RESULT(1,14));failed(call(account,1,NULL,0),MAKE_RESULT(1,14));
    r=call(account,1,g_user_id,16);CHECK(!r.result&&r.data_size==1&&r.data[0]);
    Service *manager=object(call(account,101,g_user_id,16));failed(call(manager,0,NULL,0),RAITCHI_OFFLINE_ACCOUNT_REQUEST);
    failed(call(manager,1,NULL,0),RAITCHI_OFFLINE_ACCOUNT_UNLINKED);
    uint32_t online[]={2,3,4,130,136,150,170,0xffff};for(unsigned i=0;i<8;i++)failed(call(manager,online[i],NULL,0),RAITCHI_OFFLINE_ACCOUNT_REQUEST);
    uint32_t root_online[]={102,103,120};for(unsigned i=0;i<3;i++)failed(call(account,root_online[i],NULL,0),RAITCHI_OFFLINE_ACCOUNT_REQUEST);
    failed(call(account,999,NULL,0),RAITCHI_OFFLINE_UNSUPPORTED);
    r=call(account,160,NULL,0);CHECK(!r.result); // modern offline application initialization
    Service *profile=object(call(account,5,g_user_id,16));r=call(profile,1,NULL,0);CHECK(!r.result&&r.data_size==0x38&&!memcmp(r.data,g_user_id,16));
    release(profile);release(manager);release(account);
}
static void applet_tests(void){
    Service *root=service_lookup("appletOE");Service *proxy=object(call(root,0,NULL,0));Service *creator=object(call(proxy,11,NULL,0));
    int64_t size=0x20;Service *storage=object(call(creator,10,&size,8));Service *accessor=object(call(storage,0,NULL,0));
    IpcResp r=call(accessor,0,NULL,0);CHECK(!r.result&&u64(&r)==0x20);
    _Alignas(8) int64_t offset=0;uint8_t input[32]={1,2,3,4},output[32]={0};IpcReq req={.cmd=10,.data=(const uint8_t*)&offset,.data_size=8,.a={{(uintptr_t)input,sizeof input}},.na=1};IpcResp resp={0};accessor->handler(accessor,&req,&resp);CHECK(!resp.result);
    req.cmd=11;req.nb=1;req.b[0]=(IpcBuf){(uintptr_t)output,sizeof output};resp=(IpcResp){0};accessor->handler(accessor,&req,&resp);CHECK(!resp.result&&!memcmp(output,input,sizeof input));
    release(accessor);release(storage);
    size=-1;failed(call(creator,10,&size,8),MAKE_RESULT(128,506));size=1024*1024+1;failed(call(creator,10,&size,8),MAKE_RESULT(128,506));failed(call(creator,10,NULL,0),MAKE_RESULT(128,506));
    for(uint32_t id=0x0e;id<=0x10;id++){
        uint32_t args[]={id,0};Service *applet=object(call(creator,id==0x10?3:0,args,sizeof args));
        r=call(applet,1,NULL,0);CHECK(!r.result&&r.data_size==1&&!r.data[0]);
        failed(call(applet,30,NULL,0),MAKE_RESULT(1,125));
        r=call(applet,0,NULL,0);CHECK(!r.result&&r.ncopy==1);unsigned event=nevents-1;CHECK(!events[event].signaled);close_copies(r);
        r=call(applet,10,NULL,0);CHECK(!r.result&&events[event].signaled);
        r=call(applet,1,NULL,0);CHECK(!r.result&&r.data[0]);
        r=call(applet,30,NULL,0);CHECK(r.result==(id==0x0f?RAITCHI_OFFLINE_NETWORK_DISABLED:0));
        r=call(applet,105,NULL,0);CHECK(!r.result&&r.ncopy==1&&events[nevents-1].signaled);close_copies(r);
        if(id==0x10){
            storage=object(call(applet,101,NULL,0));accessor=object(call(storage,0,NULL,0));r=call(accessor,0,NULL,0);CHECK(!r.result&&u64(&r)==0x18);
            uint8_t cancelled[0x18]={0};offset=0;req=(IpcReq){.cmd=11,.data=(const uint8_t*)&offset,.data_size=8,.b={{(uintptr_t)cancelled,sizeof cancelled}},.nb=1};resp=(IpcResp){0};accessor->handler(accessor,&req,&resp);CHECK(!resp.result);
            uint64_t result;memcpy(&result,cancelled,8);CHECK(result==RAITCHI_OFFLINE_ACCOUNT_CANCELLED);for(unsigned j=8;j<sizeof cancelled;j++)CHECK(!cancelled[j]);
            failed(call(applet,101,NULL,0),MAKE_RESULT(128,2));release(accessor);release(storage);
        }else failed(call(applet,101,NULL,0),MAKE_RESULT(128,2));
        failed(call(applet,0xffff,NULL,0),RAITCHI_OFFLINE_UNSUPPORTED);release(applet);
    }
    uint32_t unsupported[]={0x19,0};failed(call(creator,0,unsupported,sizeof unsupported),RAITCHI_OFFLINE_UNSUPPORTED);failed(call(creator,0,NULL,0),RAITCHI_OFFLINE_UNSUPPORTED);
    release(creator);release(proxy);release(root);
}
// Exercise the production CMIF/TIPC input-object resolution, not a direct
// handler call that manually fills the new resolved-object field.
static uint32_t wire_push(Service *applet,Service *input,bool domain,unsigned variant) {
    _Alignas(16) uint32_t message[64]={0};Cpu cpu={.tpidrro=(uintptr_t)message};
    uint32_t target_handle=session_new(applet);KSession *target=(KSession*)handle_get(target_handle);
    uint32_t input_handle=0;KSession *input_session=NULL;Domain local_domain={0};
    if(domain){
        local_domain.objs[1]=applet;local_domain.objs[2]=input;target->domain=&local_domain;
        message[0]=4;message[1]=13;((uint8_t*)(message+4))[0]=1;((uint8_t*)(message+4))[1]=1;
        ((uint16_t*)(message+4))[1]=16;message[5]=1;memcpy(message+8,"SFCI",4);message[10]=100;message[12]=variant==1?300:2;
    }else{
        message[0]=variant==3?116:4;message[1]=variant==3?0:8;
        if(input||variant==2){
            input_handle=variant==2?kernel_new_handle(&events[0]):session_new(input);
            if(variant!=2)input_session=(KSession*)handle_get(input_handle);
            message[1]|=0x80000000u;message[2]=1<<5;message[3]=input_handle;
        }
        if(variant!=3){memcpy(message+4,"SFCI",4);message[6]=100;}
    }
    CHECK(!ipc_handle_request(&cpu,target_handle));uint32_t result=message[variant==3?2:domain?10:6];
    if(input_handle){KObject *object=handle_get(input_handle);kernel_close_service_handle(input_handle,object);if(input_session){input->refs--;free(input_session);}}
    kernel_close_service_handle(target_handle,&target->obj);applet->refs--;free(target);return result;
}
static Service *make_storage(Service *creator,const uint8_t *bytes,size_t size){
    int64_t n=(int64_t)size;Service *storage=object(call(creator,10,&n,8));Service *accessor=object(call(storage,0,NULL,0));
    int64_t offset=0;IpcReq req={.cmd=10,.data=(const uint8_t*)&offset,.data_size=8,.a={{(uintptr_t)bytes,size}},.na=1};IpcResp r={0};accessor->handler(accessor,&req,&r);CHECK(!r.result);release(accessor);return storage;
}
static void profile_result(Service *applet,bool local){
    CHECK(!call(applet,10,NULL,0).result);CHECK(!call(applet,30,NULL,0).result);
    Service *storage=object(call(applet,101,NULL,0)),*accessor=object(call(storage,0,NULL,0));
    uint8_t out[0x18]={0};int64_t offset=0;IpcReq req={.cmd=11,.data=(const uint8_t*)&offset,.data_size=8,.b={{(uintptr_t)out,sizeof out}},.nb=1};IpcResp r={0};accessor->handler(accessor,&req,&r);CHECK(!r.result);
    uint64_t result;memcpy(&result,out,8);CHECK(result==(local?0:RAITCHI_OFFLINE_ACCOUNT_CANCELLED));
    if(local)CHECK(!memcmp(out+8,g_user_id,16));else for(unsigned i=8;i<sizeof out;i++)CHECK(!out[i]);release(accessor);release(storage);
}
static void wire_profile_tests(void){
    Service *root=service_lookup("appletOE"),*proxy=object(call(root,0,NULL,0)),*creator=object(call(proxy,11,NULL,0));
    for(unsigned test=0;test<11;test++){
        uint32_t args[]={0x10,0};Service *applet=object(call(creator,0,args,sizeof args));
        uint8_t config[0xa0]={0};size_t size=test==1?0x98:sizeof config;
        if(test==2)config[0x90]=1; // local selector that requires a Nintendo account
        if(test==3)config[0]=2; // EnsureNetworkServiceAccountAvailable
        if(test==4)memcpy(config+8,g_user_id,16); // this local UID is explicitly disallowed
        if(test==5)size=0x97;
        Service *input=test==6?service_new("not-storage",NULL,NULL):test==7||test==9?NULL:make_storage(creator,config,size);
        bool domain=test==1||test==8;unsigned variant=test==8?1:test==9?2:test==10?3:0;
        uint32_t result=wire_push(applet,input,domain,variant);
        if(test==5)CHECK(result==MAKE_RESULT(128,500));
        else if(test==6||test==7||test==8||test==9)CHECK(result==MAKE_RESULT(128,518));
        else CHECK(!result);
        bool local=test==0||test==1||test==10;profile_result(applet,local);
        if(input)release(input);release(applet);
    }
    release(creator);release(proxy);release(root);
}

int main(void){network_tests();account_tests();applet_tests();wire_profile_tests();CHECK(handles_closed>0);printf("{\"assertions\":%u,\"network_roots\":3,\"applet_protocols\":3}\n",checks);return 0;}
