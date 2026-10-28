#define _POSIX_C_SOURCE 200809L
#include "native_prepare.h"
#include "native_manifest.h"
#include "nso_image.h"
#include "aot_cpu.h"
#include "../adapters/title_adapter.h"
#include "../adapters/generic/generic.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool problem(char *why,size_t cap,const char *text){if(why&&cap)snprintf(why,cap,"%s",text);return false;}
static const TitleAdapter *find_title(const char *title){
 if(!title)return NULL;
 const enum TitleId ids[]={TITLE_LAS,TITLE_EEVEE,TITLE_ECHOES,TITLE_WONDER};
 for(size_t i=0;i<sizeof ids/sizeof ids[0];i++){const TitleAdapter*a=title_adapter_find(ids[i]);if(a&&!strcmp(a->key,title))return a;}return NULL;
}
uint64_t native_prepare_program_id(const char *title){const TitleAdapter*a=find_title(title);return a?a->program_id:0;}
static uint32_t le32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(i*8));}
static void put64(uint8_t*p,uint64_t x){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(x>>(i*8));}
static bool same_stat(const struct stat*a,const struct stat*b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;}
static bool read_all(int fd,void*dst,size_t size){uint8_t*p=dst;while(size){ssize_t n=read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=(size_t)n;}return true;}
static bool write_all(int fd,const void*src,size_t size){const uint8_t*p=src;while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=(size_t)n;}return true;}
static bool same_file(const char*path,const uint8_t*wire,size_t size){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;struct stat st;uint8_t bytes[NATIVE_MANIFEST_MAX_SIZE];
 bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size==(off_t)size&&read_all(fd,bytes,size)&&!memcmp(bytes,wire,size);close(fd);return ok;
}
static bool publish(const char*path,const uint8_t*wire,size_t size,bool*created,char*why,size_t cap){
 size_t n=strlen(path);if(n>4000)return problem(why,cap,"Native metadata output path is too long");
 char*temporary=malloc(n+16);if(!temporary)return problem(why,cap,"Native metadata allocation failed");
 snprintf(temporary,n+16,"%s.tmp-XXXXXX",path);int fd=mkstemp(temporary);
 if(fd<0){free(temporary);return problem(why,cap,"Cannot create native metadata temporary file");}
 bool ok=write_all(fd,wire,size)&&!fsync(fd);if(close(fd))ok=false;
 if(!ok){unlink(temporary);free(temporary);return problem(why,cap,"Cannot write native metadata");}
 if(!link(temporary,path))*created=true;
 else if(errno==EEXIST&&same_file(path,wire,size))*created=false;
 else{unlink(temporary);free(temporary);return problem(why,cap,"Conflicting native metadata exists; original preserved");}
 unlink(temporary);free(temporary);return true;
}
bool native_prepare_exefs(const char*title,const char*exefs,const char*path,const uint8_t contract[32],NativePrepareHash hash,NativePrepareResult*out,char*why,size_t cap){
 if(out)memset(out,0,sizeof*out);if(why&&cap)why[0]=0;
 const TitleAdapter*a=find_title(title);
 // Generic: derive from this ExeFS. The version only names the save directory and is not manifest input.
 static const TitleGenericOptions generic_options={0};TitleGenericAdapter generic;
 if(!a&&title&&!strcmp(title,TITLE_GENERIC_KEY)&&exefs&&*exefs){if(!title_generic_derive_directory(exefs,&generic_options,&generic,why,cap))return false;a=&generic.adapter;}
 if(!a||!exefs||!*exefs||!path||!*path||!contract||!hash||!out)return problem(why,cap,"Invalid native preparation arguments/title");
 NativeManifest m={.struct_size=sizeof m,.version=NATIVE_MANIFEST_VERSION,.title_id=(uint32_t)a->id,.module_count=a->module_count,.program_id=a->program_id,.mapping_alignment=16384,
  .identity={.struct_size=sizeof(ModuleAbiIdentity),.abi_version=MODULE_ABI_VERSION,.target=MODULE_ABI_TARGET_ANDROID_ARM64_LP64,.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560,.cpu_size=sizeof(Cpu),.cpu_alignment=_Alignof(Cpu),.guest_shift=a->guest_shift,.guest_window_lo=a->canonical_window_lo-a->guest_shift,.guest_window_hi=a->canonical_window_hi-a->guest_shift}};
 memcpy(m.contract_sha256,contract,32);
 for(unsigned i=0;i<m.module_count;i++){
  char file[4096];int len=snprintf(file,sizeof file,"%s/%s",exefs,a->modules[i].name);if(len<0||(size_t)len>=sizeof file)return problem(why,cap,"Original module path is too long");
  int fd=open(file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return problem(why,cap,"Required original NSO is missing or not a regular file");
  struct stat before,after;uint8_t*raw=NULL;bool ok=!fstat(fd,&before)&&S_ISREG(before.st_mode)&&before.st_size>=256&&before.st_size<=(512u<<20);
  if(ok){raw=malloc((size_t)before.st_size);ok=raw&&read_all(fd,raw,(size_t)before.st_size)&&!fstat(fd,&after)&&same_stat(&before,&after);}
  close(fd);if(!ok){free(raw);return problem(why,cap,"Cannot read bounded immutable original NSO");}
  if(memcmp(raw,"NSO0",4)){free(raw);return problem(why,cap,"Invalid original NSO header");}
  NativeModuleSpec*s=&m.modules[i];s->struct_size=sizeof*s;s->module_id=a->modules[i].module_id;s->canonical_base=a->modules[i].canonical_base;
  memcpy(s->nso_build_id,raw+0x40,32);s->text_offset=le32(raw+0x14);s->text_size=le32(raw+0x18);
  uint64_t end=0;for(unsigned j=0;j<3;j++){uint64_t e=(uint64_t)le32(raw+0x14+j*16)+le32(raw+0x18+j*16)+(j==2?le32(raw+0x3c):0);if(e>end)end=e;}
  s->mapped_size=(end+16383)&~UINT64_C(16383);
  if(s->mapped_size>(256u<<20)||!hash(raw,(size_t)before.st_size,s->nso_sha256)){free(raw);return problem(why,cap,"Original NSO image/hash exceeds preparation policy");}
  // The actual decoder validates file/header/compressed-block bounds before allocation.
  NsoImage image;NsoImageFailure nf;enum NsoImageStatus ns=nso_image_decode_native(raw,(size_t)before.st_size,s,&m.identity,m.mapping_alignment,&image,&nf);free(raw);
  if(ns!=NSO_IMAGE_OK)return problem(why,cap,nso_image_status_string(ns));nso_image_destroy(&image);
 }
 SessionLayoutSpec layout;TitleAdapterFailure af;enum TitleAdapterStatus as=title_adapter_build_native_layout_for(a,&m,contract,16384,&layout,&af);
 if(as!=TITLE_ADAPTER_OK)return problem(why,cap,title_adapter_status_string(as));
 uint8_t wire[NATIVE_MANIFEST_MAX_SIZE]={0};size_t size=128+104*m.module_count;memcpy(wire,"NATVM001",8);put32(wire+8,(uint32_t)size);put32(wire+12,1);put32(wire+16,m.title_id);put32(wire+20,m.module_count);put64(wire+24,m.program_id);
 put32(wire+32,m.identity.target);put32(wire+36,m.identity.cpu_identity);put32(wire+40,m.identity.cpu_size);put32(wire+44,m.identity.cpu_alignment);put32(wire+48,m.mapping_alignment);put64(wire+56,m.identity.guest_shift);put64(wire+64,m.identity.guest_window_lo);put64(wire+72,m.identity.guest_window_hi);memcpy(wire+80,contract,32);
 for(unsigned i=0;i<m.module_count;i++){const NativeModuleSpec*s=&m.modules[i];uint8_t*r=wire+128+104*i;put32(r,s->module_id);memcpy(r+8,s->nso_build_id,32);memcpy(r+40,s->nso_sha256,32);put64(r+72,s->canonical_base);put64(r+80,s->mapped_size);put64(r+88,s->text_offset);put64(r+96,s->text_size);}
 NativeManifest checked;NativeManifestFailure mf;if(native_manifest_parse(wire,size,contract,&checked,&mf)!=NATIVE_MANIFEST_OK)return problem(why,cap,"Prepared native metadata failed wire validation");
 NativePrepareResult result={.title=a->key,.modules=m.module_count,.program_id=m.program_id,.manifest_bytes=size};
 if(!hash(wire,size,result.manifest_sha256)||!publish(path,wire,size,&result.created,why,cap))return false;
 *out=result;return true;
}
