/* Asset-free production discovery codec/store tests. Synthetic shader spans only. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shader_discovery.h"
bool shared_sha256_bytes(const void *,size_t,uint8_t[32]);
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static uint8_t vs[136],fs[132],cs[36],game[32];
static void fill(uint8_t*p,size_t n,uint8_t seed){for(size_t i=0;i<n;i++)p[i]=(uint8_t)(seed+i*29u+(i>>2));}
static NvnDiscoveryRecipe graphics(bool no_fs){
    NvnDiscoveryRecipe r={0};fill(vs,sizeof vs,0x31);fill(fs,sizeof fs,0x72);
    r.rec.kind='G';r.rec.vs=nvn_disc_codehash(vs,sizeof vs);r.rec.fs=no_fs?0:nvn_disc_codehash(fs,sizeof fs);
    r.rec.flags=SR_FLIP_Y|SR_DEPTH_MINUS_ONE;r.rec.nattrib=2;r.rec.nstream=1;r.rec.ncolor=1;
    r.rec.attrib[0]=0x12345;r.rec.attrib[1]=0x2ab;r.rec.stream[0][0]=20;r.rec.stream[0][1]=1;
    r.rec.color[0]=0x14;r.rec.cview[0]=0x18;r.rec.depth=0x8a;r.rec.samples=4;
    r.rec.channel_mask=0x00f3f00f;r.rec.blend_on=0x81; /* preserve full state input */
    for(unsigned i=0;i<8;i++)r.rec.blend[0][i]=(uint8_t)(0x11+i);
    r.rec.a2c=1;r.rec.topo=2;r.rec.poly_fill=1;
    r.vkey.glsl=1;r.vkey.attr_kind[0]=2;r.vkey.attr_kind[1]=1;r.vkey.flip_y=1;r.vkey.depth_minus_one=1;r.vkey.tex_uint=0x80000001;
    r.fkey.glsl=1;r.fkey.rt_kind[0]=1;r.fkey.flip_y=1;r.fkey.tex_uint=0x01020304;
    r.code[0]=vs;r.code_bytes[0]=128;r.constant_bytes[0]=8;if(!no_fs){r.code[1]=fs;r.code_bytes[1]=128;r.constant_bytes[1]=4;}return r;
}
static NvnDiscoveryRecipe compute(void){NvnDiscoveryRecipe r={0};fill(cs,sizeof cs,0xc3);r.rec.kind='C';r.rec.vs=nvn_disc_codehash(cs,sizeof cs);r.rec.local[0]=32;r.rec.local[1]=2;r.rec.local[2]=1;r.vkey.glsl=1;r.code[0]=cs;r.code_bytes[0]=32;r.constant_bytes[0]=4;return r;}
static uint8_t *slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long z=ftell(f);CHECK(z>=0);CHECK(!fseek(f,0,SEEK_SET));uint8_t*b=malloc((size_t)z?(size_t)z:1);CHECK(b);CHECK(fread(b,1,(size_t)z,f)==(size_t)z);CHECK(!fclose(f));*n=(size_t)z;return b;}
static void put(const char*p,const void*b,size_t n){FILE*f=fopen(p,"wb");CHECK(f);CHECK(fwrite(b,1,n,f)==n);CHECK(!fclose(f));}
static void compare(const NvnDiscoveryRecipe*a,const NvnDiscoveryRecipe*b){
    CHECK(!memcmp(&a->rec,&b->rec,sizeof a->rec));
    CHECK(!memcmp(a->vkey.attr_kind,b->vkey.attr_kind,32)&&!memcmp(a->vkey.rt_kind,b->vkey.rt_kind,8));CHECK(a->vkey.flip_y==b->vkey.flip_y&&a->vkey.depth_minus_one==b->vkey.depth_minus_one&&a->vkey.flip_x==b->vkey.flip_x&&a->vkey.glsl==b->vkey.glsl&&a->vkey.tex_uint==b->vkey.tex_uint);
    CHECK(!memcmp(a->fkey.attr_kind,b->fkey.attr_kind,32)&&!memcmp(a->fkey.rt_kind,b->fkey.rt_kind,8));CHECK(a->fkey.flip_y==b->fkey.flip_y&&a->fkey.depth_minus_one==b->fkey.depth_minus_one&&a->fkey.flip_x==b->fkey.flip_x&&a->fkey.glsl==b->fkey.glsl&&a->fkey.tex_uint==b->fkey.tex_uint);
    for(unsigned i=0;i<2;i++){CHECK(a->code_bytes[i]==b->code_bytes[i]&&a->constant_bytes[i]==b->constant_bytes[i]);size_t n=(size_t)a->code_bytes[i]+a->constant_bytes[i];CHECK((!n&&!b->code[i])||(n&&b->code[i]&&!memcmp(a->code[i],b->code[i],n)));}
}
static void codec(const char*dir){
    uint8_t hash[32];static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};CHECK(shared_sha256_bytes("abc",3,hash));CHECK(!memcmp(hash,abc,32));
    for(unsigned i=0;i<32;i++)game[i]=(uint8_t)(i*7+3);char path[1024],bad[1024],torn[1024],dest[1024],sentinel[1024];snprintf(path,sizeof path,"%s/discovery.bin",dir);snprintf(bad,sizeof bad,"%s/bad.bin",dir);snprintf(torn,sizeof torn,"%s/torn.bin",dir);snprintf(dest,sizeof dest,"%s/dest",dir);snprintf(sentinel,sizeof sentinel,"%s/keep",dest);
    NvnDiscoveryRecipe g=graphics(false),c=compute();NvnDiscoveryStore s={0};CHECK(nvn_discovery_open(&s,path,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_EMPTY);CHECK(nvn_discovery_count(&s)==0);
    CHECK(nvn_discovery_append(&s,&g)==NVN_DISCOVERY_APPEND_APPENDED);size_t before=s.size;CHECK(nvn_discovery_append(&s,&g)==NVN_DISCOVERY_APPEND_DUPLICATE);CHECK(s.size==before&&s.count==1);CHECK(nvn_discovery_append(&s,&c)==NVN_DISCOVERY_APPEND_APPENDED);CHECK(nvn_discovery_count(&s)==2);CHECK(nvn_discovery_commit(&s));nvn_discovery_close(&s);
    size_t original_n;uint8_t*original=slurp(path,&original_n);
    CHECK(!setenv("LARECOMP_TEST_RENDERER_ID","driver-A",1));CHECK(nvn_discovery_open(&s,path,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_LOADED);CHECK(nvn_discovery_count(&s)==2);struct stat before_commit,after_commit;CHECK(!stat(path,&before_commit));CHECK(nvn_discovery_commit(&s));CHECK(!stat(path,&after_commit));CHECK(before_commit.st_ino==after_commit.st_ino);
    NvnDiscoveryRecipe got={0};CHECK(nvn_discovery_read_at(&s,0,&got));compare(&g,&got);nvn_discovery_recipe_release(&got);CHECK(!setenv("LARECOMP_TEST_RENDERER_ID","driver-B",1));CHECK(nvn_discovery_read_at(&s,1,&got));compare(&c,&got);nvn_discovery_recipe_release(&got);nvn_discovery_close(&s);
    CHECK(nvn_discovery_init_empty(&s,path,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS));CHECK(nvn_discovery_append(&s,&g)==NVN_DISCOVERY_APPEND_APPENDED);before=s.size;
    NvnDiscoveryRecipe invalid=g;invalid.rec.nattrib=17;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before&&s.count==1);invalid=g;invalid.vkey.attr_kind[0]=3;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);invalid=g;invalid.rec.topo=3;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);invalid=g;invalid.rec.samples=3;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);invalid=g;invalid.rec.nstream=17;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);invalid=g;invalid.rec.ncolor=9;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);invalid=g;invalid.code_bytes[0]=UINT32_MAX;invalid.constant_bytes[0]=UINT32_MAX;CHECK(nvn_discovery_append(&s,&invalid)==NVN_DISCOVERY_APPEND_ERROR&&s.size==before);nvn_discovery_close(&s);size_t unchanged_n;uint8_t*unchanged=slurp(path,&unchanged_n);CHECK(unchanged_n==original_n&&!memcmp(unchanged,original,unchanged_n));free(unchanged);
    uint8_t other[32];memcpy(other,game,32);other[0]^=0x80;CHECK(nvn_discovery_open(&s,path,other,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_INCOMPATIBLE);CHECK(!s.initialized);size_t n;uint8_t*b=slurp(path,&n);CHECK(n==original_n&&!memcmp(b,original,n));free(b);
    uint8_t*schema=malloc(original_n);CHECK(schema);memcpy(schema,original,original_n);nvn_disc_put32(schema+8,NVN_DISCOVERY_SCHEMA+1);put(bad,schema,original_n);CHECK(nvn_discovery_open(&s,bad,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_INCOMPATIBLE);b=slurp(bad,&n);CHECK(n==original_n&&!memcmp(b,schema,n));free(b);free(schema);
    b=malloc(original_n);CHECK(b);memcpy(b,original,original_n);CHECK(original_n>NVN_DISCOVERY_HEADER_BYTES+40);b[NVN_DISCOVERY_HEADER_BYTES+8]^=1;put(bad,b,original_n);free(b);CHECK(nvn_discovery_open(&s,bad,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_CORRUPT);b=slurp(bad,&n);CHECK(n==original_n&&b[NVN_DISCOVERY_HEADER_BYTES+8]!=(uint8_t)(original[NVN_DISCOVERY_HEADER_BYTES+8]));free(b);
    /* Recomputed digests do not make malformed enum/count encodings valid. */
    for(unsigned which=0;which<3;which++){b=malloc(original_n);CHECK(b);memcpy(b,original,original_n);size_t rec=NVN_DISCOVERY_HEADER_BYTES,payload_n=nvn_disc_get32(b+rec),payload=rec+4;size_t field=which==0?18:which==1?369:369+40;b[payload+field]=which==0?17:which==1?3:2;CHECK(shared_sha256_bytes(b+payload,payload_n,b+payload+payload_n));put(bad,b,original_n);free(b);CHECK(nvn_discovery_open(&s,bad,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_CORRUPT);}
    /* Valid digests cannot legitimize key/record relationships the live
     * program-key producer never emits (especially fkey.flip_x, omitted by
     * prog_content_hash). Offsets are explicit wire fields, not C layout. */
    for(unsigned which=0;which<3;which++){b=malloc(original_n);CHECK(b);memcpy(b,original,original_n);size_t payload=NVN_DISCOVERY_HEADER_BYTES+4,payload_n=nvn_disc_get32(b+NVN_DISCOVERY_HEADER_BYTES);size_t field=which==0?369+48+42:which==1?369+48+40:17;b[payload+field]=1;CHECK(shared_sha256_bytes(b+payload,payload_n,b+payload+payload_n));put(bad,b,original_n);free(b);CHECK(nvn_discovery_open(&s,bad,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_CORRUPT);}
    CHECK(original_n>8);put(torn,original,original_n-5);CHECK(nvn_discovery_open(&s,torn,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_RECOVERED);CHECK(nvn_discovery_count(&s)==1);CHECK(nvn_discovery_commit(&s));nvn_discovery_close(&s);b=slurp(torn,&n);CHECK(n<original_n&&nvn_disc_get32(b+16)==1);free(b);
    CHECK(nvn_discovery_open(&s,path,game,shared_sha256_bytes,NVN_DISCOVERY_HEADER_BYTES,NVN_DISCOVERY_MAX_RECORDS)==NVN_DISCOVERY_OPEN_LIMIT);
    CHECK(nvn_discovery_init_empty(&s,dest,game,shared_sha256_bytes,NVN_DISCOVERY_HEADER_BYTES+8,4));CHECK(nvn_discovery_append(&s,&g)==NVN_DISCOVERY_APPEND_ERROR&&s.count==0);nvn_discovery_close(&s);
    CHECK(!mkdir(dest,0700));put(sentinel,"keep",4);CHECK(nvn_discovery_init_empty(&s,dest,game,shared_sha256_bytes,NVN_DISCOVERY_MAX_FILE_BYTES,4));CHECK(nvn_discovery_append(&s,&g)==NVN_DISCOVERY_APPEND_APPENDED);CHECK(!nvn_discovery_commit(&s));b=slurp(sentinel,&n);CHECK(n==4&&!memcmp(b,"keep",4));free(b);nvn_discovery_close(&s);
    free(original);printf("{\"assertions\":%u,\"records\":2,\"scope\":\"synthetic\"}\n",checks);
}
int main(int argc,char**argv){CHECK(argc==2);codec(argv[1]);return 0;}
