/* Asset-free continuous execution, synthetic Cpu only; no game/KThread claim. */
#define _GNU_SOURCE
#include "continuous_context.h"
#include "continuous_program.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONTINUOUS_CONTEXT_HOST_TEST
static unsigned assertions;
#define CHECK(x) do{assertions++;if(!(x)){fprintf(stderr,"line%d:%s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 Cpu *cpu=aligned_alloc(16,sizeof *cpu);CnceContext ctx;CHECK(cpu!=NULL);memset(cpu,0,sizeof *cpu);
 CHECK(continuous_context_init(NULL,cpu)==-1);CHECK(continuous_context_init(&ctx,NULL)==-1);
 CHECK(continuous_context_init(&ctx,(Cpu*)((char*)cpu+1))==-1);CHECK(!continuous_context_init(&ctx,cpu));
 CHECK(ctx.magic==CNCE_CONTEXT_MAGIC&&ctx.self==&ctx&&ctx.cpu==cpu);CHECK(!ctx.mode&&!ctx.event);
 CHECK(continuous_context_run(&ctx,(void*)0x1000)==-1);cpu->sp=0x10000;
 CHECK(continuous_context_run(&ctx,NULL)==-1);CHECK(continuous_context_run(&ctx,(void*)0x1001)==-1);
 CHECK(continuous_context_run(&ctx,(void*)0x1000)==-2);ctx.mode=1;CHECK(continuous_context_run(&ctx,(void*)0x1000)==-1);ctx.mode=0;
 ctx.self=NULL;CHECK(continuous_context_run(&ctx,(void*)0x1000)==-1);ctx.self=&ctx;
 ctx.magic=0;CHECK(continuous_context_run(&ctx,(void*)0x1000)==-1);ctx.magic=CNCE_CONTEXT_MAGIC;
 for(unsigned reg=0;reg<31;reg++){CnceSite s={.original_pc=0x10000+reg*4,.resume_pc=0x10004+reg*4,.original_word=0xd53bd040u|reg,.kind=CNCE_PATCH_TPIDR_READ,.original_kind=CNCE_PATCH_TPIDR_READ,.operand=reg};unsigned char stub[32],resume[8];uint32_t patch=0;CHECK(cnce_patch_emit(&s,0x20000,0x30000,0x40000,&patch,stub,sizeof stub)==CNCE_PATCH_OK);CHECK(cnce_resume_emit(0x20040,s.resume_pc,resume,sizeof resume)==CNCE_PATCH_OK);CHECK(!memcmp(stub,"\xf0\x47\xbf\xa9",4));CHECK(!memcmp(resume,"\xf0\x47\xc1\xa8",4));}
 free(cpu);printf("assertions=%u\n",assertions);return 0;
}
#else
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <asm/sigcontext.h>
#define THREADS 4u
#define ITERATIONS 8u
#define LOOP_COUNT 100000u
typedef struct __attribute__((aligned(16))) HostObserved {uint64_t x[12];__uint128_t q[8];uint64_t fpcr,fpsr,x18,tls,sp_before,sp_after;}HostObserved;
int probe_checked_context(CnceContext*,const void*,HostObserved*);
__asm__(
".text\n.p2align 2\n.hidden probe_checked_context\n.global probe_checked_context\nprobe_checked_context:\n"
"sub sp,sp,#256\n"
"stp x19,x20,[sp,#0]\nstp x21,x22,[sp,#16]\nstp x23,x24,[sp,#32]\nstp x25,x26,[sp,#48]\nstp x27,x28,[sp,#64]\nstp x29,x30,[sp,#80]\n"
"stp q8,q9,[sp,#96]\nstp q10,q11,[sp,#128]\nstp q12,q13,[sp,#160]\nstp q14,q15,[sp,#192]\n"
"mrs x9,fpcr\nstr x9,[sp,#224]\nmrs x9,fpsr\nstr x9,[sp,#232]\nstr x2,[sp,#240]\nmov x9,sp\nstr x9,[x2,#256]\n"
"mov x19,#0x9013\nmov x20,#0x9014\nmov x21,#0x9015\nmov x22,#0x9016\nmov x23,#0x9017\nmov x24,#0x9018\nmov x25,#0x9019\nmov x26,#0x901a\nmov x27,#0x901b\nmov x28,#0x901c\nmov x29,#0x901d\n"
"movi v8.16b,#8\nmovi v9.16b,#9\nmovi v10.16b,#10\nmovi v11.16b,#11\nmovi v12.16b,#12\nmovi v13.16b,#13\nmovi v14.16b,#14\nmovi v15.16b,#15\n"
"bl continuous_context_run\n1:\nstr w0,[sp,#248]\nldr x9,[sp,#240]\n"
"stp x19,x20,[x9,#0]\nstp x21,x22,[x9,#16]\nstp x23,x24,[x9,#32]\nstp x25,x26,[x9,#48]\nstp x27,x28,[x9,#64]\nstp x29,x30,[x9,#80]\n"
"stp q8,q9,[x9,#96]\nstp q10,q11,[x9,#128]\nstp q12,q13,[x9,#160]\nstp q14,q15,[x9,#192]\n"
"mrs x10,fpcr\nstr x10,[x9,#224]\nmrs x10,fpsr\nstr x10,[x9,#232]\nstr x18,[x9,#240]\nmrs x10,tpidr_el0\nstr x10,[x9,#248]\nmov x10,sp\nstr x10,[x9,#264]\n"
"adr x10,1b\ncmp x30,x10\nb.eq 2f\nmov w10,#1\nstr w10,[sp,#248]\n2:\n"
"ldr x9,[sp,#224]\nmsr fpcr,x9\nldr x9,[sp,#232]\nmsr fpsr,x9\n"
"ldp q8,q9,[sp,#96]\nldp q10,q11,[sp,#128]\nldp q12,q13,[sp,#160]\nldp q14,q15,[sp,#192]\n"
"ldp x19,x20,[sp,#0]\nldp x21,x22,[sp,#16]\nldp x23,x24,[sp,#32]\nldp x25,x26,[sp,#48]\nldp x27,x28,[sp,#64]\nldp x29,x30,[sp,#80]\nldr w0,[sp,#248]\nadd sp,sp,#256\nret\n");
static uint64_t tls_value(void){uint64_t v;__asm__ volatile("mrs %0,tpidr_el0":"=r"(v));return v;}
static uint64_t x18_value(void){uint64_t v;__asm__ volatile("mov %0,x18":"=r"(v));return v;}
typedef struct Worker{
 unsigned id,checks,failures,cpu_failures,host_failures,signal_failures,native_calls,nested_calls,guest_faults,async_stops,async_resumes,signal_host_checks;
 uint64_t host_tls,host_x18;size_t page;
 unsigned cpu_diff_mask,first_cpu_case,first_cpu_offset;uint64_t first_cpu_actual,first_cpu_expected;
 unsigned char *code,*stack_map,*alt_map,*fault_map;uintptr_t alt_lo,alt_hi;
 CnceContext ctx;Cpu *cpu;CnceSite sites[3];_Atomic uint32_t spin_flag,stop_sender;
 pthread_t thread;
}Worker;
static _Thread_local Worker *current;
static void check(Worker*w,int pass){w->checks++;w->failures+=!pass;}
static void seed(Cpu *cpu,uintptr_t sp,unsigned salt){
 memset(cpu,0xa5^salt,sizeof *cpu);for(unsigned i=0;i<32;i++){cpu->x[i]=UINT64_C(0x13579bdf00000000)^((uint64_t)i<<32)^salt;cpu->v[i]=((__uint128_t)(UINT64_C(0x9988776655443322)^((uint64_t)i<<32)^salt)<<64)|(UINT64_C(0x1020304050607080)^i^salt);}
 cpu->sp=sp;cpu->pc=0;cpu->nzcv=0x90000000;cpu->fpcr=0x01400000;cpu->fpsr=0x10;cpu->v[4]=0x3f800000;cpu->v[5]=0;cpu->tpidr=0x12340001;cpu->tpidrro=0x87650002;
}
static int checked(Worker*w,CnceContext*ctx,const void*resume){
 HostObserved obs={0};uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();int rc=probe_checked_context(ctx,resume,&obs),bad=0;w->native_calls++;
 for(unsigned i=0;i<11;i++)bad|=obs.x[i]!=0x9013+i;
 for(unsigned i=0;i<8;i++){uint64_t low=0;memset(&low,i+8,sizeof low);bad|=(uint64_t)obs.q[i]!=low;}
 bad|=obs.fpcr!=fpcr||obs.fpsr!=fpsr||obs.x18!=w->host_x18||obs.tls!=w->host_tls||obs.sp_before!=obs.sp_after;
 w->host_failures+=bad;check(w,!bad);check(w,rc==0&&ctx->mode==0);return rc;
}
/* Phase1 loop/SVC,2 nested callback,3 outer resume,4 fault retry,5 async
 * snapshot,6 async resume. Bitmask: GP1/SP2/PC4/NZCV8/FPCR16/FPSR32/Q64/opaque128. */
static void compare(Worker*w,const Cpu*cpu,const Cpu*expected,unsigned phase){
 int bad=memcmp(cpu,expected,sizeof *cpu)!=0;
 if(bad){
  w->cpu_diff_mask|=(memcmp(cpu->x,expected->x,sizeof cpu->x)?1u:0u)|(cpu->sp!=expected->sp?2u:0u)|(cpu->pc!=expected->pc?4u:0u)|(cpu->nzcv!=expected->nzcv?8u:0u)|(cpu->fpcr!=expected->fpcr?16u:0u)|(cpu->fpsr!=expected->fpsr?32u:0u)|(memcmp(cpu->v,expected->v,sizeof cpu->v)?64u:0u);
  w->cpu_diff_mask|=(memcmp((const char*)cpu+280,(const char*)expected+280,40)||memcmp((const char*)cpu+336,(const char*)expected+336,176)||memcmp(cpu->dcache,expected->dcache,sizeof cpu->dcache))?128u:0u;
  if(!w->first_cpu_case){size_t at=0;while(at<sizeof *cpu&&((const unsigned char*)cpu)[at]==((const unsigned char*)expected)[at])at++;at&=~(size_t)7;w->first_cpu_case=phase;w->first_cpu_offset=(unsigned)at;memcpy(&w->first_cpu_actual,(const char*)cpu+at,8);memcpy(&w->first_cpu_expected,(const char*)expected+at,8);}
 }
 w->cpu_failures+=bad;check(w,!bad);
}
static int emit_resume(Worker*w,unsigned offset,uint64_t target){
 if(mprotect(w->code,w->page,PROT_READ|PROT_WRITE))return -1;
 if(cnce_resume_emit((uintptr_t)w->code+offset,target,w->code+offset,8)!=CNCE_PATCH_OK)return -1;
 __builtin___clear_cache((char*)w->code,(char*)w->code+w->page);return mprotect(w->code,w->page,PROT_READ|PROT_EXEC);
}
/* Called only after the assembly frontend has restored Android TLS and X18.
 * A deliberately protected data read must retry its original PC; no skip. */
int continuous_signals_dispatch(int sig,void*info,void*raw,CnceContext*ctx){
 (void)info;Worker*w=current;uintptr_t local=(uintptr_t)&w;
 if(!w||!ctx)return 0;
 int good=ctx==&w->ctx&&tls_value()==w->host_tls&&x18_value()==w->host_x18&&local>=w->alt_lo&&local<w->alt_hi;
 w->signal_host_checks++;w->signal_failures+=!good;
 if(!good)return 1;
 ucontext_t *uc=raw;
 if(sig==SIGSEGV && uc->uc_mcontext.pc==(uintptr_t)w->code+PC_FAULT){
  if(syscall(SYS_mprotect,w->fault_map,w->page,PROT_READ|PROT_WRITE))w->signal_failures++;else w->guest_faults++;
  return 1;
 }
 if(sig==SIGUSR1){
  if(continuous_context_is_transition(uc->uc_mcontext.pc))return 1;
  Cpu*c=ctx->cpu;for(unsigned i=0;i<31;i++)c->x[i]=uc->uc_mcontext.regs[i];c->sp=uc->uc_mcontext.sp;c->pc=uc->uc_mcontext.pc;c->nzcv=uc->uc_mcontext.pstate&0xf0000000;
  size_t at=0;int found=0;while(at+sizeof(struct _aarch64_ctx)<=sizeof uc->uc_mcontext.__reserved){struct _aarch64_ctx*h=(void*)(uc->uc_mcontext.__reserved+at);if(!h->magic||!h->size)break;if(h->size<sizeof *h||h->size>sizeof uc->uc_mcontext.__reserved-at)break;if(h->magic==FPSIMD_MAGIC&&h->size>=sizeof(struct fpsimd_context)){struct fpsimd_context*f=(void*)h;for(unsigned i=0;i<32;i++)c->v[i]=f->vregs[i];c->fpcr=f->fpcr;c->fpsr=f->fpsr;found=1;break;}at+=h->size;}
  w->signal_failures+=!found;ctx->event=NULL;uc->uc_mcontext.pc=(uintptr_t)continuous_context_signal_return;uc->uc_mcontext.regs[0]=(uintptr_t)ctx;uc->uc_mcontext.regs[18]=ctx->host_x18;uc->uc_mcontext.sp=ctx->host_sp;w->async_stops++;return 0;
 }
 w->signal_failures++;return 1;
}
static void*send_stop(void*opaque){Worker*w=opaque;while(!atomic_load_explicit(&w->spin_flag,memory_order_acquire))sched_yield();do{usleep(2000);if(!atomic_load_explicit(&w->stop_sender,memory_order_acquire))pthread_kill(w->thread,SIGUSR1);}while(!atomic_load_explicit(&w->stop_sender,memory_order_acquire));return NULL;}
static void *worker(void*opaque){
 Worker*w=opaque;current=w;w->thread=pthread_self();w->page=(size_t)sysconf(_SC_PAGESIZE);w->host_tls=tls_value();w->host_x18=x18_value();
 w->stack_map=mmap(NULL,w->page*4,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->alt_map=mmap(NULL,w->page*18,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->fault_map=mmap(NULL,w->page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->code=mmap(NULL,w->page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 w->cpu=aligned_alloc(16,sizeof(Cpu));Cpu*expected=aligned_alloc(16,sizeof(Cpu)),*nested=aligned_alloc(16,sizeof(Cpu));if(w->stack_map==MAP_FAILED||w->alt_map==MAP_FAILED||w->fault_map==MAP_FAILED||w->code==MAP_FAILED||!w->cpu||!expected||!nested){w->failures++;return NULL;}
 if(mprotect(w->stack_map+w->page,w->page*2,PROT_READ|PROT_WRITE)||mprotect(w->alt_map+w->page,w->page*16,PROT_READ|PROT_WRITE)){w->failures++;return NULL;}
 w->alt_lo=(uintptr_t)w->alt_map+w->page;w->alt_hi=w->alt_lo+w->page*16;stack_t ss={.ss_sp=(void*)w->alt_lo,.ss_size=w->page*16},old;if(sigaltstack(&ss,&old)){w->failures++;return NULL;}
 memcpy(w->code,program_bytes,sizeof program_bytes);unsigned sites[3]={PC_SITE1,PC_SITE2,PC_SPIN_SITE};
 for(unsigned i=0;i<3;i++){w->sites[i]=(CnceSite){.original_pc=(uintptr_t)w->code+sites[i],.resume_pc=(uintptr_t)w->code+sites[i]+4,.original_word=0xd4000001u|((i==0?0x10u:i==1?0x25u:0x1eu)<<5),.kind=CNCE_PATCH_SVC,.original_kind=CNCE_PATCH_SVC,.operand=(i==0?0x10u:i==1?0x25u:0x1eu)};uint32_t patch=0;if(cnce_patch_emit(&w->sites[i],(uintptr_t)w->code+512+i*32,(uintptr_t)&w->sites[i],(uintptr_t)continuous_context_exit,&patch,w->code+512+i*32,32)!=CNCE_PATCH_OK)w->failures++;memcpy(w->code+sites[i],&patch,4);}
 unsigned targets[4]={PC_START,PC_STAGE2,PC_FAULT,PC_SPIN};for(unsigned i=0;i<4;i++)if(cnce_resume_emit((uintptr_t)w->code+1024+i*8,(uintptr_t)w->code+targets[i],w->code+1024+i*8,8)!=CNCE_PATCH_OK)w->failures++;
 __builtin___clear_cache((char*)w->code,(char*)w->code+w->page);if(mprotect(w->code,w->page,PROT_READ|PROT_EXEC))w->failures++;
 uintptr_t top=(uintptr_t)w->stack_map+w->page*3-64;uint64_t oldfpcr=rt_get_fpcr(),oldfpsr=rt_get_fpsr();rt_set_fpcr(0x00800000);rt_set_fpsr(0x1f);
 for(unsigned i=0;i<ITERATIONS;i++){
  seed(w->cpu,top,i+w->id*16);w->cpu->x[0]=LOOP_COUNT;w->cpu->pc=(uintptr_t)w->code+PC_START;memcpy(expected,w->cpu,sizeof *expected);
  expected->x[0]=0;expected->x[1]+=3*LOOP_COUNT;expected->x[18]+=7;expected->x[19]+=11;expected->x[20]=expected->x[18];expected->x[21]=expected->x[19];expected->v[0]^=expected->v[31];expected->v[3]=0x7f800000;expected->fpsr|=2;expected->nzcv=0x60000000;expected->pc=w->sites[0].resume_pc;
  check(w,!continuous_context_init(&w->ctx,w->cpu));checked(w,&w->ctx,w->code+1024);check(w,w->ctx.event==&w->sites[0]);compare(w,w->cpu,expected,1);check(w,!memcmp((void*)(top-32),&expected->x[18],16));
  /* Host-service -> nested guest callback while outer Cpu remains suspended. */
  CnceContext nested_ctx;seed(nested,top-256,i+99);Cpu nested_expected;memcpy(&nested_expected,nested,sizeof *nested);nested_expected.x[0]+=9;nested_expected.x[18]+=5;nested_expected.x[17]+=19;nested_expected.x[30]=(uintptr_t)w->code+PC_AFTER_BL;nested_expected.pc=w->sites[1].resume_pc;
  check(w,!continuous_context_init(&nested_ctx,nested));checked(w,&nested_ctx,w->code+1032);compare(w,nested,&nested_expected,2);w->nested_calls++;
  expected->x[0]+=9;expected->x[18]+=5;expected->x[17]+=19;expected->x[30]=(uintptr_t)w->code+PC_AFTER_BL;expected->pc=w->sites[1].resume_pc;
  checked(w,&w->ctx,w->code+1032);compare(w,w->cpu,expected,3);check(w,w->ctx.event==&w->sites[1]);
 }
 seed(w->cpu,top,77);w->cpu->x[2]=(uintptr_t)w->fault_map;*(uint64_t*)w->fault_map=0x76543210;memcpy(expected,w->cpu,sizeof *expected);expected->x[0]=0x76543210;expected->pc=w->sites[1].resume_pc;
 check(w,!continuous_context_init(&w->ctx,w->cpu));check(w,!mprotect(w->fault_map,w->page,PROT_NONE));checked(w,&w->ctx,w->code+1040);compare(w,w->cpu,expected,4);check(w,w->guest_faults==1);
 seed(w->cpu,top,91);w->cpu->x[2]=(uintptr_t)&w->spin_flag;w->cpu->pc=(uintptr_t)w->code+PC_SPIN;memcpy(expected,w->cpu,sizeof *expected);check(w,!continuous_context_init(&w->ctx,w->cpu));atomic_store(&w->spin_flag,1);pthread_t sender;check(w,!pthread_create(&sender,NULL,send_stop,w));checked(w,&w->ctx,w->code+1048);atomic_store_explicit(&w->stop_sender,1,memory_order_release);check(w,!pthread_join(sender,NULL));check(w,w->async_stops==1&&!w->ctx.event);
 check(w,w->cpu->pc==(uintptr_t)w->code+PC_SPIN||w->cpu->pc==(uintptr_t)w->code+PC_SPIN+4);expected->pc=w->cpu->pc;expected->x[3]=w->cpu->x[3];check(w,expected->x[3]==1);compare(w,w->cpu,expected,5);
 atomic_store(&w->spin_flag,0);check(w,!emit_resume(w,1080,w->cpu->pc));checked(w,&w->ctx,w->code+1080);expected->x[3]=0;expected->pc=w->sites[2].resume_pc;compare(w,w->cpu,expected,6);check(w,w->ctx.event==&w->sites[2]);w->async_resumes++;
 check(w,w->signal_host_checks>=2&&!w->signal_failures);rt_set_fpcr(oldfpcr);rt_set_fpsr(oldfpsr);check(w,!sigaltstack(&old,NULL));
 free(w->cpu);free(expected);free(nested);munmap(w->code,w->page);munmap(w->fault_map,w->page);munmap(w->alt_map,w->page*18);munmap(w->stack_map,w->page*4);current=NULL;return NULL;
}
__attribute__((visibility("default"))) int continuous_context_probe(char*out,size_t capacity){
 struct sigaction action={0},oldsegv,oldusr;action.sa_sigaction=(void*)continuous_context_signal_front;action.sa_flags=SA_SIGINFO|SA_ONSTACK;sigfillset(&action.sa_mask);if(sigaction(SIGSEGV,&action,&oldsegv)||sigaction(SIGUSR1,&action,&oldusr))return -1;
 Worker workers[THREADS]={0};pthread_t threads[THREADS];unsigned created=0,fail=0,checks=0,cpu=0,host=0,signals=0,calls=0,nested=0,faults=0,stops=0,resumes=0,sighost=0,diffmask=0;Worker*first=NULL;
 for(unsigned i=0;i<THREADS;i++){workers[i].id=i;if(pthread_create(&threads[i],NULL,worker,&workers[i])){fail++;break;}created++;}
 for(unsigned i=0;i<created;i++){if(pthread_join(threads[i],NULL))fail++;Worker*w=&workers[i];fail+=w->failures;checks+=w->checks;cpu+=w->cpu_failures;host+=w->host_failures;signals+=w->signal_failures;calls+=w->native_calls;nested+=w->nested_calls;faults+=w->guest_faults;stops+=w->async_stops;resumes+=w->async_resumes;sighost+=w->signal_host_checks;diffmask|=w->cpu_diff_mask;if(!first&&w->first_cpu_case)first=w;}
 sigaction(SIGSEGV,&oldsegv,NULL);sigaction(SIGUSR1,&oldusr,NULL);
 snprintf(out,capacity,"{\"schema\":1,\"state\":\"%s\",\"cpu_diff_mask\":%u,\"first_cpu_case\":%u,\"first_cpu_offset\":%u,\"first_cpu_actual\":\"%016"PRIx64"\",\"first_cpu_expected\":\"%016"PRIx64"\",\"checks\":%u,\"failures\":%u,\"threads\":%u,\"native_calls\":%u,\"native_loop_iterations\":%u,\"nested_callbacks\":%u,\"guest_fault_retries\":%u,\"async_stops\":%u,\"async_resumes\":%u,\"signal_host_checks\":%u,\"cpu_failures\":%u,\"host_failures\":%u,\"signal_failures\":%u,\"guard_pages\":%u,\"physical_guest_x18\":true,\"hardware_private_tpidr\":true,\"real_kthread\":false,\"game_started\":false}",fail?"failed":"passed",diffmask,first?first->first_cpu_case:0,first?first->first_cpu_offset:0,first?first->first_cpu_actual:0,first?first->first_cpu_expected:0,checks,fail,created,calls,created*ITERATIONS*LOOP_COUNT,nested,faults,stops,resumes,sighost,cpu,host,signals,created*4);return fail?-1:0;
}
#endif
