/*
 * ARM lock definitions
 */

/*
 * !!! DO NOT MODIFY THIS FILE !!!
 * !!! IT WORKS PROPERLY, YOU MIGHT BREAK SOMETHING !!!
 */

#include <mach_assert.h>
#include <assym.s>
#include <arm/asm_help.h>
#include <mach/arm/asm.h>

/*
 * OMAP3530 on BeagleBoard xM has issues with STREX/LDREX. Do ourselves a favor
 * and not use them. It doesn't matter as this SoC is uniprocessor.
 */
#ifdef BOARD_CONFIG_OMAP3530
#undef BOARD_CONFIG_OMAP3530
#endif

#ifdef NO_EXCLUSIVES
#define ldrex           ldr
#endif

#if defined(_ARM_ARCH_7) && !defined(NO_EXCLUSIVES)
#define ARM_LOCK_EXCLUSIVES 1
#endif

#ifndef _ARM_ARCH_7
#undef EnterThumb
#define EnterThumb EnterARM
#endif

/*
 * Lock bit definitions are not defined here. Please don't touch. Thanks.
 */

/* Yes, this was blatantly taken from the real kernel, sorry about that. */

/**
 * hw_lock_init/arm_usimple_lock_init
 *
 * Initialize a lock and all of its bits to zero.
 */ 
EnterThumb(hw_lock_init)
EnterThumb(arm_usimple_lock_init)
    mov     r1, #0
    str     r1, [r0]
    bx      lr

/**
 * hw_lock_held
 *
 * Check a lock and see if the interlock bit is set or not.
 */
EnterThumb(hw_lock_held)
    ldr     r3, [r0]
    mov     r2, #1
    and     r0, r2, r3
    bx      lr

.align 4

/**
 * arm_usimple_lock and friends
 */
EnterARM(arm_usimple_lock)
EnterARM(lck_spin_lock)
EnterARM(hw_lock_lock)
    LoadLockHardwareRegister(r12)
    IncrementPreemptLevel(r12, r2)
#ifdef ARM_LOCK_EXCLUSIVES
hw_lock_acquire_retry:
    ldrex   r3, [r0]
    tst     r3, #1
    bne     hw_lock_lock_wait
    orr     r1, r3, #1
    strex   r2, r1, [r0]
    cmp     r2, #0
    bne     hw_lock_acquire_retry
    dmb     ish
    bx      lr

hw_lock_lock_wait:
    stmfd   sp!, {r0,r1,r7,lr}
    add     r7, sp, #8
    mov     r12, #1
    bl      hw_lock_wait_default
    cmp     r0, #0
    ldmfd   sp!, {r0,r1,r7,lr}
    bxne    lr
    b       hw_lock_lock_panic
#else
    ldr     r3, [r0]
    mov     r2, #1
    orr     r1, r3, #1
    ands    r2, r2, r3
    streq   r1, [r0]
    bxeq    lr
#endif
hw_lock_lock_panic:
    mov     r1, r0
    ldr     r2, [r1]
    adr     r0, _panicString
    blx     _panic
    b       .
_panicString:
    .asciz "hw_lock_lock(): PANIC: Lock 0x%08x = 0x%08x"


/**
 * hw_lock_unlock and friends
 */
EnterARM(lck_spin_unlock)
EnterARM(hw_lock_unlock)
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ishst
hw_lock_release_retry:
    ldrex   r3, [r0]
    bic     r3, r3, #1
    strex   r1, r3, [r0]
    cmp     r1, #0
    bne     hw_lock_release_retry
    dsb     ishst
    sev
#else
    ldr     r3, [r0]
    bic     r3, r3, #1
    str     r3, [r0]
#endif
    b       __enable_preemption

/**
 * arm_usimple_lock_try and friends
 */
EnterARM(arm_usimple_lock_try)
EnterARM(lck_spin_try_lock)
EnterARM(hw_lock_try)
    mrs     r1, cpsr
    cpsid   if
#ifdef ARM_LOCK_EXCLUSIVES
hw_lock_try_retry:
    ldrex   r3, [r0]
    tst     r3, #1
    bne     hw_lock_try_fail
    orr     r2, r3, #1
    strex   r12, r2, [r0]
    cmp     r12, #0
    bne     hw_lock_try_retry
    dmb     ish
#else
    ldr     r3, [r0]
    mov     r2, #1
    orr     r12, r3, #1
    ands    r2, r2, r3
    streq   r12, [r0]
    bne     hw_lock_try_fail
#endif
    LoadLockHardwareRegister(r12)
    IncrementPreemptLevel(r12, r2)
    mov     r0, #1
    b       hw_lock_try_return
hw_lock_try_fail:
    mov     r0, #0
hw_lock_try_return:
    msr     cpsr_cf, r1
    bx      lr

/**
 * hw_lock_to
 */
EnterARM(hw_lock_to)
    LoadLockHardwareRegister(r12)
    IncrementPreemptLevel(r12, r2)
#ifdef ARM_LOCK_EXCLUSIVES
hw_lock_to_retry:
    ldrex   r3, [r0]
    tst     r3, #1
    bne     hw_lock_to_wait
    orr     r3, r3, #1
    strex   r2, r3, [r0]
    cmp     r2, #0
    bne     hw_lock_to_retry
    dmb     ish
    mov     r0, #1
    bx      lr

hw_lock_to_wait:
    mov     r12, #1
    mov     r2, #0
hw_lock_mask_to_wait:
    /* Retain the caller's absolute-time timeout across clock calls. The
     * preemption hold was acquired once, before entering this slow path. */
    stmfd   sp!, {r4,r5,r6,r7,lr}
    add     r7, sp, #12
    stmfd   sp!, {r8,r10,r11}
    mov     r4, r0
    mov     r5, r1
    mov     r10, r12
    mov     r11, r2
    blx     _mach_absolute_time
    mov     r6, r0
    mov     r8, r1
hw_lock_to_wait_retry:
    ldrex   r3, [r4]
    tst     r3, r10
    bne     hw_lock_to_wait_clock
    orr     r3, r3, r10
    strex   r2, r3, [r4]
    cmp     r2, #0
    bne     hw_lock_to_wait_retry
    dmb     ish
    mov     r0, #1
    b       hw_lock_to_wait_return
hw_lock_to_wait_clock:
    /* Poll actively so an IRQ-masked caller can still reach its deadline. */
    blx     _mach_absolute_time
    subs    r0, r0, r6
    sbcs    r1, r1, r8
    bne     hw_lock_to_wait_expired
    cmp     r0, r5
    blo     hw_lock_to_wait_retry
hw_lock_to_wait_expired:
    cmp     r11, #0
    bne     hw_lock_to_wait_failed
    bl      __enable_preemption
hw_lock_to_wait_failed:
    mov     r0, #0
hw_lock_to_wait_return:
    ldmfd   sp!, {r8,r10,r11}
    ldmfd   sp!, {r4,r5,r6,r7,pc}

hw_lock_wait_default:
    LOAD_ADDR(r1, LockTimeOut)
    ldr     r1, [r1]
    cmp     r1, #0
    bne     hw_lock_wait_default_ready
    /* Use ml_init_lock_timeout's default until early bootstrap sets it. */
    LoadConstantToReg(250000000, r1)
hw_lock_wait_default_ready:
    mov     r2, #1
    b       hw_lock_mask_to_wait
#else
    ldr     r3, [r0]
    mov     r2, #1
    orr     r1, r3, #1
    ands    r2, r2, r3
    streq   r1, [r0]
    eors    r0, r2, #1
    beq     hw_lock_to_enable_preempt
    bx      lr
#endif
hw_lock_to_enable_preempt:
    stmfd   sp!, {r0,r1,r7,lr}
    add     r7, sp, #8
    blx     __enable_preemption
    ldmfd   sp!, {r0,r1,r7,lr}
    bx      lr

/**
 * lock_read and friends
 */
EnterARM(lock_read)
EnterARM(lck_rw_lock_shared)
#ifdef NO_EXCLUSIVES
    mrs         r9, cpsr
    orr         r3, r9, #0xc0
    msr         cpsr_cf, r3
#endif
    mov     r3, #0xD
rwlsloop:
    ldrex   r1, [r0]
    ands    r2, r1, r3
    bne     rwlsopt
rwlsloopres:
    add     r1, r1, #0x10000
#ifndef BOARD_CONFIG_OMAP3530
    strex   r2, r1, [r0]
    movs    r2, r2
#ifdef ARM_LOCK_EXCLUSIVES
    bne     rwlsloop
    dmb     ish
    bx      lr
#else
    bxeq    lr
    b       rwlsloop
#endif
    str     r1, [r0]
    bx      lr
#endif
    str     r1, [r0]
    bx      lr
rwlsopt:
#ifndef _ARM_ARCH_7
    mov     r2, #0x0001
    orr     r2, r2, #0x8000
#else
    mov     r2, #0x8001
#endif
    ands    r2, r1, r2
    LOAD_ADDR(r12, lck_rw_lock_shared_gen)
    bx      r12
    movs    r2, r1, lsr#16
    bne     rwlsloopres
rwlsloolow:
    LOAD_ADDR(r12, lck_rw_lock_shared_gen)
    bx      r12

/**
 * lock_write and friends
 */
EnterARM(lock_write)
EnterARM(lck_rw_lock_exclusive)
    LoadConstantToReg(0xFFFF000D, r3)
#ifdef NO_EXCLUSIVES
    mrs         r12, cpsr
    orr         r2, r12, #0xc0
    msr         cpsr_cf, r2
#endif
rwleloop:
    ldrex       r1, [r0]
    ands        r2, r1, r3
    bne         rwleslow
    orr         r1, r1, #8
#ifndef NO_EXCLUSIVES
    strex       r2, r1, [r0]
    movs        r2, r2
#ifdef ARM_LOCK_EXCLUSIVES
    bne         rwleloop
    dmb         ish
    bx          lr
#else
    bxeq        lr
    b           rwleloop
#endif
#else
    str         r1, [r0]
    msr         cpsr_cf, r12
    bx          lr
#endif
rwleslow:
    LOAD_ADDR(r12, lck_rw_lock_exclusive_gen)
    bx          r12

/**
 * lock_done and friends
 */
EnterARM(lock_done)
EnterARM(lck_rw_done)
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ishst
#endif
#ifdef BOARD_CONFIG_OMAP3530
    mrs         r9, cpsr
    cpsid       if
#endif
_lock_done_enter:
    ldrex       r1, [r0]
    ands        r2, r1, #1
#ifdef ARM_LOCK_EXCLUSIVES
    bne         rwldinterlock_wait
#else
    bne         rwldpanic
#endif
    LoadConstantToReg(0xFFFF0000, r3)
    ands        r2, r1, r3
    beq         rwldexcl
    sub         r1, r1, #0x10000
    ands        r2, r1, r3
    mov         r12, #0
    mov         r3, #1
    bne         rwldshared1
    ands        r12, r1, #2
    bics        r1, r1, #2
rwldshared1:
    b           rwldstore
rwldexcl:
    ands        r2, r1, #4
    orrne       r2, r2, #2
    bne         rwldexcl1
    mov         r2, #0xA
rwldexcl1:
    mov         r3, #2
    and         r12, r1, #2
    bic         r1, r1, r2
rwldstore:
#ifndef BOARD_CONFIG_OMAP3530
    strex       r2, r1, [r0]
    movs        r2, r2
    bne         _lock_done_enter
#else
    str         r1, [r0]
#endif
#ifdef BOARD_CONFIG_OMAP3530
    msr         cpsr_cf, r9
#endif
    movs        r12, r12
    moveq       r0, r3
    bxeq        lr
    stmfd       sp!,{r0,r3,r7,lr}
    add         r7, sp, #8
    add         r0, r0, #8
    blx         _thread_wakeup
    ldmfd       sp!,{r0,r3,r7,lr}
    mov         r0, r3
    bx          lr
#ifdef ARM_LOCK_EXCLUSIVES
rwldinterlock_wait:
    ldr         r1, [r0]
    tst         r1, #1
    bne         rwldinterlock_wait
    b           _lock_done_enter
#endif
rwldpanic:
    mov         r2, r1
    mov         r1, r0
    adr         r0, lckPanicString
    blx         _panic
lckPanicString:
    .asciz "lck_rw_done(): lock (0x%08x: 0x%08x)"

/**
 * lock_read_to_write and friends
 */
EnterARM(lock_read_to_write)
EnterARM(lck_rw_lock_shared_to_exclusive)
    ldrex       r1, [r0]
    LoadConstantToReg(0xFFFF0000, r3)
    ands        r2, r1, r3
    beq         rwlsepanic
    bic         r1, r1, r3
    LOAD_ADDR(r12, lck_rw_lock_shared_to_exclusive_gen)
    subs        r2, r2, #0x10000
    bxne        r12
    ands        r3, r1, #5
    bxne        r12
    orr         r1, r1, #4
#ifndef BOARD_CONFIG_OMAP3530
    strex       r2, r1, [r0]
    movs        r2, r2
    bne         _lock_read_to_write
#else
    str         r1, [r0]
#endif
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ish
#endif
    mov         r0, #1
    bx          lr
rwlsepanic:
    mov         r1, r2
    mov         r1, r0
    adr         r0, lckReadToWritePanicString
    blx         _panic
lckReadToWritePanicString:
    .asciz "lck_rw_lock_shared_to_exclusive(): lock (0x%08x 0x%08x)"

/**
 * lock_write_to_read and friends
 */
EnterARM(lck_rw_lock_exclusive_to_shared)
EnterARM(lock_write_to_read)
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ish
#endif
#ifdef NO_EXCLUSIVES
    mrs         r12, cpsr
    orr         r2, r12, #0xc0
    msr         cpsr_cf, r2
#endif
.Lrw_downgrade_retry:
    ldrex       r1, [r0]
    ands        r2, r1, #1
#ifdef ARM_LOCK_EXCLUSIVES
    bne         rwlstinterlock_wait
#else
    bne         rwlstexit
#endif
    and         r2, r1, #2
    ands        r3, r1, #4
    mov         r3, #6
    moveq       r3, #0xA
    bic         r1, r1, r3
    orr         r1, r1, #0x10000
#ifndef BOARD_CONFIG_OMAP3530
    strex       r3, r1, [r0]
    movs        r3, r3
    bne         _lock_write_to_read
#else
    str         r1, [r0]
    msr         cpsr_cf, r12
#endif
    movs        r2, r2
    bxeq        lr
    add         r0, r0, #8
    LOAD_ADDR(r12, thread_wakeup)
    bx          r12
#ifdef ARM_LOCK_EXCLUSIVES
rwlstinterlock_wait:
    ldr         r1, [r0]
    tst         r1, #1
    bne         rwlstinterlock_wait
    b           .Lrw_downgrade_retry
#endif
rwlstexit:
    mov         r2, r1
    mov         r1, r0
    adr         r0, rwlstePanicString
    blx         _panic
rwlstePanicString:
    .asciz "lck_rw_lock_exclusive_to_shared(): lock (0x%08x 0x%08x)"


/**
 * lck_mtx_unlock
 */
EnterARM(lck_mtx_unlock)
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ishst
#endif
#ifdef NO_EXCLUSIVES
    mrs     r9, cpsr
    orr     r2, r9, #0xc0
    msr     cpsr_cf, r2
#endif
    mov     r2, #0
    LoadLockHardwareRegister(r12)
mluloop:
    ldrex       r1, [r0]
    ands        r3, r1, #3
    bne         lmuslow
    bic         r3, r1, #3
    cmp         r3, r12
    bne         lmupanic
#ifndef NO_EXCLUSIVES
    strex       r1, r2, [r0]
    movs        r1, r1
    bxeq        lr
#else
    str         r2, [r0] 
    msr         cpsr_cf, r9
    bx          lr
#endif
    
    b           mluloop
lmuslow:
#ifdef NO_EXCLUSIVES
    msr         cpsr_cf, r9
#endif
    stmfd       sp!,{r0,r1,r7,lr}
    add         r7, sp, #8
#ifdef ARM_LOCK_EXCLUSIVES
    bl          _hw_lock_lock
    ldr         r1, [r0]
    mov         r3, r1
#else
    LoadLockHardwareRegister(r12)
    IncrementPreemptLevel(r12, r2)
    ldr         r1, [r0]
    ands        r3, r1, #1
    bne         lmupanic
    orr         r3, r1, #1
    str         r3, [r0]
#endif
    bics        r1, r1, #3
    ands        r2, r3, #2
    beq         lmu_release_interlock
    blx         _lck_mtx_unlock_wakeup
lmu_release_interlock:
    ldmfd       sp!,{r0,r1,r7,lr}
    ldr         r1, [r0]
    and         r3, r1, #2
    str         r3, [r0]
lmuret:
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ish
#endif
    b           __enable_preemption
lmupanic:
    mov         r1, r0
    ldr         r2, [r1]
    adr         r0, lmuPanicString
    blx         _panic
lmuPanicString:
    .asciz      "lck_mtx_unlock(): mutex (0x%08x, 0x%08x)"

/**
 * lck_mtx_lock
 */
EnterARM(lck_mtx_lock)
    LoadLockHardwareRegister(r12)
mlckretry:
#ifdef NO_EXCLUSIVES
    mrs     r2, cpsr
    orr     r3, r2, #0xc0
    msr     cpsr_cf, r3
#endif
    ldrex   r3, [r0]
    movs    r3, r3
    bne     mlckslow
#ifndef NO_EXCLUSIVES
    strex   r1, r12, [r0]
    movs    r1, r1
    bne     mlckretry
#else
    str     r12, [r0]
    msr     cpsr_cf, r2
#endif 
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ish
#endif
    bx      lr
mlckslow:
#ifdef NO_EXCLUSIVES
    msr     cpsr_cf, r2
#endif
    stmfd   sp!,{r0,r1,r7,lr}
    add     r7, sp, #8
#ifdef ARM_LOCK_EXCLUSIVES
    bl      _hw_lock_lock
    ldr     r3, [r0]
    bics    r1, r3, #3
#else
    LoadLockHardwareRegister(r12)
    IncrementPreemptLevel(r12, r2)
    ldr     r3, [r0]
    ands    r2, r3, #1
    bne     mlckpanic
    bics    r1, r3, #2
#endif
    bne     mlckwait
#ifndef ARM_LOCK_EXCLUSIVES
    orr     r3, r3, #1
    str     r3, [r0]
#endif
    blx     _lck_mtx_lock_acquire
    ands    r0, r0, r0
    ldmfd   sp!,{r0,r1,r7,lr}
    LoadLockHardwareRegister(r12)
    mov     r3, r12
    orrne   r3, r3, #2
    str     r3, [r0]
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ish
#endif
    b       __enable_preemption
mlckwait:
    /* lock_wait requires the interlock; unlock needs bit 1 to wake waiters. */
    orr     r3, r3, #3
    str     r3, [r0]
    blx     _lck_mtx_lock_wait
    ldmfd   sp!,{r0,r1,r7,lr}
    LoadLockHardwareRegister(r12)
    b       mlckretry
mlckpanic:
    mov     r1, r0
    ldr     r2, [r1]
    adr     r0, mlckpanicString
    blx     _panic
mlckpanicString:
    .asciz  "lck_mtx_lock(): mutex (0x%08x, 0x%08x)"

/**
 * lck_mtx_try_lock
 */
EnterARM(lck_mtx_try_lock)
    LoadLockHardwareRegister(r12)
#ifdef NO_EXCLUSIVES
    mrs         r2, cpsr
    orr         r3, r2, #0xc0
    msr         cpsr_cf, r3
#endif
lmtstart:
    ldrex       r3, [r0]
    movs        r3, r3
    bne         lmtslow
#ifndef NO_EXCLUSIVES
    strex       r1, r12, [r0]
    movs        r1, r1
    bne         lmtstart
#else
    str         r12, [r0]
    msr         cpsr_cf, r2
#endif
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ish
#endif
    mov         r0, #1
    bx          lr
lmtslow:
#ifdef NO_EXCLUSIVES
    msr         cpsr_cf, r2
#endif
    mov         r1, #0
    stmfd       sp!, {r0,r1,r7,lr}
    add         r7, sp, #8
#ifdef ARM_LOCK_EXCLUSIVES
    bl          _hw_lock_lock
    ldr         r3, [r0]
    bics        r2, r3, #3
    bic         r1, r3, #1
    strne       r1, [r0]
#else
    IncrementPreemptLevel(r12, r2)
    ldr         r3, [r0]
    ands        r2, r3, #1
    bne         lmtpanic
    bics        r2, r3, #2
#endif
    bne         lmtret
#ifndef ARM_LOCK_EXCLUSIVES
    orr         r3, r3, #1
    str         r3, [r0]
#endif
    blx         _lck_mtx_lock_acquire
    ands        r0, r0, r0
    ldmfd       sp!,{r0,r1,r7,lr}
    LoadLockHardwareRegister(r12)
    mov         r3, r12
    orrne       r3, r3, #2
    str         r3, [r0]
#ifdef ARM_LOCK_EXCLUSIVES
    dmb         ish
#endif
    mov         r1, #1
    stmfd       sp!,{r0,r1,r7,lr}
    add         r7, sp, #8
lmtret:
    bl          __enable_preemption
    ldmfd       sp!,{r0,r1,r7,lr}
    mov         r0, r1
    bx          lr
lmtpanic:
    mov         r1, r0
    ldr         r2, [r1]
    adr         r0, lmtPanicString
    blx         _panic
lmtPanicString:
    .asciz "lck_mtx_try_lock(): mutex (0x%08x, 0x%08x)"

/**
 * lck_mtx_assert
 */
EnterARM(lck_mtx_assert)
    ldr     r12, [r0]
    bics    r3, r12, #3
    cmp     r1, #1
    bne     lck_mtx_assert_owned
    LoadLockHardwareRegister(r12)
    cmp     r3, r12
    bxeq    lr
    mov     r1, r0
    adr     r0, panicString_lockNotOwned
    blx     _panic
lck_mtx_assert_owned:
    cmp     r1, #2
    bne     lck_mtx_assert_bad_arg
    LoadLockHardwareRegister(r12)
    cmp     r3, r12
    bxne    lr
    mov     r1, r0
    adr     r0, panicString_lockOwned
    blx     _panic
lck_mtx_assert_bad_arg:
    adr     r0, panicString_lockBadArgument
    blx     _panic

    .align 4
panicString_lockNotOwned:
    .asciz  "lck_mtx_assert(): mutex (0x%08X) not owned\n"
    .align 4
panicString_lockOwned:
    .asciz  "lck_mtx_assert(): mutex (0x%08X) owned\n"
    .align 4
panicString_lockBadArgument:
    .asciz  "lck_mtx_assert(): arg1 (0x%08X) invalid\n"

/**
 * lck_rw_try_lock_shared
 */
EnterARM(lck_rw_try_lock_shared)
#ifdef NO_EXCLUSIVES
    mrs     r9, cpsr
    orr     r1, r9, #0xc0
    msr     cpsr_cf, r1
#endif
.Lrw_try_shared_retry:
    ldrex   r1, [r0]
    ands    r2, r1, #1
#ifdef ARM_LOCK_EXCLUSIVES
    bne     rwtlsinterlock_wait
#else
    bne     rwtlspanic
#endif
    ands    r2, r1, #0xC
    bne     rwtlsopt
rwtlsloopres:
    add     r1, r1, #0x10000
#ifndef NO_EXCLUSIVES
    strex   r2, r1, [r0]
    movs    r2, r2
    bne     _lck_rw_try_lock_shared
#else
    str     r1, [r0]
    msr     cpsr_cf, r9
#endif
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ish
#endif
    mov     r0, #1
    bx      lr
rwtlsopt:
    ands    r2, r1, #0x8000
    bne     rwtlsfail
    LoadConstantToReg(0xFFFF0000, r3)
    ands    r2, r1, r3
    bne     rwtlsloopres
rwtlsfail:
    mov     r0, #0
    bx      lr
#ifdef ARM_LOCK_EXCLUSIVES
rwtlsinterlock_wait:
    ldr     r1, [r0]
    tst     r1, #1
    bne     rwtlsinterlock_wait
    b       .Lrw_try_shared_retry
#endif
rwtlspanic:
    mov     r2, r1
    mov     r1, r0
    adr     r0, rwtlsPanicString
    blx     _panic
rwtlsPanicString:
    .asciz  "lck_rw_try_lock_shared: lock (0x%08x, 0x%08x)"

/**
 * lck_rw_try_lock_exclusive
 */
EnterARM(lck_rw_try_lock_exclusive)
#ifdef NO_EXCLUSIVES
    mrs     r9, cpsr
    orr     r1, r9, #0xc0
    msr     cpsr_cf, r1
#endif
.Lrw_try_exclusive_retry:
    ldrex   r1, [r0]
    ands    r2, r1, #1
#ifdef ARM_LOCK_EXCLUSIVES
    bne     rwtleinterlock_wait
#else
    bne     rwtlepanic
#endif
    LoadConstantToReg(0xFFFF000D, r3)
    ands    r2, r1, r3
    bne     rwtlefail
    orr     r1, r1, #8
#ifndef NO_EXCLUSIVES
    strex   r2, r1, [r0]
    movs    r2, r2
    bne     _lck_rw_try_lock_exclusive
#else
    str     r1, [r0]
    msr     cpsr_cf, r9
#endif
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ish
#endif
    mov     r0, #1
    bx      lr
rwtlefail:
    mov     r0, #0
    bx      lr
#ifdef ARM_LOCK_EXCLUSIVES
rwtleinterlock_wait:
    ldr     r1, [r0]
    tst     r1, #1
    bne     rwtleinterlock_wait
    b       .Lrw_try_exclusive_retry
#endif
rwtlepanic:
    mov     r2, r1
    mov     r1, r0
    adr     r0, rwtlePanicString
    blx     _panic
rwtlePanicString:
    .asciz  "lck_rw_try_lock_exclusive: lock (0x%08x, 0x%08x)"
    
/**
 * bitlock
 */
EnterARM(hw_lock_bit)
    mov     r12, #1
    mov     r12, r12, lsl r1
    LoadLockHardwareRegister(r9)
    IncrementPreemptLevel(r9, r2)
#ifdef ARM_LOCK_EXCLUSIVES
hw_lock_bit_retry:
    ldrex   r3, [r0]
    tst     r3, r12
    bne     hw_lock_bit_wait
    orr     r1, r3, r12
    strex   r2, r1, [r0]
    cmp     r2, #0
    bne     hw_lock_bit_retry
    dmb     ish
    bx      lr

hw_lock_bit_wait:
    stmfd   sp!, {r0,r1,r7,lr}
    add     r7, sp, #8
    bl      hw_lock_wait_default
    cmp     r0, #0
    ldmfd   sp!, {r0,r1,r7,lr}
    bxne    lr
    b       hwlbitpanic
#else
    ldrex    r3, [r0]
    orr     r1, r3, r12
    ands    r2, r12, r3
    streq   r1, [r0]
    bxeq    lr
#endif
hwlbitpanic:
    mov     r1, r0
    adr     r0, hwlbitPanicString
    blx     _panic
hwlbitPanicString:
    .asciz "hw_lock_bit: lock 0x%08x"

EnterARM(hw_unlock_bit)
    mov     r12, #1
    mov     r12, r12, lsl r1
#ifdef ARM_LOCK_EXCLUSIVES
    dmb     ishst
hw_unlock_bit_retry:
    ldrex   r3, [r0]
    bic     r3, r3, r12
    strex   r1, r3, [r0]
    cmp     r1, #0
    bne     hw_unlock_bit_retry
    dsb     ishst
    sev
#else
    ldr     r3, [r0]
    bic     r3, r3, r12
    str     r3, [r0]
#endif
    b       __enable_preemption

LOAD_ADDR_GEN_DEF(thread_wakeup)
LOAD_ADDR_GEN_DEF(lck_rw_lock_shared_gen)
LOAD_ADDR_GEN_DEF(lck_rw_lock_exclusive_gen)
LOAD_ADDR_GEN_DEF(lck_rw_lock_shared_to_exclusive_gen)
LOAD_ADDR_GEN_DEF(LockTimeOut)
