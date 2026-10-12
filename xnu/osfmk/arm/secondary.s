/* SPDX-License-Identifier: BSD-2-Clause */
#include <arm/arch.h>
#include <arm/asm_help.h>
#include <assym.s>
#include <mach/arm/asm.h>
/* Nexus7's physical and kernel virtual bases are both 0x80000000. The
 * kernel text remains identity mapped, so this entry needs no scratch TTB. */
EnterARM(arm_secondary_start)
    cpsid if, #0x13
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c8, c7, 0
    /* A reset does not initialize L1 data-cache tags. Discard the target
     * CPU's old lines without writing them over live shared memory. */
    mcr p15, 2, r0, c0, c0, 0
    isb sy
    mrc p15, 1, r1, c0, c0, 0
    and r2, r1, #7
    add r2, r2, #4
    ubfx r3, r1, #3, #10
    clz r4, r3
    ubfx r5, r1, #13, #15
1:  mov r6, r5
2:  lsl r7, r3, r4
    orr r7, r7, r6, lsl r2
    mcr p15, 0, r7, c7, c6, 2
    subs r6, r6, #1
    bge 2b
    subs r3, r3, #1
    bge 1b
    dsb sy
    mov r0, #0
    mcr p15, 0, r0, c13, c0, 1
    mcr p15, 0, r0, c2, c0, 2
    mov r0, #1
    mcr p15, 0, r0, c3, c0, 0
    mrc p15, 0, r0, c1, c0, 1
    orr r0, r0, #0x41 /* ACTLR.SMP and maintenance broadcast */
    mcr p15, 0, r0, c1, c0, 1
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(0xf << 20)
    bic r0, r0, #(1 << 30)
    bic r0, r0, #(1 << 31)
    mcr p15, 0, r0, c1, c0, 2
    isb sy
    mov r0, #0
    vmsr fpexc, r0
    LOAD_ADDR(r1, arm_secondary_ttbr)
    ldr r0, [r1]
    orr r0, r0, #0x4a
    mcr p15, 0, r0, c2, c0, 0
    mcr p15, 0, r0, c2, c0, 1
    LOAD_ADDR(r1, ExceptionVectorsBase)
    mcr p15, 0, r1, c12, c0, 0
    LOAD_ADDR(r1, arm_secondary_sctlr)
    ldr r0, [r1]
    dsb sy
    mcr p15, 0, r0, c1, c0, 0
    isb sy
    mrc p15, 0, r0, c0, c0, 5
    and r0, r0, #0xff
    LOAD_ADDR(r1, arm_cpu_logical)
    ldr r0, [r1, r0, lsl #2]
    LOAD_ADDR(r1, cpu_data_ptr)
    ldr r1, [r1, r0, lsl #2]
    ldr sp, [r1, CPU_INT_STACK_TOP]
    mov r7, #0
    blx _arm_secondary_init
1:  wfi
    b 1b
LOAD_ADDR_GEN_DEF(arm_secondary_ttbr)
LOAD_ADDR_GEN_DEF(arm_secondary_sctlr)
LOAD_ADDR_GEN_DEF(ExceptionVectorsBase)
LOAD_ADDR_GEN_DEF(arm_cpu_logical)
LOAD_ADDR_GEN_DEF(cpu_data_ptr)
