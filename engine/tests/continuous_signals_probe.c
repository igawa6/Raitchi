/* Combined real continuous context/image/signal routing. Synthetic Cpu and
 * production-like dirty-page handler; no actual KThread/game/GPU claim. */
#define _GNU_SOURCE
#include "continuous_context.h"
#include "continuous_image.h"
#include "continuous_signals.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "continuous_signals_words.h"
#define THREADS 4u
#define INSTALLS 128u
#define WINDOW_BYTES (96u*1024u*1024u)
typedef struct Worker {
 Cpu cpu;CnceContext context;pthread_t host;uint64_t tls,x18;
 unsigned char *stack,*read_page,*write_page;size_t page;
 _Atomic unsigned ready,done,stopped;
 unsigned checks,failures,host_failures,cpu_failures,handler_calls,handler_failures,async_returns;
 uint64_t first_offset,actual,expected;
} Worker;
static Worker workers[THREADS];
static _Thread_local Worker *current_worker;
static _Thread_local Cpu *bound_cpu;
static _Atomic unsigned production_installs,renderer_installs,renderer_failures,renderer_done;
static unsigned char *image_mapping;
static uint64_t text_base;
static size_t page_size;
Cpu *kernel_current_cpu(void){return bound_cpu;}
static uint64_t tls_value(void){
#if defined(__aarch64__)
 uint64_t value;__asm__ volatile("mrs %0,tpidr_el0":"=r"(value));return value;
#else
 return (uintptr_t)pthread_self();
#endif
}
static uint64_t x18_value(void){
#if defined(__aarch64__)
 uint64_t value;__asm__ volatile("mov %0,x18":"=r"(value));return value;
#else
 return 0;
#endif
}
static void check(Worker*w,int condition){w->checks++;w->failures+=!condition;}
static int is_guest_pc(uint64_t pc){return continuous_image_is_original_guest_pc(pc);}
static int original_pc(uint64_t pc,uint64_t *result){uint64_t original=continuous_image_original_pc(pc);if(!original)return 0;*result=original;return 1;}
/* Real C prologue/stack protector/TLS/libc are deliberately exercised here.
 * This C is reached ONLY through the production assembly signal frontend. */
static void production_dirty_handler(int sig,siginfo_t*info,void*raw){
 volatile unsigned char protected_local[96];for(unsigned i=0;i<sizeof protected_local;i++)protected_local[i]=(unsigned char)i;
 Worker*w=current_worker;stack_t stack={0};int before_errno=errno;errno=E2BIG;
 int stack_ok=!sigaltstack(NULL,&stack)&&(stack.ss_flags&SS_ONSTACK);
 int good=w&&bound_cpu==&w->cpu&&tls_value()==w->tls&&x18_value()==w->x18&&stack_ok&&sig==SIGSEGV&&rt_get_fpcr()==w->context.host_fpcr;
 if(!w)_exit(90);
 w->handler_calls++;w->handler_failures+=!good;
 uintptr_t address=(uintptr_t)info->si_addr,read=(uintptr_t)w->read_page,write=(uintptr_t)w->write_page;
 if(!good||!((address>=read&&address<read+w->page)||(address>=write&&address<write+w->page)))_exit(91);
#if defined(__aarch64__)
 ucontext_t*uc=raw;uint64_t expected_pc=text_base+(address>=write&&address<write+w->page?PC_DIRTY:PC_FAULT);
 if(uc->uc_mcontext.pc!=expected_pc||w->cpu.pc!=expected_pc||uc->uc_mcontext.regs[18]!=UINT64_C(0x8912345677889911)){w->handler_failures++;_exit(92);}
#else
 (void)raw;
#endif
 /* The real renderer dirty-page callback retries the same instruction. */
 void*page=(void*)(address&~(uintptr_t)(w->page-1));
 if(syscall(SYS_mprotect,page,w->page,PROT_READ|PROT_WRITE))_exit(93);
 if(protected_local[95]!=95||errno!=E2BIG)w->handler_failures++;
 errno=before_errno;
}
static void production_install(void){
 atomic_fetch_add(&production_installs,1);struct sigaction action={0};action.sa_sigaction=production_dirty_handler;action.sa_flags=SA_SIGINFO|SA_ONSTACK;sigfillset(&action.sa_mask);
 if(sigaction(SIGSEGV,&action,NULL))abort();
}
static void seed(Cpu*c,uintptr_t sp,unsigned salt){memset(c,0x5a^salt,sizeof *c);for(unsigned i=0;i<32;i++){c->x[i]=UINT64_C(0x1234567800000000)+i+salt;c->v[i]=((__uint128_t)(UINT64_C(0xfedcba9876543210)^i)<<64)|(UINT64_C(0x1122334455667788)^salt^i);}c->x[18]=UINT64_C(0x8912345677889911);c->sp=sp;c->pc=text_base+PC_FAULT;c->nzcv=0x90000000;c->tpidr=0x12344321;c->tpidrro=0x98766789;c->fpcr=0x00800000;c->fpsr=0x10;}
static void compare(Worker*w,const Cpu*expected){int bad=memcmp(&w->cpu,expected,sizeof *expected)!=0;if(bad&&!w->cpu_failures){size_t at=0;while(at<sizeof *expected&&((unsigned char*)&w->cpu)[at]==((const unsigned char*)expected)[at])at++;at&=~(size_t)7;w->first_offset=at;memcpy(&w->actual,(char*)&w->cpu+at,8);memcpy(&w->expected,(const char*)expected+at,8);}w->cpu_failures+=bad;check(w,!bad);}
static int run_native(Worker*w,const void*resume){uint64_t tls=tls_value(),x18=x18_value(),fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();int rc=continuous_context_run(&w->context,resume);int bad=tls_value()!=tls||x18_value()!=x18||rt_get_fpcr()!=fpcr||rt_get_fpsr()!=fpsr;w->host_failures+=bad;check(w,!bad&&rc==0&&!w->context.mode);return rc;}
static void *worker_main(void*opaque){
 Worker*w=opaque;current_worker=w;bound_cpu=&w->cpu;w->host=pthread_self();w->tls=tls_value();w->x18=x18_value();w->page=page_size;
 w->stack=mmap(NULL,w->page*4,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->read_page=mmap(NULL,w->page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->write_page=mmap(NULL,w->page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(w->stack==MAP_FAILED||w->read_page==MAP_FAILED||w->write_page==MAP_FAILED){w->failures++;atomic_store(&w->done,1);return NULL;}
 check(w,!mprotect(w->stack+w->page,w->page*2,PROT_READ|PROT_WRITE));check(w,!continuous_signals_install_thread(production_install));
 uintptr_t sp=(uintptr_t)w->stack+w->page*3-64;*(uint64_t*)w->read_page=UINT64_C(0x778899aabbccddee);*(uint64_t*)w->write_page=0;
 seed(&w->cpu,sp,(unsigned)(w-workers));w->cpu.x[2]=(uintptr_t)w->read_page;w->cpu.x[3]=(uintptr_t)w->write_page;Cpu*expected=malloc(sizeof *expected);if(!expected)abort();memcpy(expected,&w->cpu,sizeof *expected);expected->x[0]=UINT64_C(0x778899aabbccddee);expected->x[18]+=9;expected->pc=text_base+PC_SITE+4;
 check(w,!continuous_context_init(&w->context,&w->cpu));check(w,!mprotect(w->read_page,w->page,PROT_NONE));check(w,!mprotect(w->write_page,w->page,PROT_READ));const void*resume=continuous_image_resume(&w->cpu,text_base+PC_FAULT);check(w,resume!=NULL);run_native(w,resume);compare(w,expected);check(w,w->handler_calls==2&&!w->handler_failures);check(w,*(uint64_t*)w->write_page==UINT64_C(0x778899aabbccddee));check(w,continuous_image_valid_event(w->context.event)&&w->context.event->operand==0x10);
 /* Renderer-like host thread repeatedly installs its own altstack while these
  * registered guests remain in native loops. It must not replace global fronts. */
 seed(&w->cpu,sp,19);w->cpu.x[4]=(uintptr_t)&w->ready;w->cpu.pc=text_base+PC_SPIN;memcpy(expected,&w->cpu,sizeof *expected);check(w,!continuous_context_init(&w->context,&w->cpu));resume=continuous_image_resume(&w->cpu,text_base+PC_SPIN);check(w,resume!=NULL);atomic_store_explicit(&w->ready,1,memory_order_release);run_native(w,resume);check(w,w->context.event==NULL);uint32_t reason=continuous_signals_take_reason(&w->cpu);check(w,reason==CONTINUOUS_SIGNAL_STOP);check(w,w->cpu.pc==text_base+PC_SPIN||w->cpu.pc==text_base+PC_SPIN+4);expected->pc=w->cpu.pc;expected->x[0]=w->cpu.x[0];check(w,expected->x[0]==1);compare(w,expected);w->async_returns++;
 atomic_store(&w->ready,0);atomic_store_explicit(&w->stopped,1,memory_order_release);while(!atomic_load_explicit(&renderer_done,memory_order_acquire))sched_yield();(void)continuous_signals_take_reason(&w->cpu);resume=continuous_image_resume(&w->cpu,w->cpu.pc);check(w,resume!=NULL);run_native(w,resume);expected->x[0]=0;expected->pc=text_base+PC_SPIN_SITE+4;compare(w,expected);check(w,continuous_image_valid_event(w->context.event)&&w->context.event->operand==0x1e);
 atomic_store_explicit(&w->done,1,memory_order_release);free(expected);munmap(w->read_page,w->page);munmap(w->write_page,w->page);munmap(w->stack,w->page*4);current_worker=NULL;bound_cpu=NULL;return NULL;
}
static void *renderer_main(void*unused){(void)unused;bound_cpu=NULL;while(1){unsigned ready=0;for(unsigned i=0;i<THREADS;i++)ready+=atomic_load_explicit(&workers[i].ready,memory_order_acquire);if(ready==THREADS)break;sched_yield();}
 for(unsigned i=0;i<INSTALLS;i++){if(continuous_signals_install_thread(production_install))atomic_fetch_add(&renderer_failures,1);atomic_fetch_add(&renderer_installs,1);}
 for(unsigned attempts=0;attempts<2000;attempts++){unsigned done=0;for(unsigned i=0;i<THREADS;i++){done+=atomic_load_explicit(&workers[i].stopped,memory_order_acquire);if(!atomic_load(&workers[i].stopped)&&atomic_load(&workers[i].ready)&&continuous_signals_interrupt_cpu(&workers[i].cpu,CONTINUOUS_SIGNAL_STOP))atomic_fetch_add(&renderer_failures,1);}if(done==THREADS)break;usleep(2000);}atomic_store_explicit(&renderer_done,1,memory_order_release);return NULL;}
static int setup_image(char*error,size_t capacity){
 page_size=(size_t)sysconf(_SC_PAGESIZE);image_mapping=mmap(NULL,WINDOW_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(image_mapping==MAP_FAILED)return -1;text_base=(uintptr_t)image_mapping;
 SessionLayout layout={.struct_size=sizeof layout,.version=SESSION_LAYOUT_VERSION,.module_count=1,.page_size=(uint32_t)page_size,.window={text_base,text_base+WINDOW_BYTES}};CnceImageModule module={text_base,2u*1024u*1024u,0,page_size};layout.modules[0].image=(SessionRange){text_base,text_base+module.image_size};layout.regions[SESSION_HLE]=(SessionRange){text_base+64u*1024u*1024u,text_base+65u*1024u*1024u};for(unsigned r=1;r<SESSION_REGION_COUNT;r++)layout.regions[r]=(SessionRange){text_base+(70u+r)*1024u*1024u,text_base+(71u+r)*1024u*1024u};
 for(size_t off=0;off<page_size;off+=4){uint32_t nop=0xd503201f;memcpy(image_mapping+off,&nop,4);}memcpy(image_mapping,program_bytes,sizeof program_bytes);
 return continuous_image_prepare(&layout,&module,1,NULL,0,(uintptr_t)continuous_context_exit,error,capacity);
}
#ifdef CONTINUOUS_SIGNALS_HOST_TEST
/* Host tests use production POSIX routing but cannot execute ARM/private TLS. */
void continuous_context_enter(CnceContext*c,const void*r){(void)c;(void)r;}
void continuous_context_exit(void){}
void continuous_context_signal_return(CnceContext*c){(void)c;}
void continuous_context_signal_front(int s,void*i,void*u){continuous_signals_dispatch(s,i,u,NULL);}
int main(void){
 unsigned failures=0,checks=0;char error[256];checks++;failures+=setup_image(error,sizeof error)!=0;ContinuousSignalHooks hooks={is_guest_pc,original_pc};checks++;failures+=continuous_signals_configure(&hooks)!=0;checks++;failures+=continuous_signals_install_thread(production_install)!=0;checks++;failures+=atomic_load(&production_installs)!=1;
 for(unsigned i=0;i<INSTALLS;i++){checks++;failures+=continuous_signals_install_thread(production_install)!=0;}checks++;failures+=atomic_load(&production_installs)!=1;ContinuousSignalStats stats;continuous_signals_stats(&stats);checks++;failures+=stats.registered_threads!=0;
 continuous_image_finish_after_join();munmap(image_mapping,WINDOW_BYTES);printf("checks=%u failures=%u host-native-execution=false\n",checks,failures);return failures!=0;
}
#else
__attribute__((visibility("default"))) int continuous_signals_probe(char*out,size_t capacity){
 memset(workers,0,sizeof workers);char error[256];if(setup_image(error,sizeof error)){snprintf(out,capacity,"{\"schema\":1,\"state\":\"failed\",\"stage\":\"image\",\"error\":\"%.100s\"}",error);return -1;}
 ContinuousSignalHooks hooks={is_guest_pc,original_pc};if(continuous_signals_configure(&hooks)){snprintf(out,capacity,"{\"schema\":1,\"state\":\"failed\",\"stage\":\"configure\"}");return -1;}
 pthread_t threads[THREADS],renderer;for(unsigned i=0;i<THREADS;i++)if(pthread_create(&threads[i],NULL,worker_main,&workers[i]))abort();if(pthread_create(&renderer,NULL,renderer_main,NULL))abort();for(unsigned i=0;i<THREADS;i++)pthread_join(threads[i],NULL);pthread_join(renderer,NULL);
 unsigned checks=0,failures=atomic_load(&renderer_failures),cpu=0,host=0,handlers=0,handlerfail=0,returns=0;Worker*first=NULL;for(unsigned i=0;i<THREADS;i++){Worker*w=&workers[i];checks+=w->checks;failures+=w->failures;cpu+=w->cpu_failures;host+=w->host_failures;handlers+=w->handler_calls;handlerfail+=w->handler_failures;returns+=w->async_returns;if(!first&&w->cpu_failures)first=w;}
 ContinuousSignalStats stats;continuous_signals_stats(&stats);failures+=atomic_load(&production_installs)!=1||atomic_load(&renderer_installs)!=INSTALLS||stats.registered_threads!=THREADS||stats.chained_faults!=THREADS*2||stats.resumed_faults!=THREADS*2||stats.interrupted_guests!=THREADS;
 snprintf(out,capacity,"{\"schema\":1,\"state\":\"%s\",\"checks\":%u,\"failures\":%u,\"cpu_failures\":%u,\"host_failures\":%u,\"production_handler_calls\":%u,\"handler_failures\":%u,\"async_returns\":%u,\"production_installs\":%u,\"renderer_installs\":%u,\"renderer_failures\":%u,\"registered_threads\":%"PRIu64",\"chained_faults\":%"PRIu64",\"resumed_faults\":%"PRIu64",\"interrupted_guests\":%"PRIu64",\"deferred_interrupts\":%"PRIu64",\"first_cpu_offset\":%"PRIu64",\"first_cpu_actual\":\"%016"PRIx64"\",\"first_cpu_expected\":\"%016"PRIx64"\",\"guard_pages\":%u,\"stack_protector_strong\":true,\"actual_signal_module\":true,\"actual_image_module\":true,\"real_kthread\":false,\"game_started\":false}",failures?"failed":"passed",checks,failures,cpu,host,handlers,handlerfail,returns,atomic_load(&production_installs),atomic_load(&renderer_installs),atomic_load(&renderer_failures),stats.registered_threads,stats.chained_faults,stats.resumed_faults,stats.interrupted_guests,stats.deferred_interrupts,first?first->first_offset:0,first?first->actual:0,first?first->expected:0,THREADS*4+2);
 continuous_image_finish_after_join();munmap(image_mapping,WINDOW_BYTES);return failures?-1:0;
}
#endif
