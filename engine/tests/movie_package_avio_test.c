/* Actual movie callbacks + provider + system FFmpeg; private original inputs supplied by checker. */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "nxpkg.h"
#include "runtime/guest_path.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"line%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char *g_romfs_dir="/nonexistent-raitchi-package-probe";
static NxRomfs *mounted;
static uint64_t file_offset,file_size;
static const char *selected;
static int raw_fd=-1;
static bool shared_title_las(void){return false;}
static bool rando_romfs_override(const char *p,char *o,size_t n){(void)p;(void)o;(void)n;return false;}
static bool romfs_image_file(const char *p,int *fd,uint64_t *off,uint64_t *size){
    if(raw_fd<0||strcmp(p,selected))return false;*fd=dup(raw_fd);*off=0;*size=file_size;return *fd>=0;
}
static bool romfs_package_file(const char *p,NxRomfs **reader,uint64_t *off,uint64_t *size){
    if(!mounted||strcmp(p,selected))return false;char why[256];*reader=nx_romfs_clone(mounted,why,sizeof why);
    CHECK(*reader);*off=file_offset;*size=file_size;return true;
}
#include "production_movie_avio.inc"
static uint32_t u32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}
static void read_exact(NxRomfs *r,void *p,uint64_t n,uint64_t off){CHECK(nx_romfs_pread(r,p,n,off)==(ssize_t)n);}
static void directory(const uint8_t *dirs,uint64_t size,uint32_t at,char *out,size_t cap,unsigned depth){
    CHECK(depth<64&&at<=size&&size-at>=24);uint32_t len=u32(dirs+at+20);CHECK(len<=size-at-24&&len<cap);
    uint32_t parent=u32(dirs+at);if(at&&parent!=UINT32_MAX){directory(dirs,size,parent,out,cap,depth+1);if(*out)strcat(out,"/");}
    CHECK(strlen(out)+len<cap);strncat(out,(const char*)dirs+at+24,len);
}
static void lookup(NxRomfs *r,const char *path){
    uint8_t h[80];read_exact(r,h,80,0);CHECK(u64(h)==80);
    uint64_t ds=u64(h+32),fs=u64(h+64),data=u64(h+72);CHECK(ds&&fs&&ds<(64u<<20)&&fs<(64u<<20));
    uint8_t *dirs=malloc(ds),*files=malloc(fs);CHECK(dirs&&files);read_exact(r,dirs,ds,u64(h+24));read_exact(r,files,fs,u64(h+56));
    bool found=false;for(uint64_t off=0;off<fs;){CHECK(fs-off>=32);uint32_t len=u32(files+off+28);CHECK(len<=fs-off-32);
        char name[4096]="";directory(dirs,ds,u32(files+off),name,sizeof name,0);if(*name)strcat(name,"/");CHECK(strlen(name)+len<sizeof name);strncat(name,(char*)files+off+32,len);
        if(!strcmp(name,path)){CHECK(!found);file_offset=data+u64(files+off+8);file_size=u64(files+off+16);found=true;}
        off=(off+32+len+3)&~UINT64_C(3);
    }free(dirs);free(files);CHECK(found&&file_size>0&&file_offset<=nx_romfs_size(r)&&file_size<=nx_romfs_size(r)-file_offset);
}
static uint64_t hash(uint64_t h,const void *bytes,size_t n){const uint8_t *p=bytes;while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
typedef struct{uint64_t first_packets,middle_packets,first_frame;int width,height,codec;int64_t duration;} Summary;
static Summary inspect(AVFormatContext *fmt){
    CHECK(avformat_find_stream_info(fmt,NULL)>=0);int video=av_find_best_stream(fmt,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);CHECK(video>=0);
    AVCodecParameters *par=fmt->streams[video]->codecpar;const AVCodec *codec=avcodec_find_decoder(par->codec_id);CHECK(codec);
    AVCodecContext *decoder=avcodec_alloc_context3(codec);CHECK(decoder);CHECK(avcodec_parameters_to_context(decoder,par)>=0);decoder->thread_count=1;CHECK(avcodec_open2(decoder,codec,NULL)>=0);
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();CHECK(frame&&packet);
    Summary s={.width=par->width,.height=par->height,.codec=par->codec_id,.duration=fmt->duration,.first_packets=1,.middle_packets=1};
    CHECK(av_seek_frame(fmt,-1,0,AVSEEK_FLAG_BACKWARD)>=0);
    for(unsigned i=0;i<32;i++){
        CHECK(av_read_frame(fmt,packet)>=0);s.first_packets=hash(s.first_packets,packet->data,packet->size);s.first_packets=hash(s.first_packets,&packet->pts,sizeof packet->pts);
        if(packet->stream_index==video&&!s.first_frame){CHECK(avcodec_send_packet(decoder,packet)>=0);int r=avcodec_receive_frame(decoder,frame);CHECK(r>=0||r==AVERROR(EAGAIN));
            if(r>=0){int bytes=av_image_get_buffer_size(frame->format,frame->width,frame->height,1);CHECK(bytes>0);uint8_t *pixels=malloc(bytes);CHECK(pixels);CHECK(av_image_copy_to_buffer(pixels,bytes,(const uint8_t *const*)frame->data,frame->linesize,frame->format,frame->width,frame->height,1)==bytes);s.first_frame=hash(1,pixels,bytes);free(pixels);}}
        av_packet_unref(packet);
    }
    CHECK(s.first_frame&&s.duration>0);CHECK(av_seek_frame(fmt,-1,s.duration/2,AVSEEK_FLAG_BACKWARD)>=0);
    for(unsigned i=0;i<32;i++){CHECK(av_read_frame(fmt,packet)>=0);s.middle_packets=hash(s.middle_packets,packet->data,packet->size);s.middle_packets=hash(s.middle_packets,&packet->pts,sizeof packet->pts);av_packet_unref(packet);}
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);return s;
}
static void same(const Summary *a,const Summary *b){CHECK(a->first_packets==b->first_packets);CHECK(a->middle_packets==b->middle_packets);CHECK(a->first_frame==b->first_frame);CHECK(a->width==b->width&&a->height==b->height&&a->codec==b->codec&&a->duration==b->duration);}
int main(int argc,char **argv){
    CHECK(argc==5);char why[512],host[4096];selected=argv[3];
    int fd=open(argv[1],O_RDONLY);CHECK(fd>=0);NxPackage *package=nx_package_open_fd(fd,argv[2],why,sizeof why);close(fd);CHECK(package);
    mounted=nx_package_open_romfs(package,why,sizeof why);CHECK(mounted);lookup(mounted,selected);
    Extractor e={.img_fd=-1};CHECK(ex_open(&e,selected,host,sizeof host));CHECK(e.img_package&&e.avio&&e.fmt&&e.img_fd==-1);
    nx_romfs_close(mounted);mounted=NULL;nx_package_close(package);package=NULL;
    Summary direct=inspect(e.fmt);CHECK(img_seek(&e,INT64_MIN,SEEK_CUR)<0);CHECK(img_seek(&e,0,AVSEEK_SIZE)==(int64_t)file_size);
    ex_close(&e);CHECK(!e.img_package&&!e.fmt&&!e.avio&&e.img_fd==-1);ex_close(&e);
    AVFormatContext *reference=NULL;CHECK(avformat_open_input(&reference,argv[4],NULL,NULL)>=0);Summary plain=inspect(reference);same(&direct,&plain);avformat_close_input(&reference);
    raw_fd=open(argv[4],O_RDONLY);CHECK(raw_fd>=0);struct stat st;CHECK(!fstat(raw_fd,&st)&&st.st_size==(int64_t)file_size);
    CHECK(ex_open(&e,selected,host,sizeof host));CHECK(!e.img_package&&e.img_fd>=0&&e.avio);close(raw_fd);raw_fd=-1;
    Summary raw=inspect(e.fmt);same(&direct,&raw);ex_close(&e);
    CHECK(!ex_open(&e,"../escape.webm",host,sizeof host));CHECK(!e.img_package&&!e.avio&&e.img_fd==-1);
    printf("{\"state\":\"passed\",\"checks\":%u,\"movie_bytes\":%llu,\"width\":%d,\"height\":%d,\"first_frame_hash\":\"%016llx\",\"packet_groups\":6,\"packets_per_group\":32,\"parent_closed_before_demux\":true,\"package_raw_reference_match\":true,\"movie_extracted_by_probe\":false}\n",checks,(unsigned long long)file_size,direct.width,direct.height,(unsigned long long)direct.first_frame);
}
