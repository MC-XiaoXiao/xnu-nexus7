/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NEXUS_USB_DEVICE_FAMILY_H
#define NEXUS_USB_DEVICE_FAMILY_H
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "USBDeviceCore.h"
class IOUSBDeviceInterface;
class IOUSBDeviceInterfaceUserClient;
class NexusUSBMux;
class IOCommandGate;
namespace NexusUSB {
struct Request {
    unsigned endpoint;IOMemoryDescriptor *memory;Request *next;
    IOReturn status;unsigned timeout,length,actual;bool terminate;
    void *target;void (*completion)(void *,void *,IOReturn,unsigned);void *parameter;
};
class Transport {
public:
    virtual IOReturn request(Request *)=0;
    virtual IOReturn cancel(unsigned,IOReturn)=0;
    virtual IOReturn enableEndpoint(unsigned,unsigned,unsigned)=0;
    virtual IOReturn disableEndpoint(unsigned)=0;
    virtual IOReturn haltEndpoint(unsigned,bool)=0;
    virtual bool isHalted(unsigned)=0;
    virtual IOReturn setAddress(unsigned)=0;
    virtual IOReturn goOnBus(bool)=0;
    virtual int busSpeed()=0;
};
enum { BusReset,BusSuspend,BusResume };
// iOS 6 DeviceLib's IOGeneralInterest messages.
const UInt32 InterfaceActivated=(UInt32)-469794544;
const UInt32 InterfaceDeactivated=(UInt32)-469794543;
}
class IOUSBDeviceController : public IOService {
    OSDeclareDefaultStructors(IOUSBDeviceController)
    NexusUSB::Transport *transport;
    IOCommandGate *gate;
    IOBufferMemoryDescriptor *controlBuffer;
    IOUSBDeviceInterface *interfaces[2];
    NexusUSBMux *mux;
    NexusUSB::Device device;
    NexusUSB::Setup setupPacket;
    unsigned controlReceived;
    bool classPending,forcedOff,controlAccepted,stopped,interfaceFinalized;
    IOMemoryDescriptor *classMemory;uint64_t classDeadline,activationDeadline;
    static IOReturn propertyAction(OSObject *,void *,void *,void *,void *);
    IOReturn changeProperties(OSObject *);
    void endClass();void finishClassOut();
    static void controlDone(void *,void *,IOReturn,unsigned);
    IOReturn sendControl(const void *,unsigned,bool zlp=false);
public:
    bool initWithTransport(NexusUSB::Transport *);
    virtual bool start(IOService *);
    virtual void stop(IOService *);
    virtual void free();
    virtual IOWorkLoop *getWorkLoop() const;
    virtual IOReturn setProperties(OSObject *);
    IOReturn configure(unsigned);
    IOReturn finalizeInterface();
    void handleControl(UInt8 *,bool,unsigned);
    void busEvent(unsigned);
    void poll();
    void returnRequest(NexusUSB::Request *);
    IOReturn submit(NexusUSB::Request *r){return transport->request(r);}
    NexusUSB::Transport *hardware(){return transport;}
    IOReturn classResponse(bool,unsigned,IOMemoryDescriptor *,IOReturn);
    bool available() const {return !stopped;}
    bool isConfigured() const {return !stopped && device.configuration!=0;}
    IOUSBDeviceInterface *interface(unsigned i){return i<2?interfaces[i]:0;}
};
class IOUSBDeviceInterface : public IOService {
    OSDeclareDefaultStructors(IOUSBDeviceInterface)
    IOUSBDeviceInterfaceUserClient *client;
    unsigned number;bool active;
public:
    bool initWithNumber(unsigned);
    virtual bool start(IOService *);
    virtual IOWorkLoop *getWorkLoop() const;
    virtual IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **);
    IOUSBDeviceController *controller(){return static_cast<IOUSBDeviceController *>(getProvider());}
    unsigned index() const {return number;}
    bool isActivated() const {return active;}
    bool claim(IOUSBDeviceInterfaceUserClient *);
    void unclaim(IOUSBDeviceInterfaceUserClient *);
    void setActivated(bool);
    bool classCommand(const NexusUSB::Setup &,const UInt8 *,unsigned);
};
#endif
