/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
#ifndef SHIM_UART_PHYS
#define SHIM_UART_PHYS 0x70006000U /* mainline grouper U-Boot uses UARTA */
#endif
#define MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
uint32_t platform_ram_base(void) { return 0x80000000U; }
uint32_t platform_ram_limit(void) { return 0xc0000000U; } /* 1 GiB physical, not all handed to XNU */
uint32_t platform_virtual_base(void) { return 0x80000000U; }
uint32_t platform_outer_cache_base(void) { return 0x50043000U; }
struct shim_identity platform_identity(void)
{ return (struct shim_identity){"Nexus 7 (2012)","nvidia,tegra30",SHIM_UART_PHYS}; }
void platform_putc(char c)
{
    if (c=='\n') platform_putc('\r');
    unsigned int budget=100000;
    while (!(MMIO(SHIM_UART_PHYS+0x14)&0x20) && --budget) {}
    if (budget) MMIO(SHIM_UART_PHYS)=(unsigned char)c;
}
void platform_drain_uart(void)
{
    unsigned int budget=1000000;
    while (!(MMIO(SHIM_UART_PHYS+0x14)&0x40) && --budget) {}
    __asm__ volatile("dsb sy" ::: "memory");
}
const char *platform_prepare(bool keep_display)
{
    if (!armv7_identity_range(0x50040000,0x4000) ||
        !armv7_identity_range(0x60004000,0x3000) ||
        !armv7_identity_range(SHIM_UART_PHYS,0x100)) return "non-identity peripheral mappings";
    uint32_t midr;
    __asm__ volatile("mrc p15,0,%0,c0,c0,0":"=r"(midr));
    if (((midr>>4)&0xfff)!=0xc09) return "Nexus7 port requires Cortex-A9";
    if (MMIO(SHIM_UART_PHYS+0xc)&0x80) return "UART divisor latch is open";
    /* Mainline tegra-timer already derives this from CLK_M. Verify its known
     * divider encoding and that the counter runs, without guessing OSC rate. */
    uint32_t cfg=MMIO(0x60005014)&0xffff;
    if (cfg!=0xb && cfg!=0x43f && cfg!=0xc && cfg!=0x453 && cfg!=0x45f &&
        cfg!=0x19 && cfg!=0x4bf && cfg!=0x2f) return "unexpected TIMERUS_USEC_CFG";
    uint32_t first=MMIO(0x60005010); unsigned int budget=100000;
    while (MMIO(0x60005010)==first && --budget) {}
    if (!budget) return "microsecond counter is stopped";
    /* go does not run U-Boot's OS device cleanup. Hold the U-Boot-used DMA
     * engines in reset: USB1/2/3, SDMMC1/2/3/4, APB/AHB DMA. Retain display and host1x for a framebuffer handoff.
     * Preserve EMC, CPU, UART and timer. No peripheral clocks are reprogrammed. */
    MMIO(0x60006300)=(1U<<9)|(1U<<14)|(1U<<15)|(1U<<22) |
        (keep_display ? 0 : (1U<<26)|(1U<<27)|(1U<<28));
    MMIO(0x60006308)=(1U<<1)|(1U<<2)|(1U<<26)|(1U<<27);
    MMIO(0x60006310)=1U<<5;
    MMIO(0x50041000)=0; MMIO(0x50040100)=0;
    for (unsigned int i=0;i<5;++i) MMIO(0x60004028+0x100*i)=0xffffffffU;
    MMIO(0x60005000)=0; MMIO(0x60005004)=1U<<30;
    __asm__ volatile("dsb sy" ::: "memory");
    return NULL;
}
