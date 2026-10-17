/* SPDX-License-Identifier: BSD-2-Clause
 * USB1 UTMI/ChipIdea transport for the source-built iOS6-compatible family.
 * DeviceFamily owns descriptors, interface matching and USB multiplexing.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include "USBDeviceFamily.h"
#include "Tegra30USBHardware.h"

class Tegra30USB : public IOService, public NexusUSB::Transport {
    OSDeclareDefaultStructors(Tegra30USB)
    struct Endpoint {
        NexusUSB::Request *head,*tail;
        unsigned packet,programmed,completions;
        uint64_t deadline;
        bool enabled,active,receiver,canceling;
    } endpoints[TegraUSB::Channels];
    IOMemoryMap *usbMap,*carMap;
    volatile UInt8 *registers;
    volatile UInt32 *car;
    IOBufferMemoryDescriptor *dma;
    volatile TegraUSB::QueueHead *heads;
    volatile TegraUSB::Transfer *transfers;
    UInt8 *buffers;
    UInt32 physical;
    unsigned endpointPairs;
    uint64_t reconnectAfter;
    IOUSBDeviceController *controller;
    IOCommandGate *gate;
    IOWorkLoop *loop;
    IOInterruptEventSource *interrupt;
    IOTimerEventSource *timer;
    bool attached,started,desired,running,faulted,stopping,dmaPrepared,suspended;
    UInt32 cr(unsigned o) {TegraUSB::barrier();return car[o/4];}
    void cw(unsigned o,UInt32 v) {car[o/4]=v;TegraUSB::barrier();}
    void change(unsigned o,UInt32 clear,UInt32 set) {write(o,(read(o)&~clear)|set);}
    void carChange(unsigned o,UInt32 clear,UInt32 set) {cw(o,(cr(o)&~clear)|set);}
    struct ClockIO {
        volatile UInt32 *base;
        UInt32 read(unsigned o){TegraUSB::barrier();return base[o/4];}
        void write(unsigned o,UInt32 v){base[o/4]=v;TegraUSB::barrier();}
        void delay(unsigned us){IODelay(us);}
    };
    bool phy();
    bool allocateDMA();
    bool makeController();
    bool halt();
    void fail();
    void finish(NexusUSB::Request *,IOReturn);
    void cancelEndpoint(unsigned,IOReturn);
    void cancelAll(IOReturn);
    IOReturn activate(unsigned,unsigned,unsigned);
    IOReturn submit(NexusUSB::Request *);
    bool prime(unsigned,unsigned);
    void kick(unsigned);
    void receiveControl();
    void complete(unsigned);
    void service();
    void connection();
    void familyEvent(unsigned event) {controller->busEvent(event);}
    void control(UInt8 *p,bool setup,unsigned length) {
        controller->handleControl(p,setup,length);
        receiveControl();
    }
    enum Operation {OnBus,Stall,Abort,Submit,Address,Activate,Deactivate};
    static IOReturn action(OSObject *,void *,void *,void *,void *);
    IOReturn perform(Operation op,uintptr_t a=0,uintptr_t b=0,uintptr_t c=0) {
        return gate->runAction(action,(void *)op,(void *)a,(void *)b,(void *)c);
    }
    static void irq(OSObject *o,IOInterruptEventSource *,int) {static_cast<Tegra30USB *>(o)->service();}
    static void tick(OSObject *,IOTimerEventSource *);
    void releaseResources();
    bool quiesce();
    static IOReturn shutdown(OSObject *o,void *,void *,void *,void *){return static_cast<Tegra30USB *>(o)->quiesce()?kIOReturnSuccess:kIOReturnIOError;}
public:
    virtual bool init(OSDictionary *p=0);
    virtual bool start(IOService *p);
    virtual void stop(IOService *p);
    virtual void free();
    virtual IOReturn request(NexusUSB::Request *r){return perform(Submit,(uintptr_t)r);}
    virtual IOReturn cancel(unsigned ep,IOReturn s){return perform(Abort,ep,s);}
    virtual IOReturn enableEndpoint(unsigned ep,unsigned type,unsigned packet){return perform(Activate,ep,type,packet);}
    virtual IOReturn disableEndpoint(unsigned ep){return perform(Deactivate,ep);}
    virtual IOReturn haltEndpoint(unsigned ep,bool on){return perform(Stall,ep,on);}
    virtual bool isHalted(unsigned ep){return TegraUSB::valid(ep) && (read(TegraUSB::Control+4*(ep&15))&(1U<<((ep&128)?16:0)));}
    virtual IOReturn setAddress(unsigned a){return perform(Address,a);}
    virtual IOReturn goOnBus(bool on){return perform(OnBus,on);}
    virtual int busSpeed(){return !running?-1:((read(TegraUSB::HostPort)>>25)&3)==2?2:1;}
    virtual IOWorkLoop *getWorkLoop() const {return loop;}
    UInt32 read(unsigned o) {TegraUSB::barrier();UInt32 v=*(volatile UInt32 *)(registers+o);TegraUSB::barrier();return v;}
    void write(unsigned o,UInt32 v) {*(volatile UInt32 *)(registers+o)=v;TegraUSB::barrier();}
    void delay(unsigned us) {IODelay(us);}
};
OSDefineMetaClassAndStructors(Tegra30USB,IOService)

static uint64_t usbNow() {
    AbsoluteTime now;uint64_t ns;clock_get_uptime(&now);absolutetime_to_nanoseconds(now,&ns);return ns;
}
bool Tegra30USB::init(OSDictionary *p) {
    if(!IOService::init(p))return false;
    usbMap=carMap=0;registers=0;car=0;dma=0;heads=0;transfers=0;buffers=0;physical=0;reconnectAfter=0;endpointPairs=TegraUSB::EndpointPairs;
    controller=0;gate=0;loop=0;interrupt=0;timer=0;
    attached=started=desired=running=faulted=stopping=dmaPrepared=suspended=false;
    bzero(endpoints,sizeof(endpoints));return true;
}
bool Tegra30USB::allocateDMA() {
    const unsigned bytes=4096+TegraUSB::Channels*TegraUSB::Chunk;
    dma=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIOMemoryPhysicallyContiguous|kIODirectionInOut|kIOMapInhibitCache,bytes,0xfffff000ULL);
    if(!dma || dma->prepare()!=kIOReturnSuccess)return false;
    dmaPrepared=true;IOByteCount length=0;
    uint64_t pa=dma->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    if(!pa || (pa&4095) || length<bytes || pa+bytes>0x100000000ULL)return false;
    physical=(UInt32)pa;void *base=dma->getBytesNoCopy();if(!base)return false;
    bzero(base,bytes);heads=(volatile TegraUSB::QueueHead *)base;
    transfers=(volatile TegraUSB::Transfer *)((UInt8 *)base+2048);buffers=(UInt8 *)base+4096;
    return true;
}
bool Tegra30USB::phy() {
    // Grouper's oscillator is 12 MHz. Do not retune a PLL used by other USB ports.
    OSData *frequency=OSDynamicCast(OSData,getProvider()->getProperty("clock-frequency"));
    if(!frequency || frequency->getLength()!=4 || *(const UInt32 *)frequency->getBytesNoCopy()!=12000000)return false;
    ClockIO clock={car};
    if(!TegraUSB::enablePLLU(clock)){IOLog("[tegra-usb] PLLU enable failed base=%08x misc=%08x\n",cr(0xc0),cr(0xcc));return false;}
    cw(0x320,1U<<22);cw(0x300,1U<<22);IODelay(2);cw(0x304,1U<<22);IODelay(2);
    change(0x410,7,1); // USB1 nonlegacy map; required before core reset.
    change(0x400,0,(1U<<11)|(1U<<12));change(0x828,1U<<30,0);IODelay(1);
    carChange(0x484,(31U<<27)|0xfff|(1U<<16)|(1U<<14)|(1U<<12),(2U<<27)|118);
    carChange(0x488,(0xfffU<<6)|(31U<<18)|1,(47U<<6)|(8U<<18));
    change(0x808,(3U<<8)|(3U<<10)|(3U<<4)|(127U<<25)|(1U<<21),(3U<<8)|(2U<<25));
    change(0x838,15U<<18,7U<<18);change(0x80c,15,2|(1U<<2)|(1U<<24));
    change(0x824,1U<<22,0);change(0x83c,31U<<3,5U<<3);change(0x82c,0xffff,0x7530);
    change(0x820,0,1U<<19);change(0x830,0,1);change(0x834,0,1U<<3);
    change(0x810,(31U<<15)|(31U<<10),(17U<<15)|(16U<<10));change(0x814,31U<<1,9U<<1);
    IODelay(1);change(0x828,0,1U<<30);change(0x400,1U<<11,0);
    unsigned i;for(i=0;i<10000;i++){if(read(0x400)&(1U<<7))break;IODelay(10);}if(i==10000)return false;
    if(!TegraUSB::reset(*this)){IOLog("[tegra-usb] core reset timeout\n");return false;}
    change(0x15c,1U<<3,0);change(TegraUSB::HostPort,(7U<<29)|(1U<<22),0);
    change(0x808,(1U<<14)|(1U<<16)|(1U<<18),0);change(0x838,1|(1U<<2)|(1U<<4),0);
    change(0x80c,1U<<10,0);IODelay(25);change(0x83c,1,0);change(0x410,7,1|(3U<<1));
    // Grouper's peripheral route uses B-session override, as its working
    // U-Boot gadget does. This enables the PHY session, never the VBUS supply.
    change(0x404,0,(1U<<11)|(1U<<12));
    endpointPairs=read(0x124)&31;if(endpointPairs<4)return false;
    if(endpointPairs>TegraUSB::EndpointPairs)endpointPairs=TegraUSB::EndpointPairs;
    write(TegraUSB::Mode,2|(1U<<3));write(TegraUSB::EndpointList,physical);
    write(TegraUSB::Status,0xffffffff);write(TegraUSB::Complete,0xffffffff);
    write(TegraUSB::SetupStatus,0xffff);
    return activate(0,0,64)==0 && activate(128,0,64)==0;
}
bool Tegra30USB::makeController() {
    controller=new IOUSBDeviceController;
    if(!controller || !controller->initWithTransport(this))return false;
    if(!controller->attach(this))return false;attached=true;
    if(!controller->start(this))return false;started=true;
    controller->registerService();return true;
}
bool Tegra30USB::start(IOService *p) {
    if(!IOService::start(p))return false;
    usbMap=p->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);carMap=p->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
    if(!usbMap || !carMap || usbMap->getLength()<0x1000 || carMap->getLength()<0x1000)goto failed;
    registers=(volatile UInt8 *)usbMap->getAddress();car=(volatile UInt32 *)carMap->getAddress();
    loop=IOWorkLoop::workLoop();if(!loop)goto failed;
    gate=IOCommandGate::commandGate(this);if(!gate || loop->addEventSource(gate)!=0)goto failed;
    interrupt=IOInterruptEventSource::interruptEventSource(this,irq,p,0);
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!interrupt || !timer || loop->addEventSource(interrupt)!=0 || loop->addEventSource(timer)!=0)goto failed;
    if(!allocateDMA()){IOLog("[tegra-usb] DMA allocation failed\n");goto failed;}
    if(!phy()){IOLog("[tegra-usb] PHY failed susp=%08x vbus=%08x mode=%08x\n",read(0x400),read(0x404),read(TegraUSB::Mode));goto failed;}
    if(!makeController()){IOLog("[tegra-usb] DeviceFamily start failed\n");goto failed;}
    interrupt->enable();timer->setTimeoutMS(10);registerService();
    IOLog("[tegra-usb] source DeviceFamily USB1 transport ready\n");return true;
failed:
    releaseResources();IOService::stop(p);return false;
}
bool Tegra30USB::halt() {
    if(!registers)return true;
    write(TegraUSB::Interrupt,0);change(TegraUSB::Command,TegraUSB::Run,0);
    if(TegraUSB::reset(*this)){running=false;return true;}
    // A module reset stops the bus master even if the core cannot acknowledge.
    if(car){cw(0x300,1U<<22);IODelay(10);if(cr(4)&(1U<<22)){running=false;return true;}}
    return false;
}
void Tegra30USB::finish(NexusUSB::Request *r,IOReturn status) {
    r->status=status;if(r->memory)r->memory->complete(kIODirectionNone);
    controller->returnRequest(r);
}
void Tegra30USB::cancelEndpoint(unsigned ep,IOReturn status) {
    Endpoint &e=endpoints[TegraUSB::channel(ep)];
    if(e.active && !TegraUSB::flush(*this,ep)){fail();return;}
    e.canceling=true;NexusUSB::Request *requests=e.head;e.head=e.tail=0;e.active=e.receiver=false;
    while(requests){NexusUSB::Request *next=requests->next;requests->next=0;finish(requests,status);requests=next;}e.canceling=false;
}
void Tegra30USB::cancelAll(IOReturn status) {
    for(unsigned ch=0;ch<TegraUSB::Channels;ch++)cancelEndpoint((ch/2)|((ch&1)?128:0),status);
}
void Tegra30USB::fail() {
    if(faulted)return;faulted=true;desired=false;
    if(!halt()){IOLog("[tegra-usb] reset failed, retaining DMA ownership\n");return;}
    for(unsigned ch=0;ch<TegraUSB::Channels;ch++)endpoints[ch].active=false;
    cancelAll(kIOReturnIOError);IOLog("[tegra-usb] transport stopped after controller failure\n");
}
IOReturn Tegra30USB::activate(unsigned ep,unsigned type,unsigned packet) {
    if(!TegraUSB::valid(ep) || (ep&15)>=endpointPairs || type==1 || type>3 || !packet || packet>1024 ||
       (!(ep&15) && (type || packet!=64)))return kIOReturnUnsupported;
    unsigned ch=TegraUSB::channel(ep),shift=(ep&128)?16:0;Endpoint &e=endpoints[ch];
    if(e.active || e.head)cancelEndpoint(ep,kIOReturnAborted);if(faulted)return kIOReturnIOError;
    e.packet=packet;e.enabled=true;
    heads[ch].capability=(packet<<16)|(1U<<29)|((ep&15)?0:(1U<<15));
    heads[ch].next=1;heads[ch].token=0;
    change(TegraUSB::Control+4*(ep&15),0xffffU<<shift,((type<<2)|(1U<<7)|(1U<<6))<<shift);
    return kIOReturnSuccess;
}
bool Tegra30USB::prime(unsigned ep,unsigned length) {
    unsigned ch=TegraUSB::channel(ep);Endpoint &e=endpoints[ch];
    if(!TegraUSB::descriptor(transfers[ch],physical+4096+ch*TegraUSB::Chunk,length))return false;
    heads[ch].next=physical+2048+ch*32;heads[ch].token=0;TegraUSB::barrier();
    e.programmed=length;e.active=true;write(TegraUSB::Prime,TegraUSB::bit(ep));
    if(!TegraUSB::waitClear(*this,TegraUSB::Prime,TegraUSB::bit(ep))){fail();return false;}
    return true;
}
void Tegra30USB::kick(unsigned ep) {
    unsigned ch=TegraUSB::channel(ep);Endpoint &e=endpoints[ch];
    if(!running || faulted || !e.enabled || e.active || !e.head || isHalted(ep))return;
    NexusUSB::Request *r=e.head;unsigned n=r->length-r->actual;if(n>TegraUSB::Chunk)n=TegraUSB::Chunk;
    if(!r->actual)e.deadline=r->timeout?usbNow()+uint64_t(r->timeout)*1000000:0;
    if((ep&128) && n && r->memory->readBytes(r->actual,buffers+ch*TegraUSB::Chunk,n)!=n) {
        e.head=r->next;if(!e.head)e.tail=0;finish(r,kIOReturnVMError);kick(ep);return;
    }
    prime(ep,n);
}
IOReturn Tegra30USB::submit(NexusUSB::Request *r) {
    if(!r || !TegraUSB::valid(r->endpoint) || (r->length && !r->memory) ||
       (r->memory && r->length>r->memory->getLength()))return kIOReturnBadArgument;
    if(faulted || stopping || !running)return kIOReturnNotReady;
    Endpoint &e=endpoints[TegraUSB::channel(r->endpoint)];if(!e.enabled || e.canceling)return kIOReturnNotReady;
    if(r->memory && r->memory->prepare(kIODirectionNone)!=0)return kIOReturnVMError;
    r->next=0;r->actual=0;r->status=kIOReturnNotReady;
    if(!r->timeout && (r->endpoint&128))r->timeout=5000;
    if(e.tail)e.tail->next=r;else e.head=r;e.tail=r;kick(r->endpoint);return kIOReturnSuccess;
}
void Tegra30USB::receiveControl() {
    Endpoint &e=endpoints[0];if(!running || faulted || e.active || e.head)return;
    e.receiver=true;prime(0,64);
}
void Tegra30USB::complete(unsigned ep) {
    unsigned ch=TegraUSB::channel(ep);Endpoint &e=endpoints[ch];if(!e.active)return;
    TegraUSB::barrier();unsigned actual=0;bool ok=TegraUSB::completed(transfers[ch].token,e.programmed,actual);
    if((ep&15) && e.completions++<4)IOLog("[tegra-usb] endpoint %02x complete=%u/%u token=%08x\n",ep,actual,e.programmed,transfers[ch].token);
    e.active=false;
    if(e.receiver){e.receiver=false;if(ok)control(buffers,false,actual);else{haltEndpoint(0,true);haltEndpoint(128,true);}return;}
    NexusUSB::Request *r=e.head;if(!r)return;
    if(ok && !(ep&128) && actual && r->memory->writeBytes(r->actual,buffers+ch*TegraUSB::Chunk,actual)!=actual)ok=false;
    r->actual+=actual;
    if(ok && actual==e.programmed && r->actual<r->length){kick(ep);return;}
    // Terminate a USBMux frame even when its last data packet fills the max packet.
    if(ok && r->terminate && (ep&128) && e.programmed && !(r->actual%e.packet)){r->terminate=false;prime(ep,0);return;}
    e.head=r->next;if(!e.head)e.tail=0;r->next=0;finish(r,ok?kIOReturnSuccess:kIOReturnIOError);
    kick(ep);if(!(ep&15))receiveControl();
}
void Tegra30USB::connection() {
    // A_VBUS_VLD (bit 26) is not the board's peripheral cable detector. Do
    // not gate the gadget on a host-mode comparator; PORTSC reports bus events.
    if(running && !desired) {
        running=false;reconnectAfter=usbNow()+50000000ULL;change(TegraUSB::Command,TegraUSB::Run,0);cancelAll(kIOReturnNotAttached);
        if(controller && started)familyEvent(NexusUSB::BusReset);
    }
    if(desired && !running && !faulted && !stopping && usbNow()>=reconnectAfter) {
        write(TegraUSB::Interrupt,TegraUSB::USBInterrupt|TegraUSB::USBError|TegraUSB::PortChange|TegraUSB::BusReset|TegraUSB::Suspend);
        running=true;change(TegraUSB::Command,0,TegraUSB::Run);receiveControl();
        IOLog("[tegra-usb] device pullup enabled cmd=%08x port=%08x otg=%08x\n",read(TegraUSB::Command),read(TegraUSB::Port),read(0x1f4));
    }
}
void Tegra30USB::service() {
    if(stopping || faulted)return;
    UInt32 status=read(TegraUSB::Status);write(TegraUSB::Status,status);
    connection();if(!running)return;
    if(status&TegraUSB::BusReset) {
        IOLog("[tegra-usb] bus reset speed=%d port=%08x\n",busSpeed(),read(TegraUSB::Port));
        cancelAll(kIOReturnAborted);if(faulted)return;
        for(unsigned ep=1;ep<endpointPairs;ep++){write(TegraUSB::Control+4*ep,0);endpoints[ep*2].enabled=endpoints[ep*2+1].enabled=false;}
        write(TegraUSB::Complete,0xffffffff);write(TegraUSB::Address,0);
        familyEvent(NexusUSB::BusReset);activate(0,0,64);activate(128,0,64);receiveControl();
    }
    if(status&TegraUSB::PortChange) {
        bool now=(read(TegraUSB::Port)&(1U<<7))!=0;
        if(now!=suspended){suspended=now;familyEvent(now?NexusUSB::BusSuspend:NexusUSB::BusResume);}
    } else if((status&TegraUSB::Suspend) && !suspended){suspended=true;familyEvent(NexusUSB::BusSuspend);}
    UInt32 done=read(TegraUSB::Complete);write(TegraUSB::Complete,done);
    // A fresh SETUP supersedes both pending control directions before callbacks.
    if(read(TegraUSB::SetupStatus)&1) {
        UInt8 packet[8];if(!TegraUSB::setup(*this,heads[0],packet)){fail();return;}
        cancelEndpoint(0,kIOReturnAborted);cancelEndpoint(128,kIOReturnAborted);done&=~0x10001U;
        haltEndpoint(0,false);haltEndpoint(128,false);
        IOLog("[tegra-usb] SETUP %02x/%02x value=%04x index=%04x length=%u\n",packet[0],packet[1],packet[2]|(packet[3]<<8),packet[4]|(packet[5]<<8),packet[6]|(packet[7]<<8));
        control(packet,true,8);
    }
    for(unsigned ch=0;ch<TegraUSB::Channels;ch++) {
        unsigned ep=(ch/2)|((ch&1)?128:0);if(done&TegraUSB::bit(ep))complete(ep);
    }
}
void Tegra30USB::tick(OSObject *object,IOTimerEventSource *source) {
    Tegra30USB *d=static_cast<Tegra30USB *>(object);if(d->stopping || d->faulted)return;
    d->service();uint64_t now=usbNow();
    for(unsigned ch=0;ch<TegraUSB::Channels;ch++) {
        Endpoint &e=d->endpoints[ch];if(e.head && e.deadline && now>=e.deadline)d->cancelEndpoint((ch/2)|((ch&1)?128:0),kIOReturnTimeout);
    }
    if(d->controller && d->running)d->controller->poll();
    source->setTimeoutMS(10);
}
IOReturn Tegra30USB::action(OSObject *object,void *op,void *a,void *b,void *c) {
    Tegra30USB *d=static_cast<Tegra30USB *>(object);unsigned ep=(uintptr_t)a;
    if(d->faulted || d->stopping)return kIOReturnNotReady;
    if((uintptr_t)op==OnBus){d->desired=ep!=0;d->connection();return kIOReturnSuccess;}
    if((uintptr_t)op==Submit)return d->submit((NexusUSB::Request *)a);
    if((uintptr_t)op==Address){if(ep>127)return kIOReturnBadArgument;TegraUSB::address(*d,ep);return kIOReturnSuccess;}
    if(!TegraUSB::valid(ep))return kIOReturnBadArgument;
    unsigned shift=(ep&128)?16:0;Endpoint &e=d->endpoints[TegraUSB::channel(ep)];
    switch((uintptr_t)op) {
    case Stall:
        d->change(TegraUSB::Control+4*(ep&15),1U<<shift,((uintptr_t)b?1U:1U<<6)<<shift);
        if(!b)d->kick(ep);break;
    case Abort:d->cancelEndpoint(ep,(IOReturn)(uintptr_t)b);break;
    case Activate:return d->activate(ep,(uintptr_t)b,(uintptr_t)c);
    case Deactivate:
        e.enabled=false;d->cancelEndpoint(ep,kIOReturnAborted);d->change(TegraUSB::Control+4*(ep&15),0xffffU<<shift,0);break;
    default:return kIOReturnUnsupported;
    }
    return d->faulted?kIOReturnIOError:kIOReturnSuccess;
}
bool Tegra30USB::quiesce() {
    stopping=true;desired=false;
    if(timer)timer->cancelTimeout();if(interrupt)interrupt->disable();
    bool safe=halt();
    if(safe){for(unsigned ch=0;ch<TegraUSB::Channels;ch++)endpoints[ch].active=false;if(controller)cancelAll(kIOReturnAborted);}
    if(controller && started)controller->busEvent(NexusUSB::BusReset);
    return safe;
}
void Tegra30USB::releaseResources() {
    // Wait for the workloop gate before touching queues or DMA ownership.
    bool safe=gate && gate->getWorkLoop()?gate->runAction(shutdown)==kIOReturnSuccess:quiesce();
    if(controller){if(started)controller->stop(this);if(attached)controller->detach(this);controller->release();controller=0;started=attached=false;}
    if(loop){if(timer)loop->removeEventSource(timer);if(interrupt)loop->removeEventSource(interrupt);if(gate)loop->removeEventSource(gate);}
    if(timer){timer->release();timer=0;}if(interrupt){interrupt->release();interrupt=0;}if(gate){gate->release();gate=0;}
    if(loop){loop->release();loop=0;}
    if(dma){if(safe){if(dmaPrepared)dma->complete();dma->release();}dma=0;dmaPrepared=false;}
    // A failed hardware reset deliberately retains pinned DMA rather than reuse live pages.
    if(usbMap){usbMap->release();usbMap=0;}if(carMap){carMap->release();carMap=0;}registers=0;car=0;
}
void Tegra30USB::stop(IOService *p) {releaseResources();IOService::stop(p);}
void Tegra30USB::free() {releaseResources();IOService::free();}
