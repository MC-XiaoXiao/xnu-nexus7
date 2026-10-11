/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef TEGRA30_CPU_H
#define TEGRA30_CPU_H

#include "../AppleARMPlatform/AppleARMCPU.h"

/* Reuse the ARM master binding/interrupt path. Physically stopped secondary
 * cores remain unregistered with Mach until Tegra reset/IPI support exists. */
class Tegra30CPU : public AppleARMCPU {
    OSDeclareDefaultStructors(Tegra30CPU);
public:
    virtual IOService *probe(IOService *provider, SInt32 *score);
    virtual bool start(IOService *provider);
    virtual const OSSymbol *getCPUName(void);
};
#endif
