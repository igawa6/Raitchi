/* Actual headstart policy/recording/hash and bounded BRX resolver. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "nvn/maxwell/shader.h"
static unsigned checks,writes;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define LOG(...) ((void)0)
typedef struct{uint64_t at;int n;uint32_t t[256];}Brx;
typedef struct{int nbrx;Brx brx[4];}Ctx;
#define RZ 255
static uint64_t bits(uint64_t w,int start,int n){return(w>>start)&((1ull<<n)-1);}
static int64_t sbits(uint64_t w,int start,int n){uint64_t x=bits(w,start,n);return(int64_t)(x^(1ull<<(n-1)))-(int64_t)(1ull<<(n-1));}
typedef struct{bool headstart;uint64_t code[2],cmax[2],csize[2],chash;const uint8_t*read_code[2];ShaderKey vkey,fkey;}Prog;
typedef struct{Prog*prog;uint64_t payload[2];}PipeKey;
static uint8_t record[0x1000];static size_t record_size;
static void hs_write(uint32_t type,const void*a,size_t na,const void*b,size_t nb,const void*c,size_t nc){CHECK(type==1||type==2);writes++;CHECK(na+nb+nc<=sizeof record);memcpy(record,a,na);if(nb)memcpy(record+na,b,nb);if(nc)memcpy(record+na+nb,c,nc);record_size=na+nb+nc;}
#include "headstart_external_production.h"
static void put64(void*p,uint64_t word){memcpy(p,&word,8);}
static void policy(void){
 uint8_t code[0x280]={0};CHECK(hs_stage_replayable(code,sizeof code,0));CHECK(!hs_stage_replayable(NULL,128,0));CHECK(!hs_stage_replayable(code,127,0));CHECK(hs_stage_replayable(NULL,0,0));CHECK(!hs_program_replayable(NULL,0,0,code,sizeof code,0));CHECK(hs_program_replayable(code,sizeof code,0,NULL,0,0));
 for(unsigned off=0;off<0x80;off+=8){put64(code+off,0xe250000000000000ull);CHECK(hs_stage_replayable(code,sizeof code,0));put64(code+off,0);}
 for(unsigned i=0;i<(sizeof code-0x80)/8;i++){put64(code+0x80+i*8,0xe250000000000000ull);CHECK(hs_stage_replayable(code,sizeof code,0)==(i%4==0));CHECK(hs_stage_replayable(code,sizeof code,16));put64(code+0x80+i*8,0);}
 uint8_t short_code[0x89]={0};CHECK(hs_stage_replayable(short_code+1,sizeof short_code-1,0));CHECK(hs_stage_replayable(short_code+1,sizeof short_code-2,0));
 Prog p={.code={(uintptr_t)code,0},.cmax={sizeof code-16,0},.csize={16,0},.chash=1};PipeKey key={.prog=&p};hs_record_program(&p);CHECK(writes==1);CHECK(record_size==sizeof(HsProgHeader)+sizeof code);HsProgHeader h;memcpy(&h,record,sizeof h);CHECK(h.csize[0]==16&&h.csize[1]==0);CHECK(hs_program_size_valid(record_size,&h));CHECK(!memcmp(record+sizeof h,code,sizeof code));hs_record_pipe(&key);CHECK(writes==2);
 put64(code+0x88,0xe250000000000000ull);hs_record_program(&p);hs_record_pipe(&key);CHECK(writes==4);p.csize[0]=0;hs_record_program(&p);hs_record_pipe(&key);CHECK(writes==4);
 uint8_t fs[0x100]={0};p.code[0]=(uintptr_t)fs;p.cmax[0]=sizeof fs;p.code[1]=(uintptr_t)code;p.cmax[1]=sizeof code;hs_record_program(&p);hs_record_pipe(&key);CHECK(writes==4);p.csize[1]=8;p.cmax[1]-=8;hs_record_program(&p);CHECK(writes==5);CHECK(record_size==sizeof h+sizeof fs+sizeof code);CHECK(!memcmp(record+sizeof h,fs,sizeof fs));CHECK(!memcmp(record+sizeof h+sizeof fs,code,sizeof code));
 CHECK(HS_MAGIC==0x3753484cu); // LHS6 and earlier identities cannot replay
 h=(HsProgHeader){.cmax={128,128},.csize={8,8}};CHECK(hs_program_size_valid(sizeof h+272,&h));h.csize[0]=UINT64_MAX;CHECK(!hs_program_size_valid(sizeof h+272,&h));h.csize[0]=8;h.cmax[1]=0;CHECK(!hs_program_size_valid(sizeof h+144,&h));
}
static void counterexample(void){
 uint8_t original[0x300]={0},replayed[0x300]={0};const size_t cmax=0x200;
 put64(original+0x80+5*8,0xef94001000070007ull);put64(original+0x80+6*8,0xe250000000000700ull|((uint64_t)(0x1000000-56)<<20));
 uint32_t table[3]={64,96,0};memcpy(original+cmax,table,sizeof table);memcpy(replayed,original,cmax);uint32_t next_shader_header=0x12345678;memcpy(replayed+cmax,&next_shader_header,4);
 Ctx live={0},wrong={0};uint8_t labels[64]={0};CHECK(brx_table(&live,(const uint64_t*)(original+0x80),6,48,(const uint32_t*)(original+cmax),sizeof table,labels));CHECK(live.nbrx==1&&live.brx[0].n==2);
 CHECK(!brx_table(&wrong,(const uint64_t*)(replayed+0x80),6,48,(const uint32_t*)(replayed+cmax),sizeof table,labels)&&wrong.nbrx==0);CHECK(!hs_stage_replayable(replayed,cmax,0));
 // LHS5 copies the declared table and restores exactly the same control flow.
 memcpy(replayed,original,cmax+sizeof table);Ctx fixed={0};CHECK(brx_table(&fixed,(const uint64_t*)(replayed+0x80),6,48,(const uint32_t*)(replayed+cmax),sizeof table,labels));CHECK(!memcmp(&live,&fixed,sizeof live));CHECK(hs_stage_replayable(replayed,cmax,sizeof table));
 // A complete table may end exactly at its declared span, without a sentinel.
 // Empty spans and partial words still reject; ASan guards every allocation.
 for(size_t n=0;n<sizeof table;n++){uint8_t*constants=malloc(n?n:1);if(n)memcpy(constants,table,n);Ctx bounded={0};bool complete=n==4||n==8;CHECK(brx_table(&bounded,(const uint64_t*)(original+0x80),6,48,(const uint32_t*)constants,n,labels)==complete);CHECK(bounded.nbrx==(complete?1:0));if(complete)CHECK(bounded.brx[0].n==(int)(n/4));free(constants);}
 put64(original+0x80+5*8,0xef94001000070007ull|((uint64_t)0xfffc<<20));Ctx offset={0};CHECK(!brx_table(&offset,(const uint64_t*)(original+0x80),6,48,table,sizeof table,labels));
}
static void mapped_spans(void){
 CHECK(hs_constants_valid(0x1040,128,16,0x1000,208));
 CHECK(!hs_constants_valid(0x1040,128,17,0x1000,208));
 CHECK(!hs_constants_valid(0xfff,128,16,0x1000,4096));
 CHECK(!hs_constants_valid(0x2000,128,16,0x1000,4096));
 CHECK(!hs_constants_valid(0x1040,UINT64_MAX,1,0x1000,4096));
 CHECK(!hs_constants_valid(0x1040,128,UINT64_MAX,0x1000,4096));
 CHECK(!hs_constants_valid(0x1040,0,16,0x1000,4096));
 CHECK(!hs_constants_valid(0x1040,128,16,0x1000,0));
 CHECK(!hs_constants_valid(UINT64_MAX,128,16,0x1000,4096));
}
static void identity(void){
 uint8_t v[256]={0},f[256]={0};ShaderKey vk={0},fk={0};uint64_t base=prog_content_hash(v,128,16,f,128,16,&vk,&fk);CHECK(base);
 for(unsigned i=128;i<144;i++){v[i]=1;CHECK(base!=prog_content_hash(v,128,16,f,128,16,&vk,&fk));v[i]=0;f[i]=1;CHECK(base!=prog_content_hash(v,128,16,f,128,16,&vk,&fk));f[i]=0;}
 CHECK(base!=prog_content_hash(v,136,8,f,128,16,&vk,&fk));CHECK(base!=prog_content_hash(v,128,16,f,136,8,&vk,&fk));fk.tex_uint=1;CHECK(base!=prog_content_hash(v,128,16,f,128,16,&vk,&fk));fk.tex_uint=0;
 // Allocator padding beyond the exact declared constants never affects identity.
 v[144]=1;f[144]=2;CHECK(base==prog_content_hash(v,128,16,f,128,16,&vk,&fk));
}
int main(void){policy();counterexample();mapped_spans();identity();printf("{\"state\":\"passed\",\"checks\":%u,\"actual_brx_counterexample\":true,\"complete_constants_replay\":true,\"bounded_table_reads\":true}\n",checks);return 0;}
