/* Synthetic RomFS/BNSH only; never opens a game. */
#define _GNU_SOURCE
#include "../ports/shared-android/runtime/nxpkg.c"
#include "../ports/shared-android/runtime/nvn/shader_source_index.c"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void p32(void*p,uint32_t x){uint8_t*b=p;for(int i=0;i<4;i++)b[i]=(uint8_t)(x>>(i*8));}
static void p64(void*p,uint64_t x){p32(p,(uint32_t)x);p32((uint8_t*)p+4,(uint32_t)(x>>32));}
typedef struct{uint8_t*bytes;uint64_t size,reads,maxread;volatile int*cancel;unsigned until_cancel;} Image;
static bool image_read(void*ctx,uint64_t off,void*dst,size_t n){Image*i=ctx;CHECK(off<=i->size&&n<=i->size-off);i->reads++;if(n>i->maxread)i->maxread=n;if(i->cancel&&i->reads>=i->until_cancel)*i->cancel=1;memcpy(dst,i->bytes+off,n);return true;}
static void dirent_make(uint8_t*p,uint32_t parent,uint32_t sibling,uint32_t child,uint32_t file,const char*name){p32(p,parent);p32(p+4,sibling);p32(p+8,child);p32(p+12,file);p32(p+16,UINT32_MAX);p32(p+20,(uint32_t)strlen(name));memcpy(p+24,name,strlen(name));}
static void file_make(uint8_t*p,uint32_t parent,uint32_t sibling,uint64_t off,uint64_t size,const char*name){p32(p,parent);p32(p+4,sibling);p64(p+8,off);p64(p+16,size);p32(p+24,UINT32_MAX);p32(p+28,(uint32_t)strlen(name));memcpy(p+32,name,strlen(name));}
static const uint64_t RF_DATA=0x200;
static Image image_make(void){Image i={calloc(1,0x22000),0x22000,0,0,NULL,0};CHECK(i.bytes);p64(i.bytes,0x50);p64(i.bytes+8,0x50);p64(i.bytes+16,4);p64(i.bytes+24,0x54);p64(i.bytes+32,52);p64(i.bytes+40,0x88);p64(i.bytes+48,4);p64(i.bytes+56,0x8c);p64(i.bytes+64,84);p64(i.bytes+72,RF_DATA);dirent_make(i.bytes+0x54,0,UINT32_MAX,24,UINT32_MAX,"");dirent_make(i.bytes+0x54+24,0,UINT32_MAX,UINT32_MAX,0,"shd");file_make(i.bytes+0x8c,24,44,0,0x20000,"test.bfsha");file_make(i.bytes+0x8c+44,24,UINT32_MAX,0x20000,16,"no.txt");return i;}
static uint64_t bnsh_make(uint8_t*p){memcpy(p,"BNSH\0\0\0\0",8);p[0xc]=0xff;p[0xd]=0xfe;p[0x16]=0x40;p32(p+0x1c,0x1000);memcpy(p+0x40,"grsc",4);p32(p+0x40+0x1c,2);p64(p+0x40+0x20,0x80);p64(p+0x80+0x10,0x100);p64(p+0xc0+0x10,0x100);p64(p+0x100+8,0x140);p64(p+0x100+8+4*8,0x140);p64(p+0x140+8,0x180);p64(p+0x140+0x10,0x900);p32(p+0x140+0x18,0x90);p32(p+0x180+0x6fc,0x10);p32(p+0x180+0x700,0x80);for(unsigned k=0;k<0x90;k++)p[0x900+k]=(uint8_t)(k*7+3);return ssi_hash_update(UINT64_C(0xcbf29ce484222325),p+0x900,0x90);}
typedef struct{unsigned n;bool fail;volatile int*cancel;char paths[2][256];uint64_t off[2],size[2];} Visits;
static bool visitor(void*ctx,const char*path,uint64_t off,uint64_t n){Visits*v=ctx;if(v->n<2){snprintf(v->paths[v->n],256,"%s",path);v->off[v->n]=off;v->size[v->n]=n;}v->n++;if(v->cancel)*v->cancel=1;return !v->fail;}
static void traversal(void){
 Image i=image_make();Visits v={0};char why[256];CHECK(nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(v.n==2&&!strcmp(v.paths[0],"shd/test.bfsha")&&!strcmp(v.paths[1],"shd/no.txt")&&v.off[0]==RF_DATA&&v.size[0]==0x20000);CHECK(i.reads==3);free(i.bytes);
 for(unsigned mode=0;mode<13;mode++){
  i=image_make();v=(Visits){0};
  switch(mode){case 0:p64(i.bytes+24,UINT64_MAX);break;case 1:p64(i.bytes+32,64u<<20);break;case 2:p32(i.bytes+0x54+24+4,24);break;case 3:p32(i.bytes+0x8c+4,0);break;case 4:p32(i.bytes+0x8c,0);break;case 5:p32(i.bytes+0x8c+28,UINT32_MAX);break;case 6:i.bytes[0x8c+32]='/';break;case 7:p64(i.bytes+0x8c+8,UINT64_MAX);break;case 8:p64(i.bytes+0x8c+16,UINT64_MAX);break;case 9:p32(i.bytes+0x54+8,25);break;case 10:p32(i.bytes+0x54+8,UINT32_MAX);break;case 11:p64(i.bytes+56,0x54);break;case 12:p32(i.bytes+0x54+24+20,4);break;}
  CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);free(i.bytes);
 }
 i=image_make();volatile int cancel=1;v=(Visits){0};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,&cancel,why,sizeof why)&&!v.n);cancel=0;v.cancel=&cancel;CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,&cancel,why,sizeof why)&&v.n==1);cancel=0;v=(Visits){.fail=true};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,&cancel,why,sizeof why)&&v.n==1);free(i.bytes);
}
/* Real RomFS images may store metadata after their payloads (data base0x200).
 * Keep the same bounded tree and require overlap rejection before callbacks. */
static void trailing_metadata(void){
 Image i=image_make();char why[256];Visits v={0};
 const uint64_t offsets[4]={0x21000,0x21004,0x21038,0x2103c};
 const uint64_t sizes[4]={4,52,4,84};
 memcpy(i.bytes+offsets[1],i.bytes+0x54,52);memcpy(i.bytes+offsets[3],i.bytes+0x8c,84);
 for(unsigned n=0;n<4;n++){p64(i.bytes+8+16*n,offsets[n]);p64(i.bytes+16+16*n,sizes[n]);}
 CHECK(nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(v.n==2&&v.off[0]==RF_DATA);
 /* Every metadata table is excluded from a nonempty file's payload extent. */
 for(unsigned n=0;n<4;n++){
  p64(i.bytes+offsets[3]+8,offsets[n]-RF_DATA);p64(i.bytes+offsets[3]+16,1);v=(Visits){0};
  CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);
 }
 p64(i.bytes+offsets[3]+8,0);p64(i.bytes+offsets[3]+16,0x20000);
 /* An invalid second file must suppress callbacks for the valid first one. */
 p64(i.bytes+offsets[3]+44+8,offsets[1]-RF_DATA);v=(Visits){0};
 CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);
 p64(i.bytes+offsets[3]+44+8,0x20000);
 /* Empty tables and files occupy no bytes, but positions remain bounded. */
 p64(i.bytes+8,i.size);p64(i.bytes+16,0);v=(Visits){0};
 CHECK(nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(v.n==2);
 p64(i.bytes+offsets[3]+8,offsets[1]-RF_DATA);p64(i.bytes+offsets[3]+16,0);v=(Visits){0};
 CHECK(nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(v.n==2);
 p64(i.bytes+72,0x4f);v=(Visits){0};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);p64(i.bytes+72,RF_DATA);
 p64(i.bytes+8,i.size+1);v=(Visits){0};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);
 p64(i.bytes+8,0x4f);v=(Visits){0};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);
 p64(i.bytes+8,offsets[0]);p64(i.bytes+16,sizes[0]);p64(i.bytes+offsets[3]+8,0);p64(i.bytes+offsets[3]+16,0x20000);
 p64(i.bytes+8,i.size-1);v=(Visits){0};CHECK(!nx_romfs_visit_reader(image_read,&i,i.size,visitor,&v,NULL,why,sizeof why));CHECK(!v.n);
 free(i.bytes);
}
typedef struct{Image*image;uint64_t base;} Sparse;
static bool sparse_read(void*ctx,uint64_t off,void*dst,size_t size){Sparse*s=ctx;CHECK(off>=s->base);return image_read(s->image,off-s->base,dst,size);}
static void large_offsets(void){
 char why[256];Image i=image_make();uint64_t base=(UINT64_C(1)<<33)+0x88;uint64_t hash=bnsh_make(i.bytes);Sparse sparse={&i,base};ShaderSourceIndex*idx=shader_source_index_create();CHECK(shader_source_index_scan(idx,sparse_read,&sparse,base+i.size,base,0x1000,NULL,why,sizeof why));ShaderSourceSpan span;CHECK(shader_source_index_lookup(idx,hash,&span)&&span.offset==base+0x900);uint8_t*p=shader_source_index_read_exact(idx,hash,4,0x80,0x10,NULL);CHECK(p&&!memcmp(p,i.bytes+0x900,0x90));free(p);shader_source_index_destroy(idx);free(i.bytes);
 i=image_make();p64(i.bytes+72,base);Visits v={0};CHECK(nx_romfs_visit_reader(image_read,&i,base+0x22000,visitor,&v,NULL,why,sizeof why));CHECK(v.n==2&&v.off[0]==base&&v.off[1]==base+0x20000);free(i.bytes);
}
static void scanner(void){
 Image i=image_make();uint64_t b=RF_DATA+0xfff8,hash=bnsh_make(i.bytes+b);char why[256];ShaderSourceIndex*idx=shader_source_index_create();CHECK(idx);CHECK(shader_source_index_romfs_reader(idx,image_read,&i,i.size,NULL,why,sizeof why));CHECK(shader_source_index_count(idx)==1);CHECK(i.maxread<=SSI_CHUNK+7);ShaderSourceSpan span;CHECK(shader_source_index_lookup(idx,hash,&span));CHECK(span.offset==b+0x900&&span.size==0x90&&span.const_offset==0x80&&span.const_size==0x10&&span.stage_mask==((1<<0)|(1<<4)));uint8_t*p=shader_source_index_read_exact(idx,hash,4,0x80,0x10,&span);CHECK(p&&!memcmp(p,i.bytes+b+0x900,0x90));for(unsigned k=0;k<0x100;k++)CHECK(!p[0x90+k]);free(p);CHECK(!shader_source_index_read_exact(idx,hash,5,0x80,0x10,NULL));CHECK(!shader_source_index_read_exact(idx,hash,4,0x70,0x20,NULL));i.bytes[b+0x900]^=1;CHECK(!shader_source_index_read(idx,hash,NULL));i.bytes[b+0x900]^=1;CHECK(shader_source_index_scan(idx,image_read,&i,i.size,RF_DATA,0x20000,NULL,why,sizeof why));CHECK(shader_source_index_count(idx)==1);shader_source_index_destroy(idx);
 for(unsigned mode=0;mode<10;mode++){
  Image bad=image_make();bnsh_make(bad.bytes+b);
  switch(mode){case 0:bad.bytes[b+0xc]=0;break;case 1:p32(bad.bytes+b+0x1c,UINT32_MAX);break;case 2:p64(bad.bytes+b+0x40+0x20,UINT64_MAX);break;case 3:p64(bad.bytes+b+0x80+0x10,UINT64_MAX);p64(bad.bytes+b+0xc0+0x10,UINT64_MAX);break;case 4:p64(bad.bytes+b+0x100+8,UINT64_MAX);p64(bad.bytes+b+0x100+8+32,UINT64_MAX);break;case 5:p64(bad.bytes+b+0x140+8,UINT64_MAX);break;case 6:p64(bad.bytes+b+0x140+0x10,UINT64_MAX);break;case 7:p32(bad.bytes+b+0x180+0x6fc,UINT32_MAX);break;case 8:p32(bad.bytes+b+0x180+0x700,0);break;case 9:p32(bad.bytes+b+0x140+0x18,0x70);break;}
  idx=shader_source_index_create();CHECK(shader_source_index_romfs_reader(idx,image_read,&bad,bad.size,NULL,why,sizeof why));CHECK(!shader_source_index_count(idx));shader_source_index_destroy(idx);free(bad.bytes);
 }
 volatile int cancel=1;idx=shader_source_index_create();CHECK(!shader_source_index_romfs_reader(idx,image_read,&i,i.size,&cancel,why,sizeof why));CHECK(!shader_source_index_count(idx));shader_source_index_destroy(idx);cancel=0;i.cancel=&cancel;i.until_cancel=(unsigned)i.reads+6;idx=shader_source_index_create();CHECK(!shader_source_index_romfs_reader(idx,image_read,&i,i.size,&cancel,why,sizeof why));shader_source_index_destroy(idx);
 idx=shader_source_index_create();CHECK(!shader_source_index_scan(idx,image_read,&i,i.size,UINT64_MAX,1,NULL,why,sizeof why));CHECK(!shader_source_index_scan(idx,image_read,&i,UINT64_MAX,0,SSI_MAX_SCAN+1,NULL,why,sizeof why));shader_source_index_destroy(idx);free(i.bytes);
}
static void directory(void){
 char root[]="/tmp/raitchi-source-index-XXXXXX",folder[512],file[512],link[512],why[256];CHECK(mkdtemp(root));snprintf(folder,sizeof folder,"%s/shd",root);CHECK(!mkdir(folder,0700));snprintf(file,sizeof file,"%s/shd/test.bfsha",root);uint8_t*bytes=calloc(1,0x1000);CHECK(bytes);uint64_t hash=bnsh_make(bytes);FILE*f=fopen(file,"wb");CHECK(f);CHECK(fwrite(bytes,1,0x1000,f)==0x1000);CHECK(!fclose(f));snprintf(link,sizeof link,"%s/loop",root);CHECK(!symlink(root,link));
 ShaderSourceIndex*i=shader_source_index_create();CHECK(shader_source_index_directory(i,root,NULL,why,sizeof why));CHECK(shader_source_index_count(i)==1);uint8_t*p=shader_source_index_read_exact(i,hash,0,0x80,0x10,NULL);CHECK(p&&!memcmp(p,bytes+0x900,0x90));free(p);
 /* Existing contexts refuse a replacement symlink or a replaced inode. */
 CHECK(!unlink(file));CHECK(!symlink("/dev/zero",file));CHECK(!shader_source_index_read(i,hash,NULL));CHECK(!unlink(file));CHECK(!unlink(link));CHECK(!rmdir(folder));CHECK(!rmdir(root));shader_source_index_destroy(i);free(bytes);
}
static size_t lz4_literal(uint8_t*out,const uint8_t*src,size_t n){CHECK(n>=15);size_t at=0;out[at++]=0xf0;size_t extra=n-15;while(extra>=255){out[at++]=255;extra-=255;}out[at++]=(uint8_t)extra;memcpy(out+at,src,n);return at+n;}
static void nso(void){
 char path[]="/tmp/raitchi-source-nso-XXXXXX",why[256];int fd=mkstemp(path);CHECK(fd>=0);uint8_t*raw=calloc(1,0x4000),*stage=calloc(1,0x1000);CHECK(raw&&stage);uint64_t first=bnsh_make(stage);memcpy(raw,"NSO0",4);p32(raw+0xc,1u<<2);p32(raw+0x20,0x100);p32(raw+0x28,0x1000);p32(raw+0x64,0x1000);memcpy(raw+0x100,stage,0x1000);
 stage[0x900]^=0x33;uint64_t second=ssi_hash_update(UINT64_C(0xcbf29ce484222325),stage+0x900,0x90);size_t packed=lz4_literal(raw+0x1100,stage,0x1000);p32(raw+0x30,0x1100);p32(raw+0x38,0x1000);p32(raw+0x68,(uint32_t)packed);CHECK(pwrite(fd,raw,0x4000,0)==0x4000);
 ShaderSourceIndex*i=shader_source_index_create();CHECK(shader_source_index_nso(i,path,NULL,why,sizeof why));CHECK(shader_source_index_count(i)==2);CHECK(i->nsos&&!i->nsos->cache);
 uint8_t*p=shader_source_index_read_exact(i,first,0,0x80,0x10,NULL);CHECK(p&&!memcmp(p,raw+0x100+0x900,0x90));free(p);p=shader_source_index_read_exact(i,second,4,0x80,0x10,NULL);CHECK(p&&!memcmp(p,stage+0x900,0x90));free(p);p=shader_source_index_read_exact(i,first,0,0x80,0x10,NULL);CHECK(p&&!memcmp(p,raw+0x100+0x900,0x90));free(p);shader_source_index_destroy(i);
 /* Bounds and malformed compressed streams fail without unbounded allocation. */
 for(unsigned mode=0;mode<5;mode++){
  uint8_t saved[0x100];memcpy(saved,raw,sizeof saved);
  if(mode==0)p32(raw+0x28,UINT32_MAX);
  if(mode==1)p32(raw+0x20,UINT32_MAX);
  if(mode==2)p32(raw+0x30,0x100);
  if(mode==3)p32(raw+0x68,0);
  if(mode==4)raw[0]='x';
  CHECK(pwrite(fd,raw,sizeof saved,0)==sizeof saved);i=shader_source_index_create();CHECK(!shader_source_index_nso(i,path,NULL,why,sizeof why));shader_source_index_destroy(i);memcpy(raw,saved,sizeof saved);
 }
 CHECK(pwrite(fd,raw,sizeof raw[0]*0x100,0)==0x100);uint8_t bad=0;CHECK(pwrite(fd,&bad,1,0x1100)==1);i=shader_source_index_create();CHECK(!shader_source_index_nso(i,path,NULL,why,sizeof why));shader_source_index_destroy(i);
 CHECK(!close(fd));CHECK(!unlink(path));free(raw);free(stage);
}
int main(void){traversal();trailing_metadata();scanner();large_offsets();directory();nso();printf("{\"state\":\"passed\",\"assertions\":%u,\"synthetic_only\":true}\n",checks);return 0;}
