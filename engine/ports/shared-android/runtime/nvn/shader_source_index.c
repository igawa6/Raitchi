/* Bounded streaming BNSH source-stage indexing. Layout follows the original LAS
 * shader_prep.c reader (MIT, Copyright2026 Peiikai and contributors). No shader
 * code or pipeline state is included here. */
#include "shader_source_index.h"
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define SSI_MAX_STAGES 65536u
#define SSI_MAX_SOURCES 4096u
#define SSI_MAX_SCAN (UINT64_C(1) << 30)
#define SSI_MAX_STAGE (16u << 20)
#define SSI_MAX_CONTAINER (256u << 20)
#define SSI_CHUNK (64u << 10)
#define SSI_SLOTS (SSI_MAX_STAGES * 2u)
#define SSI_FAIL(...) (why && why_size ? (void)snprintf(why,why_size,__VA_ARGS__) : (void)0,false)
typedef struct { NxReadAt read; void *context; uint64_t size; void (*destroy)(void *); } SsiSource;
typedef struct SsiNso SsiNso;
typedef struct { SsiNso *owner; uint32_t file_offset, packed, size; bool compressed; } SsiNsoSegment;
struct SsiNso {int fd;uint8_t *cache;SsiNsoSegment segments[2],*cached;SsiNso *next;};
typedef struct SsiRoot { int fd; struct SsiRoot *next; } SsiRoot;
typedef struct { SsiRoot *root; char *path; int scan_fd; uint64_t size; dev_t dev; ino_t ino; } SsiPath;
typedef struct { ShaderSourceSpan span; uint32_t source; } SsiEntry;
struct ShaderSourceIndex {
    SsiSource *sources; size_t nsources, sources_cap;
    SsiEntry *entries; size_t count, capacity;
    uint32_t *hash_slots, *offset_slots;
    uint64_t scanned;
    SsiRoot *roots;
    SsiNso *nsos;
    uint32_t directory_visits;
};
static uint32_t ssi_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t ssi_u64(const uint8_t *p) { return (uint64_t)ssi_u32(p) | (uint64_t)ssi_u32(p+4)<<32; }
static bool ssi_bounds(uint64_t off, uint64_t size, uint64_t total) { return off<=total && size<=total-off; }
static bool ssi_read(const SsiSource *s, uint64_t off, void *dst, size_t size) {
    return ssi_bounds(off,size,s->size) && s->read(s->context,off,dst,size);
}
static uint64_t ssi_hash_update(uint64_t h, const uint8_t *p, size_t n) {
    for(size_t i=0;i<n;i++) {h^=p[i]; h*=UINT64_C(0x100000001b3);} return h;
}
static uint64_t ssi_mix(uint64_t h) {h^=h>>33;h*=UINT64_C(0xff51afd7ed558ccd);h^=h>>33;return h;}
ShaderSourceIndex *shader_source_index_create(void) { return calloc(1,sizeof(ShaderSourceIndex)); }
void shader_source_index_destroy(ShaderSourceIndex *i) {
    if(!i)return;
    for(size_t k=0;k<i->nsources;k++)if(i->sources[k].destroy)i->sources[k].destroy(i->sources[k].context);
    while(i->roots){SsiRoot*r=i->roots;i->roots=r->next;close(r->fd);free(r);}
    while(i->nsos){SsiNso*n=i->nsos;i->nsos=n->next;free(n->cache);close(n->fd);free(n);}
    free(i->sources);free(i->entries);free(i->hash_slots);free(i->offset_slots);free(i);
}
size_t shader_source_index_count(const ShaderSourceIndex *i) {return i?i->count:0;}
static bool ssi_source(ShaderSourceIndex *i,NxReadAt read,void *ctx,uint64_t size,uint32_t *out) {
    for(size_t k=0;k<i->nsources;k++)if(i->sources[k].read==read&&i->sources[k].context==ctx&&i->sources[k].size==size){*out=(uint32_t)k;return true;}
    if(i->nsources==SSI_MAX_SOURCES)return false;
    if(i->nsources==i->sources_cap){size_t n=i->sources_cap?i->sources_cap*2:8;void*p=realloc(i->sources,n*sizeof*i->sources);if(!p)return false;i->sources=p;i->sources_cap=n;}
    *out=(uint32_t)i->nsources;i->sources[i->nsources++]=(SsiSource){read,ctx,size,NULL};return true;
}
static const SsiEntry *ssi_find(const ShaderSourceIndex *i,uint64_t hash,int stage,uint32_t code,uint32_t constants) {
    if(!i||!i->hash_slots||!hash||stage>5)return NULL;
    size_t at=(size_t)ssi_mix(hash)&(SSI_SLOTS-1);
    while(i->hash_slots[at]){
        const SsiEntry*e=&i->entries[i->hash_slots[at]-1];
        if(e->span.hash==hash&&(stage<0||((e->span.stage_mask&(1u<<stage))&&e->span.const_offset==code&&e->span.const_size==constants)))return e;
        at=(at+1)&(SSI_SLOTS-1);
    }return NULL;
}
static SsiEntry *ssi_find_offset(ShaderSourceIndex*i,uint32_t source,uint64_t off,uint32_t code,uint32_t constants) {
    if(!i->offset_slots)return NULL;
    size_t at=(size_t)ssi_mix(off^((uint64_t)source<<32))&(SSI_SLOTS-1);
    while(i->offset_slots[at]){SsiEntry*e=&i->entries[i->offset_slots[at]-1];if(e->source==source&&e->span.offset==off&&e->span.const_offset==code&&e->span.const_size==constants)return e;at=(at+1)&(SSI_SLOTS-1);}return NULL;
}
static bool ssi_add(ShaderSourceIndex*i,uint32_t source,ShaderSourceSpan span) {
    if(!span.hash)return true;
    if(i->count==SSI_MAX_STAGES)return false;
    if(!i->hash_slots||!i->offset_slots){if(!i->hash_slots)i->hash_slots=calloc(SSI_SLOTS,sizeof*i->hash_slots);if(!i->offset_slots)i->offset_slots=calloc(SSI_SLOTS,sizeof*i->offset_slots);if(!i->hash_slots||!i->offset_slots)return false;}
    if(i->count==i->capacity){size_t n=i->capacity?i->capacity*2:256;void*p=realloc(i->entries,n*sizeof*i->entries);if(!p)return false;i->entries=p;i->capacity=n;}
    size_t at=(size_t)ssi_mix(span.hash)&(SSI_SLOTS-1);
    while(i->hash_slots[at]){
        SsiEntry*e=&i->entries[i->hash_slots[at]-1];
        if(e->span.hash==span.hash&&e->span.const_offset==span.const_offset&&e->span.const_size==span.const_size){e->span.stage_mask|=span.stage_mask;return true;}
        at=(at+1)&(SSI_SLOTS-1);
    }
    uint32_t id=(uint32_t)i->count+1;i->entries[i->count++]=(SsiEntry){span,source};i->hash_slots[at]=id;
    at=(size_t)ssi_mix(span.offset^((uint64_t)source<<32))&(SSI_SLOTS-1);
    while(i->offset_slots[at])at=(at+1)&(SSI_SLOTS-1);i->offset_slots[at]=id;
    return true;
}
/* A malformed BNSH candidate is ignored; read failures and resource ceilings
 * abort the scan. Everything is bounded to this file and this container. */
static bool ssi_bnsh(ShaderSourceIndex*i,uint32_t source,uint64_t b,uint64_t file_end,const volatile int*cancel,uint8_t*scratch) {
    const SsiSource*s=&i->sources[source];uint8_t h[0x40];
    if(!ssi_bounds(b,sizeof h,file_end))return true;
    if(!ssi_read(s,b,h,sizeof h))return false;
    if(memcmp(h,"BNSH\0\0\0\0",8)||h[0xc]!=0xff||h[0xd]!=0xfe)return true;
    uint32_t n=ssi_u32(h+0x1c),first=(uint32_t)h[0x16]|(uint32_t)h[0x17]<<8;
    if(n<0x40||n>SSI_MAX_CONTAINER||!ssi_bounds(b,n,file_end)||!ssi_bounds(first,0x28,n))return true;
    if(!ssi_read(s,b+first,h,0x28))return false;
    if(memcmp(h,"grsc",4))return true;
    uint32_t nvar=ssi_u32(h+0x1c);uint64_t pvar=ssi_u64(h+0x20);
    if(nvar>65536||!ssi_bounds(pvar,(uint64_t)nvar*0x40,n))return true;
    for(uint32_t v=0;v<nvar;v++){
        if(cancel&&*cancel)return false;
        if(!ssi_read(s,b+pvar+(uint64_t)v*0x40,h,0x40))return false;
        uint64_t pbin=ssi_u64(h+0x10);
        if(!pbin||!ssi_bounds(pbin,0x38,n))continue;
        uint8_t bin[0x38];if(!ssi_read(s,b+pbin,bin,sizeof bin))return false;
        for(uint8_t st=0;st<6;st++){
            uint64_t pc=ssi_u64(bin+8+8*st);if(!pc||!ssi_bounds(pc,0x20,n))continue;
            if(!ssi_read(s,b+pc,h,0x20))return false;
            uint64_t ctl=ssi_u64(h+8),code=ssi_u64(h+0x10);uint32_t size=ssi_u32(h+0x18);
            if(!ssi_bounds(ctl,0x750,n)||!ssi_bounds(code,size,n))continue;
            if(!ssi_read(s,b+ctl+0x6fc,h,8))return false;
            uint32_t constants=ssi_u32(h),coff=ssi_u32(h+4);
            if(!coff||coff>size||constants>size-coff||coff>SSI_MAX_STAGE||constants>SSI_MAX_STAGE-coff)continue;
            SsiEntry*old=ssi_find_offset(i,source,b+code,coff,constants);if(old){old->span.stage_mask|=(uint8_t)(1u<<st);continue;}
            uint32_t length=coff+constants;uint64_t hash=UINT64_C(0xcbf29ce484222325);
            for(uint32_t done=0;done<length;){
                if(cancel&&*cancel)return false;
                size_t count=length-done<SSI_CHUNK?length-done:SSI_CHUNK;
                if(!ssi_read(s,b+code+done,scratch,count))return false;
                hash=ssi_hash_update(hash,scratch,count);done+=(uint32_t)count;
            }
            if(!ssi_add(i,source,(ShaderSourceSpan){hash,b+code,length,coff,constants,(uint8_t)(1u<<st)}))return false;
        }
    }return true;
}
bool shader_source_index_scan(ShaderSourceIndex*i,NxReadAt read,void*ctx,uint64_t size,uint64_t offset,uint64_t length,const volatile int*cancel,char*why,size_t why_size) {
    if(!i||!read||!ssi_bounds(offset,length,size)||length>SSI_MAX_SCAN-i->scanned||(cancel&&*cancel))return SSI_FAIL("invalid, oversized or cancelled shader inventory");
    uint32_t source;if(!ssi_source(i,read,ctx,size,&source))return SSI_FAIL("shader source limit or allocation failure");
    uint8_t*chunk=malloc(SSI_CHUNK+7),*scratch=malloc(SSI_CHUNK);if(!chunk||!scratch){free(chunk);free(scratch);return SSI_FAIL("shader inventory allocation failure");}
    bool ok=true;uint64_t end=offset+length;
    for(uint64_t at=0;at<length&&ok;){
        if(cancel&&*cancel){ok=false;break;}
        size_t count=(size_t)(length-at>SSI_CHUNK+7?SSI_CHUNK+7:length-at);
        ok=ssi_read(&i->sources[source],offset+at,chunk,count);if(!ok)break;
        for(size_t k=0;k+8<=count;k+=8)if(!memcmp(chunk+k,"BNSH\0\0\0\0",8)){
            if(!ssi_bnsh(i,source,offset+at+k,end,cancel,scratch)){ok=false;break;}
        }
        uint64_t step=length-at<SSI_CHUNK?length-at:SSI_CHUNK;i->scanned+=step;at+=step;
    }
    free(chunk);free(scratch);if(!ok)return SSI_FAIL("shader inventory read, resource limit or cancellation failure");return true;
}
bool shader_source_index_lookup(const ShaderSourceIndex*i,uint64_t hash,ShaderSourceSpan*out) {const SsiEntry*e=ssi_find(i,hash,-1,0,0);if(!e)return false;if(out)*out=e->span;return true;}
static uint8_t *ssi_blob(const ShaderSourceIndex*i,const SsiEntry*e,ShaderSourceSpan*out) {
    if(!e||e->source>=i->nsources)return NULL;
    uint8_t*p=calloc(1,(size_t)e->span.size+0x100);if(!p)return NULL;
    /* Bound each physical request, even when the stage is several MiB. */
    for(uint32_t at=0;at<e->span.size;){size_t n=e->span.size-at<SSI_CHUNK?e->span.size-at:SSI_CHUNK;if(!ssi_read(&i->sources[e->source],e->span.offset+at,p+at,n)){free(p);return NULL;}at+=(uint32_t)n;}
    if(ssi_hash_update(UINT64_C(0xcbf29ce484222325),p,e->span.size)!=e->span.hash){free(p);return NULL;}if(out)*out=e->span;return p;
}
uint8_t *shader_source_index_read(const ShaderSourceIndex*i,uint64_t hash,ShaderSourceSpan*out) {return ssi_blob(i,ssi_find(i,hash,-1,0,0),out);}
uint8_t *shader_source_index_read_exact(const ShaderSourceIndex*i,uint64_t hash,uint8_t stage,uint32_t code,uint32_t constants,ShaderSourceSpan*out) {return ssi_blob(i,ssi_find(i,hash,stage,code,constants),out);}
static bool ssi_candidate(const char*name) {
    size_t n=strlen(name);return(n>=6&&!strcmp(name+n-6,".bfsha"))||(n>=5&&!strcmp(name+n-5,".ptcl"))||(n>=4&&!strcmp(name+n-4,".arc"));
}
typedef struct {ShaderSourceIndex*i;NxReadAt read;void*reader;uint64_t size;const volatile int*cancel;char*why;size_t why_size;} SsiVisit;
static bool ssi_visit(void*arg,const char*name,uint64_t off,uint64_t size) {
    SsiVisit*v=arg;bool candidate=ssi_candidate(name);
    return !candidate||shader_source_index_scan(v->i,v->read,v->reader,v->size,off,size,v->cancel,v->why,v->why_size);
}
bool shader_source_index_romfs_reader(ShaderSourceIndex*i,NxReadAt read,void*ctx,uint64_t size,const volatile int*cancel,char*why,size_t why_size) {
    if(!i||!read)return SSI_FAIL("invalid shader RomFS source");
    SsiVisit v={i,read,ctx,size,cancel,why,why_size};return nx_romfs_visit_reader(read,ctx,size,ssi_visit,&v,cancel,why,why_size);
}
static bool ssi_native_read(void*ctx,uint64_t off,void*dst,size_t size){return nx_romfs_pread(ctx,dst,size,off)==(ssize_t)size;}
bool shader_source_index_romfs(ShaderSourceIndex*i,NxRomfs*r,const volatile int*cancel,char*why,size_t why_size) {
    return shader_source_index_romfs_reader(i,ssi_native_read,r,nx_romfs_size(r),cancel,why,why_size);
}

static void ssi_path_destroy(void*ctx) {SsiPath*p=ctx;if(p->scan_fd>=0)close(p->scan_fd);free(p->path);free(p);}
static int ssi_path_open(SsiPath*p) {
    char path[4096];size_t n=strlen(p->path);if(n>=sizeof path)return -1;memcpy(path,p->path,n+1);
    int dir=fcntl(p->root->fd,F_DUPFD_CLOEXEC,0);if(dir<0)return -1;
    char*at=path;for(;;){char*slash=strchr(at,'/');if(!slash)break;*slash=0;int next=openat(dir,at,O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);close(dir);if(next<0)return -1;dir=next;at=slash+1;}
    int fd=openat(dir,at,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);close(dir);
    struct stat st;if(fd>=0&&(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size!=p->size||st.st_dev!=p->dev||st.st_ino!=p->ino)){close(fd);fd=-1;}
    return fd;
}
static bool ssi_path_read(void*ctx,uint64_t off,void*dst,size_t size) {
    SsiPath*p=ctx;if(!ssi_bounds(off,size,p->size)||off>INT64_MAX)return false;
    int fd=p->scan_fd>=0?p->scan_fd:ssi_path_open(p);if(fd<0)return false;
    size_t done=0;bool ok=true;while(done<size){ssize_t n=pread(fd,(uint8_t*)dst+done,size-done,(off_t)(off+done));if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=(size_t)n;}
    if(p->scan_fd<0)close(fd);return ok;
}
static bool ssi_dir(ShaderSourceIndex*i,SsiRoot*root,int fd,const char*prefix,unsigned depth,const volatile int*cancel,char*why,size_t why_size) {
    if(depth>=64)return SSI_FAIL("shader directory nesting limit exceeded");
    int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);if(copy<0)return SSI_FAIL("cannot open shader source directory");
    DIR*d=fdopendir(copy);if(!d){close(copy);return SSI_FAIL("cannot enumerate shader source directory");}
    bool ok=true;struct dirent*e;
    while((e=readdir(d))){
        if(cancel&&*cancel){ok=false;break;}
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(++i->directory_visits>262144){ok=false;break;}
        char path[4096];size_t a=strlen(prefix),b=strlen(e->d_name),sep=a?1:0;
        if(b>=sizeof path-a-sep){ok=false;break;}
        memcpy(path,prefix,a);if(sep)path[a]='/';memcpy(path+a+sep,e->d_name,b+1);
        struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=false;break;}
        if(S_ISLNK(st.st_mode))continue;
        if(S_ISDIR(st.st_mode)){
            int sub=openat(fd,e->d_name,O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
            if(sub<0){ok=false;break;}ok=ssi_dir(i,root,sub,path,depth+1,cancel,why,why_size);close(sub);if(!ok)break;
        }else if(S_ISREG(st.st_mode)&&ssi_candidate(path)){
            int file=openat(fd,e->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            struct stat current;if(file<0||fstat(file,&current)||!S_ISREG(current.st_mode)||current.st_size<0||current.st_dev!=st.st_dev||current.st_ino!=st.st_ino){if(file>=0)close(file);ok=false;break;}
            SsiPath*p=calloc(1,sizeof*p);if(!p){close(file);ok=false;break;}
            *p=(SsiPath){root,strdup(path),file,(uint64_t)current.st_size,current.st_dev,current.st_ino};
            uint32_t source;
            if(!p->path||!ssi_source(i,ssi_path_read,p,p->size,&source)){ssi_path_destroy(p);ok=false;break;}
            i->sources[source].destroy=ssi_path_destroy;
            ok=shader_source_index_scan(i,ssi_path_read,p,p->size,0,p->size,cancel,why,why_size);
            close(p->scan_fd);p->scan_fd=-1;if(!ok)break;
        }
    }
    closedir(d);if(!ok)return SSI_FAIL("invalid, cancelled or oversized shader directory inventory");return true;
}
bool shader_source_index_directory(ShaderSourceIndex*i,const char*path,const volatile int*cancel,char*why,size_t why_size) {
    if(!i||!path||!*path||(cancel&&*cancel))return SSI_FAIL("invalid or cancelled shader directory");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return SSI_FAIL("cannot open shader source root");
    SsiRoot*r=malloc(sizeof*r);if(!r){close(fd);return SSI_FAIL("shader directory allocation failure");}
    *r=(SsiRoot){fd,i->roots};i->roots=r;
    return ssi_dir(i,r,fd,"",0,cancel,why,why_size);
}

static bool ssi_fd_read(int fd,uint64_t off,void*dst,size_t size) {
    size_t done=0;if(off>INT64_MAX||size>(uint64_t)INT64_MAX-off)return false;
    while(done<size){size_t count=size-done<SSI_CHUNK?size-done:SSI_CHUNK;ssize_t n=pread(fd,(uint8_t*)dst+done,count,(off_t)(off+done));if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=(size_t)n;}return true;
}
static bool ssi_nso_read(void*ctx,uint64_t off,void*dst,size_t size) {
    SsiNsoSegment*s=ctx;SsiNso*n=s->owner;if(!ssi_bounds(off,size,s->size))return false;
    if(n->cached!=s){
        free(n->cache);n->cache=NULL;n->cached=NULL;
        uint8_t*cache=malloc(s->size);if(!cache)return false;bool ok;
        if(s->compressed){uint8_t*raw=malloc(s->packed);if(!raw){free(cache);return false;}ok=ssi_fd_read(n->fd,s->file_offset,raw,s->packed)&&nx_lz4_decompress(raw,(int)s->packed,cache,(int)s->size)==(int)s->size;free(raw);}
        else ok=ssi_fd_read(n->fd,s->file_offset,cache,s->size);
        if(!ok){free(cache);return false;}n->cache=cache;n->cached=s;
    }
    if(size)memcpy(dst,n->cache+off,size);return true;
}
bool shader_source_index_nso(ShaderSourceIndex*i,const char*path,const volatile int*cancel,char*why,size_t why_size) {
    if(!i||!path||!*path||(cancel&&*cancel))return SSI_FAIL("invalid or cancelled shader NSO source");
    /* main is the one package-verified ExeFS image; limit retained descriptors
     * and decompression cache even if a caller supplies additional NSOs. */
    unsigned count=0;for(SsiNso*n=i->nsos;n;n=n->next)count++;if(count>=4)return SSI_FAIL("NSO source count exceeded");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);uint8_t h[0x100];struct stat st;
    if(fd<0)return SSI_FAIL("cannot open shader NSO source");
    bool valid=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>=0x100&&ssi_fd_read(fd,0,h,sizeof h)&&!memcmp(h,"NSO0",4);
    if(!valid){close(fd);return SSI_FAIL("invalid shader NSO header");}
    SsiNso*n=calloc(1,sizeof*n);if(!n){close(fd);return SSI_FAIL("shader NSO allocation failure");}n->fd=fd;
    uint32_t flags=ssi_u32(h+0xc);
    for(unsigned k=0;k<2;k++){
        unsigned sg=k+1;uint32_t offset=ssi_u32(h+0x10+16*sg),size=ssi_u32(h+0x18+16*sg),packed=ssi_u32(h+0x60+4*sg);bool compressed=(flags&(1u<<sg))!=0;
        if(!compressed)packed=size;
        if(size&&(!packed||size>(64u<<20)||packed>(65u<<20)||offset<sizeof h||!ssi_bounds(offset,packed,(uint64_t)st.st_size))){close(fd);free(n);return SSI_FAIL("NSO shader segment exceeds supported bounds");}
        n->segments[k]=(SsiNsoSegment){n,offset,packed,size,compressed};
    }
    SsiNsoSegment*a=&n->segments[0],*b=&n->segments[1];
    if(a->size&&b->size&&(uint64_t)a->file_offset<(uint64_t)b->file_offset+b->packed&&
       (uint64_t)b->file_offset<(uint64_t)a->file_offset+a->packed){close(fd);free(n);return SSI_FAIL("overlapping NSO shader segments");}
    n->next=i->nsos;i->nsos=n;
    for(unsigned k=0;k<2;k++)if(n->segments[k].size&&!shader_source_index_scan(i,ssi_nso_read,&n->segments[k],n->segments[k].size,0,n->segments[k].size,cancel,why,why_size))return false;
    /* Keep metadata only after inventory; exact profile resolution lazily
     * materializes at most one bounded segment of this NSO. */
    free(n->cache);n->cache=NULL;n->cached=NULL;return true;
}
