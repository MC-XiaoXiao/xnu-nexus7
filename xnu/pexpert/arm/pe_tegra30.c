/* SPDX-License-Identifier: BSD-2-Clause
 * Minimal Tegra30 / Nexus 7 (2012) platform expert.
 * Register facts: archived tegra30.dtsi, board-grouper.c and Tegra timer docs.
 * Boot CPU only. The loader must initialize SDRAM, UART pinmux/clock/divisor,
 * TIMERUS_USEC_CFG (1 MHz), disable secondary CPUs, and provide XNU boot_args.
 */
#if defined(BOARD_CONFIG_TEGRA30_NEXUS7)
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm/protos.h>
#include <pexpert/arm/boot.h>
#include <machine/machine_routines.h>
#include <arm/pmap.h>
#include <kern/debug.h>

/* Android grouper defaults to UARTD. Override at compile time for UARTA pads. */
#ifndef NEXUS7_UART_PHYS
#define NEXUS7_UART_PHYS 0x70006300U
#endif
#define TIMER_PHYS 0x60005000U
#define GICD_PHYS  0x50041000U
#define GICC_PHYS  0x50040100U
#define LIC_PHYS   0x60004000U
#define TIMER_IRQ 32U /* TIMER1 = GIC SPI 0 */
#define TICK_US 10000U
#define REG(b, o) (*(volatile uint32_t *)((b) + (o)))
static vm_offset_t uart_base, timer_base, gicd_base, gicc_base, lic_base;
static uint32_t last_usec;
static uint64_t elapsed_usec;
static boolean_t clock_ready;
static boolean_t uart_transition;
static char early_text[4096];
static unsigned int early_count;
void tegra30_uart_init(void);
void tegra30_uart_suspend(void) { uart_transition = TRUE; }
extern void rtc_configure(uint64_t);
extern void rtclock_intr(arm_saved_state_t *);

static void io_sync(void) { __asm__ volatile("dsb sy" ::: "memory"); }
void tegra30_early_putc(char c)
{
    vm_offset_t base;
    if (uart_transition) {
        if (early_count < sizeof(early_text)) early_text[early_count++] = c;
        return;
    }
    base = uart_base ? uart_base : NEXUS7_UART_PHYS;
    unsigned int budget = 100000;
    if (c == '\n') tegra30_early_putc('\r');
    while (!(REG(base, 0x14) & 0x20) && --budget) {}
    if (budget) { REG(base, 0) = (unsigned char)c; io_sync(); }
}
static int tegra_getc(void)
{
    if (!uart_base || !(REG(uart_base, 0x14) & 1)) return -1;
    return REG(uart_base, 0) & 0xff;
}
void tegra30_uart_init(void)
{
    if (!uart_base) {
        vm_offset_t page = ml_io_map(NEXUS7_UART_PHYS & ~0xfffU, 0x1000);
        uart_base = page + (NEXUS7_UART_PHYS & 0xfffU);
    }
    uart_transition = FALSE;
    for (unsigned int i = 0; i < early_count; ++i) tegra30_early_putc(early_text[i]);
    early_count = 0;
}
static void tegra_interrupt_init(void)
{
    uint32_t i, banks;
    gicd_base = ml_io_map(GICD_PHYS, 0x1000);
    gicc_base = ml_io_map(GICC_PHYS & ~0xfffU, 0x1000) + 0x100;
    lic_base = ml_io_map(LIC_PHYS, 0x1000);
    timer_base = ml_io_map(TIMER_PHYS, 0x1000);
    REG(gicd_base, 0) = 0;
    REG(gicc_base, 0) = 0;
    banks = (REG(gicd_base, 4) & 31) + 1;
    for (i = 0; i < banks; ++i) {
        REG(gicd_base, 0x180 + 4*i) = 0xffffffffU;
        REG(gicd_base, 0x280 + 4*i) = 0xffffffffU;
    }
    /* Mask legacy interrupt controllers, then route only TIMER1 to CPU0. */
    for (i = 0; i < 5; ++i) {
        REG(lic_base, 0x28 + 0x100*i) = 0xffffffffU;
        REG(lic_base, 0x2c + 0x100*i) = 0; /* Route IRQ, not FIQ. */
        REG(lic_base, 0x1c + 0x100*i) = 0xffffffffU; /* Clear forced IRQs. */
    }
    REG(lic_base, 0x24) = 1;
    REG(timer_base, 0) = 0;
    REG(timer_base, 4) = 1U << 30;
    REG(gicd_base, 0x420) = 0x80808080U;
    REG(gicd_base, 0x820) = 0x01010101U;
    REG(gicd_base, 0xc08) &= ~3U; /* SPI0 level sensitive */
    REG(gicd_base, 0x104) = 1;
    REG(gicc_base, 4) = 0xff;
    REG(gicc_base, 8) = 0;
    io_sync();
    REG(gicd_base, 0) = 1;
    REG(gicc_base, 0) = 1;
    io_sync();
}
static uint64_t tegra_timebase(void)
{
    boolean_t enabled;
    uint32_t now;
    uint64_t result;
    if (!clock_ready) return 0;
    /* UP: IRQ exclusion makes the 32-bit wrap extension and 64-bit read atomic.
     * Requires a sample at least once every 2^32 us, including idle. */
    enabled = ml_set_interrupts_enabled(FALSE);
    now = REG(timer_base, 0x10);
    elapsed_usec += (uint32_t)(now - last_usec);
    last_usec = now;
    result = elapsed_usec;
    ml_set_interrupts_enabled(enabled);
    return result;
}
static void tegra_timer_enabled(int enabled)
{
    if (!timer_base) return;
    REG(timer_base, 0) = enabled ? (0xc0000000U | (TICK_US - 1)) : 0;
    io_sync();
}
static uint64_t tegra_timer_value(void) { return REG(timer_base, 4) & 0x1fffffffU; }
static void tegra_timebase_init(void)
{
    last_usec = REG(timer_base, 0x10);
    elapsed_usec = 0;
    clock_ready = TRUE;
    gPEClockFrequencyInfo.timebase_frequency_hz = 1000000;
    gPEClockFrequencyInfo.dec_clock_rate_hz = 1000000;
    rtc_configure(1000000);
    tegra_timer_enabled(TRUE);
}
static void tegra_handle_interrupt(void *context)
{
    uint32_t ack = REG(gicc_base, 0xc), irq = ack & 0x3ff;
    if (irq >= 1020) return; /* GIC special/spurious IDs have no EOI. */
    if (irq == TIMER_IRQ) {
        REG(timer_base, 4) = 1U << 30;
        io_sync();
        (void)tegra_timebase();
        rtclock_intr((arm_saved_state_t *)context);
    } else {
        /* No other device driver is installed yet; prevent an IRQ storm. */
        REG(gicd_base, 0x180 + 4*(irq / 32)) = 1U << (irq % 32);
    }
    if (irq >= 32 && irq < 192)
        REG(lic_base, 0x1c + 0x100*((irq - 32)/32)) = 1U << ((irq - 32)%32);
    io_sync();
    REG(gicc_base, 0x10) = ack; /* Preserve the complete acknowledge token. */
    io_sync();
}
static void tegra_framebuffer_init(void) { /* Serial-only bring-up. */ }
void PE_init_SocSupport_stub(void)
{
    gPESocDispatch.uart_getc = tegra_getc;
    gPESocDispatch.uart_putc = tegra30_early_putc;
    gPESocDispatch.uart_init = tegra30_uart_init;
    gPESocDispatch.interrupt_init = tegra_interrupt_init;
    gPESocDispatch.timebase_init = tegra_timebase_init;
    gPESocDispatch.get_timebase = tegra_timebase;
    gPESocDispatch.handle_interrupt = tegra_handle_interrupt;
    gPESocDispatch.timer_value = tegra_timer_value;
    gPESocDispatch.timer_enabled = tegra_timer_enabled;
    gPESocDispatch.framebuffer_init = tegra_framebuffer_init;
    tegra30_uart_init();
    PE_early_puts("Tegra30 Nexus7: serial/GIC/TIMER1, boot CPU only\n");
}
#endif
