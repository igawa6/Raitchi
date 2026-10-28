/* Synthetic Maxwell fixtures: production direct and legacy metadata/source. */
#define _POSIX_C_SOURCE 200809L
#include "direct_spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <shaderc/shaderc.h>
static unsigned checks,fixtures,rejections;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);exit(1);}}while(0)
bool shared_title_las(void){return false;}
static uint8_t blob[8192];static size_t nwords;
static void start(void){memset(blob,0,sizeof blob);nwords=0;}
static void instruction(uint64_t word){if(nwords%4==0)nwords++;CHECK(0x80+nwords*8+8<=sizeof blob);memcpy(blob+0x80+nwords*8,&word,8);nwords++;}
static size_t end(void){instruction(UINT64_C(0xe30000000007000f));return 0x80+nwords*8;}
static uint64_t ld(unsigned dst,unsigned address,unsigned count){return UINT64_C(0xefd8000000070000)|dst|(UINT64_C(255)<<8)|((uint64_t)address<<20)|((uint64_t)(count-1)<<47)|(UINT64_C(255)<<39);}
static uint64_t st(unsigned src,unsigned address,unsigned count){return UINT64_C(0xeff0000000070000)|src|(UINT64_C(255)<<8)|((uint64_t)address<<20)|((uint64_t)(count-1)<<47)|(UINT64_C(255)<<39);}
static uint64_t mov(unsigned dst,uint32_t bits){return UINT64_C(0x010000000007f000)|dst|((uint64_t)bits<<20);}
static uint64_t alu(uint64_t opcode,unsigned dst,unsigned a,unsigned b){return opcode|dst|((uint64_t)a<<8)|((uint64_t)b<<20);}
static void save(const char*dir,const char*name,const char*ext,const void*bytes,size_t count){char path[2048];CHECK(snprintf(path,sizeof path,"%s/%s.%s",dir,name,ext)>0);FILE*f=fopen(path,"wb");CHECK(f&&fwrite(bytes,1,count,f)==count&&!fclose(f));}
static void emit(const char*dir,FILE*index,const char*name,size_t bytes,int stage,ShaderKey key){
 MaxwellSpirvOutput direct={0};MaxwellSpirvStatus status=maxwell_direct_spirv(blob,bytes,0,stage,&key,&direct);
 if(status!=MAXWELL_SPIRV_OK){fprintf(stderr,"fixture %s rejected status%d at%zu: %s\n",name,status,direct.instruction_offset,direct.reason);exit(1);}
 CHECK(direct.words&&direct.word_count>=5&&direct.words[0]==0x07230203&&!direct.info.msl);
 ShaderInfo *legacy=maxwell_translate_sized(blob,bytes,0,stage,&key);if(!legacy||!legacy->msl){fprintf(stderr,"legacy %s: %s\n",name,legacy?legacy->error:"allocation");exit(1);}
 CHECK(direct.info.stage==legacy->stage&&direct.info.cbuf_mask==legacy->cbuf_mask&&direct.info.cbuf_dynamic==legacy->cbuf_dynamic);
 CHECK(direct.info.attr_mask==legacy->attr_mask&&direct.info.out_mask==legacy->out_mask&&direct.info.writes_depth==legacy->writes_depth);
 CHECK(!direct.info.ntex&&!direct.info.uses_gmem&&!direct.info.uses_tint&&direct.info.uses_rscale==legacy->uses_rscale);
 for(unsigned i=0;i<18;i++)CHECK(direct.info.cbuf_size[i]==legacy->cbuf_size[i]);
 shaderc_compiler_t compiler=shaderc_compiler_initialize();CHECK(compiler);shaderc_compile_options_t options=shaderc_compile_options_initialize();CHECK(options);shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_1);shaderc_compile_options_set_optimization_level(options,shaderc_optimization_level_zero);
 shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,legacy->msl,strlen(legacy->msl),stage==STAGE_VERTEX?shaderc_vertex_shader:shaderc_fragment_shader,name,"main",options);
 if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success){fprintf(stderr,"%s",shaderc_result_get_error_message(result));exit(1);}
 save(dir,name,"legacy.spv",shaderc_result_get_bytes(result),shaderc_result_get_length(result));shaderc_result_release(result);shaderc_compile_options_release(options);shaderc_compiler_release(compiler);
 save(dir,name,"direct.spv",direct.words,direct.word_count*4);save(dir,name,stage==STAGE_VERTEX?"vert":"frag",legacy->msl,strlen(legacy->msl));save(dir,name,"maxwell",blob,bytes);
 fprintf(index,"%s %d %u %u %u %u %u %u %u\n",name,stage,key.rt_kind[0],key.attr_kind[1],direct.info.out_mask,direct.info.writes_depth,key.flip_x,key.flip_y,key.depth_minus_one);
 maxwell_free(legacy);maxwell_direct_spirv_free(&direct);CHECK(!direct.words&&!direct.word_count);fixtures++;
}
static void reject(size_t bytes,int stage,ShaderKey *key){MaxwellSpirvOutput out={0};MaxwellSpirvStatus status=maxwell_direct_spirv(blob,bytes,0,stage,key,&out);CHECK(status!=MAXWELL_SPIRV_OK);CHECK(!out.words&&!out.word_count&&out.reason[0]);maxwell_direct_spirv_free(&out);rejections++;}
static uint64_t now(void){struct timespec ts;CHECK(!clock_gettime(CLOCK_MONOTONIC,&ts));return(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;}
/* Independent scalar oracle: C f32/fmaf, then explicit exponent-bit flush.
 * It deliberately does not reuse direct emitter operations or legacy GLSL. */
static uint32_t flush_bits(uint32_t bits){return(bits&0x7f800000u)?bits:bits&0x80000000u;}
static float as_float(uint32_t bits){float value;memcpy(&value,&bits,4);return value;}
static uint32_t as_uint(float value){uint32_t bits;memcpy(&bits,&value,4);return bits;}
static uint32_t ftz_reference(unsigned family,uint32_t a,uint32_t b,uint32_t c,bool sat){
 volatile float x=as_float(flush_bits(a)),y=as_float(flush_bits(b)),z=as_float(flush_bits(c));
 float value=family==0?x+y:family==1?x*y:fmaf(x,y,z);
 value=as_float(flush_bits(as_uint(value)));
 if(sat){if(isnan(value)||value<0)value=0;else if(value>1)value=1;}
 return as_uint(value);
}
static uint64_t immediate20(uint32_t bits){return((uint64_t)((bits>>12)&0x7ffff)<<20)|((uint64_t)(bits>>31)<<56);}
static void ftz_fixtures(const char*dir,FILE*index){
 for(unsigned form=0;form<13;form++)for(unsigned variant=0;variant<(form>=8?11u:9u);variant++){
  unsigned family=form<4?0:form<8?1:2;bool sat=variant==7||variant==8;
  uint32_t a=1,b=family?0x71800000:0,c=0;
  if(variant==1){a=0x80000001;b=family?0x71800000:0x80000000;c=0x80000000;}
  if(variant==2){a=family?0x00800000:0x00800001;b=family?0x3f000000:0x80800000;}
  if(variant==3){a=family?0x80800000:0x80800001;b=family?0x3f000000:0x00800000;c=0x80000000;}
  if(variant==4){a=0x80000000;b=family?0x3f800000:0x80000000;c=0x80000000;}
  if(variant==5){a=0x7f800000;b=0x3f800000;c=0xbf800000;}
  if(variant==6){a=0x7fc00000;b=0x3f800000;}
  if(variant==7){a=0x40000000;b=family?0x40000000:0x3f800000;c=0xbf000000;}
  if(variant==8){a=0x7fc00000;b=0x3f800000;c=0;}
  if(variant==9){a=0xbf800000;b=0x3f800000;c=0x3f800000;}
  if(variant==10){a=0x80000000;b=0x3f800000;c=0;}
  start();uint32_t mask=15;memcpy(blob+0x78,&mask,4);instruction(mov(0,a));instruction(mov(1,b));instruction(mov(2,c));instruction(mov(3,0x3f800000));uint64_t word=0;uint32_t cb=b;
  switch(form){
  case 0:word=alu(UINT64_C(0x5c58000000070000),0,0,1)|(UINT64_C(1)<<44);break;
  case 1:word=UINT64_C(0x4c58000000070000)|(UINT64_C(1)<<44);break;
  case 2:word=UINT64_C(0x3858000000070000)|immediate20(b)|(UINT64_C(1)<<44);break;
  case 3:word=UINT64_C(0x0800000000070000)|((uint64_t)b<<20)|(UINT64_C(1)<<55);break;
  case 4:word=alu(UINT64_C(0x5c68000000070000),0,0,1)|(UINT64_C(1)<<44);break;
  case 5:word=UINT64_C(0x4c68000000070000)|(UINT64_C(1)<<44);break;
  case 6:word=UINT64_C(0x3868000000070000)|immediate20(b)|(UINT64_C(1)<<44);break;
  case 7:word=UINT64_C(0x1e00000000070000)|((uint64_t)b<<20)|(UINT64_C(1)<<53);break;
  case 8:word=alu(UINT64_C(0x5980000000070000),0,0,1)|(UINT64_C(2)<<39)|(UINT64_C(1)<<53);break;
  case 9:word=UINT64_C(0x4980000000070000)|(UINT64_C(2)<<39)|(UINT64_C(1)<<53);break;
  case 10:word=UINT64_C(0x5180000000070000)|(UINT64_C(1)<<39)|(UINT64_C(1)<<53);cb=c;break;
  case 11:word=UINT64_C(0x3280000000070000)|immediate20(b)|(UINT64_C(2)<<39)|(UINT64_C(1)<<53);break;
  case 12:instruction(mov(0,c));instruction(mov(4,a));word=UINT64_C(0x0c00000000070000)|((uint64_t)b<<20)|(UINT64_C(4)<<8)|(UINT64_C(1)<<53);break;
  }
  if(sat){if(form==3){/* FADD32 has no admitted SAT field. Keep its normalFTZ case. */sat=false;}else word|=UINT64_C(1)<<(form==7||form==12?55:50);}
  instruction(word);size_t size=end();ShaderKey key={.glsl=1};key.rt_kind[0]=2;char name[64];snprintf(name,sizeof name,"fragment-ftz-%u-%u",form,variant);emit(dir,index,name,size,STAGE_FRAGMENT,key);
  uint8_t uniforms[65536]={0};memcpy(uniforms,&cb,4);save(dir,name,"ubo",uniforms,sizeof uniforms);
  uint32_t expected[4]={ftz_reference(family,a,b,c,sat),b,c,0x3f800000};save(dir,name,"oracle",expected,sizeof expected);
 }
}
static void benchmark(size_t bytes,ShaderKey *key){
 enum{ITER=25};uint64_t a=now();for(unsigned i=0;i<ITER;i++){MaxwellSpirvOutput out={0};CHECK(maxwell_direct_spirv(blob,bytes,0,STAGE_VERTEX,key,&out)==MAXWELL_SPIRV_OK);maxwell_direct_spirv_free(&out);}uint64_t direct=now()-a;
 shaderc_compiler_t compiler=shaderc_compiler_initialize();CHECK(compiler);shaderc_compile_options_t options=shaderc_compile_options_initialize();CHECK(options);shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_1);shaderc_compile_options_set_optimization_level(options,shaderc_optimization_level_zero);
 a=now();for(unsigned i=0;i<ITER;i++){ShaderInfo *s=maxwell_translate_sized(blob,bytes,0,STAGE_VERTEX,key);CHECK(s&&s->msl);shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,s->msl,strlen(s->msl),shaderc_vertex_shader,"synthetic","main",options);if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success){fprintf(stderr,"%s",shaderc_result_get_error_message(result));exit(1);}shaderc_result_release(result);maxwell_free(s);}uint64_t legacy=now()-a;shaderc_compile_options_release(options);shaderc_compiler_release(compiler);
 printf("{\"state\":\"passed\",\"checks\":%u,\"fixtures\":%u,\"rejections\":%u,\"benchmark_iterations\":%u,\"direct_frontend_ns\":%llu,\"legacy_translation_shaderc_ns\":%llu,\"synthetic_only\":true}\n",checks,fixtures,rejections,ITER,(unsigned long long)direct,(unsigned long long)legacy);
}
int main(int argc,char**argv){CHECK(argc==2);char path[2048];snprintf(path,sizeof path,"%s/index.txt",argv[1]);FILE*index=fopen(path,"w");CHECK(index);maxwell_precise_pos=1;maxwell_shader_int64=0;maxwell_scalar_cbuf=0;
 ShaderKey key={.glsl=1};size_t bytes;
 for(unsigned flags=0;flags<8;flags++){start();instruction(ld(0,0x80,4));instruction(st(0,0x70,4));instruction(ld(4,0x90,4));instruction(st(4,0x80,4));bytes=end();key=(ShaderKey){.glsl=1,.flip_x=!!(flags&1),.flip_y=!!(flags&2),.depth_minus_one=!!(flags&4)};char name[64];snprintf(name,sizeof name,"vertex-transform-%u",flags);emit(argv[1],index,name,bytes,STAGE_VERTEX,key);}
 for(unsigned kind=1;kind<3;kind++){start();instruction(ld(0,0x80,4));instruction(st(0,0x70,4));instruction(ld(4,0x90,4));instruction(st(4,0x80,4));bytes=end();key=(ShaderKey){.glsl=1};key.attr_kind[1]=(uint8_t)kind;char name[64];snprintf(name,sizeof name,"vertex-kind-%u",kind);emit(argv[1],index,name,bytes,STAGE_VERTEX,key);}
 start();instruction(ld(0,0x80,4));instruction((UINT64_C(0x4c98000000070002)|(UINT64_C(15)<<39)));instruction(st(0,0x70,4));instruction(ld(4,0x90,4));instruction(st(4,0x80,4));bytes=end();key=(ShaderKey){.glsl=1};emit(argv[1],index,"vertex-cbuf",bytes,STAGE_VERTEX,key);
 start();uint32_t mask=3;memcpy(blob+0x78,&mask,4);instruction(ld(0,0x80,2));bytes=end();key=(ShaderKey){.glsl=1,.vs_out_mask=1};emit(argv[1],index,"fragment-varying",bytes,STAGE_FRAGMENT,key);
 for(unsigned op=0;op<4;op++){start();mask=15;memcpy(blob+0x78,&mask,4);instruction(mov(0,0x3e800000));instruction(mov(1,0x3f400000));instruction(mov(2,0xbf000000));instruction(mov(3,0x3f800000));if(op==0)instruction(alu(UINT64_C(0x5c58000000070000),0,0,1));if(op==1)instruction(alu(UINT64_C(0x5c68000000070000),0,0,1));if(op==2)instruction(alu(UINT64_C(0x5980000000070000),0,0,1)|(UINT64_C(2)<<39));if(op==3){instruction((UINT64_C(0x4c98000000070000)|(UINT64_C(15)<<39)));instruction(UINT64_C(0xef90000000070001)|(UINT64_C(255)<<8)|(UINT64_C(4)<<20)|(UINT64_C(4)<<48));}bytes=end();key=(ShaderKey){.glsl=1};key.rt_kind[0]=2;char name[64];snprintf(name,sizeof name,"fragment-alu-%u",op);emit(argv[1],index,name,bytes,STAGE_FRAGMENT,key);}
 for(unsigned op=0;op<25;op++){start();mask=15;memcpy(blob+0x78,&mask,4);instruction(mov(0,0x3e800000));instruction(mov(1,0x3f400000));instruction(mov(2,0xbf000000));instruction(mov(3,0x3f800000));
 uint64_t word=0;
 switch(op){
 case 0:word=UINT64_C(0x5c98000000070000)|(UINT64_C(15)<<39)|(UINT64_C(1)<<20);break;
 case 1:word=UINT64_C(0x3898000000070000)|(UINT64_C(15)<<39)|(UINT64_C(0x12345)<<20);break;
 case 2:word=UINT64_C(0x4c58000000070000);break;
 case 3:word=UINT64_C(0x3858000000070000)|(UINT64_C(0x3e000)<<20);break;
 case 4:word=UINT64_C(0x0800000000070000)|(UINT64_C(0x3e000000)<<20);break;
 case 5:word=UINT64_C(0x4c68000000070000);break;
 case 6:word=UINT64_C(0x3868000000070000)|(UINT64_C(0x3e000)<<20);break;
 case 7:word=UINT64_C(0x1e00000000070000)|(UINT64_C(0x3e000000)<<20);break;
 case 8:word=UINT64_C(0x4980000000070000)|(UINT64_C(2)<<39);break;
 case 9:word=UINT64_C(0x5180000000070000)|(UINT64_C(1)<<39);break;
 case 10:word=UINT64_C(0x3280000000070000)|(UINT64_C(0x3e000)<<20)|(UINT64_C(2)<<39);break;
 case 11:word=UINT64_C(0x0c00000000070000)|(UINT64_C(0x3e000000)<<20);break;
 case 12:word=UINT64_C(0x1e00000000070000)|(UINT64_C(0x41000000)<<20)|(UINT64_C(1)<<55);break;
 case 13:word=UINT64_C(0x1e00000000070000)|(UINT64_C(0xc1000000)<<20)|(UINT64_C(1)<<55);break;
 case 14:instruction(mov(0,0x7fc00000));word=UINT64_C(0x1e00000000070000)|(UINT64_C(0x41000000)<<20)|(UINT64_C(1)<<55);break;
 case 15:instruction(mov(0,0x7f800000));word=UINT64_C(0x1e00000000070000)|(UINT64_C(0x41000000)<<20)|(UINT64_C(1)<<55);break;
 case 16:word=UINT64_C(0x0800000000070000)|(UINT64_C(0x3e000000)<<20)|(UINT64_C(1)<<56)|(UINT64_C(1)<<57);break;
 case 17:word=UINT64_C(0x0c00000000070000)|(UINT64_C(0x3e000000)<<20)|(UINT64_C(1)<<56)|(UINT64_C(1)<<57)|(UINT64_C(1)<<55);break;
 case 18:word=alu(UINT64_C(0x5c58000000070000),0,0,1)|(UINT64_C(1)<<48)|(UINT64_C(1)<<46)|(UINT64_C(1)<<45)|(UINT64_C(1)<<49)|(UINT64_C(1)<<50);break;
 case 19:word=alu(UINT64_C(0x5c68000000070000),0,0,1)|(UINT64_C(1)<<48)|(UINT64_C(1)<<50);break;
 case 20:word=alu(UINT64_C(0x5980000000070000),0,0,1)|(UINT64_C(2)<<39)|(UINT64_C(1)<<48)|(UINT64_C(1)<<49)|(UINT64_C(1)<<50);break;
 case 21:word=UINT64_C(0xef90000000070000)|(UINT64_C(255)<<8)|(UINT64_C(5)<<48);break;
 case 22:word=UINT64_C(0x4c98000000070000)|(UINT64_C(15)<<39)|(UINT64_C(5)<<34)|(UINT64_C(4)<<20);break;
 case 23:instruction(mov(0,0x80000000));instruction(mov(1,0x80000000));word=alu(UINT64_C(0x5c58000000070000),0,0,1);break;
 case 24:instruction(mov(1,0x80000000));word=alu(UINT64_C(0x5c68000000070000),0,0,1);break;
 }
 instruction(word);bytes=end();key=(ShaderKey){.glsl=1};key.rt_kind[0]=2;char name[64];snprintf(name,sizeof name,"fragment-extra-%u",op);emit(argv[1],index,name,bytes,STAGE_FRAGMENT,key);}
 for(unsigned mode=0;mode<4;mode++){start();mask=3;memcpy(blob+0x78,&mask,4);instruction(mov(4,0x40000000));instruction(UINT64_C(0xe00000000007ff00)|(UINT64_C(0x80)<<28)|((uint64_t)mode<<54)|(UINT64_C(4)<<20));instruction(UINT64_C(0xe00000000007ff01)|(UINT64_C(0x84)<<28)|((uint64_t)mode<<54)|(UINT64_C(4)<<20));bytes=end();key=(ShaderKey){.glsl=1,.vs_out_mask=1};char name[64];snprintf(name,sizeof name,"fragment-ipa-%u",mode);emit(argv[1],index,name,bytes,STAGE_FRAGMENT,key);}
 start();mask=0;memcpy(blob+0x78,&mask,4);uint32_t depth_output=2;memcpy(blob+0x7c,&depth_output,4);instruction(mov(1,0x3ec00000));bytes=end();key=(ShaderKey){.glsl=1};emit(argv[1],index,"fragment-depth",bytes,STAGE_FRAGMENT,key);
 start();mask=3;memcpy(blob+0x78,&mask,4);instruction(ld(0,0x80,2));bytes=end();key=(ShaderKey){.glsl=1};emit(argv[1],index,"fragment-missing-varying",bytes,STAGE_FRAGMENT,key);
 for(unsigned mirrored=0;mirrored<2;mirrored++){start();mask=15;memcpy(blob+0x78,&mask,4);instruction(ld(0,0x70,4));bytes=end();key=(ShaderKey){.glsl=1,.flip_x=mirrored!=0};key.rt_kind[0]=2;char name[64];snprintf(name,sizeof name,"fragment-fragcoord-%u",mirrored);emit(argv[1],index,name,bytes,STAGE_FRAGMENT,key);}
 start();mask=1;memcpy(blob+0x78,&mask,4);instruction(ld(0,0x3fc,1));bytes=end();key=(ShaderKey){.glsl=1};key.rt_kind[0]=2;emit(argv[1],index,"fragment-frontface",bytes,STAGE_FRAGMENT,key);
 start();mask=15;memcpy(blob+0x78,&mask,4);instruction(mov(0,0xffffffff));instruction(mov(1,1));instruction(mov(2,0xfffffffe));instruction(mov(3,0x7fffffff));bytes=end();key=(ShaderKey){.glsl=1};key.rt_kind[0]=1;emit(argv[1],index,"fragment-signed-output",bytes,STAGE_FRAGMENT,key);
 start();mask=15;memcpy(blob+0x78,&mask,4);instruction(mov(0,0x3e800000));bytes=end();key=(ShaderKey){.glsl=1};reject(bytes,STAGE_FRAGMENT,&key); // tint cannot silently disappear
 start();instruction(UINT64_C(0xe24000000007000f));bytes=end();reject(bytes,STAGE_VERTEX,&key); // branch
 start();instruction(UINT64_C(0xd000000000070000));bytes=end();reject(bytes,STAGE_FRAGMENT,&key); // texture
 start();instruction(UINT64_C(0x0100000000000000));bytes=end();reject(bytes,STAGE_VERTEX,&key); // conditional predicate
 start();instruction((UINT64_MAX&~(UINT64_C(15)<<16))|(UINT64_C(7)<<16));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 start();instruction(UINT64_C(0xe300000000070000));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 start();instruction(UINT64_C(0xe30000000007002f));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 start();instruction(UINT64_C(0xe33000000007000f));bytes=end();reject(bytes,STAGE_FRAGMENT,&key);
 start();instruction(UINT64_C(0xeed0000000070000));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 start();instruction(UINT64_C(0xef94000000070000)|(UINT64_C(1)<<8));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 start();instruction(st(0,0x2c0,1));bytes=end();reject(bytes,STAGE_VERTEX,&key);
 reject(0,STAGE_VERTEX,&key);reject(0x7f,STAGE_VERTEX,&key);reject(bytes,STAGE_COMPUTE,&key);reject(bytes,100,&key);
 start();bytes=end();key=(ShaderKey){.glsl=1};emit(argv[1],index,"fragment-empty",bytes,STAGE_FRAGMENT,key);
 {MaxwellSpirvOutput empty={0};CHECK(maxwell_direct_spirv(NULL,bytes,0,STAGE_VERTEX,&key,&empty)==MAXWELL_SPIRV_INVALID&&!empty.words&&empty.reason[0]);maxwell_direct_spirv_free(&empty);CHECK(maxwell_direct_spirv(blob,bytes,0,STAGE_VERTEX,&key,NULL)==MAXWELL_SPIRV_INVALID);}
 reject(bytes+1,STAGE_VERTEX,&key);reject(16u*1024u*1024u+8,STAGE_VERTEX,&key);key.attr_kind[31]=3;reject(bytes,STAGE_VERTEX,&key);key.attr_kind[31]=0;key.rt_kind[7]=3;reject(bytes,STAGE_FRAGMENT,&key);key=(ShaderKey){.glsl=1};
 ftz_fixtures(argv[1],index);
 CHECK(!fclose(index));start();instruction(ld(0,0x80,4));instruction(st(0,0x70,4));bytes=end();key=(ShaderKey){.glsl=1};benchmark(bytes,&key);return 0;
}
