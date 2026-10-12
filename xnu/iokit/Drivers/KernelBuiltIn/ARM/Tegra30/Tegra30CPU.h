/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef TEGRA30_CPU_H
#define TEGRA30_CPU_H

#include "../AppleARMPlatform/AppleARMCPU.h"

class Tegra30CPU : public AppleARMCPU {
    OSDeclareDefaultStructors(Tegra30CPU);
    UInt32 physicalID;
    void startMachCPU(void);
public:
    virtual IOService *probe(IOService *provider, SInt32 *score);
    virtual bool start(IOService *provider);
    virtual const OSSymbol *getCPUName(void);
    virtual void initCPU(bool boot);
    virtual kern_return_t startCPU(vm_offset_t entry, vm_offset_t argument);
    virtual void signalCPU(IOCPU *target);
};
#endif
