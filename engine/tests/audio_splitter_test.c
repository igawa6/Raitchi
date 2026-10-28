/* Synthetic, asset-free fixtures through production parser, graph and mixer. */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include "ipc.h"
typedef struct SDL_AudioStream SDL_AudioStream;
static struct { bool reverb; } g_settings = {true};
static unsigned parsed_logs, route_logs, voice_logs, absent_logs, rejected_logs;
static void test_log(const char *format, ...) {
    if (strstr(format,"mix splitter section accepted")) parsed_logs++;
    if (strstr(format,"mix splitter route accepted")) route_logs++;
    if (strstr(format,"voice splitter route accepted")) voice_logs++;
    if (strstr(format,"mix splitter section absent")) absent_logs++;
    if (strstr(format,"rejected mix/splitter")) rejected_logs++;
}
#define LOG(...) test_log(__VA_ARGS__)
static int fail_after = -1;
static void *test_malloc(size_t n) { if (fail_after == 0) return NULL; if (fail_after > 0) fail_after--; return malloc(n); }
static void *test_calloc(size_t n, size_t size) { if (fail_after == 0) return NULL; if (fail_after > 0) fail_after--; return calloc(n,size); }
#define malloc test_malloc
#define calloc test_calloc
#include "production_audio_splitter.inc"
#undef malloc
#undef calloc

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static void u32(uint8_t *p, uint32_t v) { memcpy(p,&v,4); }
static void f32(uint8_t *p, float v) { memcpy(p,&v,4); }
static void mix_record(uint8_t *p,unsigned id,unsigned channels,float volume,int32_t dest,uint32_t splitter) {
    memset(p,0,0x930); f32(p,volume); u32(p+4,48000); u32(p+8,channels); p[12]=1; u32(p+16,id);
    u32(p+0x924,(uint32_t)dest); u32(p+0x928,splitter);
    for(unsigned i=0;i<channels;i++)f32(p+0x24+(i*MIXBUF_MAX+i)*4,1);
}
static size_t splitter_packet(uint8_t *p,unsigned count,const unsigned *ids,const unsigned *mixes,const float *gains) {
    memset(p,0,4096);u32(p,0x48444e53);u32(p+4,1);u32(p+8,count);
    u32(p+0x20,0x49444e53);u32(p+0x24,0);u32(p+0x28,48000);u32(p+0x2c,count);
    for(unsigned i=0;i<count;i++)u32(p+0x30+4*i,ids[i]);
    size_t at=0x20+0x1c+4*count;
    for(unsigned i=0;i<count;i++,at+=0x70) {
        u32(p+at,0x44444e53);u32(p+at+4,ids[i]);u32(p+at+0x68,mixes[i]);p[at+0x6c]=1;
        f32(p+at+8+4*(i%2),gains[i]);
    }
    return at;
}
static void initialize(Renderer *r) {
    memset(r,0,sizeof *r);r->sample_rate=48000;r->sample_count=16;r->nmix=11;r->voices=2;
    r->revision=0x36455652;r->splitters=2;r->splitter_destinations=8;
    r->mix=calloc(r->nmix,sizeof *r->mix);r->voice=calloc(r->voices,sizeof *r->voice);
    r->chres=calloc(r->voices,sizeof *r->chres);r->chst=calloc(r->voices,sizeof *r->chst);
    CHECK(r->mix&&r->voice&&r->chres&&r->chst);CHECK(!pthread_mutex_init(&r->lock,NULL));
    r->sink_used=1;r->sink_inputs=2;r->sink_input[1]=1;
}
static void destroy(Renderer *r) {
    for(unsigned i=0;i<r->nmix;i++)free(r->mix[i].buf);
    free_mix_splitters(r->routing);free(r->mix);free(r->voice);free(r->chres);free(r->chst);
    CHECK(!pthread_mutex_destroy(&r->lock));
}
static void pcm_source(Renderer *r,unsigned mix) {
    static int16_t pcm[2*256];for(unsigned i=0;i<256;i++){pcm[2*i]=1000;pcm[2*i+1]=-2000;}
    memset(r->voice,0,r->voices*sizeof *r->voice);memset(r->chst,0,r->voices*sizeof *r->chst);
    Voice *v=&r->voice[0];v->in_use=1;v->fmt=2;v->rate=48000;v->chans=2;v->pitch=1;
    v->volume=v->prev_volume=1;v->mix_id=(int32_t)mix;v->splitter_id=UINT32_MAX;v->chan_ids[1]=1;v->wb_count=1;
    v->wb[0]=(WaveBuf){.addr=(uintptr_t)pcm,.size=sizeof pcm,.end=256,.loop=1,.loop_count=-1,.sent=1};
    for(unsigned c=0;c<2;c++) {
        r->chst[c].valid[0]=1;
        for(unsigned i=0;i<HIST;i++)r->chst[c].hist[i]=c?-2000:1000;
        memset(&r->chres[c],0,sizeof r->chres[c]);r->chres[c].mix[c]=r->chres[c].prev[c]=1;
    }
}
static void output_is(Renderer *r,float left,float right) {
    float stereo[32];render_frame(r,stereo);
    for(unsigned i=0;i<16;i++){CHECK(fabsf(stereo[i*2]-left/32768.f)<1e-7f);CHECK(fabsf(stereo[i*2+1]-right/32768.f)<1e-7f);}
}
static void baseline(uint8_t *mix) {
    memset(mix,0,11*0x930);
    mix_record(mix,0,2,.5f,AUDREN_UNUSED_MIX,UINT32_MAX);
    mix_record(mix+0x930,1,2,2,0,UINT32_MAX);
    mix_record(mix+2*0x930,2,2,.5f,1,UINT32_MAX);
    mix_record(mix+3*0x930,3,2,1.25f,1,UINT32_MAX);
    mix_record(mix+10*0x930,10,2,.5f,AUDREN_UNUSED_MIX,0);
}
static void unchanged(Renderer *r,MixSplitters *routing,const Mix *before) {
    CHECK(r->routing==routing);CHECK(!memcmp(r->mix,before,r->nmix*sizeof *before));
}
static void test_routing_and_failures(void) {
    Renderer r;initialize(&r);uint8_t mixes[11*0x930],packet[4096],bad[4096];
    const unsigned ids[]={3,1,7,5},targets[]={2,2,3,3};const float gain[]={.25f,.5f,.75f,.25f};
    size_t n=splitter_packet(packet,4,ids,targets,gain);baseline(mixes);
    CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));CHECK(r.routing);
    CHECK(r.routing->info[0].count==4);CHECK(r.routing->ids[0]==3&&r.routing->ids[3]==5);
    CHECK(!parsed_logs&&!route_logs&&!voice_logs&&!absent_logs&&!rejected_logs);
    CHECK(!setenv("LARECOMP_AUDIO_LOG","1",1));CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    unsigned pos[11]={0};for(unsigned i=0;i<r.mix_order_count;i++)pos[r.mix_order[i]]=i;
    CHECK(pos[10]<pos[2]&&pos[10]<pos[3]);CHECK(pos[2]<pos[1]&&pos[3]<pos[1]);
    pcm_source(&r,10);output_is(&r,531.25f,-562.5f);CHECK(r.chst[0].played==16&&r.chst[1].played==16);
    // Old direct matrix routing remains a distinct, multiplicative path.
    mix_record(mixes+10*0x930,10,2,.5f,2,UINT32_MAX);
    CHECK(update_mix_routes(&r,NULL,0,mixes,sizeof mixes));pcm_source(&r,10);output_is(&r,250,-500);
    baseline(mixes);CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    Mix before[11];memcpy(before,r.mix,sizeof before);MixSplitters *old=r.routing;
    // Every truncated prefix is rejected with the complete previous graph retained.
    for(size_t length=1;length<n;length++) {CHECK(!update_mix_routes(&r,packet,length,mixes,sizeof mixes));unchanged(&r,old,before);}
    // Bad magics, counts, IDs, duplicate linkage/destination records and NaNs.
    const size_t offsets[]={0,4,8,0x20,0x24,0x2c,0x30,0x20+0x1c+16,0x20+0x1c+16+4,0x20+0x1c+16+0x68};
    for(unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        memcpy(bad,packet,n);u32(bad+offsets[i],UINT32_MAX);
        CHECK(!update_mix_routes(&r,bad,n,mixes,sizeof mixes));unchanged(&r,old,before);
    }
    memcpy(bad,packet,n);u32(bad+0x34,ids[0]);CHECK(!update_mix_routes(&r,bad,n,mixes,sizeof mixes));unchanged(&r,old,before);
    memcpy(bad,packet,n);u32(bad+0x20+0x1c+16+0x70+4,ids[0]);CHECK(!update_mix_routes(&r,bad,n,mixes,sizeof mixes));unchanged(&r,old,before);
    for(unsigned j=0;j<2;j++){memcpy(bad,packet,n);f32(bad+0x20+0x1c+16+8+4*j,NAN);CHECK(!update_mix_routes(&r,bad,n,mixes,sizeof mixes));unchanged(&r,old,before);}
    f32(mixes+10*0x930,NAN);CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));unchanged(&r,old,before);baseline(mixes);
    uint32_t limit=r.splitters;r.splitters=AUDREN_SPLITTER_MAX+1;
    CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));r.splitters=limit;unchanged(&r,old,before);
    limit=r.splitter_destinations;r.splitter_destinations=AUDREN_SPLITTER_DEST_MAX+1;
    CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));r.splitter_destinations=limit;unchanged(&r,old,before);
    // An inactive record carries no mix parameters, even when its stale payload is junk.
    Mix disabled=r.mix[2];memset(mixes+2*0x930,0xff,0x930);mixes[2*0x930+12]=0;
    CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));disabled.used=0;
    CHECK(!memcmp(&r.mix[2],&disabled,sizeof disabled));pcm_source(&r,10);output_is(&r,468.75f,-312.5f);
    baseline(mixes);CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    // Splitter-source matrices, final-mix matrices and out-of-dimension direct
    // coefficients are not consumed. They must not reject a valid transaction.
    for(unsigned j=0;j<24*24;j++) {f32(mixes+10*0x930+0x24+j*4,NAN);f32(mixes+0x24+j*4,NAN);}
    f32(mixes+2*0x930+0x24+(23*24+23)*4,NAN);
    memcpy(bad,packet,n);for(unsigned j=2;j<24;j++)f32(bad+0x20+0x1c+16+8+4*j,NAN);
    CHECK(update_mix_routes(&r,bad,n,mixes,sizeof mixes));pcm_source(&r,10);output_is(&r,531.25f,-562.5f);
    CHECK(isfinite(r.routing->dest[3].previous[23]));
    baseline(mixes);CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    memcpy(before,r.mix,sizeof before);old=r.routing;
    // Every consumed direct coefficient is validated, even if its previous gain
    // was zero; a rejected candidate cannot publish its new splitter state.
    for(unsigned a=0;a<2;a++)for(unsigned b=0;b<2;b++) {
        size_t offset=2*0x930+0x24+(a*24+b)*4;float saved;memcpy(&saved,mixes+offset,4);
        f32(mixes+offset,INFINITY);CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));unchanged(&r,old,before);
        f32(mixes+offset,saved);
    }
    // Direct cycle, splitter self-cycle and a cycle spanning a direct/split join.
    u32(mixes+0x930+0x924,2);CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));unchanged(&r,old,before);baseline(mixes);
    memcpy(bad,packet,n);u32(bad+0x20+0x1c+16+0x68,10);CHECK(!update_mix_routes(&r,bad,n,mixes,sizeof mixes));unchanged(&r,old,before);
    u32(mixes+0x930+0x924,10);CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));unchanged(&r,old,before);baseline(mixes);
    // Stage/state/info/destination/ID allocation failures all preserve old state.
    for(int failure=0;failure<5;failure++){fail_after=failure;CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));fail_after=-1;unchanged(&r,old,before);}
    free(r.mix[6].buf);r.mix[6].buf=NULL;memcpy(before,r.mix,sizeof before);
    mix_record(mixes+6*0x930,6,2,1,0,UINT32_MAX);
    fail_after=5;CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));fail_after=-1;unchanged(&r,old,before);baseline(mixes);
    pcm_source(&r,10);output_is(&r,531.25f,-562.5f);
    // Destination-only partial updates retain the ordered source-channel list.
    memset(bad,0,sizeof bad);u32(bad,0x48444e53);u32(bad+8,1);
    memcpy(bad+0x20,packet+0x20+0x1c+16,0x70);f32(bad+0x20+8,1);
    CHECK(update_mix_routes(&r,bad,0x90,NULL,0));CHECK(r.routing->info[0].count==4&&r.routing->ids[0]==3);
    pcm_source(&r,10);output_is(&r,718.75f,-562.5f);
    // Empty list update removes the route; it never guesses final mix 0.
    memset(bad,0,sizeof bad);u32(bad,0x48444e53);u32(bad+4,1);u32(bad+0x20,0x49444e53);
    CHECK(update_mix_routes(&r,bad,0x3c,NULL,0));pcm_source(&r,10);output_is(&r,0,0);
    destroy(&r);
}
static void test_revision_and_envelope(void) {
    Renderer r;initialize(&r);uint8_t packet[4096],mixes[11*0x930];baseline(mixes);
    const unsigned ids[]={0,1,2,3,4,5},targets[]={0,0,0,0,0,0};const float gains[]={1,1,1,1,1,1};
    size_t n=splitter_packet(packet,6,ids,targets,gains);
    r.revision=0x34455652;CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));CHECK(r.routing->info[0].count==4);
    r.revision=0x35455652;CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));CHECK(r.routing->info[0].count==6);
    MixSplitters *old=r.routing;Mix before[11];memcpy(before,r.mix,sizeof before);
    r.revision=0x3c455652;CHECK(!update_mix_routes(&r,packet,n,mixes,sizeof mixes));unchanged(&r,old,before);r.revision=0x36455652;
    // Actual RequestUpdate computes the splitter section before mixes, sinks and perf.
    _Alignas(16) uint8_t input[0x10000]={0},output[0x1000];uint32_t *h=(uint32_t *)input;
    h[0]=r.revision;h[1]=0x10;h[6]=sizeof mixes;h[9]=(uint32_t)n;h[15]=0x40+h[1]+h[6]+h[9];
    memcpy(input+0x50,packet,n);memcpy(input+0x50+n,mixes,sizeof mixes);
    IpcReq req={.na=1,.nb=1};IpcResp resp={0};req.a[0]=(IpcBuf){(uintptr_t)input,h[15]};req.b[0]=(IpcBuf){(uintptr_t)output,sizeof output};
    request_update(&r,&req,&resp);CHECK(r.routing!=old&&r.routing->info[0].count==6);
    old=r.routing;memcpy(before,r.mix,sizeof before);
    for(unsigned length=0;length<64;length++){req.a[0].size=length;request_update(&r,&req,&resp);unchanged(&r,old,before);}req.a[0].size=h[15];
    h[9]++;request_update(&r,&req,&resp);unchanged(&r,old,before);h[9]--;
    h[6]=UINT32_MAX;request_update(&r,&req,&resp);unchanged(&r,old,before);h[6]=sizeof mixes;
    req.b[0].size=0x40;request_update(&r,&req,&resp);unchanged(&r,old,before);
    CHECK(!pthread_mutex_trylock(&r.lock));CHECK(!pthread_mutex_unlock(&r.lock));
    req.b[0].size=sizeof output;
    uint8_t saved=input[0x50];input[0x50]=0;
    for(unsigned i=0;i<20;i++)request_update(&r,&req,&resp);
    input[0x50]=saved;unchanged(&r,old,before);CHECK(rejected_logs==8);
    for(unsigned i=0;i<20;i++)CHECK(update_mix_routes(&r,packet,n,NULL,0));
    for(unsigned i=0;i<20;i++)CHECK(update_mix_routes(&r,NULL,0,NULL,0));
    // The modern dirty-mix list preserves untouched mixes and current splitter state.
    uint8_t dirty[0x20+2*0x930]={0};u32(dirty,0xcafebabe);u32(dirty+4,1);
    mix_record(dirty+0x20,10,2,1,0,UINT32_MAX);r.revision=0x3b455652;
    CHECK(update_mix_routes(&r,NULL,0,dirty,0x20+0x930));pcm_source(&r,10);output_is(&r,500,-1000);
    old=r.routing;memcpy(before,r.mix,sizeof before);
    u32(dirty+4,2);CHECK(!update_mix_routes(&r,NULL,0,dirty,0x20+0x930));unchanged(&r,old,before);
    memcpy(dirty+0x20+0x930,dirty+0x20,0x930);CHECK(!update_mix_routes(&r,NULL,0,dirty,sizeof dirty));unchanged(&r,old,before);
    u32(dirty+4,1);u32(dirty+0x20+16,r.nmix);CHECK(!update_mix_routes(&r,NULL,0,dirty,0x20+0x930));unchanged(&r,old,before);
    destroy(&r);
}
static void test_voice_splitters(void) {
    Renderer r;initialize(&r);uint8_t packet[4096],mixes[11*0x930],partial[0x90]={0};baseline(mixes);
    const unsigned ids[]={3,1,7,5},targets[]={2,2,3,3};const float gains[]={.25f,.5f,.75f,.25f};
    size_t n=splitter_packet(packet,4,ids,targets,gains);CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    pcm_source(&r,AUDREN_UNUSED_MIX);r.voice[0].splitter_id=0;
    // Voice splitters use destination volumes, not channel-resource direct-mix coefficients.
    memset(r.chres,0,r.voices*sizeof *r.chres);output_is(&r,1062.5f,-1125);
    CHECK(r.routing->dest[3].previous[0]==.25f);
    // Two updates before a frame preserve the last rendered volume, not the last submitted volume.
    u32(partial,0x48444e53);u32(partial+8,1);memcpy(partial+0x20,packet+0x20+0x1c+16,0x70);
    f32(partial+0x20+8,.5f);CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    f32(partial+0x20+8,1);CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    CHECK(r.routing->dest[3].previous[0]==.25f&&r.routing->dest[3].volume[0]==1);
    r.voice[0].volume=.5f;float out[32];render_frame(&r,out);
    for(unsigned i=0;i<16;i++) {
        float t=i/16.f,v=1-.5f*t;
        CHECK(fabsf(out[i*2]-1000*v*(1.0625f+.375f*t)/32768.f)<1e-7f);
        CHECK(fabsf(out[i*2+1]-(-2000*v*.5625f)/32768.f)<1e-7f);
    }
    CHECK(r.routing->dest[3].previous[0]==1&&r.voice[0].prev_volume==.5f);output_is(&r,718.75f,-562.5f);
    // Inactive destinations cannot route, and reactivation initializes the previous volume.
    partial[0x20+0x6c]=0;f32(partial+0x20+8,.25f);CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    output_is(&r,468.75f,-562.5f);
    partial[0x20+0x6c]=1;CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    CHECK(r.routing->dest[3].previous[0]==.25f);output_is(&r,531.25f,-562.5f);
    // A bad float cannot alter current or previous ramp state.
    MixSplitters *old=r.routing;MixSplitterDest before=r.routing->dest[3];f32(partial+0x20+8,NAN);
    CHECK(!update_mix_routes(&r,partial,sizeof partial,NULL,0));CHECK(r.routing==old);
    CHECK(!memcmp(&before,&r.routing->dest[3],sizeof before));
    // A stopped voice never advances destination ramp history.
    f32(partial+0x20+8,1);CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    r.voice[0].play_state=1;output_is(&r,0,0);CHECK(r.routing->dest[3].previous[0]==.25f);
    // Positive invalid mix IDs do not silently use a valid splitter or mix zero.
    pcm_source(&r,123);r.voice[0].splitter_id=0;output_is(&r,0,0);
    pcm_source(&r,AUDREN_UNUSED_MIX);r.voice[0].splitter_id=r.splitters;output_is(&r,0,0);
    // Mono fan-out uses every destination ordinal (no hard-coded stereo pairing).
    CHECK(update_mix_routes(&r,packet,n,NULL,0));
    static int16_t mono[256];for(unsigned i=0;i<256;i++)mono[i]=1000;
    pcm_source(&r,AUDREN_UNUSED_MIX);r.voice[0].splitter_id=0;r.voice[0].chans=1;
    r.voice[0].wb[0].addr=(uintptr_t)mono;r.voice[0].wb[0].size=sizeof mono;
    output_is(&r,1062.5f,562.5f);
    // Old direct-voice ramp remains the combined envelope used by the existing mixer.
    pcm_source(&r,2);r.voice[0].prev_volume=0;r.voice[0].volume=1;render_frame(&r,out);
    for(unsigned i=0;i<16;i++){float t=i/16.f;CHECK(fabsf(out[i*2]-500*t/32768.f)<1e-7f);CHECK(fabsf(out[i*2+1]+1000*t/32768.f)<1e-7f);}
    destroy(&r);
}
static void test_shared_voice_ramp(void) {
    Renderer r;initialize(&r);uint8_t packet[4096],mixes[11*0x930],partial[0x90]={0};baseline(mixes);
    const unsigned ids[]={3,1,7,5},targets[]={2,2,3,3};const float gains[]={.25f,.5f,.75f,.25f};
    size_t n=splitter_packet(packet,4,ids,targets,gains);CHECK(update_mix_routes(&r,packet,n,mixes,sizeof mixes));
    static int16_t mono[256];for(unsigned i=0;i<256;i++)mono[i]=1000;
    pcm_source(&r,AUDREN_UNUSED_MIX);r.voice[0].splitter_id=0;r.voice[0].chans=1;
    r.voice[0].wb[0].addr=(uintptr_t)mono;r.voice[0].wb[0].size=sizeof mono;
    r.voice[1]=r.voice[0];r.voice[1].chan_ids[0]=1;
    for(unsigned i=0;i<HIST;i++)r.chst[1].hist[i]=1000;
    r.voice[0].prev_volume=1;r.voice[0].volume=.5f;
    r.voice[1].prev_volume=.75f;r.voice[1].volume=.25f;
    u32(partial,0x48444e53);u32(partial+8,1);memcpy(partial+0x20,packet+0x20+0x1c+16,0x70);
    f32(partial+0x20+8,1);CHECK(update_mix_routes(&r,partial,sizeof partial,NULL,0));
    float out[32];render_frame(&r,out);
    // Both independent voice envelopes see the same old destination gain.
    for(unsigned i=0;i<16;i++) {
        float t=i/16.f,sum=(1-.5f*t)+(.75f-.5f*t);
        CHECK(fabsf(out[2*i]-1000*sum*(1.0625f+.375f*t)/32768.f)<1e-7f);
        CHECK(fabsf(out[2*i+1]-1000*sum*.5625f/32768.f)<1e-7f);
    }
    CHECK(r.routing->dest[3].previous[0]==1);
    CHECK(r.voice[0].prev_volume==.5f&&r.voice[1].prev_volume==.25f);
    CHECK(r.chst[0].played==16&&r.chst[1].played==16);output_is(&r,1078.125f,421.875f);
    destroy(&r);
}
int main(void) {
    unsetenv("LARECOMP_AUDIO_LOG");
    test_routing_and_failures();test_revision_and_envelope();test_voice_splitters();test_shared_voice_ramp();
    atomic_uint cap=0;for(unsigned i=0;i<8;i++)CHECK(audio_route_log_slot(&cap));
    for(unsigned i=0;i<100;i++)CHECK(!audio_route_log_slot(&cap));CHECK(atomic_load(&cap)==8);
    CHECK(parsed_logs==8&&route_logs==8&&voice_logs==8&&absent_logs==8&&rejected_logs==8);
    printf("%u production audio splitter/parser/graph/PCM assertions passed\n",checks);return 0;
}
