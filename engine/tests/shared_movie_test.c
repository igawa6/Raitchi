/* Asset-free direct-call probes of the production movie overlay.
 * FFmpeg cleanup, guest allocation/callbacks, renderer writes, and SessionLayout
 * are explicit test doubles. No FFmpeg decode, real game data, or device proof.
 * Run tools/check_shared_movie.py; registration metadata is excluded only from
 * this host unit link so sanitizer retention does not retain unrelated methods.
 */
#include <assert.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include "hle.h"
// Registration sections are omitted in this direct-call unit probe, so sanitizer
// global retention does not pull unrelated FFmpeg entry points into the host link.
#undef HLE_FUNC
#define HLE_FUNC(sym,fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../ports/shared-android/runtime/hle/movie.c"
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); } } while (0)
static bool las;
bool shared_title_las(void) { return las; }
static HostFn registrations[64];
static unsigned registered;
void *hle_alloc(size_t size) { void *p=calloc(1,size); CHECK(p); return p; }
uint64_t hle_register(const char *name, HostFn fn) {
    CHECK(name && fn && registered<64); registrations[registered++]=fn; return registered*16;
}
static jmp_buf fatal_jmp;
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; }
void rt_fatal_trace(void) { longjmp(fatal_jmp,1); }
static unsigned allocations;
void guest_call(Cpu *c,uint64_t address) {
    CHECK(address==0x1234 && c->x[0]==0x345 && c->x[1]==0x1000 && c->x[2]==0x5678);
    ++allocations; c->x[0]=(uintptr_t)calloc(1,0x345); c->x[17]=0xffff;
}
static unsigned writes;
void backend_pool_written(uint64_t addr,uint64_t size) { CHECK(addr && size);++writes; }
void av_packet_free(AVPacket **p) {free(*p);*p=NULL;}
void avformat_close_input(AVFormatContext **p) {free(*p);*p=NULL;}
void av_freep(void *vp) { void **p=vp;free(*p);*p=NULL; }
void avio_context_free(AVIOContext **p) {free(*p);*p=NULL;}
struct NxRomfs {const uint8_t *data;uint64_t size;bool closed;};
static unsigned package_reads, package_closes;
static unsigned package_error_mode, package_error_calls;
ssize_t nx_romfs_pread(NxRomfs *r,void *out,uint64_t size,uint64_t off) {
    if(package_error_mode){package_error_calls++;
        if(package_error_mode==1){errno=0;return -1;}
        if(package_error_calls==1){errno=EINTR;return -1;}
        if(package_error_calls==2)return -1;  // deliberately leaves the caller's errno untouched
        errno=EIO;return -1;  // bounded old-code mutant: never allow a test to spin forever
    }
    CHECK(r&&!r->closed&&off<=r->size&&size<=r->size-off);package_reads++;memcpy(out,r->data+off,(size_t)size);return (ssize_t)size;
}
void nx_romfs_close(NxRomfs *r) {CHECK(r&&!r->closed);r->closed=true;package_closes++;}
static SessionLayout test_layout;
const SessionLayout *session_layout_current(void) { return &test_layout; }
// These texture-mode methods must never be used by the tight-buffer test.
bool renderer_upload_nv12(NvnTexture *t,const void *p,uint32_t w,uint32_t h,uint32_t stride,uint32_t off) {
    (void)t;(void)p;(void)w;(void)h;(void)stride;(void)off;abort();
}
void renderer_upload_rgba(NvnTexture *t,const void *p,uint32_t w,uint32_t h,uint32_t stride) {
    (void)t;(void)p;(void)w;(void)h;(void)stride;abort();
}
struct SwsContext *sws_getContext(int sw,int sh,enum AVPixelFormat sf,int dw,int dh,enum AVPixelFormat df,
                                int flags,SwsFilter *src,SwsFilter *dst,const double *param) {
    (void)sw;(void)sh;(void)sf;(void)dw;(void)dh;(void)df;(void)flags;(void)src;(void)dst;(void)param;abort();
}
int sws_scale(struct SwsContext *ctx,const uint8_t *const src[],const int strides[],int y,int h,
              uint8_t *const dst[],const int ds[]) {
    (void)ctx;(void)src;(void)strides;(void)y;(void)h;(void)dst;(void)ds;abort();
}
static void fake_slot(Cpu *c) { c->x[0]=13; }
int main(void) {
    Cpu c={0}; uint64_t handle=0; int64_t value=0;
    c.x[0]=(uintptr_t)&handle; md_ctor(&c);
    MediaData *m=(MediaData*)(uintptr_t)handle;
    md_set_int(m,"durationUs",INT64_C(0x123456789));
    md_set_int(m,"negative",INT64_C(-0x234567891));
    md_set_str(m,"mime","video/raw");
    const char *keys[]={"durationUs","negative","absent","mime"};
    for(unsigned i=0;i<4;i++) {
        value=0x77665544;c.x[0]=(uintptr_t)&handle;c.x[1]=(uintptr_t)keys[i];c.x[2]=(uintptr_t)&value;
        md_find_int64(&c);CHECK(c.x[0]==(i<2));
        CHECK(value==(i==0?INT64_C(0x123456789):i==1?INT64_C(-0x234567891):INT64_C(0x77665544)));
    }
    c.x[0]=0x55;movie_set_core_mask(&c);CHECK(c.x[0]==0x55);
    las=true;CHECK(movie_eof_result()==1);las=false;CHECK(movie_eof_result()==0x2000);
    Decoder d={.height=18,.stride=32};CHECK(dec_uv_offset(&d)==576);
    las=true;CHECK(dec_uv_offset(&d)==1024);las=false;
    c.x[0]=0x1234;c.x[3]=0x5678;movie_set_allocator(&c);
    c.x[0]=0x777;c.x[17]=0x888;uint8_t *a=buf_get(&c,0x345);
    CHECK(a && allocations==1 && c.x[0]==0x777 && c.x[17]==0x888);free(a);
    las=true;a=buf_get(&c,0x345);CHECK(a && allocations==1);free(a);
    HostFn funcs[2]={fake_slot,NULL};uint64_t *vt=(void*)(uintptr_t)make_vtable(funcs,2,"synthetic");
    CHECK(registered==64 && vt[0]==16 && vt[63]==1024);
    for(unsigned i=1;i<64;i++) CHECK(registrations[i]==movie_unsupported_slot);
    registrations[0](&c);CHECK(c.x[0]==13);
    if(!setjmp(fatal_jmp)) {registrations[1](&c);CHECK(!"unsupported virtual returned");}
    free(vt);
    char path[]="/tmp/shared-movie-probe-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);unlink(path);
    CHECK(write(fd,"__01234567__",12)==12);
    Extractor e={.img_fd=fd,.img_off=2,.img_size=8};uint8_t bytes[16];
    CHECK(img_read(&e,bytes,4)==4 && !memcmp(bytes,"0123",4));
    CHECK(img_read(&e,bytes,16)==4 && !memcmp(bytes,"4567",4));
    CHECK(img_read(&e,bytes,1)==AVERROR_EOF);
    CHECK(img_seek(&e,-3,SEEK_END)==5);CHECK(img_read(&e,bytes,3)==3 && !memcmp(bytes,"567",3));
    CHECK(img_seek(&e,0,AVSEEK_SIZE)==8);
    CHECK(img_seek(&e,INT64_MAX,SEEK_CUR)<0);
    CHECK(img_seek(&e,INT64_MIN,SEEK_CUR)<0);
    CHECK(img_seek(&e,-1,SEEK_SET)<0);CHECK(img_seek(&e,9,SEEK_SET)<0);
    CHECK(img_read(&e,bytes,0)<0);close(fd);
    NxRomfs package={.data=(const uint8_t *)"__package-video__",.size=17};
    Extractor pe={.img_fd=-1,.img_package=&package,.img_off=2,.img_size=13};
    CHECK(img_read(&pe,bytes,7)==7&&!memcmp(bytes,"package",7));
    CHECK(img_seek(&pe,-5,SEEK_END)==8);CHECK(img_read(&pe,bytes,16)==5&&!memcmp(bytes,"video",5));
    CHECK(img_read(&pe,bytes,1)==AVERROR_EOF);CHECK(img_seek(&pe,0,AVSEEK_SIZE)==13);
    CHECK(img_seek(&pe,0,SEEK_SET|AVSEEK_FORCE)==0);CHECK(img_read(&pe,bytes,13)==13&&!memcmp(bytes,"package-video",13));
    CHECK(package_reads==3);pe.img_pos=14;CHECK(img_read(&pe,bytes,1)==AVERROR(EINVAL));CHECK(img_seek(&pe,0,SEEK_CUR)==AVERROR(EINVAL));pe.img_pos=0;
    package_error_mode=1;package_error_calls=0;errno=EINTR;
    CHECK(img_read(&pe,bytes,1)==AVERROR(EIO)&&pe.img_pos==0&&package_error_calls==1);
    package_error_mode=2;package_error_calls=0;
    CHECK(img_read(&pe,bytes,1)==AVERROR(EIO)&&pe.img_pos==0&&package_error_calls==2);
    package_error_mode=0;
    ex_close(&pe);CHECK(package.closed&&package_closes==1&&!pe.img_package);ex_close(&pe);CHECK(package_closes==1);
    // Cleanup must close the owned descriptor once and tolerate repeated calls.
    e.img_fd=dup(STDERR_FILENO);CHECK(e.img_fd>=0);int owned=e.img_fd;
    e.pkt=calloc(1,sizeof(*e.pkt));e.fmt=calloc(1,sizeof(*e.fmt));
    e.avio=calloc(1,sizeof(*e.avio));e.avio->buffer=calloc(1,16);
    ex_close(&e);CHECK(e.img_fd==-1 && !e.pkt && !e.fmt && !e.avio);
    CHECK(fcntl(owned,F_GETFD)==-1 && errno==EBADF);ex_close(&e);
    // Tight LG NV12 and ring reuse (released slot is not reused immediately).
    las=false;Decoder video={.video=true,.width=4,.height=2,.stride=4};
    uint8_t y[8]={1,2,3,4,5,6,7,8},u[2]={9,10},v[2]={11,12};
    AVFrame frame={0};frame.data[0]=y;frame.data[1]=u;frame.data[2]=v;
    frame.linesize[0]=4;frame.linesize[1]=frame.linesize[2]=2;
    for(unsigned i=0;i<NOUT;i++) video.out_mem[i]=calloc(1,12);
    CHECK(emit_frame(&video,&frame,1,0));
    const uint8_t expected[12]={1,2,3,4,5,6,7,8,9,11,10,12};
    CHECK(video.out_size[0]==12 && !memcmp(video.out_mem[0],expected,12) && writes==1);
    video.out_busy[0]=false;CHECK(emit_frame(&video,&frame,2,0));
    CHECK(video.out_busy[1] && !video.out_busy[0] && video.out_next==2 && writes==2);
    uint8_t target[16];memset(target,0xa5,sizeof target);
    MBuffer output={.base=(uintptr_t)target,.capacity=12};
    test_layout.window.lo=(uintptr_t)target;test_layout.window.hi=(uintptr_t)target+sizeof target;
    uint64_t self[2]={0,(uintptr_t)&video};
    c.x[0]=(uintptr_t)self;c.x[1]=1;c.x[2]=(uintptr_t)&output;
    dec_get_output_buffer(&c);
    CHECK(c.x[0]==0 && output.base==(uintptr_t)target && output.size==12 && output.offset==0);
    CHECK(!memcmp(target,expected,12) && target[12]==0xa5 && writes==3);
    for(unsigned i=0;i<NOUT;i++)free(video.out_mem[i]);
    c.x[0]=(uintptr_t)&handle;md_dtor(&c);CHECK(!handle);
    printf("shared movie: %u checks passed\n",checks);
}
