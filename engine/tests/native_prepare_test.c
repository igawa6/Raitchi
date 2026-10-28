#define _POSIX_C_SOURCE 200809L
#include "runtime/native_prepare.h"
#include "runtime/native_manifest.h"
#include "adapters/title_adapter.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern bool shared_sha256_bytes(const void*,size_t,uint8_t[32]);
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void put32(uint8_t*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static void fixture(const char*root,const TitleAdapter*a,int compressed){
 char dir[4096];snprintf(dir,sizeof dir,"%s/exefs",root);CHECK(!mkdir(root,0700));CHECK(!mkdir(dir,0700));
 for(unsigned i=0;i<a->module_count;i++){
  uint8_t raw[512]={0};memcpy(raw,"NSO0",4);put32(raw+0xc,compressed?7:0);
  const uint8_t*prefix=a->module_build_prefix[i];unsigned n=a->module_build_prefix_size[i];if(!n&&a->modules[i].module_id==MODULE_ABI_MAIN){prefix=a->main_build_prefix;n=a->main_build_prefix_size;}
  memcpy(raw+0x40,prefix,n);raw[0x5f]=(uint8_t)(i+1);
  for(unsigned j=0;j<3;j++){put32(raw+0x10+j*16,0x100+0x20*j);put32(raw+0x14+j*16,0x1000*j);put32(raw+0x18+j*16,16);put32(raw+0x60+4*j,compressed?18:16);if(compressed){raw[0x100+0x20*j]=0xf0;raw[0x101+0x20*j]=1;}for(unsigned k=0;k<16;k++)raw[0x100+0x20*j+(compressed?2:0)+k]=(uint8_t)(i*4+j+k);}
  put32(raw+0x3c,32);char file[4096];snprintf(file,sizeof file,"%s/exefs/%s",root,a->modules[i].name);FILE*f=fopen(file,"wb");CHECK(f);CHECK(fwrite(raw,1,sizeof raw,f)==sizeof raw);CHECK(!fclose(f));
 }
}
static bool failed_hash(const void*p,size_t n,uint8_t out[32]){(void)p;(void)n;(void)out;return false;}
int main(int argc,char**argv){
 CHECK(argc==3);uint8_t contract[32];for(unsigned i=0;i<32;i++){unsigned v;CHECK(sscanf(argv[2]+2*i,"%2x",&v)==1);contract[i]=(uint8_t)v;}
 char why[256],root[4096],exefs[4096],manifest[4096];NativePrepareResult result;
 for(unsigned id=1;id<=4;id++)for(int compressed=0;compressed<2;compressed++){
  const TitleAdapter*a=title_adapter_find((enum TitleId)id);snprintf(root,sizeof root,"%s/%s-%d",argv[1],a->key,compressed);fixture(root,a,compressed);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(manifest,sizeof manifest,"%s/native.manifest",root);
  CHECK(native_prepare_program_id(a->key)==a->program_id);CHECK(native_prepare_exefs(a->key,exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(result.created&&result.modules==a->module_count);CHECK(result.manifest_bytes==128+104*a->module_count);
  CHECK(native_prepare_exefs(a->key,exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(!result.created);
  uint8_t wire[NATIVE_MANIFEST_MAX_SIZE];FILE*f=fopen(manifest,"rb");CHECK(f);size_t size=fread(wire,1,sizeof wire,f);CHECK(!fclose(f));NativeManifest m;NativeManifestFailure mf;CHECK(native_manifest_parse(wire,size,contract,&m,&mf)==NATIVE_MANIFEST_OK);
  for(unsigned j=0;j<m.module_count;j++){char file[4096];snprintf(file,sizeof file,"%s/%s",exefs,a->modules[j].name);f=fopen(file,"rb");CHECK(f);uint8_t bytes[512],digest[32];CHECK(fread(bytes,1,sizeof bytes,f)==sizeof bytes);fclose(f);CHECK(shared_sha256_bytes(bytes,sizeof bytes,digest));CHECK(!memcmp(digest,m.modules[j].nso_sha256,32));CHECK(m.modules[j].mapped_size==16384);}
 }
 CHECK(!native_prepare_program_id("unsupported"));
 snprintf(root,sizeof root,"%s/echoes-0",argv[1]);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(manifest,sizeof manifest,"%s/native.manifest",root);
 CHECK(!native_prepare_exefs("unsupported",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(result.modules==0);
 CHECK(!native_prepare_exefs("echoes",exefs,manifest,contract,failed_hash,&result,why,sizeof why));CHECK(result.modules==0);
 // Corrupt originals, LZ4, builds and conflicting metadata are rejected without replacing existing files.
 char file[4096];snprintf(file,sizeof file,"%s/main",exefs);FILE*f=fopen(file,"r+b");CHECK(f);fseek(f,0x40,SEEK_SET);fputc(0xff,f);fclose(f);
 CHECK(!native_prepare_exefs("echoes",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(strstr(why,"build"));
 f=fopen(file,"r+b");fseek(f,0x40,SEEK_SET);fputc(title_adapter_find(TITLE_ECHOES)->main_build_prefix[0],f);fclose(f);
 f=fopen(manifest,"r+b");CHECK(f);fputc('X',f);fclose(f);CHECK(!native_prepare_exefs("echoes",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(strstr(why,"Conflicting"));f=fopen(manifest,"rb");CHECK(fgetc(f)=='X');fclose(f);
 snprintf(root,sizeof root,"%s/echoes-1",argv[1]);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(file,sizeof file,"%s/main",exefs);snprintf(manifest,sizeof manifest,"%s/invalid.manifest",root);
 f=fopen(file,"r+b");CHECK(f);fseek(f,0x100,SEEK_SET);fputc(0,f);fclose(f);CHECK(!native_prepare_exefs("echoes",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(access(manifest,F_OK)!=0);
 // Existing symlink outputs and symlink original modules are never followed.
 snprintf(root,sizeof root,"%s/las-0",argv[1]);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(manifest,sizeof manifest,"%s/linked.manifest",root);CHECK(!symlink("native.manifest",manifest));CHECK(!native_prepare_exefs("las",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));
 snprintf(file,sizeof file,"%s/main",exefs);CHECK(!unlink(file));CHECK(!symlink("sdk",file));snprintf(manifest,sizeof manifest,"%s/no.manifest",root);CHECK(!native_prepare_exefs("las",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));
 snprintf(root,sizeof root,"%s/wonder-0",argv[1]);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(manifest,sizeof manifest,"%s/rejected.manifest",root);
 snprintf(file,sizeof file,"%s/sdk",exefs);CHECK(!unlink(file));CHECK(!native_prepare_exefs("wonder",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(strstr(why,"missing"));CHECK(access(manifest,F_OK)!=0);
 snprintf(root,sizeof root,"%s/wonder-1",argv[1]);snprintf(exefs,sizeof exefs,"%s/exefs",root);snprintf(file,sizeof file,"%s/main",exefs);snprintf(manifest,sizeof manifest,"%s/rejected.manifest",root);
 f=fopen(file,"r+b");CHECK(f);fseek(f,0x40,SEEK_SET);fputc(0xff,f);fclose(f);CHECK(!native_prepare_exefs("wonder",exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why));CHECK(strstr(why,"build"));CHECK(access(manifest,F_OK)!=0);
 printf("%u actual native preparation assertions passed\n",checks);return 0;
}
