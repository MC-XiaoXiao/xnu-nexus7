/* SPDX-License-Identifier: BSD-2-Clause */
.syntax unified
.arch armv7-a
.arm
.text
.global armv7_normalize
.type armv7_normalize,%function
/* r0=external PL310 base, or zero for architectural caches only.
 * No stack writes occur between cache cleaning and MMU/cache shutdown.
 * Set/way dimensions come from CLIDR/CCSIDR, never from board assumptions. */
armv7_normalize:
    push {r4-r11,lr}
    mov r8,r0
    dsb sy
    mrc p15,1,r0,c0,c0,1
    ubfx r3,r0,#24,#3              /* LoC */
    lsl r3,r3,#1
    mov r10,#0
.Llevel:
    cmp r10,r3
    bge .Linner_done
    add r2,r10,r10,lsr #1
    lsr r1,r0,r2
    and r1,r1,#7
    cmp r1,#2
    blt .Lnext_level
    mcr p15,2,r10,c0,c0,0
    isb
    mrc p15,1,r1,c0,c0,0
    and r2,r1,#7
    add r2,r2,#4                   /* log2(bytes per cache line) */
    ubfx r4,r1,#3,#10              /* ways minus one */
    clz r5,r4
    ubfx r7,r1,#13,#15             /* sets minus one */
.Lway:
    mov r9,r7
.Lset:
    orr r11,r10,r4,lsl r5
    orr r11,r11,r9,lsl r2
    mcr p15,0,r11,c7,c14,2
    subs r9,r9,#1
    bpl .Lset
    subs r4,r4,#1
    bpl .Lway
.Lnext_level:
    add r10,r10,#2
    b .Llevel
.Linner_done:
    dsb sy
    mrc p15,0,r0,c1,c0,0
    bic r0,r0,#4                  /* disable L1 D-cache before PL310 */
    mcr p15,0,r0,c1,c0,0
    isb
    cmp r8,#0
    beq .Louter_done
    ldr r0,[r8,#0x100]
    tst r0,#1
    beq .Louter_done
    ldr r0,[r8,#0x104]
    tst r0,#(1<<16)
    mov r0,#0xff
    movwne r0,#0xffff
    str r0,[r8,#0x7fc]            /* PL310 clean+invalidate by way */
    mov r2,#0x100000
.Louter_wait:
    ldr r0,[r8,#0x7fc]
    cmp r0,#0
    beq .Lsync
    subs r2,r2,#1
    bne .Louter_wait
    b .Ltimeout
.Lsync:
    str r0,[r8,#0x730]            /* cache sync */
    mov r2,#0x100000
.Lsync_wait:
    ldr r0,[r8,#0x730]
    tst r0,#1
    beq .Ldisable_outer
    subs r2,r2,#1
    bne .Lsync_wait
    b .Ltimeout
.Ldisable_outer:
    mov r0,#0
    str r0,[r8,#0x100]
    dsb sy
.Louter_done:
    mrc p15,0,r0,c1,c0,0
    bic r0,r0,#1                  /* MMU */
    bic r0,r0,#(1<<12)            /* I-cache */
    bic r0,r0,#(1<<11)            /* branch prediction */
    bic r0,r0,#(1<<28)            /* TEX remapping */
    bic r0,r0,#(1<<29)            /* access flag enable */
    bic r0,r0,#(1<<25)            /* big-endian exceptions */
    bic r0,r0,#(1<<30)            /* Thumb exceptions */
    bic r0,r0,#(1<<19)            /* write implies XN */
    dsb sy
    mcr p15,0,r0,c1,c0,0
    isb
    mov r0,#0
    mcr p15,2,r0,c0,c0,0          /* reset CSSELR */
    mcr p15,0,r0,c2,c0,2          /* TTBCR: short descriptor, TTBR0 */
    mcr p15,0,r0,c13,c0,1         /* context ID */
    mcr p15,0,r0,c8,c7,0          /* TLB */
    mcr p15,0,r0,c7,c5,0          /* I-cache */
    mcr p15,0,r0,c7,c5,6          /* branch predictor */
    dsb sy
    isb
    pop {r4-r11,pc}
.Ltimeout:
    mov r0,#1
    pop {r4-r11,pc}
.size armv7_normalize,.-armv7_normalize
.global armv7_enter
.type armv7_enter,%function
armv7_enter:
    mov r4,r0
    mov r0,r1                    /* XNU expects boot_args in r0 */
    mov r1,#0
    mov r2,#0
    mov r3,#0
    dsb sy
    isb
    bx r4
.size armv7_enter,.-armv7_enter
.section .note.GNU-stack,"",%progbits
