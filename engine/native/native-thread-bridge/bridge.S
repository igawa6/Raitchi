/* Cpu and native-frame offsets are generated with offsetof, never duplicated. */
#include "bridge_offsets.inc"
.text
.p2align 2
.macro NT_CAPTURE
    sub sp,sp,#NT_FRAME_SIZE
    stp x0,x1,[sp,#NT_X+0]
    stp x2,x3,[sp,#NT_X+16]
    stp x4,x5,[sp,#NT_X+32]
    stp x6,x7,[sp,#NT_X+48]
    stp x8,x9,[sp,#NT_X+64]
    stp x10,x11,[sp,#NT_X+80]
    stp x12,x13,[sp,#NT_X+96]
    stp x14,x15,[sp,#NT_X+112]
    stp x16,x17,[sp,#NT_X+128]
    stp x18,x19,[sp,#NT_X+144]
    stp x20,x21,[sp,#NT_X+160]
    stp x22,x23,[sp,#NT_X+176]
    stp x24,x25,[sp,#NT_X+192]
    stp x26,x27,[sp,#NT_X+208]
    stp x28,x29,[sp,#NT_X+224]
    str x30,[sp,#NT_X+240]
    add x9,sp,#NT_FRAME_SIZE
    str x9,[sp,#NT_SP]
    mrs x9,nzcv
    str x9,[sp,#NT_NZCV]
    mrs x9,fpcr
    str x9,[sp,#NT_FPCR]
    mrs x9,fpsr
    str x9,[sp,#NT_FPSR]
    stp q0,q1,[sp,#NT_Q+0]
    stp q2,q3,[sp,#NT_Q+32]
    stp q4,q5,[sp,#NT_Q+64]
    stp q6,q7,[sp,#NT_Q+96]
    stp q8,q9,[sp,#NT_Q+128]
    stp q10,q11,[sp,#NT_Q+160]
    stp q12,q13,[sp,#NT_Q+192]
    stp q14,q15,[sp,#NT_Q+224]
    stp q16,q17,[sp,#NT_Q+256]
    stp q18,q19,[sp,#NT_Q+288]
    stp q20,q21,[sp,#NT_Q+320]
    stp q22,q23,[sp,#NT_Q+352]
    stp q24,q25,[sp,#NT_Q+384]
    stp q26,q27,[sp,#NT_Q+416]
    stp q28,q29,[sp,#NT_Q+448]
    stp q30,q31,[sp,#NT_Q+480]
.endm
.macro NT_RESTORE
    ldp q0,q1,[sp,#NT_Q+0]
    ldp q2,q3,[sp,#NT_Q+32]
    ldp q4,q5,[sp,#NT_Q+64]
    ldp q6,q7,[sp,#NT_Q+96]
    ldp q8,q9,[sp,#NT_Q+128]
    ldp q10,q11,[sp,#NT_Q+160]
    ldp q12,q13,[sp,#NT_Q+192]
    ldp q14,q15,[sp,#NT_Q+224]
    ldp q16,q17,[sp,#NT_Q+256]
    ldp q18,q19,[sp,#NT_Q+288]
    ldp q20,q21,[sp,#NT_Q+320]
    ldp q22,q23,[sp,#NT_Q+352]
    ldp q24,q25,[sp,#NT_Q+384]
    ldp q26,q27,[sp,#NT_Q+416]
    ldp q28,q29,[sp,#NT_Q+448]
    ldp q30,q31,[sp,#NT_Q+480]
    ldr x9,[sp,#NT_FPCR]
    msr fpcr,x9
    ldr x9,[sp,#NT_FPSR]
    msr fpsr,x9
    ldr x9,[sp,#NT_NZCV]
    msr nzcv,x9
    ldp x0,x1,[sp,#NT_X+0]
    ldp x2,x3,[sp,#NT_X+16]
    ldp x4,x5,[sp,#NT_X+32]
    ldp x6,x7,[sp,#NT_X+48]
    ldp x8,x9,[sp,#NT_X+64]
    ldp x10,x11,[sp,#NT_X+80]
    ldp x12,x13,[sp,#NT_X+96]
    ldp x14,x15,[sp,#NT_X+112]
    ldp x16,x17,[sp,#NT_X+128]
    /* physical X18 is Android's; physical X19 remains the context */
    ldp x20,x21,[sp,#NT_X+160]
    ldp x22,x23,[sp,#NT_X+176]
    ldp x24,x25,[sp,#NT_X+192]
    ldp x26,x27,[sp,#NT_X+208]
    ldp x28,x29,[sp,#NT_X+224]
    ldr x30,[sp,#NT_X+240]
    add sp,sp,#NT_FRAME_SIZE
.endm

.macro NT_HOST_GATEWAY reject
    ldr w9,[x19,#CTX_GATEWAY_ACTIVE]
    cbnz w9,\reject
    mov w9,#1
    str w9,[x19,#CTX_GATEWAY_ACTIVE]
    mov x9,sp
    str x9,[x19,#CTX_ACTIVE_FRAME]
    ldr x9,[x19,#CTX_HOST_CALL_SP]
    mov sp,x9
.endm
.macro NT_CLEAR_GATEWAY
    str wzr,[x19,#CTX_GATEWAY_ACTIVE]
.endm

.hidden nt_enter
.global nt_enter
.type nt_enter,%function
nt_enter:
    ldr w9,[x0,#CTX_ENTRY_DEPTH]
    cbnz w9,nt_reentry_rejected
    mov w9,#1
    str w9,[x0,#CTX_ENTRY_DEPTH]
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
    mrs x9,fpcr
    str x9,[sp,#224]
    mrs x9,fpsr
    str x9,[sp,#232]
    mov x19,x0
    mov x9,sp
    str x9,[x19,#CTX_HOST_SP]
    sub x10,x9,#NT_FRAME_SIZE
    str x10,[x19,#CTX_HOST_CALL_SP]
    ldr x10,[x19,#CTX_GUEST_ENTRY_SP]
    cbz x10,1f
    mov x9,x10
1:
    str x9,[x19,#CTX_RETURN_SP]
    ldr w10,[x19,#CTX_STACK_ENABLED]
    cbz w10,2f
    sub x10,x9,#32
    str x10,[x19,#CTX_EXPECTED_SP]
    b 3f
2:
    str x9,[x19,#CTX_EXPECTED_SP]
3:
    mov sp,x9
    ldr x17,[x19,#CTX_CPU]
    ldr x9,[x17,#CPU_FPCR]
    msr fpcr,x9
    ldr x9,[x17,#CPU_FPSR]
    msr fpsr,x9
    ldr x9,[x17,#CPU_NZCV]
    msr nzcv,x9
    ldp q0,q1,[x17,#CPU_V+0]
    ldp q2,q3,[x17,#CPU_V+32]
    ldp q4,q5,[x17,#CPU_V+64]
    ldp q6,q7,[x17,#CPU_V+96]
    ldp q8,q9,[x17,#CPU_V+128]
    ldp q10,q11,[x17,#CPU_V+160]
    ldp q12,q13,[x17,#CPU_V+192]
    ldp q14,q15,[x17,#CPU_V+224]
    ldp q16,q17,[x17,#CPU_V+256]
    ldp q18,q19,[x17,#CPU_V+288]
    ldp q20,q21,[x17,#CPU_V+320]
    ldp q22,q23,[x17,#CPU_V+352]
    ldp q24,q25,[x17,#CPU_V+384]
    ldp q26,q27,[x17,#CPU_V+416]
    ldp q28,q29,[x17,#CPU_V+448]
    ldp q30,q31,[x17,#CPU_V+480]
    ldp x0,x1,[x17,#CPU_X+0]
    ldp x2,x3,[x17,#CPU_X+16]
    ldp x4,x5,[x17,#CPU_X+32]
    ldp x6,x7,[x17,#CPU_X+48]
    ldp x8,x9,[x17,#CPU_X+64]
    ldp x10,x11,[x17,#CPU_X+80]
    ldp x12,x13,[x17,#CPU_X+96]
    ldp x14,x15,[x17,#CPU_X+112]
    ldr x16,[x19,#CTX_ENTRY]
    ldp x20,x21,[x17,#CPU_X+160]
    ldp x22,x23,[x17,#CPU_X+176]
    ldp x24,x25,[x17,#CPU_X+192]
    ldp x26,x27,[x17,#CPU_X+208]
    ldp x28,x29,[x17,#CPU_X+224]
    ldr x30,[x17,#CPU_X+240]
    ldr x17,[x17,#CPU_X+136]
    br x16 /* RX entry restores guest X16/X17 before the first trap */
.hidden nt_resume
.global nt_resume
nt_resume:
    NT_CAPTURE
    mov x1,sp
    NT_HOST_GATEWAY nt_nested_rejected
    mov x0,x19
    bl nt_observe
    NT_CLEAR_GATEWAY
    str wzr,[x19,#CTX_ENTRY_DEPTH]
    ldr x9,[x19,#CTX_HOST_SP]
    mov sp,x9
    ldr x9,[sp,#224]
    msr fpcr,x9
    ldr x9,[sp,#232]
    msr fpsr,x9
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
nt_reentry_rejected:
    ldr x9,[x0,#CTX_ENTRY_REJECTIONS]
    add x9,x9,#1
    str x9,[x0,#CTX_ENTRY_REJECTIONS]
nt_nested_rejected:
    brk #0x4e54
.size nt_enter,.-nt_enter

/* This position-independent island is copied once per trap. Its final B is
 * patched to that trap's exact PC+4; guest LR never supplies SVC continuation. */
.p2align 4
.hidden nt_island_begin
.global nt_island_begin
nt_island_begin:
    NT_CAPTURE
    mov x1,sp
    NT_HOST_GATEWAY nt_island_nested_rejected
    mov x0,x19
    adr x2,nt_island_site
    ldr x16,[x19,#CTX_CALL]
    blr x16
    NT_CLEAR_GATEWAY
    ldr x9,[x19,#CTX_ACTIVE_FRAME]
    mov sp,x9
    NT_RESTORE
.hidden nt_island_return
.global nt_island_return
nt_island_return:
    b .
nt_island_nested_rejected:
    brk #0x4e54
.p2align 3
.hidden nt_island_site
.global nt_island_site
nt_island_site:
    .quad 0
    .word 0,0
.hidden nt_island_end
.global nt_island_end
nt_island_end:
.section .note.GNU-stack,"",%progbits
