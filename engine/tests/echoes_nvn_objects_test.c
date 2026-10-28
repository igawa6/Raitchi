/* Actual production NVN builder/getter/conversion bodies with real host types.
 * Asset-free guarded guest stack objects; no GPU or title execution claim. */
#define _GNU_SOURCE
#include "runtime/aot_cpu.h"
#include "echoes_nvn_object_bodies.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"check line%d:%s\n",__LINE__,#x);return 1;}}while(0)
static void argf(Cpu*c,unsigned r,float value){c->v[r]=0;memcpy(&c->v[r],&value,4);}
static float retf(Cpu*c){float f;memcpy(&f,&c->v[0],4);return f;}
static int equal_desc(const NvnSamplerDesc*d){
 return d->min_filter==2&&d->mag_filter==1&&d->wrap[0]==1&&d->wrap[1]==2&&d->wrap[2]==3&&d->compare_mode==1&&d->compare_func==6&&d->reduction==2&&d->lod_min==.25f&&d->lod_max==17.5f&&d->lod_bias==-.75f&&d->max_aniso==8&&d->backend==NULL;
}
int main(void){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);unsigned char*map=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);CHECK(!mprotect(map+page,page,PROT_READ|PROT_WRITE));unsigned char*builder=map+page*2-64;memset(builder-32,0xa5,96);uint64_t guards[4];memset(guards,0xa5,sizeof guards);Cpu*c=aligned_alloc(16,sizeof *c);CHECK(c);memset(c,0,sizeof *c);c->x[0]=(uintptr_t)builder;c->x[1]=0x987654321;n_cpu_nvnSamplerBuilderSetDevice(c);n_cpu_nvnSamplerBuilderSetDefaults(c);CHECK(((SamplerBuilder*)builder)->device==0x987654321);CHECK(sizeof(SamplerBuilder)<=64&&sizeof(NvnSamplerDesc)==72);CHECK(!memcmp(builder-32,guards,32));for(unsigned i=sizeof(SamplerBuilder);i<64;i++)CHECK(builder[i]==0xa5);
 c->x[1]=2;c->x[2]=1;n_cpu_nvnSamplerBuilderSetMinMagFilter(c);c->x[1]=1;c->x[2]=2;c->x[3]=3;n_cpu_nvnSamplerBuilderSetWrapMode(c);argf(c,0,.25f);argf(c,1,17.5f);n_cpu_nvnSamplerBuilderSetLodClamp(c);argf(c,0,-.75f);n_cpu_nvnSamplerBuilderSetLodBias(c);c->x[1]=1;c->x[2]=6;n_cpu_nvnSamplerBuilderSetCompare(c);argf(c,0,8);n_cpu_nvnSamplerBuilderSetMaxAnisotropy(c);c->x[1]=2;n_cpu_nvnSamplerBuilderSetReductionFilter(c);
 uint32_t border[4]={0x3f800000,0xffffffff,0x80000000,0x7fc01234},output[6]={0};c->x[1]=(uintptr_t)border;n_cpu_nvnSamplerBuilderSetBorderColor(c);n_cpu_nvnSamplerBuilderSetBorderColori(c);n_cpu_nvnSamplerBuilderSetBorderColorui(c);CHECK(!memcmp(builder-32,guards,32));for(unsigned i=sizeof(SamplerBuilder);i<64;i++)CHECK(builder[i]==0xa5);
 uint32_t ints[4]={0};float floats[2]={0};c->x[1]=(uintptr_t)&ints[0];c->x[2]=(uintptr_t)&ints[1];n_cpu_nvnSamplerBuilderGetMinMagFilter(c);CHECK(ints[0]==2&&ints[1]==1);for(unsigned i=0;i<3;i++)c->x[i+1]=(uintptr_t)&ints[i];n_cpu_nvnSamplerBuilderGetWrapMode(c);CHECK(ints[0]==1&&ints[1]==2&&ints[2]==3);c->x[1]=(uintptr_t)&floats[0];c->x[2]=(uintptr_t)&floats[1];n_cpu_nvnSamplerBuilderGetLodClamp(c);CHECK(floats[0]==.25f&&floats[1]==17.5f);n_cpu_nvnSamplerBuilderGetLodBias(c);CHECK(retf(c)==-.75f);n_cpu_nvnSamplerBuilderGetMaxAnisotropy(c);CHECK(retf(c)==8);n_cpu_nvnSamplerBuilderGetReductionFilter(c);CHECK(c->x[0]==2);c->x[0]=(uintptr_t)builder;c->x[1]=(uintptr_t)&ints[0];c->x[2]=(uintptr_t)&ints[1];n_cpu_nvnSamplerBuilderGetCompare(c);CHECK(ints[0]==1&&ints[1]==6);
 c->x[1]=(uintptr_t)&output[1];n_cpu_nvnSamplerBuilderGetBorderColor(c);CHECK(!memcmp(output+1,border,16)&&!output[0]&&!output[5]);n_cpu_nvnSamplerBuilderGetBorderColori(c);CHECK(!memcmp(output+1,border,16));n_cpu_nvnSamplerBuilderGetBorderColorui(c);CHECK(!memcmp(output+1,border,16));
 uint64_t handle[2]={0,0x5a5a5a5a};c->x[0]=(uintptr_t)handle;c->x[1]=(uintptr_t)builder;n_cpu_nvnSamplerInitialize(c);CHECK(c->x[0]==1&&handle[1]==0x5a5a5a5a);NvnSamplerDesc*sampler=(void*)(uintptr_t)handle[0];CHECK(equal_desc(sampler)&&!memcmp(sampler->border,border,16));CHECK(!memcmp(builder-32,guards,32));
 NvnSamplerDesc entries[3],before[3];memset(entries,0xa5,sizeof entries);memcpy(before,entries,sizeof entries);NvnDescPool pool={.count=3,.entries=entries};uint64_t pool_handle=(uintptr_t)&pool;c->x[0]=(uintptr_t)&pool_handle;c->x[1]=1;c->x[2]=(uintptr_t)builder;n_cpu_nvnSamplerPoolRegisterSamplerBuilder(c);CHECK(equal_desc(&entries[1])&&!memcmp(entries[1].border,border,16));CHECK(!memcmp(&entries[0],&before[0],sizeof entries[0])&&!memcmp(&entries[2],&before[2],sizeof entries[2]));memcpy(before,entries,sizeof entries);c->x[1]=UINT64_MAX;c->x[2]=0;n_cpu_nvnSamplerPoolRegisterSamplerBuilder(c);c->x[1]=3;n_cpu_nvnSamplerPoolRegisterSamplerBuilder(c);CHECK(!memcmp(entries,before,sizeof entries));
 c->x[0]=(uintptr_t)handle;n_cpu_nvnSamplerFinalize(c);CHECK(!handle[0]&&queued_free==sampler);CHECK(equal_desc(sampler));free(queued_free);queued_free=NULL;
 const uint32_t dims[]={0,1,15,16,17,1080,1920,UINT32_MAX};unsigned char texture_builder[128]={0};NvnTexture texture={0};uint64_t texture_handle=(uintptr_t)&texture;
 for(unsigned i=0;i<sizeof dims/sizeof *dims;i++)for(unsigned j=0;j<sizeof dims/sizeof *dims;j++){
  uint64_t w=dims[i]?dims[i]:1,h=dims[j]?dims[j]:1;uint64_t wanted=((((w+15)>>4)*((h+15)>>4)*8)+4095)&~UINT64_C(4095);memcpy(texture_builder+0x10,&dims[i],4);memcpy(texture_builder+0x14,&dims[j],4);c->x[0]=(uintptr_t)texture_builder;n_cpu_nvnTextureBuilderGetZCullStorageSize(c);CHECK(c->x[0]==wanted&&c->x[0]>=4096&&!(c->x[0]&4095));texture.width=dims[i];texture.height=dims[j];c->x[0]=(uintptr_t)&texture_handle;n_cpu_nvnTextureGetZCullStorageSize(c);CHECK(c->x[0]==wanted);
 }
 free(c);CHECK(!munmap(map,page*3));printf("checks=%u sampler_guest_bytes=%zu host_descriptor_bytes=%zu guard_pages=2 zcull_cases=64 queued_free_preserved=true\n",checks,sizeof(SamplerBuilder),sizeof(NvnSamplerDesc));return 0;
}
