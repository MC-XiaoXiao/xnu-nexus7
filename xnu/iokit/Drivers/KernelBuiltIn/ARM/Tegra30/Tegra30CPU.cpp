/* SPDX-License-Identifier: BSD-2-Clause */
#include "Tegra30CPU.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/OSByteOrder.h>
#include <arm/mp.h>
#include <pexpert/pexpert.h>
extern "C" kern_return_t arm_processor_register(cpu_id_t, unsigned, unsigned, vm_offset_t,
                                                 processor_t *, ipi_handler_t *);
extern "C" kern_return_t tegra30_start_cpu(unsigned, vm_offset_t);
extern "C" void tegra30_init_cpu(void);
extern "C" void tegra30_signal_cpu(unsigned);
static Tegra30CPU *physicalCPUs[4];
static bool masterReady;

#define super AppleARMCPU
OSDefineMetaClassAndStructors(Tegra30CPU, AppleARMCPU);

/* Apple DT cells are native little endian, unlike flattened Linux DT cells. */
static bool cpuString(IORegistryEntry *node, const char *key, const char *value)
{
    OSData *data = OSDynamicCast(OSData, node->getProperty(key));
    size_t size = strlen(value) + 1;
    return data && data->getLength() == size &&
           memcmp(data->getBytesNoCopy(), value, size) == 0;
}

static bool cpuEnabled(IORegistryEntry *node)
{
    if (!cpuString(node, "device_type", "cpu")) return false;
    return !node->getProperty("status") || cpuString(node, "status", "okay") ||
           cpuString(node, "status", "ok");
}

static UInt32 cpuID(IORegistryEntry *node)
{
    OSData *reg = OSDynamicCast(OSData, node->getProperty("reg"));
    if (!reg || reg->getLength() != sizeof(UInt32))
        panic("Tegra30 CPU: missing or invalid reg");
    UInt32 id = OSReadLittleInt32(reg->getBytesNoCopy(), 0);
    if (id >= 4) panic("Tegra30 CPU: reg %u exceeds CPU slots", id);
    return id;
}

IOService *Tegra30CPU::probe(IOService *provider, SInt32 *score)
{
    return cpuEnabled(provider) ? super::probe(provider, score) : NULL;
}

bool Tegra30CPU::start(IOService *provider)
{
    if (!cpuEnabled(provider)) return false;
    IORegistryEntry *cpus = IORegistryEntry::fromPath("/cpus", gIODTPlane);
    if (!cpus) panic("Tegra30 CPU: missing /cpus");
    OSIterator *iterator = cpus->getChildIterator(gIODTPlane);
    if (!iterator) panic("Tegra30 CPU: cannot enumerate /cpus");
    UInt32 mask = 0, count = 0, bootID = MAX_CPUS;
    IORegistryEntry *node;
    while ((node = OSDynamicCast(IORegistryEntry, iterator->getNextObject()))) {
        if (!cpuEnabled(node)) continue;
        UInt32 id = cpuID(node);
        if (mask & (1U << id)) panic("Tegra30 CPU: duplicate reg %u", id);
        mask |= 1U << id;
        ++count;
        if (cpuString(node, "state", "running")) {
            if (bootID != MAX_CPUS) panic("Tegra30 CPU: multiple running CPUs");
            bootID = id;
        }
    }
    iterator->release();
    cpus->release();
    /* This port's master CPU slot is zero. Never bind a secondary DT node
     * to the already running Mach master, regardless of matching order. */
    if (!count || bootID != 0) panic("Tegra30 CPU: missing running CPU0");
    UInt32 limit = count;
    int requested;
    if (PE_parse_boot_argn("cpus", &requested, sizeof(requested))) {
        if (requested <= 0) panic("Tegra30 CPU: cpus must be positive");
        if ((UInt32)requested < limit) limit = requested;
    }
    UInt32 id = cpuID(provider), ordinal = 0;
    for (UInt32 i = 0; i < id; ++i) if (mask & (1U << i)) ++ordinal;
    if (!(mask & (1U << id)) || ordinal >= limit) return false;
    /* IOCPU owns discovery/state; Mach owns scheduler and idle threads. */
    if (id == bootID) ml_init_max_cpus(limit);
    if (!IOCPU::start(provider)) return false;
    setCPUNumber(ordinal);
    physicalID = id;
    physicalCPUs[id] = this;
    machProcessor = PROCESSOR_NULL;
    ipi_handler = NULL;
    setCPUState(kIOCPUStateUnregistered);
    if (id == 0) {
        startMachCPU();
        masterReady = true;
        for (unsigned i = 1; i < 4; ++i)
            if (physicalCPUs[i]) physicalCPUs[i]->startMachCPU();
    } else if (masterReady) startMachCPU();
    registerService();
    IOService::setProperty("IOCPUDeviceTreeCount", count, 32);
    IOService::setProperty("IOCPUConfiguredCount", limit, 32);
    return true;
}

const OSSymbol *Tegra30CPU::getCPUName(void)
{
    return OSSymbol::withCString(getProvider()->getName());
}

extern "C" void arm_secondary_start(void);
void Tegra30CPU::initCPU(bool boot)
{
    (void)boot;
    tegra30_init_cpu();
    ml_init_delay_spin_threshold(12000);
    setCPUState(kIOCPUStateRunning);
}

kern_return_t Tegra30CPU::startCPU(vm_offset_t entry, vm_offset_t argument)
{
    (void)argument;
    return tegra30_start_cpu(physicalID, entry);
}

void Tegra30CPU::signalCPU(IOCPU *target)
{
    Tegra30CPU *cpu = OSDynamicCast(Tegra30CPU, target);
    if (cpu) tegra30_signal_cpu(cpu->physicalID);
}

/* Called on the target CPU after GIC acknowledgement. */
extern "C" void tegra30_dispatch_ipi(unsigned physical)
{
    Tegra30CPU *cpu = physical < 4 ? physicalCPUs[physical] : NULL;
    if (cpu) cpu->ipiHandler(NULL, NULL, 0);
}

void Tegra30CPU::startMachCPU(void)
{
    unsigned logical = physicalID == 0 ? 0 : real_ncpus;
    kern_return_t result = arm_processor_register((cpu_id_t)this, logical, physicalID, (vm_offset_t)arm_secondary_start,
                                                   &machProcessor, &ipi_handler);
    if (result != KERN_SUCCESS) panic("Tegra30 CPU%u registration: %d", physicalID, result);
    setCPUNumber(logical);
    setCPUState(kIOCPUStateStopped);
    result = processor_start(machProcessor);
    if (result != KERN_SUCCESS) panic("Tegra30 CPU%u start: %d", physicalID, result);
    IOService::setProperty("IOCPUStartSupported", kOSBooleanTrue);
    IOLog("[tegra-cpu] cpu=%u logical=%u state=running mach=1\n", physicalID, logical);
}
