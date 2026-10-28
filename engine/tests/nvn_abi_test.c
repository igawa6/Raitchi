#include "adapters/title_adapter.h"
#include "runtime/aot_cpu.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const TitleAdapter las_title_adapter,eevee_title_adapter,echoes_title_adapter,wonder_title_adapter;
static const TitleAdapter *selected;
#define title_adapter_selected() selected
#define NVN(name) static void name(Cpu*c)
#define A(i) (c->x[i])
#define AP(i) ((void*)(uintptr_t)c->x[i])
#define G(T,b,o) (*(T*)((uint8_t*)(uintptr_t)(b)+(o)))
#define RET(v) (c->x[0]=(uint64_t)(v))
#define LOG(...) ((void)0)
static unsigned hangs,backends,preps,checks;
static const char *g_romfs_dir="owned-package";
static void rq_hang_watch_start(void){hangs++;}
static void backend_init(void){backends++;}
void shader_prep_boot(const char *path){if(path!=g_romfs_dir)abort();preps++;}
#include "nvn_abi_bodies.h"
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 const TitleAdapter *policies[]={NULL,&las_title_adapter,&eevee_title_adapter,&echoes_title_adapter,&wonder_title_adapter};
 for(unsigned i=0;i<5;i++){
  selected=policies[i];
  for(unsigned query=0;query<3;query++){
   struct {uint32_t before,value,after;} output={0x12345678,0xdeadbeef,0x87654321};Cpu cpu={0};
   cpu.x[0]=0x12340000;cpu.x[1]=query;cpu.x[2]=(uintptr_t)&output.value;
   nvnDeviceGetInteger(&cpu);CHECK(output.before==0x12345678&&output.after==0x87654321);
   CHECK(output.value==(query==0?(i==4?55u:53u):query==1?(i==4?15u:207u):14u));CHECK(cpu.x[0]==0x12340000);
  }
 }
 CHECK(device_integer(9999)==0);
 struct {uint64_t before;unsigned char builder[64];uint64_t after;} b;memset(&b,0x5a,sizeof b);Cpu cpu={0};cpu.x[0]=(uintptr_t)b.builder;
 nvnDeviceBuilderSetDefaults(&cpu);CHECK(*(uint32_t*)b.builder==0);
 cpu.x[1]=0;nvnDeviceBuilderSetFlags(&cpu);nvnDeviceBuilderGetFlags(&cpu);CHECK(cpu.x[0]==0);
 CHECK(b.before==UINT64_C(0x5a5a5a5a5a5a5a5a)&&b.after==b.before);
 for(unsigned i=4;i<64;i++)CHECK(b.builder[i]==0x5a);
 struct {uint64_t before;unsigned char device[0x3000];uint64_t after;} d;memset(&d,0x6b,sizeof d);
 cpu.x[0]=(uintptr_t)d.device;cpu.x[1]=(uintptr_t)b.builder;nvnDeviceInitialize(&cpu);CHECK(cpu.x[0]==1&&hangs==1&&backends==1&&preps==1);
 cpu.x[0]=(uintptr_t)d.device;nvnDeviceInitialize(&cpu);CHECK(cpu.x[0]==1&&hangs==1&&backends==1&&preps==1);
 CHECK(d.before==UINT64_C(0x6b6b6b6b6b6b6b6b)&&d.after==d.before);
 for(unsigned i=0;i<sizeof d.device;i++)CHECK(d.device[i]==0x6b);
 printf("state=passed checks=%u; actual NVN query/builder/device bodies, no GPU/guest execution\n",checks);return 0;
}
