#!/usr/bin/env python3
"""Exercise the actual platform implementation against in-memory MMIO.
This checks software sequencing, not Tegra hardware or boot compatibility.
"""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'xnu/pexpert/arm/pe_tegra30.c').read_text()
source = re.sub(r'^#include .*$', '', source, flags=re.M)
source = source.replace('__asm__ volatile("dsb sy" ::: "memory");', '')
shim = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BOARD_CONFIG_TEGRA30_NEXUS7 1
#define TRUE 1
#define FALSE 0
typedef int boolean_t;
typedef uintptr_t vm_offset_t;
typedef struct { int unused; } arm_saved_state_t;
#include "pexpert/arm/protos.h"
SocDeviceDispatch gPESocDispatch;
struct { uint64_t timebase_frequency_hz, dec_clock_rate_hz; } gPEClockFrequencyInfo;
static uint32_t uart[1024], timer[1024], gicd[1024], gicc[1024], lic[1024];
static int interrupts_enabled, rtclock_calls;
static uint64_t configured_hz;
vm_offset_t ml_io_map(uint32_t address, uint32_t size) {
    assert(size == 4096);
    switch (address) {
        case 0x70006000: return (vm_offset_t)uart;
        case 0x60005000: return (vm_offset_t)timer;
        case 0x50041000: return (vm_offset_t)gicd;
        case 0x50040000: return (vm_offset_t)gicc;
        case 0x60004000: return (vm_offset_t)lic;
    }
    assert(!"unexpected mapping"); return 0;
}
boolean_t ml_set_interrupts_enabled(boolean_t e) {
    int previous = interrupts_enabled; interrupts_enabled = e; return previous;
}
void rtc_configure(uint64_t hz) { configured_hz = hz; }
void rtclock_intr(arm_saved_state_t *regs) { assert(regs); ++rtclock_calls; }
void PE_early_puts(char *s) { (void)s; }
'''
tests = r'''
int main(void) {
    arm_saved_state_t state;
    uart[(0x300+0x14)/4] = 0x20;
    PE_init_SocSupport_stub();
    assert(gPESocDispatch.uart_getc() == -1);
    uart[(0x300+0x14)/4] |= 1;
    uart[0x300/4] = 'R';
    assert(gPESocDispatch.uart_getc() == 'R');
    tegra30_uart_suspend(); tegra30_early_putc('Q');
    assert(early_count == 1 && uart[0x300/4] == 'R');
    tegra30_uart_init();
    assert(!early_count && uart[0x300/4] == 'Q');
    uart[(0x300+0x14)/4] = 0; /* Broken UART must terminate polling. */
    tegra30_early_putc('Z'); assert(uart[0x300/4] == 'Q');
    gicd[4/4] = 4;
    gPESocDispatch.interrupt_init();
    assert(gicd[0] == 1 && gicc[0x100/4] == 1);
    assert(gicd[0x104/4] == 1 && lic[0x24/4] == 1);
    for (unsigned i=0;i<5;++i) {
        assert(lic[(0x28+0x100*i)/4] == UINT32_MAX);
        assert(lic[(0x2c+0x100*i)/4] == 0);
    }
    assert(gPESocDispatch.get_timebase() == 0);
    timer[0x10/4] = UINT32_MAX - 3;
    gPESocDispatch.timebase_init();
    assert(configured_hz == 1000000);
    assert(timer[0] == (0xc0000000U | 9999));
    interrupts_enabled = 1;
    timer[0x10/4] = 2;
    assert(gPESocDispatch.get_timebase() == 6 && interrupts_enabled == 1);
    timer[0x10/4] = 100;
    assert(gPESocDispatch.get_timebase() == 104);
    gicc[(0x100+0xc)/4] = 32 | (3U<<10);
    gPESocDispatch.handle_interrupt(&state);
    assert(rtclock_calls == 1 && timer[1] == (1U<<30));
    assert(gicc[(0x100+0x10)/4] == (32 | (3U<<10)));
    assert(lic[0x1c/4] == 1);
    gicc[(0x100+0xc)/4] = 1023;
    gPESocDispatch.handle_interrupt(&state);
    assert(rtclock_calls == 1 && gicc[(0x100+0x10)/4] == (32 | (3U<<10)));
    gicc[(0x100+0xc)/4] = 40;
    gPESocDispatch.handle_interrupt(&state);
    assert(gicd[(0x180+4)/4] == (1U<<8));
    assert(gicc[(0x100+0x10)/4] == 40 && rtclock_calls == 1);
    gPESocDispatch.timer_enabled(0); assert(timer[0] == 0);
    puts("PASS: UART bounds/remap, GIC/LIC routing/EOI, timer wrap and IRQ dispatch");
}
'''
with tempfile.TemporaryDirectory(prefix='nexus7-mmio-') as d:
    c = Path(d)/'test.c'; exe=Path(d)/'test'
    c.write_text(shim+source+tests)
    subprocess.run(['clang','-std=gnu99','-Werror','-Wno-int-to-pointer-cast','-I'+str(root/'xnu/pexpert'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
