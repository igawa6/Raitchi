#define _GNU_SOURCE
#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned assertions;
#define CHECK(x) do { ++assertions;if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static NtContext *active;
static enum SharedGuestStatus current(const Cpu *cpu,SharedGuestThreadSnapshot *q) {
    CHECK(cpu==active->cpu);q->flags=SHARED_GUEST_THREAD_CURRENT|SHARED_GUEST_THREAD_CPU_STATE_VALID;q->handle=active->expected_handle;
    q->core=(int)active->expected_core;q->thread_id=active->expected_id;q->tls_base=active->expected_tlr;q->guest_tpidr=cpu->tpidr;q->cpu_address=(uintptr_t)cpu;return SHARED_GUEST_OK;
}
static void svc(Cpu *cpu,uint32_t immediate,uint64_t pc) {
    CHECK(pc==active->sites[0].pc);CHECK(cpu->pc==pc);
    if(immediate==0x10)cpu->x[0]=active->expected_core;
    if(immediate==0x25) {CHECK(cpu->x[1]==0xffff8000u);cpu->x[0]=0;cpu->x[1]=active->expected_id;}
    if(immediate==0x1e)cpu->x[0]=123456789u;
}
int main(void) {
    uint32_t branch=0;CHECK(!nt_patch_branch(0xd4000201,0x1000,0x2000,&branch));CHECK(branch==0x14000400);
    CHECK(!nt_patch_branch(0xd53bd060,0x2000,0x1000,&branch));CHECK(branch==0x17fffc00);
    CHECK(!nt_patch_branch(0xd4000141,0x8000000,0,&branch));CHECK(branch==0x16000000);
    CHECK(nt_patch_branch(0xd4000201,0,0x8000000,&branch));CHECK(nt_patch_branch(0xd4000201,0x8000004,0,&branch));
    CHECK(nt_patch_branch(0xd4000201,1,4,&branch));CHECK(nt_patch_branch(0xd4000001,0,4,&branch));CHECK(nt_patch_branch(0xd65f03c0,0,4,&branch));CHECK(nt_patch_branch(0xd4000201,0,4,NULL));
    Cpu *cpu=aligned_alloc(_Alignof(Cpu),sizeof(Cpu)),*seed=aligned_alloc(_Alignof(Cpu),sizeof(Cpu)),*before=aligned_alloc(_Alignof(Cpu),sizeof(Cpu)),*opaque=aligned_alloc(_Alignof(Cpu),sizeof(Cpu));
    CHECK(cpu&&seed&&before&&opaque);memset(cpu,0x5a,sizeof *cpu);
    for(unsigned i=0;i<32;i++) {cpu->x[i]=0x1234567800000000u+i;cpu->v[i]=((__uint128_t)(i+1)<<100)|i;}
    cpu->nzcv=0xa0000000;cpu->fpcr=0;cpu->fpsr=1;cpu->sp=0x10000;cpu->tpidrro=0x7070000000;cpu->tpidr=0x123400; /* RW and RO TLS values are intentionally distinct. */
    NtContext c={.cpu=cpu,.seed=seed,.before=before,.opaque=opaque,.current=current,.svc=svc,.expected_sp=cpu->sp,.host_x18=18,.expected_id=0x51,.expected_tlr=cpu->tpidrro,.expected_handle=0x101,.expected_core=2};active=&c;
    memcpy(seed,cpu,sizeof *cpu);memcpy(opaque,cpu,sizeof *cpu);
    SharedGuestThreadSnapshot identity={.struct_size=sizeof identity,.version=SHARED_GUEST_API_VERSION};
    CHECK(current(cpu,&identity)==SHARED_GUEST_OK);
    CHECK(identity.guest_tpidr!=identity.tls_base);
    CHECK(!nt_identity_reason(&c,SHARED_GUEST_OK,&identity));
    identity.guest_tpidr=identity.tls_base;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_TPIDR_RW);
    identity.guest_tpidr=cpu->tpidr;
    cpu->tpidrro^=1;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_TPIDR_RO);cpu->tpidrro^=1;
    cpu->tpidr^=1;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_TPIDR_RW);cpu->tpidr^=1;
    identity.flags&=~SHARED_GUEST_THREAD_CPU_STATE_VALID;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_FLAGS);identity.flags|=SHARED_GUEST_THREAD_CPU_STATE_VALID;
    identity.cpu_address++;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_CPU);identity.cpu_address--;
    identity.tls_base++;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_TLS_BASE);identity.tls_base--;
    identity.handle++;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_HANDLE);identity.handle--;
    identity.thread_id++;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_THREAD);identity.thread_id--;
    identity.core++;CHECK(nt_identity_reason(&c,SHARED_GUEST_OK,&identity)==NT_ID_CORE);identity.core--;
    CHECK(nt_identity_reason(&c,SHARED_GUEST_CURRENT_MISMATCH,&identity)==NT_ID_API);
    NtFrame f={0};nt_restore(&f,cpu);f.x[18]=18;f.x[19]=(uintptr_t)&c;
    NtSdkInput sdk={.struct_size=sizeof sdk,.version=NT_SDK_INPUT_VERSION,
        .words={0x14000004u,0xd4000201u,0xd65f03c0u},.original_delta=16,.aot_address=0x6000};
    CHECK(nt_sdk_input_valid(&sdk));sdk.original_delta=20;CHECK(!nt_sdk_input_valid(&sdk));sdk.original_delta=16;
    sdk.words[0]=0x94000004u;CHECK(!nt_sdk_input_valid(&sdk));sdk.words[0]=0x14000004u;
    sdk.words[1]=0xd40003c1u;CHECK(!nt_sdk_input_valid(&sdk));sdk.words[1]=0xd4000201u;
    sdk.words[2]=0xd61f03c0u;CHECK(!nt_sdk_input_valid(&sdk));sdk.words[2]=0xd65f03c0u;
    sdk.reserved=1;CHECK(!nt_sdk_input_valid(&sdk));sdk.reserved=0;
    sdk.aot_address=0;CHECK(!nt_sdk_input_valid(&sdk));sdk.aot_address=0x6000;
    sdk.words[0]=0x17fffffcu;sdk.original_delta=-16;CHECK(nt_sdk_input_valid(&sdk));
    sdk.words[0]=0x17ffffffu;sdk.original_delta=-4;CHECK(!nt_sdk_input_valid(&sdk));
    sdk.words[0]=0x14000000u;sdk.original_delta=0;CHECK(!nt_sdk_input_valid(&sdk));
    sdk.words[0]=0x14000004u;sdk.original_delta=16;
    const uint64_t addresses[]={0x5000,0x6000};const ModuleAbiGuestFn functions[]={NULL,(ModuleAbiGuestFn)0x1000};
    ModuleAbiModule module={.image={.module_id=MODULE_ABI_SDK,.canonical_base=0x4000,.text_size=0x4000,.function_count=2},.address_count=2,.addresses=addresses,.functions=functions};
    ModuleAbiDescriptor descriptor={.struct_size=sizeof descriptor,.module_count=1,.identity={.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560},.modules=&module};
    CHECK(nt_sdk_descriptor_entry(&descriptor,0x6000)==functions[1]);
    CHECK(!nt_sdk_descriptor_entry(&descriptor,0x6004));CHECK(!nt_sdk_descriptor_entry(&descriptor,0x5000));
    module.image.module_id=MODULE_ABI_MAIN;CHECK(!nt_sdk_descriptor_entry(&descriptor,0x6000));module.image.module_id=MODULE_ABI_SDK;
    descriptor.identity.cpu_identity=MODULE_ABI_CPU_A64_CACHE_66560;CHECK(!nt_sdk_descriptor_entry(&descriptor,0x6000));descriptor.identity.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560;
    module.image.text_size=0x100;CHECK(!nt_sdk_descriptor_entry(&descriptor,0x6000));module.image.text_size=0x4000;
    NtSite sdk_site={0x3000,NT_SITE_SDK,0x10};c.sdk_enabled=1;c.sdk_lr=0x8000;c.sdk_saved_lr=seed->x[30];
    CHECK(nt_site_allowed(&sdk_site));sdk_site.svc=0x1e;CHECK(!nt_site_allowed(&sdk_site));sdk_site.svc=0x10;
    f.x[30]=c.sdk_lr;CHECK(nt_frame_good(&c,&f,&sdk_site));CHECK(!nt_frame_good(&c,&f,NULL));
    f.x[30]++;CHECK(!nt_frame_good(&c,&f,&sdk_site));f.x[30]--;
    c.site_count=1;c.sites[0]=sdk_site;nt_call(&c,&f,&c.sites[0]);
    CHECK(c.sdk_calls==1&&c.sdk_lr_checks==1&&!c.sdk_lr_failures&&!c.state_failures&&f.x[30]==c.sdk_lr);
    c.sdk_returns=1;c.sdk_resume_seen=c.sdk_lr;c.sdk_seen=c.expected_core;c.aot_calls=1;c.aot_seen=c.expected_core;
    CHECK(nt_sdk_resume_good(&c));c.sdk_resume_seen++;CHECK(!nt_sdk_resume_good(&c));c.sdk_resume_seen--;
    c.sdk_returns++;CHECK(!nt_sdk_resume_good(&c));c.sdk_returns--;
    c.sdk_saved_lr++;CHECK(!nt_sdk_resume_good(&c));c.sdk_saved_lr--;
    c.aot_seen++;CHECK(!nt_sdk_resume_good(&c));c.aot_seen--;
    c.sdk_enabled=0;c.transitions=0;memcpy(cpu,seed,sizeof *cpu);nt_restore(&f,cpu);f.x[18]=18;f.x[19]=(uintptr_t)&c;
    c.site_count=1;
    const unsigned svcs[]={0x10,0x25,0x1e};
    for(unsigned iteration=0;iteration<16;iteration++) for(unsigned j=0;j<3;j++) {
        c.sites[0]=(NtSite){0x1000+iteration*16+j*4,NT_SITE_SVC,svcs[j]};
        if(svcs[j]==0x25)f.x[1]=0xffff8000u;
        nt_call(&c,&f,&c.sites[0]);CHECK(!c.state_failures&&!c.identity_failures&&!c.opaque_failures);
        CHECK(f.x[0]==(svcs[j]==0x10?2:svcs[j]==0x25?0:123456789));
        for(unsigned i=2;i<31;i++)if(i!=18&&i!=19)CHECK(f.x[i]==seed->x[i]);
        CHECK(!memcmp(f.q,seed->v,sizeof f.q));CHECK(f.sp==0x10000);CHECK(f.x[18]==18&&f.x[19]==(uintptr_t)&c);
    }
    CHECK(c.transitions==48);c.sites[0]=(NtSite){0x2000,NT_SITE_TLS_RO,0};nt_call(&c,&f,&c.sites[0]);CHECK(f.x[0]==c.expected_tlr&&c.tls_reads==1);
    NtSite bad={0x9999,NT_SITE_SVC,0x10};nt_call(&c,&f,&bad);CHECK(c.state_failures==1);
    f.q[17]^=1;nt_call(&c,&f,&c.sites[0]);CHECK(c.state_failures==2);f.q[17]^=1;
    CHECK(nt_opaque_equal(cpu,opaque));cpu->x[18]^=1;CHECK(!nt_opaque_equal(cpu,opaque));cpu->x[18]^=1;
    cpu->thread=(void*)0x1234;CHECK(!nt_opaque_equal(cpu,opaque));cpu->thread=opaque->thread;
    cpu->dcache[15].addr^=1;CHECK(!nt_opaque_equal(cpu,opaque));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *guard_map=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(guard_map!=MAP_FAILED);CHECK(!mprotect(guard_map+page,page,PROT_READ|PROT_WRITE));
    NtContext guarded=c;guarded.stack_enabled=1;guarded.guest_stack_lo=(uintptr_t)(guard_map+page);guarded.guest_stack_hi=(uintptr_t)(guard_map+2*page);
    guarded.guest_entry_sp=guarded.guest_stack_hi-128;guarded.return_sp=guarded.guest_entry_sp;guarded.expected_sp=guarded.return_sp-32;
    guarded.host_stack_lo=0x800000;guarded.host_stack_hi=0x900000;guarded.host_sp=0x890000;guarded.host_call_sp=0x88f000;
    guarded.entry_depth=1;guarded.gateway_active=1;guarded.stack_cookie=0xfeed123456789000;
    uint64_t *low=(uint64_t*)(uintptr_t)guarded.guest_stack_lo,*high=(uint64_t*)(uintptr_t)(guarded.guest_stack_hi-64),*spill=(uint64_t*)(uintptr_t)guarded.expected_sp;
    for(unsigned i=0;i<8;i++) {low[i]=guarded.stack_cookie^i;high[i]=~guarded.stack_cookie^i;}
    for(unsigned i=0;i<4;i++)spill[i]=seed->x[20+i];
    NtFrame *guest_frame=(NtFrame*)(uintptr_t)(guarded.expected_sp-sizeof(NtFrame));memset(guest_frame,0,sizeof *guest_frame);guest_frame->sp=guarded.expected_sp;guarded.active_frame=(uintptr_t)guest_frame;
    CHECK(nt_stack_canaries_good(&guarded));CHECK(nt_stack_transition_good(&guarded,guest_frame,0x880000,0));
    CHECK(!nt_stack_transition_good(&guarded,guest_frame,(uintptr_t)guest_frame,0));CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x900001,0));
    guarded.entry_depth=2;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));guarded.entry_depth=1;
    guarded.gateway_active=0;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));guarded.gateway_active=1;
    guarded.active_frame++;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));guarded.active_frame--;
    guarded.active_frame=guarded.guest_stack_lo-16;CHECK(!nt_stack_transition_good(&guarded,(const NtFrame*)(uintptr_t)guarded.active_frame,0x880000,0));guarded.active_frame=(uintptr_t)guest_frame;
    low[0]^=1;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));low[0]^=1;
    high[7]^=1;CHECK(!nt_stack_canaries_good(&guarded));high[7]^=1;
    spill[2]^=1;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));spill[2]^=1;
    guest_frame->sp+=16;CHECK(!nt_stack_transition_good(&guarded,guest_frame,0x880000,0));guest_frame->sp-=16;
    NtFrame *return_frame=(NtFrame*)(uintptr_t)(guarded.return_sp-sizeof(NtFrame));memmove(return_frame,guest_frame,sizeof *return_frame);return_frame->sp=guarded.return_sp;guarded.active_frame=(uintptr_t)return_frame;
    CHECK(nt_stack_transition_good(&guarded,return_frame,0x880000,1));
    /* Real inaccessible low/high guards: children must fault, never continue. */
    for(unsigned side=0;side<2;side++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {struct rlimit no_core={0,0};setrlimit(RLIMIT_CORE,&no_core);signal(SIGSEGV,SIG_DFL);volatile unsigned char *target=guard_map+(side?2*page:0);*target=1;_exit(0);}
        int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
    }
    CHECK(!munmap(guard_map,page*3));
    /* A timeout/start failure may leave callbacks mutating every counter.
     * Nonjoined reports must not read their memory, even to print a failure. */
    for(unsigned joined=0;joined<2;joined++) {
        NtTotals t=nt_totals((const NtContext *)(uintptr_t)1,joined);
        CHECK(!t.known&&!t.state_failures&&!t.identity_failures&&!t.identity_reasons&&!t.opaque_failures&&!t.transitions&&!t.tls_reads&&!t.exit_entered&&!t.exit_returned);
    }
    NtContext ended[2]={{.state_failures=1,.identity_failures=2,.identity_reasons=NT_ID_TPIDR_RW,.transitions=24,.tls_reads=8},
                        {.state_failures=3,.identity_reasons=NT_ID_CPU,.opaque_failures=4,.transitions=24,.tls_reads=8,.exit_entered=1}};
    NtTotals totals=nt_totals(ended,2);
    CHECK(totals.known&&totals.state_failures==4&&totals.identity_failures==2&&totals.opaque_failures==4);
    CHECK(totals.identity_reasons==(NT_ID_TPIDR_RW|NT_ID_CPU));
    CHECK(totals.transitions==48&&totals.tls_reads==16&&totals.exit_entered==1&&!totals.exit_returned);
    CHECK(!nt_totals(NULL,2).known);
    free(cpu);free(seed);free(before);free(opaque);printf("assertions=%u\n",assertions);return 0;
}
