/* SPDX-License-Identifier: BSD-2-Clause */
/* Test port only: synthetic kernel, not Tegra30 XNU hardware emulation. */
#include "shim.h"
#define UART ((volatile uint32_t *)(uintptr_t)0x09000000)
uint32_t platform_ram_base(void) { return 0x40000000; }
uint32_t platform_ram_limit(void) { return 0x60000000; }
uint32_t platform_virtual_base(void) { return 0x40000000; }
uint32_t platform_outer_cache_base(void) { return 0; }
struct shim_identity platform_identity(void)
{ return (struct shim_identity){"QEMU shim test","qemu,virt",0x09000000}; }
void platform_putc(char c) { while (UART[6]&(1U<<5)) {} UART[0]=(unsigned char)c; }
void platform_drain_uart(void) { while (UART[6]&(1U<<3)) {} }
const char *platform_prepare(bool keep_display) { (void)keep_display; return NULL; }
