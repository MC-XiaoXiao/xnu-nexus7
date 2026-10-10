/* SPDX-License-Identifier: BSD-2-Clause */
.syntax unified
.arch armv7-a
.arm
.section .text.entry,"ax",%progbits
.global _start
.type _start,%function
_start:
    cpsid if
    /* U-Boot must enter on CPU0 in SVC; never attempt HYP/secure transitions. */
    mrs r2, cpsr
    and r2, r2, #31
    cmp r2, #0x13
    bne .Lhalt
    mrc p15, 0, r2, c0, c0, 5
    ands r2, r2, #0xff
    bne .Lhalt
    ldr sp, =__stack_top
    ldr r2, =__bss_start
    ldr r3, =__bss_end
    mov r4, #0
.Lzero:
    cmp r2, r3
    strlo r4, [r2], #4
    blo .Lzero
    bl shim_main
.Lhalt:
    wfi
    b .Lhalt
.size _start,.-_start
.section .note.GNU-stack,"",%progbits
