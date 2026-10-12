/* SPDX-License-Identifier: BSD-2-Clause */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
extern "C" void _disable_preemption(void);

/* WDT0 uses TIMER5, independently of XNU's TIMER1 and interrupt delivery.
 * Four expirations of (250 ms * period) cause a PMC reset. Never feed from
 * the clock IRQ: that would hide a scheduler/workloop stall. */
static const UInt32 timeoutSeconds = 10, feedMilliseconds = 1000;
static unsigned feeds;
static volatile int fault;
static int faultControl SYSCTL_HANDLER_ARGS
{
    int value = 0;
    int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value < 1 || value > 4) return EINVAL;
    fault = value; /* UP, aligned word; consumed by the workloop. */
    return 0;
}
static SYSCTL_UINT(_debug, OID_AUTO, tegra_watchdog_feeds,
    CTLFLAG_RD | CTLFLAG_NOAUTO | CTLFLAG_LOCKED, &feeds, 0, "Watchdog heartbeats");
/* Registered only on an explicitly opted-in DEVELOPMENT test boot. Writes
 * retain the sysctl default root-only permission (no CTLFLAG_ANYBODY). */
static SYSCTL_PROC(_debug, OID_AUTO, tegra_watchdog_fault,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_NOAUTO | CTLFLAG_LOCKED,
    0, 0, faultControl, "I", "1=no feed 2=IRQ off 3=scheduler stall 4=panic");

class Tegra30Watchdog : public IOService {
    OSDeclareDefaultStructors(Tegra30Watchdog)
    IOMemoryMap *mapping;
    volatile UInt32 *registers;
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    bool testEnabled, installed, published;
    static Tegra30Watchdog *active;
    void write(unsigned offset, UInt32 value) {
        registers[offset/4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void disarm() {
        write(0x10c, 0xc45a);
        write(0x108, 2);
        write(0x60, 0);
    }
    static void heartbeat(OSObject *owner, IOTimerEventSource *source);
    static IOReturn shutdown(OSObject *owner, void *, void *, void *, void *);
    void releaseResources();
public:
    virtual bool init(OSDictionary *properties = 0);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
};
OSDefineMetaClassAndStructors(Tegra30Watchdog, IOService)
Tegra30Watchdog *Tegra30Watchdog::active;

bool Tegra30Watchdog::init(OSDictionary *properties)
{
    if (!IOService::init(properties)) return false;
    mapping = 0; registers = 0; loop = 0; timer = 0;
    testEnabled = installed = published = false;
    return true;
}

bool Tegra30Watchdog::start(IOService *provider)
{
    if (active) return false;
    if (!IOService::start(provider)) return false;
    mapping = provider->mapDeviceMemoryWithIndex(0);
    if (!mapping || mapping->getLength() < 0x110) goto fail;
    registers = (volatile UInt32 *)mapping->getVirtualAddress();
    loop = IOWorkLoop::workLoop();
    if (!loop) goto fail;
    timer = IOTimerEventSource::timerEventSource(this, heartbeat);
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) goto fail;
    installed = true;
    /* Reinitialize the inherited boot watchdog only after the software feed
     * path exists. No dependence on a particular loader or its deadline. */
    disarm();
    write(0x64, 1U << 30);
    write(0x60, 0xc0000000U | 250000U);
    write(0x100, 0x8005U | (timeoutSeconds << 4));
    write(0x108, 1);
    feeds = 0; fault = 0;
    sysctl_register_oid(&sysctl__debug_tegra_watchdog_feeds);
    published = true;
    active = this;
#if DEVELOPMENT || DEBUG
    int optIn;
    testEnabled = PE_parse_boot_argn("watchdog_test", &optIn, sizeof(optIn)) && optIn == 1;
    if (testEnabled) sysctl_register_oid(&sysctl__debug_tegra_watchdog_fault);
#endif
    if (timer->setTimeoutMS(feedMilliseconds) != kIOReturnSuccess)
        panic("Tegra30 watchdog: cannot schedule heartbeat");
    setProperty("WatchdogTimeoutSeconds", timeoutSeconds, 32);
    setProperty("WatchdogFeedMilliseconds", feedMilliseconds, 32);
    IOLog("[tegra-watchdog] armed timeout=%us feed=%ums test=%u\n",
          (unsigned)timeoutSeconds, (unsigned)feedMilliseconds, testEnabled);
    registerService();
    return true;
fail:
    releaseResources();
    IOService::stop(provider);
    return false;
}

void Tegra30Watchdog::heartbeat(OSObject *owner, IOTimerEventSource *source)
{
    Tegra30Watchdog *self = (Tegra30Watchdog *)owner;
    if (self->testEnabled && fault) {
        IOLog("[tegra-watchdog] fault=%d feeds=%u\n", fault, feeds);
        if (fault == 2) {
            ml_set_interrupts_enabled(FALSE);
            for (;;) __asm__ volatile("wfi");
        }
        if (fault == 3) {
            _disable_preemption();
            ml_set_interrupts_enabled(TRUE);
            for (;;) __asm__ volatile("nop");
        }
        if (fault == 4) panic("Tegra30 watchdog test panic");
        return; /* No reschedule and no ping; hardware must recover. */
    }
    self->write(0x108, 1);
    ++feeds;
    if (source->setTimeoutMS(feedMilliseconds) != kIOReturnSuccess)
        panic("Tegra30 watchdog: heartbeat lost");
}

void Tegra30Watchdog::releaseResources()
{
    if (published) {
        active = 0;
        if (testEnabled) sysctl_unregister_oid(&sysctl__debug_tegra_watchdog_fault);
        sysctl_unregister_oid(&sysctl__debug_tegra_watchdog_feeds);
        loop->runAction(shutdown, this);
        published = false;
    }
    if (timer) timer->cancelTimeout();
    if (installed) {
        loop->removeEventSource(timer);
        installed = false;
    }
    if (timer) { timer->release(); timer = 0; }
    if (loop) { loop->release(); loop = 0; }
    if (mapping) { mapping->release(); mapping = 0; }
    registers = 0;
}

IOReturn Tegra30Watchdog::shutdown(OSObject *owner, void *, void *, void *, void *)
{
    Tegra30Watchdog *self = (Tegra30Watchdog *)owner;
    self->timer->cancelTimeout();
    self->disarm();
    return kIOReturnSuccess;
}

void Tegra30Watchdog::stop(IOService *provider)
{
    releaseResources();
    IOService::stop(provider);
}
void Tegra30Watchdog::free()
{
    releaseResources();
    IOService::free();
}
