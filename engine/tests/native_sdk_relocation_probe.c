/* Synthetic-resource differential against the independently verified CPU3
 * descriptor entry. Original bytes/addresses/body stay in ignored metadata. */
#define _GNU_SOURCE
#include "native_sdk_fastpath.h"
#include "module_abi.h"
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define FILE_BYTES 65536u
#define SECTION_COUNT 3u
#define ENTRY_COUNT 8u
static unsigned checks,failures,cases,aot_calls,accelerator_calls,second_calls,rejections,abi_failures;
#define CHECK(x) do{checks++;if(!(x))failures++;}while(0)
typedef struct Section {uint64_t base;uint32_t position,size;int32_t first,count;} Section;
typedef struct Entry {uint32_t position;uint16_t groups;uint8_t pointers,gap;} Entry;
_Static_assert(sizeof(Section)==24&&sizeof(Entry)==8,"resource layout");
static uint64_t host_tls(void){
#ifdef __aarch64__
 uint64_t value;__asm__ volatile("mrs %0,tpidr_el0":"=r"(value));return value;
#else
 return 0;
#endif
}
static uint64_t host_x18(void){
#ifdef __aarch64__
 uint64_t value;__asm__ volatile("mov %0,x18":"=r"(value));return value;
#else
 return 0;
#endif
}
static void put32(unsigned char *p,uint32_t v){memcpy(p,&v,4);}
static void put64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static void seed_cpu(Cpu *cpu,uintptr_t table,uintptr_t stack,unsigned salt){
 memset(cpu,0x5a^salt,sizeof *cpu);
 for(unsigned i=0;i<32;i++){cpu->x[i]=UINT64_C(0x13579bdf24680000)^((uint64_t)i<<32)^salt;cpu->v[i]=((__uint128_t)(UINT64_C(0x9988776655443322)^i)<<64)|(UINT64_C(0x1020304050607080)^salt^i);}
 cpu->x[0]=table;cpu->sp=stack;cpu->pc=0x10000;cpu->nzcv=0xa0000000;cpu->fpcr=0x01400000;cpu->fpsr=0x10;cpu->thread=NULL;
}
static int abi_ok(const Cpu *cpu,const Cpu *before){
 if(cpu->x[0]!=before->x[0]||cpu->sp!=before->sp||cpu->pc!=before->pc)return 0;
 for(unsigned i=19;i<32;i++)if(cpu->x[i]!=before->x[i])return 0;
 /* Original uses caller-saved X1..X18 and NZCV. They are intentionally not
  * compared with a C accelerator's preserved scratch register image. */
 return !memcmp(&cpu->tpidr,&before->tpidr,sizeof *cpu-offsetof(Cpu,tpidr));
}
static void build(unsigned char *file,unsigned table_offset,unsigned pointers,unsigned groups,unsigned gap,unsigned external,int skip,int special){
 memset(file,0xa5,FILE_BYTES);uint16_t flags=0x5a54;memcpy(file+0x14,&flags,2);
 unsigned char *table=file+table_offset;put32(table+4,table_offset);put32(table+8,SECTION_COUNT);
 Section sections[SECTION_COUNT]={{0,0,FILE_BYTES,1,2},{0,0x2000,FILE_BYTES,4,1},{0,0x4000,FILE_BYTES,6,2}};
 if(external)for(unsigned i=0;i<SECTION_COUNT;i++)sections[i].base=(uintptr_t)file+FILE_BYTES-512-i*64;
 if(skip){sections[0].count=skip==1?0:-3;sections[0].first=-7;}
 memcpy(table+16,sections,sizeof sections);Entry entries[ENTRY_COUNT];
 for(unsigned i=0;i<ENTRY_COUNT;i++)entries[i]=(Entry){.position=4096+i*8192+(table_offset&1),.groups=1,.pointers=2,.gap=1};
 entries[1].pointers=(uint8_t)pointers;entries[1].groups=(uint16_t)groups;entries[1].gap=(uint8_t)gap;
 entries[2].pointers=3;entries[2].groups=2;entries[2].gap=3;entries[4].pointers=5;entries[4].groups=1;entries[6].pointers=1;entries[6].groups=4;entries[6].gap=2;entries[7].pointers=4;entries[7].groups=2;entries[7].gap=0;
 if(special==2)entries[1]=(Entry){.position=32,.groups=2,.pointers=1,.gap=255};
 memcpy(table+16+sizeof sections,entries,sizeof entries);
 const unsigned active[]={1,2,4,6,7};
 for(unsigned k=0;k<sizeof active/sizeof active[0];k++){
  unsigned index=active[k];const Entry *entry=&entries[index];
  for(unsigned g=0;g<entry->groups;g++)for(unsigned j=0;j<entry->pointers;j++){
   size_t at=entry->position+(g*(entry->pointers+entry->gap)+j)*8;uint64_t value=(j+g)%4?0x180+index*16+j*3+g:0;
   if(special==1&&index==1&&j==1&&g==0){uint64_t addend=sections[0].base?sections[0].base-sections[0].position:(uintptr_t)file;value=UINT64_MAX-addend+1;}
   if(at+8>FILE_BYTES)abort();put64(file+at,value);
  }
 }
}
static void differential(GuestFn original,unsigned char *file,unsigned char *stack,size_t stack_size,unsigned offset,Cpu *cpu,Cpu *before){
 unsigned char *input=malloc(FILE_BYTES),*first=malloc(FILE_BYTES),*second=malloc(FILE_BYTES);CHECK(input&&first&&second);if(!input||!first||!second)abort();memcpy(input,file,FILE_BYTES);
 uint64_t tls=host_tls(),x18=host_x18(),fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
 seed_cpu(cpu,(uintptr_t)(file+offset),(uintptr_t)(stack+stack_size-64),cases);memcpy(before,cpu,sizeof *cpu);original(cpu);aot_calls++;int abi=!abi_ok(cpu,before);abi_failures+=abi;CHECK(!abi);CHECK(host_tls()==tls&&host_x18()==x18&&rt_get_fpcr()==fpcr&&rt_get_fpsr()==fpsr);memcpy(first,file,FILE_BYTES);
 seed_cpu(cpu,(uintptr_t)(file+offset),(uintptr_t)(stack+stack_size-64),cases);memcpy(before,cpu,sizeof *cpu);original(cpu);aot_calls++;abi=!abi_ok(cpu,before);abi_failures+=abi;CHECK(!abi);memcpy(second,file,FILE_BYTES);
 memcpy(file,input,FILE_BYTES);NativeSdkRelocationResult result={0};CHECK(native_sdk_relocate_table((uintptr_t)(file+offset),&result)==NATIVE_SDK_RELOCATION_OK);accelerator_calls++;CHECK(!memcmp(file,first,FILE_BYTES));CHECK(result.pointers==result.fixed_pointers+result.null_pointers);
 CHECK(native_sdk_relocate_table((uintptr_t)(file+offset),&result)==NATIVE_SDK_RELOCATION_OK);accelerator_calls++;second_calls++;CHECK(!memcmp(file,second,FILE_BYTES));CHECK(host_tls()==tls&&host_x18()==x18&&rt_get_fpcr()==fpcr&&rt_get_fpsr()==fpsr);
 cases++;free(input);free(first);free(second);
}
static void negative_cases(unsigned char *file,unsigned offset,size_t page){
 unsigned char *snapshot=malloc(FILE_BYTES);CHECK(snapshot!=NULL);if(!snapshot)abort();
 for(unsigned mode=0;mode<8;mode++){
  build(file,offset,5,3,2,0,0,0);unsigned char *table=file+offset,*section2=table+16+2*sizeof(Section),*entry7=table+16+SECTION_COUNT*sizeof(Section)+7*sizeof(Entry);
  if(mode==0)put32(section2+16,UINT32_MAX);
  if(mode==1){put32(section2+16,INT32_MAX);put32(section2+20,2);}
  if(mode==2)put32(table+8,INT32_MAX);
  if(mode==3)put32(entry7,offset);
  if(mode==4)put32(entry7,0x14);
  if(mode==5)put32(entry7,FILE_BYTES);
  if(mode==6)put32(table+4,offset+(uint32_t)page);
  if(mode==7){put32(entry7,FILE_BYTES-page);if(mprotect(file+FILE_BYTES-page,page,PROT_READ))abort();}
  memcpy(snapshot,file,FILE_BYTES);NativeSdkRelocationResult result;
  CHECK(native_sdk_relocate_table((uintptr_t)table,&result)!=NATIVE_SDK_RELOCATION_OK);CHECK(!memcmp(file,snapshot,FILE_BYTES));rejections++;
  if(mode==7&&mprotect(file+FILE_BYTES-page,page,PROT_READ|PROT_WRITE))abort();
 }
 CHECK(native_sdk_relocate_table(0,NULL)==NATIVE_SDK_RELOCATION_ARGUMENT);rejections++;free(snapshot);
}
static int run_cases(GuestFn original,char *output,size_t capacity){
 checks=failures=cases=aot_calls=accelerator_calls=second_calls=rejections=abi_failures=0;
 size_t page=(size_t)sysconf(_SC_PAGESIZE);unsigned char *mapping=mmap(NULL,FILE_BYTES+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0),*stack_mapping=mmap(NULL,65536+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);Cpu *cpu=NULL,*before=NULL;
 if(mapping==MAP_FAILED||stack_mapping==MAP_FAILED||mprotect(mapping+page,FILE_BYTES,PROT_READ|PROT_WRITE)||mprotect(stack_mapping+page,65536,PROT_READ|PROT_WRITE)||posix_memalign((void**)&cpu,16,sizeof *cpu)||posix_memalign((void**)&before,16,sizeof *before))return -1;
 unsigned char *file=mapping+page,*stack=stack_mapping+page;const unsigned pointer_counts[]={0,1,3,4,5,255},group_counts[]={0,1,3};
 for(unsigned p=0;p<6;p++)for(unsigned g=0;g<3;g++)for(unsigned gap=0;gap<2;gap++)for(unsigned external=0;external<2;external++)for(unsigned unaligned=0;unaligned<2;unaligned++){
  unsigned offset=256+unaligned*3;build(file,offset,pointer_counts[p],group_counts[g],gap*2,external,0,0);differential(original,file,stack,65536,offset,cpu,before);
 }
 for(unsigned special=1;special<=2;special++){build(file,256,5,3,2,1,0,special);differential(original,file,stack,65536,256,cpu,before);}
 for(unsigned skip=1;skip<=2;skip++){build(file,256,5,3,2,1,(int)skip,0);differential(original,file,stack,65536,256,cpu,before);}
 for(unsigned empty=0;empty<2;empty++){build(file,256,5,3,2,0,0,0);put32(file+256+8,empty?UINT32_MAX:0);differential(original,file,stack,65536,256,cpu,before);}
 negative_cases(file,256,page);
 snprintf(output,capacity,"{\"schema\":1,\"state\":\"%s\",\"checks\":%u,\"failures\":%u,\"differential_cases\":%u,\"aot_calls\":%u,\"accelerator_calls\":%u,\"second_call_cases\":%u,\"negative_rejections_before_effect\":%u,\"abi_failures\":%u,\"whole_buffer_same_address_comparison\":true,\"scratch_x1_to_x18_not_compared\":true,\"synthetic_cpu_only\":true,\"sdk_startup_called\":false}",failures?"failed":"passed",checks,failures,cases,aot_calls,accelerator_calls,second_calls,rejections,abi_failures);
 free(cpu);free(before);munmap(mapping,FILE_BYTES+2*page);munmap(stack_mapping,65536+2*page);return failures!=0;
}
#ifdef NATIVE_SDK_RELOCATION_HOST_TEST
#include "relocation_reference.h"
int main(void){char output[4096];int status=run_cases(RELOCATION_REFERENCE,output,sizeof output);puts(output);return status;}
#else
__attribute__((visibility("default"))) int native_sdk_relocation_probe(const char *runtime_path,const char *root,const char *game,const char *sidecar,uint64_t address,char *output,size_t capacity){
 if(!runtime_path||!root||!game||!sidecar||!address||!output||capacity<4096)return -1;
 void *runtime=dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);if(!runtime)return -1;
 typedef bool(*Prepare)(const char*,const char*,const char*,char*,size_t);typedef const ModuleAbiDescriptor*(*Descriptor)(void);Prepare prepare;Descriptor descriptor_get;void *symbol=dlsym(runtime,"shared_session_prepare");memcpy(&prepare,&symbol,sizeof symbol);symbol=dlsym(runtime,"shared_gamelib_descriptor");memcpy(&descriptor_get,&symbol,sizeof symbol);if(!prepare||!descriptor_get)return -1;
 char why[512];if(!prepare(root,game,sidecar,why,sizeof why))return -1;
 const ModuleAbiDescriptor *descriptor=descriptor_get();if(!descriptor||descriptor->identity.cpu_identity!=MODULE_ABI_CPU_A64_SHARED_66560||descriptor->identity.cpu_size!=sizeof(Cpu)||descriptor->identity.cpu_alignment!=_Alignof(Cpu))return -1;
 GuestFn original=NULL;for(unsigned m=0;m<descriptor->module_count;m++){const ModuleAbiModule *module=&descriptor->modules[m];if(module->image.module_id!=MODULE_ABI_SDK)continue;for(uint64_t i=0;i<module->address_count;i++)if(module->addresses[i]==address){original=module->functions[i];break;}}
 if(!original)return -1;return run_cases(original,output,capacity);
 /* Runtime/title mapping stays loaded for this one fresh helper process. */
}
#endif
