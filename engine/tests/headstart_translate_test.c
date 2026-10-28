/* End-to-end source parity for live and LHS5 compiler-constant snapshots. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return true;}
static void put64(void*p,uint64_t x){memcpy(p,&x,8);}
int main(int argc,char**argv){
 CHECK(argc==2);const size_t cmax=0x200;uint8_t original[0x20c]={0};
 for(size_t i=0;i<(cmax-0x80)/8;i++)if(i%4)put64(original+0x80+i*8,UINT64_C(0x50b0000000070f00));
 put64(original+0x80+5*8,UINT64_C(0xef94001000070007));
 put64(original+0x80+6*8,UINT64_C(0xe250000000000700)|((uint64_t)(0x1000000-56)<<20));
 put64(original+0x80+47*8,UINT64_C(0xe30000000007000f));
 const uint32_t table[]={64,96,0};memcpy(original+cmax,table,sizeof table);ShaderKey key={.glsl=1};
 ShaderInfo*live=maxwell_translate(original,cmax,STAGE_VERTEX,&key);CHECK(live&&live->msl);
 ShaderInfo*bounded=maxwell_translate_sized(original,cmax,sizeof table,STAGE_VERTEX,&key);CHECK(bounded&&bounded->msl);CHECK(!strcmp(live->msl,bounded->msl));
 uint8_t*copy=malloc(sizeof original);CHECK(copy);memcpy(copy,original,sizeof original);ShaderInfo*replay=maxwell_translate_sized(copy,cmax,sizeof table,STAGE_VERTEX,&key);CHECK(replay&&replay->msl);CHECK(!strcmp(live->msl,replay->msl));
 // A declared span ending at the final complete target has the same CFG as
 // the original-memory path reading its zero padding as a sentinel. Allocate
 // exactly the declared span so ASan also proves no sentinel read is needed.
 uint8_t*exact=malloc(cmax+8);CHECK(exact);memcpy(exact,original,cmax+8);ShaderInfo*ended=maxwell_translate_sized(exact,cmax,8,STAGE_VERTEX,&key);CHECK(ended&&ended->msl);CHECK(!strcmp(live->msl,ended->msl));maxwell_free(ended);free(exact);
 char path[1024];snprintf(path,sizeof path,"%s/headstart-brx.vert",argv[1]);FILE*f=fopen(path,"w");CHECK(f);fputs(replay->msl,f);CHECK(!fclose(f));maxwell_free(live);maxwell_free(bounded);maxwell_free(replay);free(copy);
 // Partial words fall back without reading beyond the declared constants;
 // complete entry boundaries can instead terminate the optimized table.
 for(size_t n=0;n<sizeof table;n++){uint8_t*b=malloc(cmax+n);CHECK(b);memcpy(b,original,cmax+n);ShaderInfo*s=maxwell_translate_sized(b,cmax,n,STAGE_VERTEX,&key);CHECK(s&&s->msl);maxwell_free(s);free(b);}
 printf("{\"state\":\"passed\",\"checks\":%u,\"original_bounded_replay_source_equal\":true}\n",checks);return 0;
}
