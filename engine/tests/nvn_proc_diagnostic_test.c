/* Actual NVN proc wrappers; controlled registry/fatal path, no graphics or game assets. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

static unsigned checks,lookup_calls,register_calls,real_calls,trap_logs;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static jmp_buf fatal_jump;
static int fatal_expected;
static char fatal_text[1024];
static void log_message(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);char text[1024];vsnprintf(text,sizeof text,fmt,ap);va_end(ap);
    if(strstr(text,"diagnostic trap address only"))trap_logs++;
}
static _Noreturn void test_fatal(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vsnprintf(fatal_text,sizeof fatal_text,fmt,ap);va_end(ap);
    if(!fatal_expected){fprintf(stderr,"unexpected fatal: %s",fatal_text);abort();}
    longjmp(fatal_jump,1);
}
#undef LOG
#undef FATAL
#define LOG(...) log_message(__VA_ARGS__)
#define FATAL(...) test_fatal(__VA_ARGS__)
typedef struct {const char *name;HostFn fn;uint64_t pc;} Entry;
static Entry registry[32];static unsigned entries;
static void implemented(Cpu *c){real_calls++;c->x[0]=0x12345678;}
HostFn hle_lookup_impl(const char *name){lookup_calls++;return !strcmp(name,"nvnRealQuery")?implemented:NULL;}
const char *hle_name_at(uint64_t pc){for(unsigned i=0;i<entries;i++)if(registry[i].pc==pc)return registry[i].name;return NULL;}
uint64_t hle_register(const char *name,HostFn fn){
    register_calls++;if(!fn)FATAL("unimplemented strong guest import %s\n",name);
    for(unsigned i=0;i<entries;i++)if(!strcmp(registry[i].name,name))return registry[i].pc;
    CHECK(entries<32);uint64_t pc=0x100000+entries*16;registry[entries++]=(Entry){name,fn,pc};return pc;
}
#include "production_nvn_boot.c"
static Cpu *cpu,*before;
static unsigned char output[128],saved_output[128];
static void prepare(void){memset(cpu,0xa5,sizeof *cpu);memset(output,0x5a,sizeof output);memcpy(saved_output,output,sizeof output);}
static void getproc(bool device,const char *name){cpu->x[device?1:0]=(uintptr_t)name;if(device)cpu->x[0]=0x7777;
    if(device)nvn_device_get_proc_address(cpu);else nvn_bootstrap_loader(cpu);}
static void check_strict(bool device,const char *flag){
    if(flag)setenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS",flag,1);else unsetenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS");
    prepare();cpu->x[device?1:0]=(uintptr_t)"nvnMissingQuery";if(device)cpu->x[0]=0x7777;memcpy(before,cpu,sizeof *cpu);
    unsigned count=entries;fatal_expected=1;
    if(!setjmp(fatal_jump)) {if(device)nvn_device_get_proc_address(cpu);else nvn_bootstrap_loader(cpu);CHECK(false);}
    fatal_expected=0;CHECK(entries==count);CHECK(strstr(fatal_text,"unimplemented strong guest import nvnMissingQuery")!=NULL);
    CHECK(!memcmp(before,cpu,sizeof *cpu));CHECK(!memcmp(output,saved_output,sizeof output));
}
int main(void){
    cpu=malloc(sizeof *cpu);before=malloc(sizeof *before);CHECK(cpu&&before);
    for(int device=0;device<2;device++){
        check_strict(device,NULL);check_strict(device,"0");check_strict(device,"true");check_strict(device,"01");
        for(int enabled=0;enabled<2;enabled++) {
            setenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS",enabled?"1":"0",1);prepare();unsigned n=lookup_calls,r=register_calls;
            getproc(device,NULL);CHECK(cpu->x[0]==0);CHECK(lookup_calls==n&&register_calls==r);
            getproc(device,"");CHECK(cpu->x[0]==0);CHECK(lookup_calls==n&&register_calls==r);
            getproc(device,"nvnRealQuery");CHECK(cpu->x[0]==0x100000);CHECK(registry[0].fn==implemented);
            cpu->pc=cpu->x[0];registry[0].fn(cpu);CHECK(cpu->x[0]==0x12345678);
        }
        setenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS","1",1);prepare();unsigned old_real=real_calls;
        getproc(device,"nvnMissingQuery");uint64_t address=cpu->x[0];CHECK(address!=0&&address!=0x100000);CHECK(real_calls==old_real);
        Entry *trap=NULL;for(unsigned i=0;i<entries;i++)if(registry[i].pc==address)trap=&registry[i];CHECK(trap&&trap->fn==nvn_diagnostic_unimplemented);
        getproc(device,"nvnMissingQuery");CHECK(cpu->x[0]==address);CHECK(real_calls==old_real);
        cpu->pc=address;cpu->x[30]=0xabcdef00;cpu->x[1]=(uintptr_t)output;cpu->x[2]=sizeof output;memcpy(before,cpu,sizeof *cpu);
        fatal_expected=1;if(!setjmp(fatal_jump)){trap->fn(cpu);CHECK(false);}fatal_expected=0;
        CHECK(strstr(fatal_text,"unimplemented API INVOKED: nvnMissingQuery")!=NULL);CHECK(strstr(fatal_text,"lr=abcdef00")!=NULL);
        CHECK(!memcmp(before,cpu,sizeof *cpu));CHECK(!memcmp(output,saved_output,sizeof output));CHECK(real_calls==old_real);
        // Diagnostic registration must not make a later strict lookup silently succeed.
        check_strict(device,"0");
    }
    CHECK(trap_logs==4);CHECK(real_calls==4);free(before);free(cpu);unsetenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS");
    printf("%u NVN proc diagnostic assertions passed (actual wrappers; controlled registry/fatal path)\n",checks);return 0;
}
