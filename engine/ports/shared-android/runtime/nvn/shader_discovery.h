/* Portable bounded shader discovery journal codec. Header-only; wire fields are explicit little-endian. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "shader_prep.h"
#include "maxwell/shader.h"

#define NVN_DISCOVERY_SCHEMA 1u
#define NVN_DISCOVERY_MAX_FILE_BYTES (UINT64_C(256) * 1024 * 1024)
#define NVN_DISCOVERY_MAX_RECORDS 4096u
#define NVN_DISCOVERY_MAX_STAGE_BYTES (16u * 1024u * 1024u)
#define NVN_DISCOVERY_MAX_PATH_BYTES 4096u
#define NVN_DISCOVERY_HEADER_BYTES 88u
#define NVN_DISCOVERY_HEADER_PREFIX_BYTES 56u
#define NVN_DISCOVERY_RECORD_DIGEST_BYTES 32u

typedef struct {
    ShaderRec rec;
    ShaderKey vkey, fkey; /* inputs only; vs_out_mask is derived and must be zero */
    const uint8_t *code[2]; /* code_bytes + constant_bytes exact extent */
    uint32_t code_bytes[2], constant_bytes[2];
} NvnDiscoveryRecipe;
typedef bool (*NvnDiscoverySha256)(const void *bytes,size_t length,uint8_t out[32]);
typedef enum {
    NVN_DISCOVERY_OPEN_ERROR=-1, NVN_DISCOVERY_OPEN_EMPTY=0, NVN_DISCOVERY_OPEN_LOADED=1,
    NVN_DISCOVERY_OPEN_RECOVERED=2, NVN_DISCOVERY_OPEN_INCOMPATIBLE=3,
    NVN_DISCOVERY_OPEN_CORRUPT=4, NVN_DISCOVERY_OPEN_LIMIT=5
} NvnDiscoveryOpenResult;
typedef enum { NVN_DISCOVERY_APPEND_ERROR=-1,NVN_DISCOVERY_APPEND_DUPLICATE=0,NVN_DISCOVERY_APPEND_APPENDED=1 } NvnDiscoveryAppendResult;
typedef struct {
    char *path; uint8_t game_id[32]; NvnDiscoverySha256 sha256;
    uint8_t *bytes; size_t size,capacity,max_bytes; uint32_t count,max_records;
    bool initialized,dirty;
} NvnDiscoveryStore;

static const uint8_t nvn_disc_magic[8]={'N','V','N','D','I','S','C',0};
typedef struct {uint8_t *p;size_t n,cap;bool ok;} NvnDiscWriter;
typedef struct {const uint8_t *p;size_t n,at;bool ok;} NvnDiscReader;
static uint32_t nvn_disc_get32(const uint8_t*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t nvn_disc_get64(const uint8_t*p){return(uint64_t)nvn_disc_get32(p)|(uint64_t)nvn_disc_get32(p+4)<<32;}
static void nvn_disc_put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void nvn_disc_put64(uint8_t*p,uint64_t v){nvn_disc_put32(p,(uint32_t)v);nvn_disc_put32(p+4,(uint32_t)(v>>32));}
static void nvn_disc_wbytes(NvnDiscWriter*w,const void*p,size_t n){if(!w->ok||w->n>w->cap||n>w->cap-w->n){w->ok=false;return;}if(n)memcpy(w->p+w->n,p,n);w->n+=n;}
static void nvn_disc_w8(NvnDiscWriter*w,uint8_t v){nvn_disc_wbytes(w,&v,1);}
static void nvn_disc_w32(NvnDiscWriter*w,uint32_t v){uint8_t b[4];nvn_disc_put32(b,v);nvn_disc_wbytes(w,b,4);}
static void nvn_disc_w64(NvnDiscWriter*w,uint64_t v){uint8_t b[8];nvn_disc_put64(b,v);nvn_disc_wbytes(w,b,8);}
static void nvn_disc_rbytes(NvnDiscReader*r,void*out,size_t n){if(!r->ok||r->at>r->n||n>r->n-r->at){r->ok=false;return;}if(n&&out)memcpy(out,r->p+r->at,n);r->at+=n;}
static uint8_t nvn_disc_r8(NvnDiscReader*r){uint8_t v=0;nvn_disc_rbytes(r,&v,1);return v;}
static uint32_t nvn_disc_r32(NvnDiscReader*r){uint8_t b[4]={0};nvn_disc_rbytes(r,b,4);return nvn_disc_get32(b);}
static uint64_t nvn_disc_r64(NvnDiscReader*r){uint8_t b[8]={0};nvn_disc_rbytes(r,b,8);return nvn_disc_get64(b);}
static bool nvn_disc_sha(NvnDiscoverySha256 fn,const void*p,size_t n,uint8_t out[32]){return fn&&(p||!n)&&fn(p,n,out);}
static uint64_t nvn_disc_codehash(const uint8_t*p,size_t n){uint64_t h=UINT64_C(0xcbf29ce484222325);for(size_t i=0;i<n;i++){h^=p[i];h*=UINT64_C(0x100000001b3);}return h;}
static bool nvn_disc_zero(const void*p,size_t n){const uint8_t*b=p;uint8_t v=0;for(size_t i=0;i<n;i++)v|=b[i];return v==0;}
static bool nvn_disc_key_valid(const ShaderKey*k){if(k->glsl!=1||k->vs_out_mask)return false;for(size_t i=0;i<32;i++)if(k->attr_kind[i]>2)return false;for(size_t i=0;i<8;i++)if(k->rt_kind[i]>2)return false;return true;}
static bool nvn_disc_key_zero(const ShaderKey*k){
    return nvn_disc_zero(k->attr_kind,sizeof k->attr_kind)&&nvn_disc_zero(k->rt_kind,sizeof k->rt_kind)&&
        !k->depth_minus_one&&!k->flip_y&&!k->flip_x&&!k->vs_out_mask&&!k->glsl&&!k->tex_uint;
}
static bool nvn_disc_key_compute(const ShaderKey*k){return nvn_disc_zero(k->attr_kind,sizeof k->attr_kind)&&nvn_disc_zero(k->rt_kind,sizeof k->rt_kind)&&!k->depth_minus_one&&!k->flip_y&&!k->flip_x&&!k->vs_out_mask&&k->glsl==1&&!k->tex_uint;}
static bool nvn_disc_rec_valid(const ShaderRec*r){
    if(r->flags&~(SR_FLIP_Y|SR_DEPTH_MINUS_ONE|SR_FLIP_X)||r->nattrib>16||r->nstream>16||r->ncolor>8||r->topo>2||r->poly_fill>1||r->a2c>1||r->samples>64||(r->samples&&(r->samples&(r->samples-1))))return false;
    if(r->kind=='C')return r->vs&&!r->fs&&!r->flags&&!r->nattrib&&!r->nstream&&!r->ncolor&&!r->depth&&!r->samples&&!r->channel_mask&&!r->blend_on&&r->local[0]&&r->local[1]&&r->local[2];
    return r->kind=='G'&&r->vs&&!r->local[0]&&!r->local[1]&&!r->local[2]&&r->samples;
}
static bool nvn_disc_recipe_valid(const NvnDiscoveryRecipe*r){
    if(!r||!nvn_disc_rec_valid(&r->rec))return false;
    for(size_t i=0;i<2;i++){uint64_t n=(uint64_t)r->code_bytes[i]+r->constant_bytes[i];if(n>NVN_DISCOVERY_MAX_STAGE_BYTES||(!!n!=!!r->code[i]))return false;}
    if(r->rec.kind=='C')return r->code_bytes[0]>0&&!r->code[1]&&!r->code_bytes[1]&&!r->constant_bytes[1]&&nvn_disc_codehash(r->code[0],(size_t)r->code_bytes[0]+r->constant_bytes[0])==r->rec.vs&&nvn_disc_key_compute(&r->vkey)&&nvn_disc_key_zero(&r->fkey);
    if(!nvn_disc_key_valid(&r->vkey)||!nvn_disc_key_valid(&r->fkey)||!nvn_disc_zero(r->vkey.rt_kind,sizeof r->vkey.rt_kind)||!nvn_disc_zero(r->fkey.attr_kind,sizeof r->fkey.attr_kind))return false;
    /* Match the live graphics producer and program-content-key inputs. The
     * fragment key's flip_x is consumed by translation but omitted from the
     * producer's program hash, so it must be coupled to the vertex key. */
    if(r->fkey.flip_x!=r->vkey.flip_x||r->fkey.flip_y!=r->vkey.flip_y||r->fkey.depth_minus_one)return false;
    if(r->vkey.flip_x!=((r->rec.flags&SR_FLIP_X)!=0)||r->vkey.flip_y!=((r->rec.flags&SR_FLIP_Y)!=0)||
       r->vkey.depth_minus_one!=((r->rec.flags&SR_DEPTH_MINUS_ONE)!=0))return false;
    if(r->code_bytes[0]<0x80||nvn_disc_codehash(r->code[0],(size_t)r->code_bytes[0]+r->constant_bytes[0])!=r->rec.vs)return false;
    if(!r->rec.fs)return !r->code[1]&&!r->code_bytes[1]&&!r->constant_bytes[1];
    return r->code_bytes[1]>=0x80&&r->code[1]&&nvn_disc_codehash(r->code[1],(size_t)r->code_bytes[1]+r->constant_bytes[1])==r->rec.fs;
}
/* Wire record fields, in order: ShaderRec semantic fields; two ShaderKeys
 * (arrays, bool flags, dialect, tex_uint; vs_out_mask omitted); lengths; bytes. */
static void nvn_disc_write_rec(NvnDiscWriter*w,const ShaderRec*r){
    nvn_disc_w8(w,(uint8_t)r->kind);nvn_disc_w64(w,r->vs);nvn_disc_w64(w,r->fs);nvn_disc_w8(w,r->flags);nvn_disc_w8(w,r->nattrib);nvn_disc_w8(w,r->nstream);nvn_disc_w8(w,r->ncolor);
    for(size_t i=0;i<16;i++)nvn_disc_w32(w,r->attrib[i]);for(size_t i=0;i<16;i++)for(size_t j=0;j<2;j++)nvn_disc_w32(w,r->stream[i][j]);
    for(size_t i=0;i<8;i++)nvn_disc_w32(w,r->color[i]);for(size_t i=0;i<8;i++)nvn_disc_w32(w,r->cview[i]);
    nvn_disc_w32(w,r->depth);nvn_disc_w32(w,r->samples);nvn_disc_w32(w,r->channel_mask);nvn_disc_w8(w,r->blend_on);for(size_t i=0;i<8;i++)nvn_disc_wbytes(w,r->blend[i],8);
    nvn_disc_w8(w,r->a2c);nvn_disc_w8(w,r->topo);nvn_disc_w8(w,r->poly_fill);for(size_t i=0;i<3;i++)nvn_disc_w32(w,r->local[i]);
}
static void nvn_disc_read_rec(NvnDiscReader*d,ShaderRec*r){
    memset(r,0,sizeof *r);r->kind=(char)nvn_disc_r8(d);r->vs=nvn_disc_r64(d);r->fs=nvn_disc_r64(d);r->flags=nvn_disc_r8(d);r->nattrib=nvn_disc_r8(d);r->nstream=nvn_disc_r8(d);r->ncolor=nvn_disc_r8(d);
    for(size_t i=0;i<16;i++)r->attrib[i]=nvn_disc_r32(d);for(size_t i=0;i<16;i++)for(size_t j=0;j<2;j++)r->stream[i][j]=nvn_disc_r32(d);
    for(size_t i=0;i<8;i++)r->color[i]=nvn_disc_r32(d);for(size_t i=0;i<8;i++)r->cview[i]=nvn_disc_r32(d);
    r->depth=nvn_disc_r32(d);r->samples=nvn_disc_r32(d);r->channel_mask=nvn_disc_r32(d);r->blend_on=nvn_disc_r8(d);for(size_t i=0;i<8;i++)nvn_disc_rbytes(d,r->blend[i],8);
    r->a2c=nvn_disc_r8(d);r->topo=nvn_disc_r8(d);r->poly_fill=nvn_disc_r8(d);for(size_t i=0;i<3;i++)r->local[i]=nvn_disc_r32(d);
}
static void nvn_disc_write_key(NvnDiscWriter*w,const ShaderKey*k){nvn_disc_wbytes(w,k->attr_kind,32);nvn_disc_wbytes(w,k->rt_kind,8);nvn_disc_w8(w,k->depth_minus_one?1:0);nvn_disc_w8(w,k->flip_y?1:0);nvn_disc_w8(w,k->flip_x?1:0);nvn_disc_w8(w,k->glsl);nvn_disc_w32(w,k->tex_uint);}
static void nvn_disc_read_key(NvnDiscReader*d,ShaderKey*k){memset(k,0,sizeof *k);nvn_disc_rbytes(d,k->attr_kind,32);nvn_disc_rbytes(d,k->rt_kind,8);uint8_t depth=nvn_disc_r8(d),y=nvn_disc_r8(d),x=nvn_disc_r8(d);if(depth>1||y>1||x>1)d->ok=false;k->depth_minus_one=depth!=0;k->flip_y=y!=0;k->flip_x=x!=0;k->glsl=nvn_disc_r8(d);k->tex_uint=nvn_disc_r32(d);}
static uint64_t nvn_disc_fixed_payload_size(void){return 1+16+4+64+128+64+12+1+64+3+12+2*(32+8+4+4)+16;}
static bool nvn_disc_payload_make(const NvnDiscoveryRecipe*r,NvnDiscoverySha256 sha,uint8_t**out,size_t*out_n,uint8_t digest[32]){
    if(!nvn_disc_recipe_valid(r))return false;uint64_t n=nvn_disc_fixed_payload_size()+(uint64_t)r->code_bytes[0]+r->constant_bytes[0]+r->code_bytes[1]+r->constant_bytes[1];if(n>SIZE_MAX||n>NVN_DISCOVERY_MAX_STAGE_BYTES*2u+1024u)return false;
    uint8_t*p=malloc((size_t)n);if(!p)return false;NvnDiscWriter w={p,0,(size_t)n,true};nvn_disc_write_rec(&w,&r->rec);nvn_disc_write_key(&w,&r->vkey);nvn_disc_write_key(&w,&r->fkey);
    for(size_t i=0;i<2;i++){nvn_disc_w32(&w,r->code_bytes[i]);nvn_disc_w32(&w,r->constant_bytes[i]);}
    for(size_t i=0;i<2;i++)nvn_disc_wbytes(&w,r->code[i],(size_t)r->code_bytes[i]+r->constant_bytes[i]);
    if(!w.ok||w.n!=n||!nvn_disc_sha(sha,p,w.n,digest)){free(p);return false;}*out=p;*out_n=w.n;return true;
}
static void nvn_discovery_recipe_release(NvnDiscoveryRecipe*r){if(r){free((void*)r->code[0]);free((void*)r->code[1]);memset(r,0,sizeof *r);}}
static bool nvn_disc_payload_parse(const uint8_t*p,size_t n,NvnDiscoverySha256 sha,const uint8_t digest[32],NvnDiscoveryRecipe*out){
    uint8_t got[32];if(!out||!nvn_disc_sha(sha,p,n,got)||memcmp(got,digest,32))return false;memset(out,0,sizeof *out);NvnDiscReader d={p,n,0,true};nvn_disc_read_rec(&d,&out->rec);nvn_disc_read_key(&d,&out->vkey);nvn_disc_read_key(&d,&out->fkey);
    for(size_t i=0;i<2;i++){out->code_bytes[i]=nvn_disc_r32(&d);out->constant_bytes[i]=nvn_disc_r32(&d);}
    for(size_t i=0;i<2;i++){uint64_t nbytes=(uint64_t)out->code_bytes[i]+out->constant_bytes[i];if(nbytes>NVN_DISCOVERY_MAX_STAGE_BYTES||d.at>d.n||nbytes>d.n-d.at){d.ok=false;break;}if(nbytes){uint8_t*b=malloc((size_t)nbytes);if(!b){d.ok=false;break;}nvn_disc_rbytes(&d,b,(size_t)nbytes);out->code[i]=b;}}
    if(!d.ok||d.at!=d.n||!nvn_disc_recipe_valid(out)){nvn_discovery_recipe_release(out);return false;}return true;
}
static bool nvn_disc_header_make(uint8_t*h,const uint8_t game[32],uint32_t count,NvnDiscoverySha256 sha){memset(h,0,NVN_DISCOVERY_HEADER_BYTES);memcpy(h,nvn_disc_magic,8);nvn_disc_put32(h+8,NVN_DISCOVERY_SCHEMA);nvn_disc_put32(h+12,NVN_DISCOVERY_HEADER_BYTES);nvn_disc_put32(h+16,count);memcpy(h+24,game,32);return nvn_disc_sha(sha,h,NVN_DISCOVERY_HEADER_PREFIX_BYTES,h+56);}
static bool nvn_disc_limits(size_t bytes,uint32_t records){return bytes>=NVN_DISCOVERY_HEADER_BYTES&&bytes<=NVN_DISCOVERY_MAX_FILE_BYTES&&records>0&&records<=NVN_DISCOVERY_MAX_RECORDS;}
static bool nvn_disc_header_valid(const uint8_t*h,const uint8_t game[32],NvnDiscoverySha256 sha,uint32_t maxrec){uint8_t d[32];return !memcmp(h,nvn_disc_magic,8)&&nvn_disc_get32(h+8)==NVN_DISCOVERY_SCHEMA&&nvn_disc_get32(h+12)==NVN_DISCOVERY_HEADER_BYTES&&nvn_disc_get32(h+20)==0&&nvn_disc_get32(h+16)<=maxrec&&!memcmp(h+24,game,32)&&nvn_disc_sha(sha,h,NVN_DISCOVERY_HEADER_PREFIX_BYTES,d)&&!memcmp(d,h+56,32);}
static void nvn_discovery_close(NvnDiscoveryStore*s){if(s){free(s->path);free(s->bytes);memset(s,0,sizeof *s);}}
static bool nvn_discovery_init_empty(NvnDiscoveryStore*s,const char*path,const uint8_t game[32],NvnDiscoverySha256 sha,size_t maxbytes,uint32_t maxrec){
    if(!s||!path||!game||!sha||!nvn_disc_limits(maxbytes,maxrec)||!*path||strlen(path)>=NVN_DISCOVERY_MAX_PATH_BYTES)return false;memset(s,0,sizeof *s);s->path=strdup(path);s->bytes=malloc(NVN_DISCOVERY_HEADER_BYTES);if(!s->path||!s->bytes){nvn_discovery_close(s);return false;}
    s->sha256=sha;memcpy(s->game_id,game,32);s->max_bytes=maxbytes;s->max_records=maxrec;s->size=s->capacity=NVN_DISCOVERY_HEADER_BYTES;s->initialized=true;s->dirty=true;if(!nvn_disc_header_make(s->bytes,s->game_id,0,s->sha256)){nvn_discovery_close(s);return false;}return true;
}
static NvnDiscoveryOpenResult nvn_discovery_open(NvnDiscoveryStore*s,const char*path,const uint8_t game[32],NvnDiscoverySha256 sha,size_t maxbytes,uint32_t maxrec){
    if(!s||!path||!game||!sha||!nvn_disc_limits(maxbytes,maxrec)||!*path||strlen(path)>=NVN_DISCOVERY_MAX_PATH_BYTES)return NVN_DISCOVERY_OPEN_ERROR;memset(s,0,sizeof *s);struct stat st;
    if(stat(path,&st)){if(errno!=ENOENT)return NVN_DISCOVERY_OPEN_ERROR;return nvn_discovery_init_empty(s,path,game,sha,maxbytes,maxrec)?NVN_DISCOVERY_OPEN_EMPTY:NVN_DISCOVERY_OPEN_ERROR;}
    if(st.st_size<0||(uint64_t)st.st_size>maxbytes)return NVN_DISCOVERY_OPEN_LIMIT;
    FILE*f=fopen(path,"rb");if(!f)return NVN_DISCOVERY_OPEN_ERROR;uint8_t h[NVN_DISCOVERY_HEADER_BYTES];size_t got=fread(h,1,sizeof h,f);fclose(f);if(got<sizeof h)return NVN_DISCOVERY_OPEN_CORRUPT;
    if(memcmp(h,nvn_disc_magic,8)||nvn_disc_get32(h+12)!=NVN_DISCOVERY_HEADER_BYTES)return NVN_DISCOVERY_OPEN_CORRUPT;
    if(nvn_disc_get32(h+8)!=NVN_DISCOVERY_SCHEMA||memcmp(h+24,game,32))return NVN_DISCOVERY_OPEN_INCOMPATIBLE;
    if(nvn_disc_get32(h+16)>maxrec)return NVN_DISCOVERY_OPEN_LIMIT;
    if(!nvn_disc_header_valid(h,game,sha,maxrec))return NVN_DISCOVERY_OPEN_CORRUPT;
    size_t n=(size_t)st.st_size;uint8_t*data=malloc(n?n:1);if(!data)return NVN_DISCOVERY_OPEN_ERROR;f=fopen(path,"rb");if(!f){free(data);return NVN_DISCOVERY_OPEN_ERROR;}bool readok=fread(data,1,n,f)==n&&!ferror(f);if(fclose(f))readok=false;if(!readok){free(data);return NVN_DISCOVERY_OPEN_ERROR;}if(memcmp(data,h,NVN_DISCOVERY_HEADER_BYTES)){free(data);return NVN_DISCOVERY_OPEN_CORRUPT;}
    uint32_t declared=nvn_disc_get32(data+16),count=0;size_t at=NVN_DISCOVERY_HEADER_BYTES;bool recovered=false;
    while(count<declared){size_t remain=n-at;if(remain<4){recovered=true;break;}uint32_t pn=nvn_disc_get32(data+at);if(pn>NVN_DISCOVERY_MAX_STAGE_BYTES*2u+1024u){free(data);return NVN_DISCOVERY_OPEN_CORRUPT;}if((uint64_t)pn+4+32>remain){recovered=true;break;}
        const uint8_t*p=data+at+4;uint8_t digest[32];if(!nvn_disc_sha(sha,p,pn,digest)||memcmp(digest,p+pn,32)){free(data);return NVN_DISCOVERY_OPEN_CORRUPT;}NvnDiscoveryRecipe tmp;if(!nvn_disc_payload_parse(p,pn,sha,digest,&tmp)){free(data);return NVN_DISCOVERY_OPEN_CORRUPT;}nvn_discovery_recipe_release(&tmp);at+=(size_t)pn+36;count++;}
    if((!recovered&&(count!=declared||at!=n))||(recovered&&count>=declared)){free(data);return NVN_DISCOVERY_OPEN_CORRUPT;}
    NvnDiscoveryStore parsed={0};if(!nvn_discovery_init_empty(&parsed,path,game,sha,maxbytes,maxrec)){free(data);return NVN_DISCOVERY_OPEN_ERROR;}free(parsed.bytes);parsed.bytes=data;parsed.size=at;parsed.capacity=n;parsed.count=count;parsed.dirty=recovered;
    if(recovered&&!nvn_disc_header_make(parsed.bytes,parsed.game_id,count,parsed.sha256)){nvn_discovery_close(&parsed);return NVN_DISCOVERY_OPEN_ERROR;}*s=parsed;return recovered?NVN_DISCOVERY_OPEN_RECOVERED:NVN_DISCOVERY_OPEN_LOADED;
}
static size_t nvn_discovery_count(const NvnDiscoveryStore*s){return s&&s->initialized?s->count:0;}
static bool nvn_discovery_read_at(NvnDiscoveryStore*s,size_t index,NvnDiscoveryRecipe*out){if(!s||!s->initialized||!out||index>=s->count)return false;size_t at=NVN_DISCOVERY_HEADER_BYTES;for(size_t i=0;i<index;i++)at+=(size_t)nvn_disc_get32(s->bytes+at)+36;uint32_t n=nvn_disc_get32(s->bytes+at);const uint8_t*p=s->bytes+at+4;uint8_t h[32];return nvn_disc_sha(s->sha256,p,n,h)&&!memcmp(h,p+n,32)&&nvn_disc_payload_parse(p,n,s->sha256,h,out);}
static NvnDiscoveryAppendResult nvn_discovery_append(NvnDiscoveryStore*s,const NvnDiscoveryRecipe*r){
    if(!s||!s->initialized||s->count>=s->max_records||!nvn_disc_recipe_valid(r))return NVN_DISCOVERY_APPEND_ERROR;uint8_t*p=NULL,d[32];size_t n=0;if(!nvn_disc_payload_make(r,s->sha256,&p,&n,d))return NVN_DISCOVERY_APPEND_ERROR;
    size_t at=NVN_DISCOVERY_HEADER_BYTES;for(uint32_t i=0;i<s->count;i++){uint32_t oldn=nvn_disc_get32(s->bytes+at);const uint8_t*old=s->bytes+at+4;if(oldn==n){uint8_t h[32];if(!nvn_disc_sha(s->sha256,old,oldn,h)||memcmp(h,old+oldn,32)){free(p);return NVN_DISCOVERY_APPEND_ERROR;}if(!memcmp(h,d,32)){free(p);return NVN_DISCOVERY_APPEND_DUPLICATE;}}at+=(size_t)oldn+36;}
    uint64_t total=(uint64_t)s->size+4+n+32;if(total>s->max_bytes||total>NVN_DISCOVERY_MAX_FILE_BYTES){free(p);return NVN_DISCOVERY_APPEND_ERROR;}uint8_t header[NVN_DISCOVERY_HEADER_BYTES];if(!nvn_disc_header_make(header,s->game_id,s->count+1,s->sha256)){free(p);return NVN_DISCOVERY_APPEND_ERROR;}if(total>s->capacity){size_t cap=s->capacity?s->capacity:NVN_DISCOVERY_HEADER_BYTES;while(cap<total){size_t next=cap>SIZE_MAX/2?(size_t)total:cap*2;if(next>s->max_bytes)next=s->max_bytes;if(next<=cap){free(p);return NVN_DISCOVERY_APPEND_ERROR;}cap=next;}uint8_t*b=realloc(s->bytes,cap);if(!b){free(p);return NVN_DISCOVERY_APPEND_ERROR;}s->bytes=b;s->capacity=cap;}
    at=s->size;nvn_disc_put32(s->bytes+at,(uint32_t)n);memcpy(s->bytes+at+4,p,n);memcpy(s->bytes+at+4+n,d,32);memcpy(s->bytes,header,sizeof header);free(p);s->size=(size_t)total;s->count++;s->dirty=true;return NVN_DISCOVERY_APPEND_APPENDED;
}
static bool nvn_discovery_commit(NvnDiscoveryStore*s){
    if(!s||!s->initialized||!s->path||!s->bytes||s->size>s->max_bytes)return false;if(!s->dirty)return true;size_t pn=strlen(s->path);if(pn+12>=NVN_DISCOVERY_MAX_PATH_BYTES)return false;char*tmp=malloc(pn+12);if(!tmp)return false;memcpy(tmp,s->path,pn);memcpy(tmp+pn,".tmp.XXXXXX",12);int fd=mkstemp(tmp);if(fd<0){free(tmp);return false;}
    size_t at=0;bool ok=true;while(at<s->size){ssize_t n=write(fd,s->bytes+at,s->size-at);if(n<0){if(errno==EINTR)continue;ok=false;break;}if(!n){ok=false;break;}at+=(size_t)n;}if(ok&&fsync(fd))ok=false;if(close(fd))ok=false;if(ok&&rename(tmp,s->path))ok=false;if(!ok)unlink(tmp);else s->dirty=false;free(tmp);return ok;
}
