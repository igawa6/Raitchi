#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nvn/nvn.h"
#undef LOG
#define LOG(...) ((void)0)
#define TEX HOST(NvnTexture,A(0))
static uint64_t next_debug_id=1;
#include "queries.inc"
static unsigned checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void upload_rows(NvnTexture*t,const uint8_t*src,uint8_t*lin){const FormatInfo*fi=t->layout.fmt;LevelLayout l=tegra_level(&t->layout,0);uint32_t row=l.wb*fi->bpb;
#include "rows.inc"
}
static void put32(uint8_t*b,unsigned off,uint32_t v){memcpy(b+off,&v,4);}static void put64(uint8_t*b,unsigned off,uint64_t v){memcpy(b+off,&v,8);}
int main(void){CHECK(nvn_texture_row_linear(0x10)&&nvn_texture_row_linear(0x40)&&nvn_texture_row_linear(0x50));CHECK(!nvn_texture_row_linear(0x8)&&!nvn_texture_row_linear(0));
 TexLayout l={.fmt=nvn_format_info(0x29),.width=64,.height=1,.depth=1,.layers=1,.levels=1};tegra_layout_init(&l);uint64_t size=99;CHECK(nvn_texture_row_span(&l,4096,&size)&&size==4096);uint8_t input[4096],row[512],gob[512];memset(input,0,sizeof input);for(unsigned i=0;i<512;i++)input[i]=(i*37+11)&255;NvnTexture t={.layout=l,.target=1,.format=0x29,.width=64,.height=1,.depth=1,.levels=1,.stride=4096};
 for(unsigned flag=0x10;flag<=0x50;flag+=flag==0x10?0x30:0x10){t.flags=flag;memset(row,0,sizeof row);upload_rows(&t,input,row);CHECK(!memcmp(row,input,512));}
 t.flags=0;upload_rows(&t,input,gob);CHECK(memcmp(gob,input,512));CHECK(!memcmp(gob,input,16));unsigned zeros=0;for(unsigned i=0;i<512;i++)zeros+=gob[i]==0;CHECK(zeros>400);
 // Two pitched rows: copies meaningful bytes, never row padding.
 l.width=3;l.height=2;l.fmt=nvn_format_info(0x25);tegra_layout_init(&l);t.layout=l;t.flags=0x40;t.stride=32;for(unsigned i=0;i<64;i++)input[i]=i;upload_rows(&t,input,row);CHECK(!memcmp(row,input,12)&&!memcmp(row+12,input+32,12));CHECK(nvn_texture_row_span(&l,32,&size)&&size==64);CHECK(!nvn_texture_row_span(&l,11,&size)&&size==0);CHECK(!nvn_texture_row_span(&l,UINT64_MAX,&size)&&size==0);l.levels=2;CHECK(!nvn_texture_row_span(&l,32,&size));l.levels=1;l.layers=2;CHECK(!nvn_texture_row_span(&l,32,&size));l.layers=1;l.depth=2;CHECK(!nvn_texture_row_span(&l,32,&size));l.depth=1;l.width=0;CHECK(!nvn_texture_row_span(&l,32,&size));l.width=3;l.height=0;CHECK(!nvn_texture_row_span(&l,32,&size));CHECK(!nvn_texture_row_span(NULL,32,&size));CHECK(!nvn_texture_row_span(&l,32,NULL));
 // Compressed storage spans count format-block rows, not texel height.
 l=(TexLayout){.fmt=nvn_format_info(0x4c),.width=7,.height=5,.depth=1,.layers=1,.levels=1};CHECK(l.fmt->bw==4&&l.fmt->bh==4);CHECK(nvn_texture_row_span(&l,64,&size)&&size==128);
 _Alignas(16)uint8_t builder[128]={0};put32(builder,0xC,1);put32(builder,0x10,3);put32(builder,0x14,2);put32(builder,0x18,1);put32(builder,0x1c,1);put32(builder,0x20,0x25);put64(builder,0x30,32);Cpu cpu={0};
 for(unsigned flag=0x10;flag<=0x50;flag+=flag==0x10?0x30:0x10){put32(builder,8,flag);cpu.x[0]=(uintptr_t)builder;hle_nvnTextureBuilderGetStorageSize(&cpu);CHECK(cpu.x[0]==64);cpu.x[0]=(uintptr_t)builder;hle_nvnTextureBuilderGetStorageAlignment(&cpu);CHECK(cpu.x[0]==(flag&0x10?32:512));t.layout=(TexLayout){.fmt=nvn_format_info(0x25),.width=3,.height=2,.depth=1,.layers=1,.levels=1};t.target=1;t.flags=flag;t.stride=32;uint64_t guest=(uintptr_t)&t;cpu.x[0]=(uintptr_t)&guest;hle_nvnTextureGetStorageSize(&cpu);CHECK(cpu.x[0]==64);}
 put32(builder,0xc,10);put32(builder,0x10,17);put32(builder,0x14,0);put64(builder,0x30,UINT64_MAX);cpu.x[0]=(uintptr_t)builder;hle_nvnTextureBuilderGetStorageSize(&cpu);CHECK(cpu.x[0]==17*4);cpu.x[0]=(uintptr_t)builder;hle_nvnTextureBuilderGetStorageAlignment(&cpu);CHECK(cpu.x[0]==32);t.target=10;t.width=17;t.stride=UINT64_MAX;uint64_t guest=(uintptr_t)&t;cpu.x[0]=(uintptr_t)&guest;hle_nvnTextureGetStorageSize(&cpu);CHECK(cpu.x[0]==17*4);
 put32(builder,0xc,1);put32(builder,0x14,2);put32(builder,8,0x40);put64(builder,0x30,UINT64_MAX);CHECK(builder_storage_size((uintptr_t)builder)==0);put64(builder,0x30,32);put32(builder,0x1c,2);CHECK(builder_storage_size((uintptr_t)builder)==0);put32(builder,0x1c,1);put64(builder,0x30,32);put32(builder,0x10,3);put32(builder,8,0x40);guest=0;cpu.x[0]=(uintptr_t)&guest;cpu.x[1]=(uintptr_t)builder;hle_nvnTextureInitialize(&cpu);CHECK(cpu.x[0]==1&&guest);NvnTexture*created=(void*)(uintptr_t)guest;CHECK(created->flags==0x40&&nvn_texture_row_span(&created->layout,created->stride,&size)&&size==64);free(created);
 put64(builder,0x30,11);guest=123;cpu.x[0]=(uintptr_t)&guest;cpu.x[1]=(uintptr_t)builder;hle_nvnTextureInitialize(&cpu);CHECK(cpu.x[0]==0&&guest==0);
 put64(builder,0x30,32);put32(builder,8,0x10);cpu.x[0]=(uintptr_t)&guest;cpu.x[1]=(uintptr_t)builder;hle_nvnTextureInitialize(&cpu);CHECK(cpu.x[0]==1&&guest);free((void*)(uintptr_t)guest);
 printf("%u actual linear layout/query/upload assertions passed\n",checks);}
