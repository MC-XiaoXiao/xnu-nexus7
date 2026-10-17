/* SPDX-License-Identifier: BSD-2-Clause
 * Grouper ELAN touchscreen -> Apple IOHIDEventService, iOS 6 event ABI.
 * GPIO data-ready is polled on the IOKit workloop; no fabricated contacts.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include "../AppleHIDTouch/IOHIDEventService.h"
#include "Nexus7TouchEvents.h"
#include "Tegra30TouchHardware.h"

class Tegra30Touch : public IOHIDEventService {
    OSDeclareDefaultStructors(Tegra30Touch)
    IOMemoryMap *maps[4];
    volatile uint32_t *banks[4];
    IOTimerEventSource *timer;
    IOCommandGate *gate;
    NexusTouch::Frame previous;
    bool running,prepared,recoveryPending,published,gpioTransport;
    unsigned failures;
    uint64_t packets;
    unsigned rejected;
    void poll();
    void cleanup();
    static void tick(OSObject *object,IOTimerEventSource *) { static_cast<Tegra30Touch *>(object)->poll(); }
    static IOReturn shutdown(OSObject *object,void *,void *,void *,void *);
public:
    bool report(const NexusTouch::Frame &frame);
    bool receiveTouch(uint8_t *bytes,unsigned size) {
        return gpioTransport?NexusTouch::receiveGPIO(*this,bytes,size):NexusTouch::transfer(*this,bytes,size,true);
    }
    virtual bool init(OSDictionary *dictionary=0);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    uint32_t read(unsigned bank,unsigned offset) { __sync_synchronize();return banks[bank][offset/4]; }
    void write(unsigned bank,unsigned offset,uint32_t value) { banks[bank][offset/4]=value;__sync_synchronize(); }
    void delayUS(unsigned value) { if (value>=1000) IOSleep((value+999)/1000);else IODelay(value); }
};
OSDefineMetaClassAndStructors(Tegra30Touch,IOHIDEventService)
bool Tegra30Touch::init(OSDictionary *dictionary) {
    if (!IOHIDEventService::init(dictionary)) return false;
    bzero(maps,sizeof(maps));bzero(banks,sizeof(banks));bzero(&previous,sizeof(previous));
    timer=0;gate=0;running=prepared=recoveryPending=published=gpioTransport=false;failures=0;packets=rejected=0;return true;
}
bool Tegra30Touch::start(IOService *provider) {
    if (!IOHIDEventService::start(provider)) {IOLog("Tegra30Touch: HID service start failed\n");return false;}
    // Validate the board identity and resource ranges before any MMIO writes.
    OSData *profile=OSDynamicCast(OSData,provider->getProperty("TouchPanelProfile"));
    const char name[]="asus,grouper-elan-ktf3k";
    if (!profile || profile->getLength()!=sizeof(name) || memcmp(profile->getBytesNoCopy(),name,sizeof(name))) {IOLog("Tegra30Touch: panel profile mismatch\n");return false;}
    const uint32_t ranges[8]={0x7000c400,0x100,0x60006000,0x1000,0x6000d000,0x1000,0x70003000,0x400};
    OSData *reg=OSDynamicCast(OSData,provider->getProperty("reg"));
    if (!reg || reg->getLength()!=sizeof(ranges) || memcmp(reg->getBytesNoCopy(),ranges,sizeof(ranges))) {IOLog("Tegra30Touch: resource ranges mismatch\n");return false;}
    for (unsigned i=0;i<4;++i) {
        maps[i]=provider->mapDeviceMemoryWithIndex(i);
        if (!maps[i] || !maps[i]->getVirtualAddress() || maps[i]->getLength()<ranges[i*2+1]) { cleanup();return false; }
        banks[i]=(volatile uint32_t *)maps[i]->getVirtualAddress();
    }
    if (!NexusTouch::prepare(*this)) {IOLog("Tegra30Touch: clock preparation failed pll=%x osc=%x\n",read(NexusTouch::CAR,0xa0),read(NexusTouch::CAR,0x50));cleanup();return false;}
    prepared=true;
    // The LCD and ELAN share PW1. LCD cold initialization can cycle that
    // supply concurrently with driver matching. Start the workloop now and
    // publish HID only after a real hello succeeds with the rail enabled.
    recoveryPending=true;
    setProperty("PrimaryUsagePage",13,32);setProperty("PrimaryUsage",4,32);
    setProperty("Built-In",true);setProperty("DisplayIntegrated",true);
    setProperty("Transport","I2C");setProperty("Product","Nexus 7 ELAN KTF3K Touchscreen");
    setProperty("Manufacturer","ELAN");setProperty("VendorID",0x04f3,32);setProperty("ProductID",0x3624,32);
    setProperty("ReportInterval",8000,32);setProperty("QueueSize",4096,32);
    OSDictionary *pair=OSDictionary::withCapacity(2);OSArray *pairs=OSArray::withCapacity(1);
    OSNumber *page=OSNumber::withNumber(13,32),*usage=OSNumber::withNumber(4,32);
    if (!pair || !pairs || !page || !usage) {
        if (pair) pair->release();if (pairs) pairs->release();if (page) page->release();if (usage) usage->release();
        cleanup();return false;
    }
    pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",usage);pairs->setObject(pair);
    setProperty("DeviceUsagePairs",pairs);page->release();usage->release();pair->release();pairs->release();
    gate=IOCommandGate::commandGate(this);timer=IOTimerEventSource::timerEventSource(this,tick);
    if (!gate || !timer || getWorkLoop()->addEventSource(gate)!=kIOReturnSuccess ||
        getWorkLoop()->addEventSource(timer)!=kIOReturnSuccess) { cleanup();return false; }
    running=true;timer->setTimeoutMS(8);
    IOLog("Tegra30Touch: waiting for shared panel rail and ELAN hello\n");return true;
}
bool Tegra30Touch::report(const NexusTouch::Frame &frame) {
    if (!(previous.active|frame.active)) { previous=frame;return true; }
    AbsoluteTime time;clock_get_uptime(&time);
    IOHIDEvent *event=NexusTouch::event(previous,frame,time,getRegistryEntryID());
    if (!event) return false;
    dispatchEvent(event);event->release();previous=frame;++packets;return true;
}
void Tegra30Touch::poll() {
    if (!running) return;
    if (!NexusTouch::supplyPowered(*this)) {
        NexusTouch::Frame released={};report(released);recoveryPending=true;
        setProperty("TouchHardwareReady",false);timer->setTimeoutMS(250);return;
    }
    if (recoveryPending) {
        NexusTouch::resetBus(*this);
        if (!NexusTouch::handshake(*this,gpioTransport)) {
            if(gpioTransport || !NexusTouch::handshake(*this,true)) {timer->setTimeoutMS(250);return;}
            gpioTransport=true;
            setProperty("TouchI2CTransport","open-drain GPIO");
        }
        recoveryPending=false;failures=0;
        setProperty("TouchHardwareReady",true);
        if(!published) {
            published=true;registerService();
            IOLog("Tegra30Touch: ELAN ready, Apple IOHID digitizer, iOS6 ABI\n");
        }
    }
    if (NexusTouch::ready(*this)) {
        NexusTouch::BufferResult result=NexusTouch::consume(*this,*this,rejected);
        if (result==NexusTouch::BufferLost) {
            recoveryPending=true;NexusTouch::Frame released={};report(released);
        } else if (result==NexusTouch::BufferInvalid) ++failures;
        else failures=0;
    } else if (failures) ++failures;
    if (failures>=3) {
        NexusTouch::Frame released={};if (report(released)) failures=0;
    }
    if (running) timer->setTimeoutMS(8);
}
IOReturn Tegra30Touch::shutdown(OSObject *object,void *,void *,void *,void *) {
    Tegra30Touch *self=static_cast<Tegra30Touch *>(object);
    self->running=false;if (self->timer) self->timer->cancelTimeout();
    NexusTouch::Frame empty={};self->report(empty);
    if (self->prepared) { NexusTouch::gpio(*self,NexusTouch::ResetPin,0x20,false);self->prepared=false; }
    return kIOReturnSuccess;
}
void Tegra30Touch::cleanup() {
    running=false;
    if (timer) {timer->cancelTimeout();if (timer->getWorkLoop()) getWorkLoop()->removeEventSource(timer);timer->release();timer=0;}
    if (gate) {if (gate->getWorkLoop()) getWorkLoop()->removeEventSource(gate);gate->release();gate=0;}
    if (prepared) { NexusTouch::gpio(*this,NexusTouch::ResetPin,0x20,false);prepared=false; }
    for (unsigned i=0;i<4;++i) {if (maps[i]) maps[i]->release();maps[i]=0;banks[i]=0;}
}
void Tegra30Touch::stop(IOService *provider) {
    if (gate) gate->runAction(shutdown);cleanup();IOHIDEventService::stop(provider);
}
void Tegra30Touch::free() { cleanup();IOHIDEventService::free(); }
