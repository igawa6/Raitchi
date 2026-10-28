/* Actual RequestUpdate layout, with mixer/voice/effect processing controlled explicitly. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#define LOG(...) ((void)0)
static unsigned checks,voice_calls,effect_calls,mix_calls;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}} while(0)
typedef struct {unsigned in_use;} Voice;
typedef struct {uint8_t used;float mix[24];} ChanRes;
typedef struct {uint8_t seen_enabled;} Effect;
typedef struct {
    uint32_t revision,voices,effects,splitters;
    uint64_t elapsed_frames;
    pthread_mutex_t lock;
    Voice *voice;ChanRes *chres;Effect *fx;
    uint8_t sink_used,sink_inputs,sink_input[6];
} Renderer;
static void update_voice(Renderer *r,Voice *v,const uint8_t *input,uint8_t *output) {
    (void)r;voice_calls++;v->in_use=input[9]!=0;output[0]=0x51;output[1]=input[8];
}
static void effect_update(Effect *fx,const uint8_t *input) {effect_calls++;fx->seen_enabled=input[2];}
static bool audio_route_log_slot(atomic_uint *count) {(void)count;return false;}
static bool update_mix_routes(Renderer *r,const uint8_t *split,size_t split_size,const uint8_t *input,size_t size) {
    (void)r;(void)split;(void)split_size;(void)input;(void)size;return true;
}
#include "production_audio_update.inc"

static void run_revision(unsigned revision) {
    _Alignas(16) uint8_t input[0x2000]={0};_Alignas(16) uint8_t output[0x1040];memset(output,0xaa,sizeof output);
    uint32_t *h=(uint32_t *)input;
    h[0]=0x38455652; /* Deliberately REV8: the negotiated renderer revision selects output layout. */
    h[1]=0x10;h[2]=0x20;h[3]=2*0x170;h[4]=2*0x70;h[5]=2*0xc0;h[7]=0x140;h[8]=0x10;
    h[15]=0x40+h[1]+h[2]+h[3]+h[4]+h[5]+h[7]+h[8];
    uint8_t *pool=input+0x40+h[1];*(uint32_t *)(pool+0x10)=4;
    uint8_t *channels=pool+h[2];
    for(unsigned i=0;i<2;i++) {uint8_t *channel=channels+i*0x70;*(uint32_t *)channel=i;channel[0x64]=1;float value=0.5f+i;memcpy(channel+4,&value,4);}
    uint8_t *voices=channels+h[4];
    for(unsigned i=0;i<2;i++) {voices[i*0x170+9]=1;voices[i*0x170+8]=(uint8_t)i;}
    uint8_t *effects=voices+h[3];effects[0]=4;effects[2]=1;effects[0xc0]=1;effects[0xc0+2]=0;
    uint8_t *sink=effects+h[5];sink[0]=1;sink[1]=1;*(uint32_t *)(sink+0x120)=9;
    for(unsigned i=0;i<6;i++)sink[0x124+i]=(uint8_t)(i+2);
    Voice states[2]={{0},{0}};ChanRes resources[2]={{0},{0}};Effect fx[2]={{0},{0}};
    Renderer renderer={.revision=0x00564552u|((uint32_t)('0'+revision)<<24),.voices=2,.effects=2,
        .elapsed_frames=UINT64_C(0x1122334455667788),.voice=states,.chres=resources,.fx=fx};
    CHECK(!pthread_mutex_init(&renderer.lock,NULL));
    IpcReq request={.na=1,.nb=1};IpcResp response={0};
    request.a[0]=(IpcBuf){(uintptr_t)input,h[15]};request.b[0]=(IpcBuf){(uintptr_t)output,0x1000};
    unsigned old_voice=voice_calls,old_effect=effect_calls;request_update(&renderer,&request,&response);
    uint32_t *oh=(uint32_t *)output;
    CHECK(oh[0]==0x38455652);CHECK(oh[1]==0xb0);CHECK(oh[2]==0x10);CHECK(oh[3]==0x20);
    CHECK(oh[5]==(revision>=9?0x120u:0x20u));CHECK(oh[7]==0x20);CHECK(oh[8]==0x10);CHECK(oh[10]==0x10);
    CHECK(oh[15]==(revision>=9?0x280u:0x180u));
    CHECK(*(uint32_t *)(output+0x40)==5);CHECK(output[0x50]==0x51&&output[0x51]==0);
    CHECK(output[0x60]==0x51&&output[0x61]==1);CHECK(output[0x70]==3);
    unsigned second=revision>=9?0x100:0x80;CHECK(output[second]==4);
    unsigned stride=revision>=9?0x90:0x10;
    for(unsigned i=1;i<stride;i++){CHECK(output[0x70+i]==0);CHECK(output[second+i]==0);}
    CHECK(*(uint64_t *)(output+oh[15]-0x10)==renderer.elapsed_frames);
    for(unsigned i=oh[15];i<0x1000;i++)CHECK(output[i]==0);
    for(unsigned i=0x1000;i<sizeof output;i++)CHECK(output[i]==0xaa);
    CHECK(voice_calls-old_voice==2);CHECK(effect_calls-old_effect==2);CHECK(mix_calls==0);
    CHECK(states[0].in_use&&states[1].in_use);CHECK(resources[0].used&&resources[1].used);
    CHECK(resources[0].mix[0]==0.5f&&resources[1].mix[0]==1.5f);
    CHECK(fx[0].seen_enabled==1&&fx[1].seen_enabled==0);
    CHECK(renderer.sink_used==1&&renderer.sink_inputs==6);CHECK(!memcmp(renderer.sink_input,sink+0x124,6));
    CHECK(response.data_size==0&&response.result==0);CHECK(!pthread_mutex_trylock(&renderer.lock));CHECK(!pthread_mutex_unlock(&renderer.lock));
    CHECK(!pthread_mutex_destroy(&renderer.lock));
}
int main(void) {
    for(unsigned revision=0;revision<=12;revision++)run_revision(revision);
    Renderer renderer={0};IpcReq request={0};IpcResp response={0};unsigned before=voice_calls+effect_calls;
    request_update(&renderer,&request,&response);CHECK(voice_calls+effect_calls==before);
    printf("%u audio revision assertions passed (actual RequestUpdate; controlled mixer)\n",checks);return 0;
}
