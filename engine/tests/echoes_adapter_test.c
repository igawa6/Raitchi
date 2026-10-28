/* Synthetic native metadata and actual extracted startup/kernel functions.
 * No original NSO bytes, CPU execution, real KThread or game claim. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "adapters/title_adapter.h"
#include "runtime/aot_cpu.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"check line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const uint8_t contract[32]={0x71,0x22,0x33,0x44};
static NativeManifest fixture(enum TitleId id){
 const TitleAdapter*a=title_adapter_find(id);NativeManifest m={.struct_size=sizeof m,.version=NATIVE_MANIFEST_VERSION,.title_id=id,.program_id=a->program_id,.module_count=a->module_count,.mapping_alignment=16384,.identity={.struct_size=sizeof(ModuleAbiIdentity),.abi_version=MODULE_ABI_VERSION,.target=MODULE_ABI_TARGET_ANDROID_ARM64_LP64,.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560,.cpu_size=sizeof(Cpu),.cpu_alignment=_Alignof(Cpu),.guest_shift=a->guest_shift,.guest_window_lo=a->canonical_window_lo-a->guest_shift,.guest_window_hi=a->canonical_window_hi-a->guest_shift}};
 memcpy(m.contract_sha256,contract,32);
 for(unsigned i=0;i<a->module_count;i++){NativeModuleSpec *s=&m.modules[i];*s=(NativeModuleSpec){.struct_size=sizeof *s,.module_id=a->modules[i].module_id,.canonical_base=a->modules[i].canonical_base,.mapped_size=0x8000,.text_size=0x4000};s->nso_build_id[31]=0xf1;s->nso_sha256[0]=(uint8_t)(i+1);if(a->module_build_prefix_size[i])memcpy(s->nso_build_id,a->module_build_prefix[i],a->module_build_prefix_size[i]);}
 memcpy(m.modules[0].nso_build_id,a->main_build_prefix,a->main_build_prefix_size);return m;
}
static void reject(enum TitleId id,NativeManifest*m,const uint8_t*expected,unsigned page,enum TitleAdapterStatus want){SessionLayoutSpec out,zero={0};TitleAdapterFailure f;memset(&out,0xa5,sizeof out);CHECK(title_adapter_build_native_layout(id,m,expected,page,&out,&f)==want);CHECK(f.status==want);CHECK(!memcmp(&out,&zero,sizeof out));CHECK(!title_adapter_selected());}
static void metadata_tests(void){
 for(enum TitleId id=TITLE_LAS;id<=TITLE_WONDER;id++){
  const TitleAdapter*a=title_adapter_find(id);NativeManifest m=fixture(id),copy=m;SessionLayoutSpec out;SessionLayout normalized;TitleAdapterFailure f;
  for(unsigned i=0;i<2;i++){unsigned page=i?16384:4096;CHECK(title_adapter_build_native_layout(id,&m,contract,page,&out,&f)==TITLE_ADAPTER_OK);CHECK(session_layout_build(&out,&normalized,NULL)==SESSION_LAYOUT_OK);CHECK(normalized.window.lo==a->canonical_window_lo-a->guest_shift);CHECK(out.module_count==a->module_count);for(unsigned j=0;j<a->module_count;j++){CHECK(out.modules[j].module_id==a->modules[j].module_id);CHECK(out.modules[j].image.canonical_base==a->modules[j].canonical_base);}CHECK(!memcmp(&m,&copy,sizeof m));}
  uint8_t wrong[32];memcpy(wrong,contract,32);wrong[0]^=1;reject(id,&m,wrong,4096,TITLE_ADAPTER_PROFILE);reject(id,&m,contract,0,TITLE_ADAPTER_LAYOUT);reject(id,&m,contract,8193,TITLE_ADAPTER_LAYOUT);reject(id,&m,contract,65536,TITLE_ADAPTER_LAYOUT);
  m.title_id=99;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;m.program_id^=1;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;m.identity.cpu_identity=MODULE_ABI_CPU_A64_CACHE_66560;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;m.identity.guest_shift+=16384;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;m.modules[0].nso_build_id[0]^=1;reject(id,&m,contract,4096,TITLE_ADAPTER_BUILD);m=copy;memset(m.modules[0].nso_sha256,0,32);reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;m.modules[1].canonical_base=m.modules[0].canonical_base;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);m=copy;
  NativeModuleSpec swap=m.modules[0];m.modules[0]=m.modules[1];m.modules[1]=swap;reject(id,&m,contract,4096,TITLE_ADAPTER_PROFILE);
 }
 const TitleAdapter*a=title_adapter_find(TITLE_ECHOES);CHECK(a->program_id==UINT64_C(0x01008cf01baac000)&&!strcmp(a->save_namespace,"echoes-1.0.0")&&a->capabilities==0);CHECK(a->guest_shift==UINT64_C(0x6000000000));
 static const uint32_t ids[]={MODULE_ABI_MAIN,MODULE_ABI_SUBSDK0,MODULE_ABI_SUBSDK1,MODULE_ABI_SDK};static const uint64_t bases[]={UINT64_C(0x7000000000),UINT64_C(0x7008000000),UINT64_C(0x700a000000),UINT64_C(0x700c000000)};
 for(unsigned i=0;i<4;i++){CHECK(a->modules[i].module_id==ids[i]);CHECK(a->modules[i].canonical_base==bases[i]);NativeManifest m=fixture(TITLE_ECHOES);m.modules[i].nso_build_id[0]^=1;reject(TITLE_ECHOES,&m,contract,4096,TITLE_ADAPTER_BUILD);}
 NativeManifest m=fixture(TITLE_ECHOES);m.modules[0].mapped_size=0x4004000;SessionLayoutSpec out;CHECK(title_adapter_build_native_layout(TITLE_ECHOES,&m,contract,4096,&out,NULL)==TITLE_ADAPTER_OK);m.modules[3].mapped_size=0x2004000;reject(TITLE_ECHOES,&m,contract,4096,TITLE_ADAPTER_PROFILE);
 const TitleAdapter*w=title_adapter_find(TITLE_WONDER);CHECK(w->program_id==UINT64_C(0x010015100b514000)&&!strcmp(w->save_namespace,"wonder-1.0.0")&&w->module_count==2&&w->capabilities==0);CHECK(w->nvn_api_major==55&&w->nvn_api_minor==15);CHECK(w->modules[1].module_id==MODULE_ABI_SDK&&w->startup.argument_count==5&&w->startup.early_modules[0]==MODULE_ABI_SDK&&w->startup.late_modules[0]==MODULE_ABI_MAIN);
 NativeManifest wm=fixture(TITLE_WONDER);wm.modules[1].nso_build_id[19]^=1;reject(TITLE_WONDER,&wm,contract,4096,TITLE_ADAPTER_BUILD);wm=fixture(TITLE_WONDER);wm.program_id+=0x800;reject(TITLE_WONDER,&wm,contract,4096,TITLE_ADAPTER_PROFILE);wm=fixture(TITLE_WONDER);wm.module_count=1;memset(&wm.modules[1],0,sizeof wm.modules[1]);reject(TITLE_WONDER,&wm,contract,4096,TITLE_ADAPTER_PROFILE);
 ModuleAbiProfile unused={0};CHECK(title_adapter_build_layout(TITLE_WONDER,&unused,0,4096,&out,NULL)==TITLE_ADAPTER_AOT_UNSUPPORTED);TitleAdapterFailure f;CHECK(title_adapter_build_layout(TITLE_ECHOES,&unused,0,4096,&out,&f)==TITLE_ADAPTER_AOT_UNSUPPORTED);CHECK(strstr(title_adapter_status_string(f.status),"native-only")!=NULL);
 CHECK(title_adapter_build_native_layout(TITLE_ECHOES,NULL,contract,4096,&out,NULL)==TITLE_ADAPTER_ARGUMENT);CHECK(title_adapter_build_native_layout(TITLE_ECHOES,&m,NULL,4096,&out,NULL)==TITLE_ADAPTER_ARGUMENT);CHECK(title_adapter_build_native_layout(TITLE_ECHOES,&m,contract,4096,NULL,NULL)==TITLE_ADAPTER_ARGUMENT);CHECK(title_adapter_build_native_layout((enum TitleId)99,&m,contract,4096,&out,NULL)==TITLE_ADAPTER_UNKNOWN);
}
/* Stand-ins for Module/host services; function bodies below are extracted from
 * production main.c/kernel.c verbatim by check_echoes_adapter.py. */
typedef struct Module{const char*name;uint64_t init,init_array,init_array_sz;}Module;
typedef struct KThread{Cpu cpu;uint32_t handle;}KThread;
static Module g_modules[4];static uint32_t module_ids[4];static int g_nmodules;
static pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;static KThread*g_main_thread;
static uint64_t calls[32];static unsigned call_count,starts,fini_logs;
static GuestFn callbacks[3];static unsigned registered;static KThread main_thread;
static uint64_t arrays[4][3];
static void test_log(const char*format,...){if(strstr(format,"nnMain returned"))fini_logs++;}
#define LOG(...) test_log(__VA_ARGS__)
#define FATAL(...) do{fprintf(stderr,__VA_ARGS__);exit(90);}while(0)
#define R_OK 0
static void guest_call(Cpu*c,uint64_t pc){(void)c;CHECK(call_count<32);calls[call_count++]=pc;}
static uint32_t shared_module_id(uint32_t index){return module_ids[index];}
static int thread_start(KThread*t){CHECK(t==&main_thread);starts++;return R_OK;}
static void*hang_watch(void*unused){return unused;}
static uint64_t loader_find_symbol(const char*name){const char*expected=title_adapter_selected()->startup.argument_count==5?"_ZN2nn4init5StartEmmPFvvES2_S2_":"_ZN2nn4init5StartEmmPFvvES2_";CHECK(!strcmp(name,expected));return 0x12345600;}
static uint64_t hle_register(const char*name,GuestFn fn){CHECK(registered<3);CHECK(fn!=NULL);CHECK(strstr(name,"shared_")!=NULL);callbacks[registered]=fn;return 0x80000000+16*registered++;}
static KThread*kernel_create_main_thread(uint64_t entry,uint64_t stack,int priority){CHECK(entry==0x12345600&&stack==0x100000&&priority==44);memset(&main_thread,0x5a,sizeof main_thread);main_thread.handle=0x1234;return &main_thread;}
void kernel_run_main5(KThread*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
#include "echoes_startup_fragments.h"
static void constructor_tests(enum TitleId id){
 const TitleAdapter*a=title_adapter_find(id);CHECK(title_adapter_select(id)==TITLE_ADAPTER_OK);setenv("LARECOMP_HANG_DUMP","0",1);g_nmodules=(int)a->module_count;
 for(unsigned i=0;i<a->module_count;i++){module_ids[i]=a->modules[i].module_id;arrays[i][0]=0x200000+module_ids[i]*0x100;arrays[i][1]=0;arrays[i][2]=arrays[i][0]+8;g_modules[i]=(Module){a->modules[i].name,0x100000+module_ids[i]*0x100,(uintptr_t)arrays[i],sizeof arrays[i]};}
 actual_main_startup(0x100000,44);CHECK(starts==1&&g_main_thread==&main_thread);CHECK(main_thread.cpu.x[0]==main_thread.handle&&main_thread.cpu.x[1]==0);CHECK(main_thread.cpu.x[2]==0x80000000&&main_thread.cpu.x[3]==0x80000010);CHECK(main_thread.cpu.x[4]==(a->startup.argument_count==5?0x80000020:0));CHECK(registered==(a->startup.argument_count==5?3u:2u));
 Cpu expected;memset(&expected,0x5a,sizeof expected);memcpy(expected.x,main_thread.cpu.x,5*sizeof(uint64_t));CHECK(!memcmp(&main_thread.cpu,&expected,sizeof expected));
 uint32_t wanted[4];unsigned count=0;if(id==TITLE_LAS||id==TITLE_WONDER){wanted[count++]=MODULE_ABI_SDK;wanted[count++]=MODULE_ABI_MAIN;}else if(id==TITLE_EEVEE){wanted[count++]=MODULE_ABI_SDK;wanted[count++]=MODULE_ABI_SUBSDK0;wanted[count++]=MODULE_ABI_SUBSDK1;wanted[count++]=MODULE_ABI_MAIN;}else{wanted[count++]=MODULE_ABI_SDK;wanted[count++]=MODULE_ABI_SUBSDK1;wanted[count++]=MODULE_ABI_SUBSDK0;wanted[count++]=MODULE_ABI_MAIN;}
 callbacks[0](&main_thread.cpu);callbacks[1](&main_thread.cpu);CHECK(call_count==count);for(unsigned i=0;i<count;i++)CHECK(calls[i]==0x100000+wanted[i]*0x100);
 if(a->startup.argument_count==5){Cpu before=main_thread.cpu;callbacks[2](&main_thread.cpu);CHECK(fini_logs==1&&!memcmp(&before,&main_thread.cpu,sizeof before));}
 /* DT_INIT owns init-array invocation. Without it, skip null array entries;
  * constructor lookup remains correct if loaded module slots are rearranged. */
 for(unsigned i=0;i<a->module_count;i++)g_modules[i].init=0;Module swap=g_modules[0];g_modules[0]=g_modules[a->module_count-1];g_modules[a->module_count-1]=swap;uint32_t sid=module_ids[0];module_ids[0]=module_ids[a->module_count-1];module_ids[a->module_count-1]=sid;
 call_count=0;callbacks[0](&main_thread.cpu);callbacks[1](&main_thread.cpu);CHECK(call_count==count*2);for(unsigned i=0;i<count;i++){CHECK(calls[i*2]==0x200000+wanted[i]*0x100);CHECK(calls[i*2+1]==calls[i*2]+8);}
 main_thread.cpu.x[4]=0xf00d;kernel_run_main(&main_thread,1,2,3,4);CHECK(main_thread.cpu.x[0]==1&&main_thread.cpu.x[3]==4&&main_thread.cpu.x[4]==0);
}
int main(int argc,char**argv){if(argc!=2)return 2;enum TitleId id=!strcmp(argv[1],"wonder")?TITLE_WONDER:!strcmp(argv[1],"echoes")?TITLE_ECHOES:!strcmp(argv[1],"las")?TITLE_LAS:TITLE_EEVEE;metadata_tests();constructor_tests(id);printf("state=passed title=%s checks=%u\n",argv[1],checks);return 0;}
