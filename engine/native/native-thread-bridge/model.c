#include "bridge.h"

int nt_site_allowed(const NtSite *s) {
    return s && ((s->kind == NT_SITE_TLS_RO && !s->svc) ||
        (s->kind == NT_SITE_SDK && s->svc == 0x10) ||
        (s->kind == NT_SITE_SVC && (s->svc == 0x10 || s->svc == 0x25 || s->svc == 0x1e || s->svc == 0x0a)));
}
int nt_patch_branch(uint32_t source, uintptr_t pc, uintptr_t target, uint32_t *out) {
    int valid = source == 0xd53bd060u; /* MRS X0,TPIDRRO_EL0 */
    for (unsigned i = 0; i < 4; ++i) {
        static const unsigned svc[] = {0x10,0x25,0x1e,0x0a};
        valid |= source == (0xd4000001u | svc[i] << 5);
    }
    if (!out || !valid || ((pc | target) & 3u)) return -1;
    int64_t delta;
    if (target >= pc) { if (target - pc >= (1u << 27)) return -1; delta = (int64_t)(target - pc); }
    else { if (pc - target > (1u << 27)) return -1; delta = -(int64_t)(pc - target); }
    *out = 0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu);
    return 0;
}
void nt_materialize(Cpu *c, const NtFrame *f) {
    for (unsigned i = 0; i < 31; ++i) if (i != 18 && i != 19) c->x[i] = f->x[i];
    c->sp=f->sp; c->nzcv=f->nzcv; c->fpcr=f->fpcr; c->fpsr=f->fpsr;
    memcpy(c->v,f->q,sizeof c->v);
}
void nt_restore(NtFrame *f, const Cpu *c) {
    for (unsigned i = 0; i < 31; ++i) if (i != 18 && i != 19) f->x[i] = c->x[i];
    f->sp=c->sp; f->nzcv=c->nzcv; f->fpcr=c->fpcr; f->fpsr=c->fpsr;
    memcpy(f->q,c->v,sizeof f->q);
}
int nt_opaque_equal(const Cpu *a, const Cpu *b) {
    return a->x[31] == b->x[31] && a->x[18] == b->x[18] && a->x[19] == b->x[19] &&
        !memcmp(&a->tpidr,&b->tpidr,offsetof(Cpu,fpcr)-offsetof(Cpu,tpidr)) &&
        !memcmp(&a->excl_hi,&b->excl_hi,offsetof(Cpu,v)-offsetof(Cpu,excl_hi)) &&
        !memcmp(a->dcache,b->dcache,sizeof a->dcache);
}
static int host_good(const NtContext *c) {
#ifdef __aarch64__
    uint64_t tls, x18;
    __asm__ volatile("mrs %0,tpidr_el0":"=r"(tls));
    __asm__ volatile("mov %0,x18":"=r"(x18));
    return tls==c->host_tls && x18==c->host_x18;
#else
    (void)c; return 1; /* Host unit tests do not prove Android host TLS. */
#endif
}
int nt_frame_good(const NtContext *c,const NtFrame *f,const NtSite *site) {
    int ok=host_good(c) && f->sp==((c->stack_enabled&&!site)?c->return_sp:c->expected_sp) && !(f->sp&15) && f->x[18]==c->host_x18 && f->x[19]==(uintptr_t)c &&
        f->nzcv==c->seed->nzcv && f->fpcr==c->seed->fpcr && f->fpsr==c->seed->fpsr &&
        !memcmp(f->q,c->seed->v,sizeof f->q);
    for(unsigned i=2;i<31;++i) if(i!=18 && i!=19) {
        uint64_t expected=(i==30 && site && site->kind==NT_SITE_SDK)?c->sdk_lr:c->seed->x[i];
        ok &= f->x[i]==expected;
    }
    return ok;
}
void nt_call(NtContext *c, NtFrame *f, const NtSite *s) {
    volatile unsigned char host_marker=0;
    if(c->stack_enabled) {
        c->stack_checks++;c->native_stack_canary_checks++;
        if(!nt_stack_transition_good(c,f,(uintptr_t)&host_marker,0)) { c->stack_failures++;__builtin_trap(); }
    }
    int found=0;
    for(size_t i=0;i<c->site_count;++i) found |= !memcmp(s,&c->sites[i],sizeof *s);
    if(!found || !nt_site_allowed(s)) { c->state_failures++; return; }
    c->state_failures += !nt_frame_good(c,f,s);
    nt_materialize(c->cpu,f); c->cpu->pc=s->pc;
    c->opaque_failures += !nt_opaque_equal(c->cpu,c->opaque);
    SharedGuestThreadSnapshot q={.struct_size=sizeof q,.version=SHARED_GUEST_API_VERSION};
    uint32_t reasons=nt_identity_reason(c,c->current(c->cpu,&q),&q);
    c->identity_failures += reasons!=0;
    c->identity_reasons |= reasons;
    if(s->kind==NT_SITE_SDK) {
        c->sdk_calls++;c->sdk_lr_checks++;
        c->sdk_lr_failures+=f->x[30]!=c->sdk_lr || c->sdk_saved_lr!=c->seed->x[30];
    }
    memcpy(c->before,c->cpu,sizeof(Cpu));
    if(s->kind==NT_SITE_TLS_RO) { c->cpu->x[0]=c->cpu->tpidrro; c->tls_reads++; }
    else {
        if(s->svc==0x0a) c->exit_entered++;
        c->svc(c->cpu,s->svc,s->pc);
        if(s->svc==0x0a) c->exit_returned++;
        c->transitions++;
        if(s->svc==0x10) c->state_failures += c->cpu->x[0]!=c->expected_core;
        if(s->svc==0x25) c->state_failures += c->cpu->x[0]!=0 || c->cpu->x[1]!=c->expected_id;
    }
    unsigned allowed=(s->kind==NT_SITE_SVC && s->svc==0x25)?16:8;
    c->state_failures += memcmp((char*)c->cpu+allowed,(char*)c->before+allowed,sizeof(Cpu)-allowed)!=0;
    nt_restore(f,c->cpu);
}
void nt_observe(NtContext *c, NtFrame *f) {
    volatile unsigned char host_marker=0;
    if(c->stack_enabled) {
        c->stack_checks++;
        if(!nt_stack_transition_good(c,f,(uintptr_t)&host_marker,1)) { c->stack_failures++;__builtin_trap(); }
    }
    c->state_failures += !nt_frame_good(c,f,NULL);
    c->opaque_failures += !nt_opaque_equal(c->cpu,c->opaque);
    c->returned_native++;
}

NtTotals nt_totals(const NtContext *contexts, unsigned joined) {
    NtTotals totals={0};
    if(joined!=2 || !contexts) return totals;
    totals.known=1;
    for(unsigned i=0;i<2;i++) {
        totals.state_failures+=contexts[i].state_failures;
        totals.identity_failures+=contexts[i].identity_failures;
        totals.identity_reasons|=contexts[i].identity_reasons;
        totals.opaque_failures+=contexts[i].opaque_failures;
        totals.transitions+=contexts[i].transitions;
        totals.tls_reads+=contexts[i].tls_reads;
        totals.exit_entered+=contexts[i].exit_entered;
        totals.exit_returned+=contexts[i].exit_returned;
        totals.sdk_calls+=contexts[i].sdk_calls;
        totals.sdk_returns+=contexts[i].sdk_returns;
        totals.sdk_lr_checks+=contexts[i].sdk_lr_checks;
        totals.sdk_lr_failures+=contexts[i].sdk_lr_failures;
        totals.sdk_resume_failures+=contexts[i].sdk_resume_failures;
        totals.aot_calls+=contexts[i].aot_calls;
        totals.aot_failures+=contexts[i].aot_failures;
        totals.stack_checks+=contexts[i].stack_checks;
        totals.stack_failures+=contexts[i].stack_failures;
        totals.native_stack_canary_checks+=contexts[i].native_stack_canary_checks;
        totals.entry_rejections+=contexts[i].entry_rejections;
    }
    return totals;
}

uint32_t nt_identity_reason(const NtContext *c, enum SharedGuestStatus status, const SharedGuestThreadSnapshot *q) {
    if(status!=SHARED_GUEST_OK) return NT_ID_API;
    uint32_t reasons=0;
    const uint32_t flags=SHARED_GUEST_THREAD_CURRENT|SHARED_GUEST_THREAD_CPU_STATE_VALID;
    if((q->flags&flags)!=flags) reasons|=NT_ID_FLAGS;
    if(q->handle!=c->expected_handle) reasons|=NT_ID_HANDLE;
    if(q->thread_id!=c->expected_id) reasons|=NT_ID_THREAD;
    if(q->tls_base!=c->expected_tlr) reasons|=NT_ID_TLS_BASE;
    /* Snapshot guest_tpidr is the independent writable TPIDR_EL0 value.
     * The kernel assigns its TLS slot to read-only TPIDRRO_EL0 only. */
    if(q->guest_tpidr!=c->opaque->tpidr || c->cpu->tpidr!=c->opaque->tpidr) reasons|=NT_ID_TPIDR_RW;
    if(c->cpu->tpidrro!=c->expected_tlr) reasons|=NT_ID_TPIDR_RO;
    if(q->cpu_address!=(uintptr_t)c->cpu) reasons|=NT_ID_CPU;
    if(q->core!=(int)c->expected_core) reasons|=NT_ID_CORE;
    return reasons;
}

int nt_sdk_input_valid(const NtSdkInput *input) {
    if(!input || input->struct_size!=sizeof *input || input->version!=NT_SDK_INPUT_VERSION || input->reserved ||
       !input->aot_address || (input->aot_address&3) || (input->words[0]&0xfc000000u)!=0x14000000u ||
       input->words[1]!=0xd4000201u || input->words[2]!=0xd65f03c0u) return 0;
    int64_t immediate=input->words[0]&0x03ffffffu;
    if(immediate&(1u<<25)) immediate-=INT64_C(1)<<26;
    int64_t delta=immediate*4;
    return delta==input->original_delta && delta!=0 && delta!=-4;
}
int nt_sdk_resume_good(const NtContext *c) {
    return !c->sdk_enabled || (c->sdk_calls==1 && c->sdk_returns==1 && c->sdk_lr_checks==1 &&
        !c->sdk_lr_failures && c->sdk_resume_seen==c->sdk_lr && c->sdk_saved_lr==c->seed->x[30] &&
        c->sdk_seen==c->expected_core && c->aot_calls==1 && c->aot_seen==c->sdk_seen && !c->aot_failures);
}

ModuleAbiGuestFn nt_sdk_descriptor_entry(const ModuleAbiDescriptor *descriptor, uint64_t address) {
    if(!descriptor || descriptor->struct_size!=sizeof *descriptor || !descriptor->modules ||
       !descriptor->module_count || descriptor->module_count>MODULE_ABI_MAX_MODULES ||
       descriptor->identity.cpu_identity!=MODULE_ABI_CPU_A64_SHARED_66560) return NULL;
    for(uint32_t m=0;m<descriptor->module_count;m++) {
        const ModuleAbiModule *module=&descriptor->modules[m];
        if(module->image.module_id!=MODULE_ABI_SDK) continue;
        if(!module->addresses || !module->functions || module->address_count!=module->image.function_count ||
           module->image.canonical_base<descriptor->identity.guest_shift) return NULL;
        uint64_t base=module->image.canonical_base-descriptor->identity.guest_shift;
        if(address<base || address-base<module->image.text_offset ||
           address-base-module->image.text_offset>=module->image.text_size) return NULL;
        uint64_t lo=0,hi=module->address_count;
        while(lo<hi) { uint64_t mid=lo+(hi-lo)/2;if(module->addresses[mid]<address)lo=mid+1;else hi=mid; }
        return lo<module->address_count && module->addresses[lo]==address ? module->functions[lo] : NULL;
    }
    return NULL;
}

int nt_stack_canaries_good(const NtContext *c) {
    uint64_t lo=c->guest_stack_lo,hi=c->guest_stack_hi;
    if(!lo || hi<=lo || ((lo|hi)&15) || hi-lo<sizeof(NtFrame)+256) return 0;
    const uint64_t *low=(const uint64_t *)(uintptr_t)lo,*high=(const uint64_t *)(uintptr_t)(hi-64);
    for(unsigned i=0;i<8;i++) if(low[i]!=(c->stack_cookie^i) || high[i]!=(~c->stack_cookie^i)) return 0;
    return 1;
}
int nt_stack_transition_good(const NtContext *c,const NtFrame *frame,uintptr_t host_address,int returning) {
    if(!c->stack_enabled) return 1;
    uintptr_t address=(uintptr_t)frame;
    if(c->entry_depth!=1 || c->gateway_active!=1 || c->active_frame!=address ||
       c->host_stack_hi<=c->host_stack_lo || host_address<c->host_stack_lo || host_address>=c->host_stack_hi ||
       host_address>=c->host_call_sp || c->host_sp>=c->host_stack_hi || c->host_sp<c->host_stack_lo ||
       c->guest_stack_hi<=c->guest_stack_lo || address<c->guest_stack_lo+64 ||
       address>c->guest_stack_hi-sizeof *frame-64 || (address&15) ||
       (host_address>=c->guest_stack_lo && host_address<c->guest_stack_hi)) return 0;
    if(frame->sp!=address+sizeof *frame || frame->sp!=(returning?c->return_sp:c->expected_sp) ||
       !nt_stack_canaries_good(c)) return 0;
    if(!returning) {
        if(frame->sp>c->guest_stack_hi-64-32) return 0;
        const uint64_t *spill=(const uint64_t *)(uintptr_t)frame->sp;
        for(unsigned i=0;i<4;i++) if(spill[i]!=c->seed->x[20+i]) return 0;
    }
    return 1;
}
