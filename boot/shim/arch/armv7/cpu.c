/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
bool armv7_identity_range(uint32_t base,uint32_t size)
{
    uint32_t sctlr,par;
    __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(sctlr));
    if (!(sctlr&1)) return true;
    if (!shim_range_valid((struct shim_range){base,size})) return false;
    uint32_t last=(base+size-1)&~0xfffU;
    for (uint32_t page=base&~0xfffU;;page+=0x1000) {
        __asm__ volatile("mcr p15,0,%1,c7,c8,0\n\tisb\n\tmrc p15,0,%0,c7,c4,0"
                         :"=r"(par):"r"(page):"memory");
        if ((par&1) || (par&~0xfffU)!=page) return false;
        if (page==last) break;
    }
    return true;
}
