/* Original synthetic kernels; Android actual x18/TPIDR are never written. */
.text
.p2align 2
.hidden nap_call
.global nap_call
.type nap_call,%function
nap_call:
    stp x29,x30,[sp,#-16]!
    stp x19,x20,[sp,#-16]!
    mov x19,x0
    mov x0,x2
    blr x1
    ldp x19,x20,[sp],#16
    ldp x29,x30,[sp],#16
    ret
.size nap_call,.-nap_call

/* Guest-visible live state survives a host callback. Context is x19. */
.hidden nap_live
.global nap_live
.type nap_live,%function
nap_live:
    stp x29,x30,[sp,#-16]!
    stp x19,x20,[sp,#-16]!
    mov x19,x0
    mrs x9,fpcr
    str x9,[x19,#112]
    mrs x9,fpsr
    str x9,[x19,#120]
    mov x9,#0x1234
    movi v0.16b,#0x5a
    movi v31.16b,#0xa5
    cmp xzr,xzr
    bl nap_host_gate
    mrs x10,nzcv
    str x10,[x19,#64]
    str x9,[x19,#72]
    str q0,[x19,#80]
    str q31,[x19,#96]
    mrs x10,fpcr
    str x10,[x19,#128]
    mrs x10,fpsr
    str x10,[x19,#136]
    ldp x19,x20,[sp],#16
    ldp x29,x30,[sp],#16
    ret
.size nap_live,.-nap_live

/* No guest/host TLS switch. Save guest live state on this synthetic stack.
 * Callback receives context in x0; results are recorded through that context. */
.type nap_host_gate,%function
nap_host_gate:
    sub sp,sp,#704
    stp x0,x1,[sp,#0]
    stp x2,x3,[sp,#16]
    stp x4,x5,[sp,#32]
    stp x6,x7,[sp,#48]
    stp x8,x9,[sp,#64]
    stp x10,x11,[sp,#80]
    stp x12,x13,[sp,#96]
    stp x14,x15,[sp,#112]
    stp x16,x17,[sp,#128]
    stp x29,x30,[sp,#144]
    mrs x9,nzcv
    str x9,[sp,#160]
    mrs x9,fpcr
    str x9,[sp,#168]
    mrs x9,fpsr
    str x9,[sp,#176]
    stp q0,q1,[sp,#192]
    stp q2,q3,[sp,#224]
    stp q4,q5,[sp,#256]
    stp q6,q7,[sp,#288]
    stp q8,q9,[sp,#320]
    stp q10,q11,[sp,#352]
    stp q12,q13,[sp,#384]
    stp q14,q15,[sp,#416]
    stp q16,q17,[sp,#448]
    stp q18,q19,[sp,#480]
    stp q20,q21,[sp,#512]
    stp q22,q23,[sp,#544]
    stp q24,q25,[sp,#576]
    stp q26,q27,[sp,#608]
    stp q28,q29,[sp,#640]
    stp q30,q31,[sp,#672]
    blr x1
    ldp q0,q1,[sp,#192]
    ldp q2,q3,[sp,#224]
    ldp q4,q5,[sp,#256]
    ldp q6,q7,[sp,#288]
    ldp q8,q9,[sp,#320]
    ldp q10,q11,[sp,#352]
    ldp q12,q13,[sp,#384]
    ldp q14,q15,[sp,#416]
    ldp q16,q17,[sp,#448]
    ldp q18,q19,[sp,#480]
    ldp q20,q21,[sp,#512]
    ldp q22,q23,[sp,#544]
    ldp q24,q25,[sp,#576]
    ldp q26,q27,[sp,#608]
    ldp q28,q29,[sp,#640]
    ldp q30,q31,[sp,#672]
    ldr x9,[sp,#168]
    msr fpcr,x9
    ldr x9,[sp,#176]
    msr fpsr,x9
    ldr x9,[sp,#160]
    msr nzcv,x9
    ldp x0,x1,[sp,#0]
    ldp x2,x3,[sp,#16]
    ldp x4,x5,[sp,#32]
    ldp x6,x7,[sp,#48]
    ldp x8,x9,[sp,#64]
    ldp x10,x11,[sp,#80]
    ldp x12,x13,[sp,#96]
    ldp x14,x15,[sp,#112]
    ldp x16,x17,[sp,#128]
    ldp x29,x30,[sp,#144]
    add sp,sp,#704
    ret
.size nap_host_gate,.-nap_host_gate

.hidden nap_signal_wait
.global nap_signal_wait
.type nap_signal_wait,%function
nap_signal_wait:
    add x10,x0,#144
    add x11,x0,#148
    mov w9,#1
    stlr w9,[x10]
.hidden nap_signal_begin
.global nap_signal_begin
nap_signal_begin:
    ldar w9,[x11]
    cbz w9,nap_signal_begin
.hidden nap_signal_end
.global nap_signal_end
nap_signal_end:
    ret
.size nap_signal_wait,.-nap_signal_wait

.hidden nap_atomic_increment
.global nap_atomic_increment
.type nap_atomic_increment,%function
nap_atomic_increment:
    mov x12,#0
1:  ldaxr x9,[x0]
    add x9,x9,#1
    stlxr w10,x9,[x0]
    cbz w10,2f
    add x12,x12,#1
    cmp x12,#0x100,lsl #12
    b.lo 1b
    mov x0,#-1
    ret
2:  mov x0,x12
    ret
.size nap_atomic_increment,.-nap_atomic_increment

.hidden nap_pair_increment
.global nap_pair_increment
.type nap_pair_increment,%function
nap_pair_increment:
    mov x12,#0
1:  ldaxp x9,x10,[x0]
    add x9,x9,#1
    add x10,x10,#3
    stlxp w11,x9,x10,[x0]
    cbz w11,2f
    add x12,x12,#1
    cmp x12,#0x100,lsl #12
    b.lo 1b
    mov x0,#-1
    ret
2:  mov x0,x12
    ret
.size nap_pair_increment,.-nap_pair_increment
.section .note.GNU-stack,"",%progbits
