/*-
 * Copyright (c) 2010 Per Odlund <per.odlund@armagedon.se>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* ARMv7 assembly functions for manipulating caches and other core functions.
 * Based on cpufuncs for v6 and xscale.
 */

#include <mach/arm/asm.h>
#include <arm/asm_help.h>
#include <arm/arch.h>

#ifdef _ARM_ARCH_7

#define ENTRY_NP ENTRY
#define _C_LABEL(x) _ ##x

ENTRY(arm_cpu_sleep)
	wfi				@ wait for an interrupt
END(arm_cpu_sleep)

ENTRY(arm_wait)
	mrc	p15, 0, r0, c2, c0, 0	@ arbitrary read of CP15
	add	r0, r0, #0		@ a stall
	bx	lr
END(arm_wait)

ENTRY(arm_context_switch)
	dsb				@ data synchronization barrier
    orr r0, r0, #0x4a @ Shareable WBWA translation table walks
	mcr	p15, 0, r0, c2, c0, 0 	@ set the new TTB
#ifdef MULTIPROCESSOR
	mcr	p15, 0, r0, c8, c3, 0	@ flush the I+D
#else
	mcr	p15, 0, r0, c8, c7, 0	@ flush the I+D
#endif
	dsb
	isb
	bx	lr
END(arm_context_switch)

ENTRY(arm_tlb_flushID_ASID)
#ifdef MULTIPROCESSOR
	mcr	p15, 0, r0, c8, c3, 2	@ flush I+D tlb per ASID inner
#else
	mcr	p15, 0, r0, c8, c7, 2	@ flush I+D tlb per ASID
#endif
	dsb				@ data synchronization barrier
	isb
	bx	lr
END(arm_tlb_flushID_ASID)

ENTRY(arm_tlb_flushID)
	mov r0, #0
#ifdef MULTIPROCESSOR
	mcr	p15, 0, r0, c8, c3, 0	@ flush I+D tlb all
#else
	mcr	p15, 0, r0, c8, c7, 0	@ flush I+D tlb all
#endif
	dsb				@ data synchronization barrier
	isb
	bx	lr
END(arm_tlb_flushID)

ENTRY(arm_tlb_flushID_RANGE)
	dsb  			@ data synchronization barrier
	mov r0, r0, lsr#12
	mov r1, r1, lsr#12
	mov r0, r0, lsl#12
	mov r1, r1, lsl#12
1:	mcr p15, 0, r0, c8, c7, 1 	@ flush I+D tlb single entry
	add r0, r0, #0x1000 		@ page size
	cmp r0, r1
	bcc 1b
	mov r2, #0
	mcr p15, 0, r2, c7, c5, 6 	@ BPIALL
	dsb 
	isb
	bx lr
END(arm_tlb_flushID_RANGE)

ENTRY(arm_tlb_flushID_SE)
#ifdef MULTIPROCESSOR
	mcr	p15, 0, r0, c8, c3, 1	@ flush I+D tlb single entry
#else
	mcr	p15, 0, r0, c8, c7, 1	@ flush I+D tlb single entry
#endif
	dsb				@ data synchronization barrier
	isb
	bx	lr
END(arm_tlb_flushID_SE)


ENTRY_NP(arm_setttb)
	mrc	p15, 0, r2, c0, c0, 5	@ get MPIDR
	cmp	r2, #0
	orrlt	r0, r0, #0x58		@ MP, cachable
	orrge	r0, r0, #0x18		@ Non-MP, cacheable
	mcr	p15, 0, r0, c2, c0, 0   @ load new TTB
	cmp	r1, #0
#ifdef MULTIPROCESSOR
	mcrne	p15, 0, r0, c8, c3, 0	@ invalidate all I+D TLBs
#else
	mcrne	p15, 0, r0, c8, c7, 0   @ invalidate all I+D TLBs
#endif
	dsb				@ data synchronization barrier
	isb
	bx	lr
END(arm_setttb)

/* Other functions. */

ENTRY_NP(arm_drain_writebuf)
	dsb				@ data synchronization barrier
	RET
END(arm_drain_writebuf)

/* Cache operations. */

/* LINTSTUB: void arm_icache_sync_range(vaddr_t, vsize_t); */
ENTRY_NP(arm_icache_sync_range)
    cmp r1, #0
    bxeq lr
    /* Inclusive last byte, saturating at the end of the address space. */
    sub r1, r1, #1
    adds r1, r0, r1
    mvncs r1, #0

    /* CTR gives the minimum line sizes without changing CSSELR. */
    mrc p15, 0, r2, c0, c0, 1
    ubfx r2, r2, #16, #4       @ DminLine, in words
    mov r3, #4
    lsl r2, r3, r2
    sub r3, r2, #1
    bic ip, r0, r3
    dsb
1:
    mcr p15, 0, ip, c7, c10, 1 @ clean D-cache to PoC
    sub r3, r1, ip
    cmp r3, r2
    addhs ip, ip, r2
    bhs 1b
    /* Instruction fills must observe the completed data writeback. */
    dsb
    mrc p15, 0, r2, c0, c0, 1
    and r2, r2, #0xf           @ IminLine, in words
    mov r3, #4
    lsl r2, r3, r2
    sub r3, r2, #1
    bic ip, r0, r3
2:
    mcr p15, 0, ip, c7, c5, 1  @ invalidate I-cache to PoU
    sub r3, r1, ip
    cmp r3, r2
    addhs ip, ip, r2
    bhs 2b
    dsb
    isb
    bx lr
END(arm_icache_sync_range)

ENTRY(arm_set_context_id)
	mcr p15, 0, r0, c13, c0, 1
	isb
	bx lr
END(arm_set_context_id)

/* LINTSTUB: void arm_icache_sync_all(void); */
ENTRY_NP(arm_icache_sync_all)
    b   _C_LABEL(arm_idcache_wbinv_all)
END(arm_icache_sync_all)

ENTRY(arm_dcache_wb_range)
    cmp r1, #0
    bxeq lr
    sub r1, r1, #1
    adds r1, r0, r1
    mvncs r1, #0               @ do not wrap maintenance through address zero
    mrc p15, 0, r2, c0, c0, 1  @ CTR, independent of CSSELR
    ubfx r2, r2, #16, #4
    mov ip, #4
    lsl r2, ip, r2
    sub ip, r2, #1
    bic r0, r0, ip
    bic r1, r1, ip
    dsb
1:
    mcr p15, 0, r0, c7, c10, 1
    cmp r0, r1
    addlo r0, r0, r2
    blo 1b
    dsb
    bx lr
END(arm_dcache_wb_range)

/* LINTSTUB: void arm_dcache_wbinv_range(vaddr_t, vsize_t); */
ENTRY(arm_dcache_wbinv_range)
    cmp r1, #0
    bxeq lr
    sub r1, r1, #1
    adds r1, r0, r1
    mvncs r1, #0               @ do not wrap maintenance through address zero
    mrc p15, 0, r2, c0, c0, 1  @ CTR, independent of CSSELR
    ubfx r2, r2, #16, #4
    mov ip, #4
    lsl r2, ip, r2
    sub ip, r2, #1
    bic r0, r0, ip
    bic r1, r1, ip
    dsb
1:
    mcr p15, 0, r0, c7, c14, 1
    cmp r0, r1
    addlo r0, r0, r2
    blo 1b
    dsb
    bx lr
END(arm_dcache_wbinv_range)

/* * LINTSTUB: void arm_dcache_inv_range(vaddr_t, vsize_t); */
ENTRY(arm_dcache_inv_range)
    cmp r1, #0
    bxeq lr
    sub r1, r1, #1
    adds r1, r0, r1
    mvncs r1, #0               @ do not wrap maintenance through address zero
    mrc p15, 0, r2, c0, c0, 1  @ CTR, independent of CSSELR
    ubfx r2, r2, #16, #4
    mov ip, #4
    lsl r2, ip, r2
    sub ip, r2, #1
    bic r0, r0, ip
    bic r1, r1, ip
    dsb
1:
    mcr p15, 0, r0, c7, c6, 1
    cmp r0, r1
    addlo r0, r0, r2
    blo 1b
    dsb
    bx lr
END(arm_dcache_inv_range)


/* * LINTSTUB: void arm_idcache_wbinv_range(vaddr_t, vsize_t); */
ENTRY(arm_idcache_wbinv_range)
	mrc	p15, 1, r2, c0, c0, 0	@ read CCSIDR
	and	r2, r2, #7		@ get line size (log2(size)-4, 0=16)
	mov	ip, #16			@ make a bit mask
	lsl	r2, ip, r2		@ and shift into position
	sub	ip, r2, #1		@ make into a mask
	and	r3, r0, ip		@ get offset into cache line
	add	r1, r1, r3		@ add to length
	bic	r0, r0, ip		@ clear offset from start.
	dsb
1:
	mcr	p15, 0, r0, c7, c5, 1	@ invalidate the I-Cache line
	mcr	p15, 0, r0, c7, c14, 1 	@ wb and inv the D-Cache line
	add	r0, r0, r2
	subs	r1, r1, r2
	bhi	1b

	dsb				@ data synchronization barrier
	isb
	bx	lr
END(arm_idcache_wbinv_range)

/* * LINTSTUB: void arm_idcache_wbinv_all(void); */
ENTRY_NP(arm_idcache_wbinv_all)
    /* Complete dirty data publication before invalidating instruction lines. */
    stmdb sp!, {r0, lr}
    bl  _C_LABEL(arm_dcache_wbinv_all)
    dsb
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    dsb
    isb
    ldmia sp!, {r0, pc}
END(arm_idcache_wbinv_all)

/*
 * These work very hard to not push registers onto the stack and to limit themselves
 * to use r0-r3 and ip.
 */
/* * LINTSTUB: void arm_icache_inv_all(void); */
ENTRY_NP(arm_icache_inv_all)
    /* I-cache invalidation must never invalidate dirty data-cache lines. */
    dsb
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0 @ ICIALLU: all VA colours
    mcr p15, 0, r0, c7, c5, 6 @ BPIALL: old branch targets
    dsb
    isb
    bx lr
END(arm_icache_inv_all)

/* * LINTSTUB: void arm_dcache_inv_all(void); */
ENTRY_NP(arm_dcache_inv_all)
    /* Walk implemented cache levels from L1 up to CLIDR.LoC. Keep the
     * caller's r9/thread register and all other callee-saved registers. */
    push    {r4-r10, lr}
    /* Invalidate must not discard the dirty register-save frame. Cover
     * every possible cache line (minimum line size is 16 bytes). */
    mov     r0, sp
    mcr     p15, 0, r0, c7, c10, 1
    add     r0, sp, #16
    mcr     p15, 0, r0, c7, c10, 1
    add     r0, sp, #31
    mcr     p15, 0, r0, c7, c10, 1
    dsb     sy
    mrc     p15, 1, r0, c0, c0, 1  @ CLIDR
    ands    r3, r0, #0x07000000
    beq     .Larm_dcache_inv_all_done
    lsr     r3, r3, #23            @ exclusive limit: 2 * LoC
    mov     r10, #0                @ CSSELR level starts at L1
.Larm_dcache_inv_all_level:
    add     r2, r10, r10, lsr #1
    mov     r1, r0, lsr r2
    and     r1, r1, #7
    cmp     r1, #2                 @ skip absent/instruction-only caches
    blt     .Larm_dcache_inv_all_next
    mcr     p15, 2, r10, c0, c0, 0
    isb     sy
    mrc     p15, 1, r1, c0, c0, 0  @ CCSIDR of the selected data cache
    and     r2, r1, #7
    add     r2, r2, #4             @ log2(line bytes)
    ubfx    r4, r1, #3, #10        @ ways - 1
    clz     r5, r4                 @ way field shift; direct-mapped is 32
    ubfx    r7, r1, #13, #15       @ sets - 1
.Larm_dcache_inv_all_way:
    mov     r8, r7
.Larm_dcache_inv_all_set:
    lsl     r6, r4, r5
    orr     r6, r6, r10
    orr     r6, r6, r8, lsl r2
    mcr     p15, 0, r6, c7, c6, 2
    subs    r8, r8, #1
    bge     .Larm_dcache_inv_all_set
    subs    r4, r4, #1
    bge     .Larm_dcache_inv_all_way
.Larm_dcache_inv_all_next:
    add     r10, r10, #2
    cmp     r10, r3
    blt     .Larm_dcache_inv_all_level
.Larm_dcache_inv_all_done:
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0  @ restore CSSELR to L1 data/unified
    dsb     sy
    isb     sy
    pop     {r4-r10, pc}
END(arm_dcache_inv_all)

/* * LINTSTUB: void arm_dcache_wbinv_all(void); */
ENTRY_NP(arm_dcache_wbinv_all)
    /* Walk implemented cache levels from L1 up to CLIDR.LoC. Keep the
     * caller's r9/thread register and all other callee-saved registers. */
    push    {r4-r10, lr}
    dsb     sy
    mrc     p15, 1, r0, c0, c0, 1  @ CLIDR
    ands    r3, r0, #0x07000000
    beq     .Larm_dcache_wbinv_all_done
    lsr     r3, r3, #23            @ exclusive limit: 2 * LoC
    mov     r10, #0                @ CSSELR level starts at L1
.Larm_dcache_wbinv_all_level:
    add     r2, r10, r10, lsr #1
    mov     r1, r0, lsr r2
    and     r1, r1, #7
    cmp     r1, #2                 @ skip absent/instruction-only caches
    blt     .Larm_dcache_wbinv_all_next
    mcr     p15, 2, r10, c0, c0, 0
    isb     sy
    mrc     p15, 1, r1, c0, c0, 0  @ CCSIDR of the selected data cache
    and     r2, r1, #7
    add     r2, r2, #4             @ log2(line bytes)
    ubfx    r4, r1, #3, #10        @ ways - 1
    clz     r5, r4                 @ way field shift; direct-mapped is 32
    ubfx    r7, r1, #13, #15       @ sets - 1
.Larm_dcache_wbinv_all_way:
    mov     r8, r7
.Larm_dcache_wbinv_all_set:
    lsl     r6, r4, r5
    orr     r6, r6, r10
    orr     r6, r6, r8, lsl r2
    mcr     p15, 0, r6, c7, c14, 2
    subs    r8, r8, #1
    bge     .Larm_dcache_wbinv_all_set
    subs    r4, r4, #1
    bge     .Larm_dcache_wbinv_all_way
.Larm_dcache_wbinv_all_next:
    add     r10, r10, #2
    cmp     r10, r3
    blt     .Larm_dcache_wbinv_all_level
.Larm_dcache_wbinv_all_done:
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0  @ restore CSSELR to L1 data/unified
    dsb     sy
    isb     sy
    pop     {r4-r10, pc}
END(arm_dcache_wbinv_all)

#endif