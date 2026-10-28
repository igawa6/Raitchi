/* Real GOB copy functions, format inventory and NVN entry points. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nvn/nvn.h"
#undef FATAL
#define FATAL(...) do{fprintf(stderr,__VA_ARGS__);abort();}while(0)
static unsigned checks,notifications;static uint64_t notification_size;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
void backend_pool_written(uint64_t address,uint64_t size){(void)address;notification_size=size;notifications++;}
#include "texture_write_bodies.h"
static void pattern(uint8_t*p,size_t n,unsigned salt){for(size_t i=0;i<n;i++)p[i]=(uint8_t)(i*37+i/11+salt);}
static void patch_reference(TexLayout*t,uint8_t*expected,unsigned level,TexWriteRegion*r,uint8_t*src,size_t row,size_t image){
 LevelLayout l=tegra_level(t,level);unsigned b=t->fmt->bpb,rows=(r->height+t->fmt->bh-1)/t->fmt->bh,bytes=(r->width+t->fmt->bw-1)/t->fmt->bw*b;
 uint64_t layer=tegra_layer_size(t);size_t linear_size=(size_t)l.wb*l.hb*l.d*b;uint8_t*lin=malloc(linear_size);CHECK(lin);
 if(t->depth>1){tegra_deswizzle(expected+l.offset,lin,l.wb,l.hb,l.d,b,l.block_height_log2,l.block_depth_log2);for(unsigned z=0;z<r->depth;z++)for(unsigned y=0;y<rows;y++)memcpy(lin+(((size_t)(r->z+z)*l.hb+r->y/t->fmt->bh+y)*l.wb+r->x/t->fmt->bw)*b,src+z*image+y*row,bytes);tegra_swizzle(lin,expected+l.offset,l.wb,l.hb,l.d,b,l.block_height_log2,l.block_depth_log2);}
 else for(unsigned z=0;z<r->depth;z++){uint8_t*at=expected+(r->z+z)*layer+l.offset;tegra_deswizzle(at,lin,l.wb,l.hb,1,b,l.block_height_log2,l.block_depth_log2);for(unsigned y=0;y<rows;y++)memcpy(lin+((size_t)(r->y/t->fmt->bh+y)*l.wb+r->x/t->fmt->bw)*b,src+z*image+y*row,bytes);tegra_swizzle(lin,at,l.wb,l.hb,1,b,l.block_height_log2,l.block_depth_log2);}
 free(lin);
}
static void tiled(unsigned format,int volume,unsigned level){
 TexLayout t={.width=37,.height=29,.depth=volume?7:1,.layers=volume?1:3,.levels=3,.fmt=nvn_format_info(format)};tegra_layout_init(&t);size_t total=tegra_total_size(&t);uint8_t*allocation=malloc(total+64),*expected=malloc(total),*dst=allocation+32;CHECK(allocation&&expected);memset(allocation,0xa5,total+64);
 // Populate all layers and mips using the existing whole upload operation.
 for(unsigned layer=0;layer<t.layers;layer++)for(unsigned mip=0;mip<t.levels;mip++){LevelLayout l=tegra_level(&t,mip);size_t size=(size_t)l.wb*l.hb*l.d*t.fmt->bpb;uint8_t*linear=malloc(size);CHECK(linear);pattern(linear,size,layer*31+mip*7);tegra_swizzle(linear,dst+layer*tegra_layer_size(&t)+l.offset,l.wb,l.hb,l.d,t.fmt->bpb,l.block_height_log2,l.block_depth_log2);free(linear);}
 memcpy(expected,dst,total);LevelLayout l=tegra_level(&t,level);unsigned x=l.w>t.fmt->bw?t.fmt->bw:0,y=l.h>t.fmt->bh?t.fmt->bh:0;
 TexWriteRegion r={x,y,volume&&l.d<2?0:1,l.w-x,l.h-y,volume?(l.d>2?2:1):2};unsigned rows=(r.height+t.fmt->bh-1)/t.fmt->bh,bytes=(r.width+t.fmt->bw-1)/t.fmt->bw*t.fmt->bpb;size_t row=bytes+7,image=row*rows+11;uint8_t*src=malloc(image*r.depth);CHECK(src);pattern(src,image*r.depth,91);
 patch_reference(&t,expected,level,&r,src,row,image);CHECK(tegra_write_region(&t,level,false,0,dst,total,&r,src,row,image)==TEX_WRITE_OK);CHECK(!memcmp(dst,expected,total));for(unsigned i=0;i<32;i++){CHECK(allocation[i]==0xa5);CHECK(allocation[total+32+i]==0xa5);}
 // Invalid updates are transactional: no partial texel mutation.
 r.x=l.w;CHECK(tegra_write_region(&t,level,false,0,dst,total,&r,src,row,image)==TEX_WRITE_BOUNDS);CHECK(!memcmp(dst,expected,total));free(src);free(expected);free(allocation);
}
static void linear(void){
 TexLayout t={.width=19,.height=13,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x19)};tegra_layout_init(&t);size_t pitch=19*3+23,total=pitch*13;uint8_t dst[1040],expected[1040],src[9*5+7*5];memset(dst,0x68,sizeof dst);memcpy(expected,dst,sizeof dst);pattern(src,sizeof src,33);TexWriteRegion r={3,4,0,3,5,1};for(unsigned y=0;y<5;y++)memcpy(expected+(4+y)*pitch+9,src+y*16,9);
 CHECK(tegra_write_region(&t,0,true,pitch,dst,total,&r,src,16,0)==TEX_WRITE_OK);CHECK(!memcmp(dst,expected,sizeof dst));CHECK(tegra_write_region(&t,0,true,pitch,dst,total-1,&r,src,16,0)==TEX_WRITE_STORAGE);CHECK(tegra_write_region(&t,0,true,pitch,dst,total,&r,src,8,0)==TEX_WRITE_STRIDE);CHECK(tegra_write_region(&t,0,true,pitch,dst,total,&r,src,16,72)==TEX_WRITE_STRIDE);CHECK(!memcmp(dst,expected,sizeof dst));
}
static void wrappers(void){
 TexLayout l={.width=32,.height=24,.depth=1,.layers=4,.levels=3,.fmt=nvn_format_info(0x25)};tegra_layout_init(&l);size_t size=tegra_total_size(&l);uint8_t*dst=malloc(size),*expected=malloc(size),src[4096];CHECK(dst&&expected);memset(dst,0x97,size);memcpy(expected,dst,size);pattern(src,sizeof src,11);
 NvnPool pool={.storage=(uintptr_t)dst,.size=size};NvnTexture t={.layout=l,.target=4,.format=0x25,.width=32,.height=24,.depth=4,.levels=3,.pool=&pool,.addr=(uintptr_t)dst};NvnTextureView v={.setmask=1|2,.base_level=1,.num_levels=1,.min_layer=1,.num_layers=2};NvnCopyRegion region={.x=3,.y=2,.z=0,.width=9,.height=7,.depth=2};TexWriteRegion translated={3,2,1,9,7,2};patch_reference(&l,expected,1,&translated,src,40,300);
 uint64_t guest=(uintptr_t)&t;Cpu cpu={0};cpu.x[0]=(uintptr_t)&guest;cpu.x[1]=(uintptr_t)&v;cpu.x[2]=(uintptr_t)&region;cpu.x[3]=(uintptr_t)src;cpu.x[4]=40;cpu.x[5]=300;unsigned before=notifications;hle_nvnTextureWriteTexelsStrided(&cpu);CHECK(notifications==before+1);CHECK(t.cpu_write_generation==1);CHECK(!memcmp(dst,expected,size));
 // Reproduce the observed base client ABI: default view + levels(0,1), zero
 // offsets, tightly packed smaller-than-storage dimensions; preserve remainder.
 v=(NvnTextureView){.setmask=1,.num_levels=1};region=(NvnCopyRegion){0,0,0,12,8,1};translated=(TexWriteRegion){0,0,0,12,8,1};patch_reference(&l,expected,0,&translated,src,48,384);hle_nvnTextureWriteTexels(&cpu);CHECK(!memcmp(dst,expected,size));
 CHECK(t.cpu_write_generation==2);before=notifications;v.format=0x19;v.setmask|=4;CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_FORMAT);CHECK(notifications==before);v.setmask=0;region.width=33;CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_BOUNDS);CHECK(!memcmp(dst,expected,size));region.width=12;pool.size=size-1;CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_STORAGE);pool.size=size;t.samples=4;CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_FORMAT);t.samples=0;t.format=0xffff;CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_FORMAT);t.format=0x25;
 CHECK(texture_write_texels(&t,&v,&region,src,-1,0)==TEX_WRITE_ARGUMENT);CHECK(texture_write_texels(&t,&v,&region,src,UINT64_C(0x7fffffffffffffff),0)==TEX_WRITE_STRIDE);CHECK(!memcmp(dst,expected,size));CHECK(t.cpu_write_generation==2);__atomic_store_n(&t.gpu_write_owned,1,__ATOMIC_RELEASE);CHECK(texture_write_texels(&t,&v,&region,src,0,0)==TEX_WRITE_GPU_OWNED);CHECK(t.cpu_write_generation==2);CHECK(!memcmp(dst,expected,size));free(expected);free(dst);
}
static void array_1d(void){
 TexLayout l={.width=16,.height=1,.depth=1,.layers=4,.levels=1,.fmt=nvn_format_info(1)};tegra_layout_init(&l);size_t n=tegra_total_size(&l);uint8_t*dst=malloc(n),*expected=malloc(n),src[128];CHECK(dst&&expected);memset(dst,0x33,n);memcpy(expected,dst,n);pattern(src,sizeof src,19);NvnPool pool={.storage=(uintptr_t)dst,.size=n};NvnTexture t={.layout=l,.target=3,.format=1,.width=16,.height=4,.depth=1,.levels=1,.addr=(uintptr_t)dst,.pool=&pool,.swizzle={5,4,3,2}};NvnTexture original=t;NvnCopyRegion region={3,1,0,5,2,1};TexWriteRegion translated={3,0,1,5,1,2};patch_reference(&l,expected,0,&translated,src,11,11);CHECK(texture_write_texels(&t,NULL,&region,src,11,100)==TEX_WRITE_OK);CHECK(!memcmp(dst,expected,n));original.cpu_write_generation=1;CHECK(!memcmp(&t,&original,sizeof t));CHECK(notification_size==n);free(expected);free(dst);
 // A linear upload preserves row padding and notifies only its own footprint.
 uint8_t memory[256],exp[256],input[32];memset(memory,0x99,sizeof memory);memcpy(exp,memory,sizeof exp);pattern(input,sizeof input,14);l=(TexLayout){.width=10,.height=4,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x25)};tegra_layout_init(&l);pool=(NvnPool){.storage=(uintptr_t)memory,.size=sizeof memory};t=(NvnTexture){.layout=l,.target=1,.format=0x25,.flags=0x10,.width=10,.height=4,.levels=1,.stride=48,.addr=(uintptr_t)memory,.pool=&pool};region=(NvnCopyRegion){2,1,0,3,2,1};memcpy(exp+48+8,input,12);memcpy(exp+96+8,input+12,12);CHECK(texture_write_texels(&t,NULL,&region,input,0,0)==TEX_WRITE_OK);CHECK(!memcmp(memory,exp,sizeof memory));CHECK(notification_size==192);
}
static void row_layout_flags(void){
 for(unsigned flag=0x10;flag<=0x50;flag+=flag==0x10?0x30:0x10){
  uint8_t memory[4096],input[512];memset(memory,0x79,sizeof memory);pattern(input,sizeof input,22);
  TexLayout l={.width=64,.height=1,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x29)};tegra_layout_init(&l);
  NvnPool pool={.storage=(uintptr_t)memory,.size=sizeof memory};NvnTexture t={.layout=l,.target=1,.format=0x29,.flags=flag,.stride=4096,.pool=&pool,.addr=(uintptr_t)memory};NvnCopyRegion r={0,0,0,64,1,1};
  CHECK(texture_write_texels(&t,NULL,&r,input,0,0)==TEX_WRITE_OK);CHECK(!memcmp(memory,input,512));for(unsigned i=512;i<sizeof memory;i++)CHECK(memory[i]==0x79);CHECK(notification_size==4096&&t.cpu_write_generation==1);
  pool.size=4095;CHECK(texture_write_texels(&t,NULL,&r,input,0,0)==TEX_WRITE_STORAGE);CHECK(t.cpu_write_generation==1);pool.size=4096;
  t.stride=511;CHECK(texture_write_texels(&t,NULL,&r,input,0,0)==TEX_WRITE_STORAGE);CHECK(!memcmp(memory,input,512));
 }
 // Buffer target10 retains tight width*bpb even when its stride is unrelated.
 uint8_t memory[32],input[8];memset(memory,0x22,sizeof memory);memset(input,0x66,sizeof input);TexLayout l={.width=8,.height=1,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x25)};tegra_layout_init(&l);NvnPool pool={.storage=(uintptr_t)memory,.size=sizeof memory};NvnTexture t={.layout=l,.target=10,.format=0x25,.stride=UINT64_MAX,.pool=&pool,.addr=(uintptr_t)memory};NvnCopyRegion r={2,0,0,2,1,1};CHECK(texture_write_texels(&t,NULL,&r,input,0,0)==TEX_WRITE_OK);CHECK(!memcmp(memory+8,input,8)&&memory[7]==0x22&&memory[16]==0x22&&notification_size==32);
}
static void invalid(void){
 TexLayout t={.width=13,.height=9,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x41)};tegra_layout_init(&t);size_t n=tegra_total_size(&t);uint8_t*dst=malloc(n),src[128];CHECK(dst);memset(dst,0x55,n);TexWriteRegion r={1,0,0,4,4,1};CHECK(tegra_write_region(&t,0,false,0,dst,n,&r,src,0,0)==TEX_WRITE_FORMAT);r=(TexWriteRegion){0,0,0,5,4,1};CHECK(tegra_write_region(&t,0,false,0,dst,n,&r,src,0,0)==TEX_WRITE_FORMAT);r=(TexWriteRegion){12,8,0,1,1,1};CHECK(tegra_write_region(&t,0,false,0,dst,n,&r,src,0,0)==TEX_WRITE_OK);CHECK(tegra_write_region(&t,0,false,0,dst,n,&r,dst,0,0)==TEX_WRITE_ARGUMENT);r.depth=UINT32_MAX;CHECK(tegra_write_region(&t,0,false,0,dst,n,&r,src,0,0)==TEX_WRITE_BOUNDS);free(dst);
}
static void virtual_write(void){
 uint8_t a[256],b[256],src[32];memset(a,0x33,sizeof a);memset(b,0x44,sizeof b);memset(src,0x77,sizeof src);
 NvnPool pool={.size=1u<<27,.mappings=nvn_pool_mappings_create()};CHECK(pool.mappings);
 NvnPoolMappingRequest r={0x6ef2400,192,(uintptr_t)a};CHECK(nvn_pool_mappings_apply(pool.mappings,pool.size,&r,1));
 TexLayout l={.width=10,.height=4,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x25)};tegra_layout_init(&l);
 NvnTexture t={.layout=l,.target=1,.format=0x25,.flags=0x10,.stride=48,.pool=&pool,.pool_offset=0x6ef2400};NvnCopyRegion region={2,1,0,3,2,1};
 CHECK(texture_write_texels(&t,NULL,&region,src,0,0)==TEX_WRITE_OK);CHECK(a[56]==0x77&&a[68]==0x33&&a[104]==0x77&&a[192]==0x33);CHECK(notification_size==192);
 r.address=(uintptr_t)b;CHECK(nvn_pool_mappings_apply(pool.mappings,pool.size,&r,1));src[0]=0x99;CHECK(texture_write_texels(&t,NULL,&region,src,0,0)==TEX_WRITE_OK);CHECK(b[56]==0x99&&a[56]==0x77);
 r=(NvnPoolMappingRequest){t.pool_offset+100,1,0};CHECK(nvn_pool_mappings_apply(pool.mappings,pool.size,&r,1));uint8_t saved[256];memcpy(saved,b,sizeof b);unsigned before=notifications;CHECK(texture_write_texels(&t,NULL,&region,src,0,0)==TEX_WRITE_STORAGE);CHECK(!memcmp(saved,b,sizeof b)&&notifications==before);
 nvn_pool_mappings_destroy(pool.mappings);
}
int main(void){unsigned formats=0;for(unsigned f=1;f<256;f++)if(nvn_format_known(f)){formats++;for(unsigned level=0;level<2;level++){tiled(f,0,level);tiled(f,1,level);}}CHECK(formats>100);CHECK(!nvn_format_known(0)&&!nvn_format_known(0xffff));CHECK(nvn_format_info(0x98)->bpb==2);linear();wrappers();array_1d();row_layout_flags();invalid();virtual_write();printf("state=passed checks=%u known_formats=%u; actual GOB/raw-block updates and NVN wrappers\n",checks,formats);return 0;}
