/* Independently implemented shared-Cpu adapter. Architectural reference:
 * Eden/yuzu NCE, pinned in docs/native-nce-next-step.md. No literal source reuse.
 * All TLS-changing paths are assembly-only until Android TLS/X18 are restored. */
.text
.p2align 2
.hidden continuous_context_transition_begin
.global continuous_context_transition_begin
continuous_context_transition_begin:
.hidden continuous_context_enter
.global continuous_context_enter
.type continuous_context_enter,%function
continuous_context_enter:
    clrex
    sub sp,sp,#256
    stp x19,x20,[sp,#0]
    stp x21,x22,[sp,#16]
    stp x23,x24,[sp,#32]
    stp x25,x26,[sp,#48]
    stp x27,x28,[sp,#64]
    stp x29,x30,[sp,#80]
    stp q8,q9,[sp,#96]
    stp q10,q11,[sp,#128]
    stp q12,q13,[sp,#160]
    stp q14,q15,[sp,#192]
    mrs x9,nzcv
    str x9,[sp,#224]
    mrs x9,tpidr_el0
    str x9,[x0,#16]
    str x18,[x0,#24]
    mov x9,sp
    str x9,[x0,#32]
    str xzr,[x0,#40]
    mrs x9,fpcr
    str x9,[x0,#56]
    mrs x9,fpsr
    str x9,[x0,#64]
    mov x9,#1
    str x9,[x0,#48]
    mov x16,x1
    ldr x17,[x0,#8]
    msr tpidr_el0,x0
    ldr x9,[x17,#256]
    sub x9,x9,#16
    ldp x10,x11,[x17,#128]
    stp x10,x11,[x9]
    mov sp,x9
    ldr x9,[x17,#320]
    msr fpcr,x9
    ldr x9,[x17,#328]
    msr fpsr,x9
    ldr x9,[x17,#272]
    msr nzcv,x9
    ldp q0,q1,[x17,#512]
    ldp q2,q3,[x17,#544]
    ldp q4,q5,[x17,#576]
    ldp q6,q7,[x17,#608]
    ldp q8,q9,[x17,#640]
    ldp q10,q11,[x17,#672]
    ldp q12,q13,[x17,#704]
    ldp q14,q15,[x17,#736]
    ldp q16,q17,[x17,#768]
    ldp q18,q19,[x17,#800]
    ldp q20,q21,[x17,#832]
    ldp q22,q23,[x17,#864]
    ldp q24,q25,[x17,#896]
    ldp q26,q27,[x17,#928]
    ldp q28,q29,[x17,#960]
    ldp q30,q31,[x17,#992]
    ldp x0,x1,[x17,#0]
    ldp x2,x3,[x17,#16]
    ldp x4,x5,[x17,#32]
    ldp x6,x7,[x17,#48]
    ldp x8,x9,[x17,#64]
    ldp x10,x11,[x17,#80]
    ldp x12,x13,[x17,#96]
    ldp x14,x15,[x17,#112]
    ldp x18,x19,[x17,#144]
    ldp x20,x21,[x17,#160]
    ldp x22,x23,[x17,#176]
    ldp x24,x25,[x17,#192]
    ldp x26,x27,[x17,#208]
    ldp x28,x29,[x17,#224]
    ldr x30,[x17,#240]
    br x16
.size continuous_context_enter,.-continuous_context_enter
.p2align 2
.hidden continuous_context_exit
.global continuous_context_exit
.type continuous_context_exit,%function
continuous_context_exit:
    clrex
    mrs x17,tpidr_el0
    str x16,[x17,#40]
    ldr x16,[x17,#8]
    stp x0,x1,[x16,#0]
    stp x2,x3,[x16,#16]
    stp x4,x5,[x16,#32]
    stp x6,x7,[x16,#48]
    stp x8,x9,[x16,#64]
    stp x10,x11,[x16,#80]
    stp x12,x13,[x16,#96]
    stp x14,x15,[x16,#112]
    stp x18,x19,[x16,#144]
    stp x20,x21,[x16,#160]
    stp x22,x23,[x16,#176]
    stp x24,x25,[x16,#192]
    stp x26,x27,[x16,#208]
    stp x28,x29,[x16,#224]
    str x30,[x16,#240]
    ldp x0,x1,[sp]
    stp x0,x1,[x16,#128]
    add x0,sp,#16
    str x0,[x16,#256]
    ldr x0,[x17,#40]
    ldr x0,[x0,#8]
    str x0,[x16,#264]
    mrs x0,nzcv
    str x0,[x16,#272]
    mrs x0,fpcr
    str x0,[x16,#320]
    mrs x0,fpsr
    str x0,[x16,#328]
    stp q0,q1,[x16,#512]
    stp q2,q3,[x16,#544]
    stp q4,q5,[x16,#576]
    stp q6,q7,[x16,#608]
    stp q8,q9,[x16,#640]
    stp q10,q11,[x16,#672]
    stp q12,q13,[x16,#704]
    stp q14,q15,[x16,#736]
    stp q16,q17,[x16,#768]
    stp q18,q19,[x16,#800]
    stp q20,q21,[x16,#832]
    stp q22,q23,[x16,#864]
    stp q24,q25,[x16,#896]
    stp q26,q27,[x16,#928]
    stp q28,q29,[x16,#960]
    stp q30,q31,[x16,#992]
    mov x0,x17
    b continuous_context_signal_return
.size continuous_context_exit,.-continuous_context_exit
.p2align 2
.hidden continuous_context_signal_return
.global continuous_context_signal_return
.type continuous_context_signal_return,%function
continuous_context_signal_return:
    clrex
    ldr x9,[x0,#16]
    msr tpidr_el0,x9
    ldr x18,[x0,#24]
    ldr x9,[x0,#32]
    mov sp,x9
    ldr x9,[x0,#56]
    msr fpcr,x9
    ldr x9,[x0,#64]
    msr fpsr,x9
    str xzr,[x0,#48]
    ldr x9,[sp,#224]
    msr nzcv,x9
    ldp q8,q9,[sp,#96]
    ldp q10,q11,[sp,#128]
    ldp q12,q13,[sp,#160]
    ldp q14,q15,[sp,#192]
    ldp x19,x20,[sp,#0]
    ldp x21,x22,[sp,#16]
    ldp x23,x24,[sp,#32]
    ldp x25,x26,[sp,#48]
    ldp x27,x28,[sp,#64]
    ldp x29,x30,[sp,#80]
    add sp,sp,#256
    ret
.size continuous_context_signal_return,.-continuous_context_signal_return
.hidden continuous_context_transition_end
.global continuous_context_transition_end
continuous_context_transition_end:

/* Linux supplies the host alternate signal stack. x0-x2 remain sig/info/ctx.
 * Like the pinned Eden frontend, identify private hardware TLS before any C.
 * A self pointer provides an additional check beyond the 64-bit magic. */
.p2align 2
.hidden continuous_context_signal_front
.global continuous_context_signal_front
.type continuous_context_signal_front,%function
continuous_context_signal_front:
    mrs x3,tpidr_el0
    ldr x9,[x3,#0]
    mov x10,#0x5831
    movk x10,#0x4354,lsl #16
    movk x10,#0x4345,lsl #32
    movk x10,#0x434e,lsl #48
    cmp x9,x10
    b.ne 1f
    ldr x9,[x3,#72]
    cmp x9,x3
    b.ne 1f
    stp x29,x30,[sp,#-32]!
    stp x19,x20,[sp,#16]
    mov x29,sp
    mov x19,x3
    ldr x18,[x19,#24]
    ldr x9,[x19,#16]
    msr tpidr_el0,x9
    ldr x9,[x19,#56]
    msr fpcr,x9
    ldr x9,[x19,#64]
    msr fpsr,x9
    bl continuous_signals_dispatch
    cbz w0,2f
    msr tpidr_el0,x19
2:
    ldp x19,x20,[sp,#16]
    ldp x29,x30,[sp],#32
    ret
1:
    mov x3,xzr
    b continuous_signals_dispatch
.size continuous_context_signal_front,.-continuous_context_signal_front
.section .note.GNU-stack,"",%progbits
