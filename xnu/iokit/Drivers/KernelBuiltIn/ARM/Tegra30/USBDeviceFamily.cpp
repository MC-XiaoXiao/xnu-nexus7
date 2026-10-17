/* SPDX-License-Identifier: BSD-2-Clause */
#include "USBDeviceFamily.h"
#include "USBDeviceUserClient.h"
#include "USBDeviceMux.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <kern/clock.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <IOKit/IODeviceTreeSupport.h>
OSDefineMetaClassAndStructors(IOUSBDeviceController,IOService)
OSDefineMetaClassAndStructors(IOUSBDeviceInterface,IOService)

static OSDictionary *defaultDescription(const NexusUSB::Device &device){
    OSDictionary *description=OSDictionary::withCapacity(7),*config=OSDictionary::withCapacity(2);
    OSArray *configs=OSArray::withCapacity(1),*functions=OSArray::withCapacity(2);
    bool valid=description && config && configs && functions;
    const char *keys[]={"vendorID","productID","deviceID","manufacturerString","productString","serialNumber"};
    const unsigned ids[]={device.vendor,device.product,device.revision};
    const char *strings[]={device.manufacturer,device.name,device.serial};
    for(unsigned i=0;valid && i<6;i++){
        OSObject *value=i<3?(OSObject *)OSNumber::withNumber(ids[i],16):(OSObject *)OSString::withCString(strings[i-3]);
        if(!value){valid=false;break;}description->setObject(keys[i],value);value->release();
    }
    for(unsigned i=0;valid && i<2;i++){
        OSString *name=OSString::withCString(i?"PTP":"AppleUSBMux");
        if(!name){valid=false;break;}functions->setObject(name);name->release();
    }
    if(valid){config->setObject("Interfaces",functions);configs->setObject(config);description->setObject("ConfigurationDescriptors",configs);}
    if(config)config->release();if(configs)configs->release();if(functions)functions->release();
    if(!valid && description){description->release();description=0;}return description;
}

bool IOUSBDeviceController::initWithTransport(NexusUSB::Transport *t){
    if(!IOService::init())return false;
    interfaceFinalized=false;stopped=true;transport=t;gate=0;classMemory=0;classDeadline=activationDeadline=0;controlAccepted=false;controlBuffer=0;interfaces[0]=interfaces[1]=0;mux=0;
    controlReceived=0;classPending=forcedOff=false;return true;
}
bool IOUSBDeviceController::start(IOService *p){
    if(!IOService::start(p))return false;
    gate=IOCommandGate::commandGate(this);
    if(!gate || getWorkLoop()->addEventSource(gate))goto failed;
    controlBuffer=IOBufferMemoryDescriptor::withCapacity(4096,kIODirectionInOut);
    if(!controlBuffer)goto failed;
    stopped=false;setName("IOUSBDeviceController");
    // Firmware identity is provisioned in the device tree. Keep usbmuxd's
    // identifier equal to the UDID computed by the original MobileGestalt.
    {IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/usb-device",gIODTPlane);
        OSData *serial=node?OSDynamicCast(OSData,node->getProperty("USBSerialNumber")):0;
        if(serial && serial->getLength()==41){
            const char *text=(const char *)serial->getBytesNoCopy();bool valid=text[40]==0;
            for(unsigned i=0;i<40;i++)if(!((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f')))valid=false;
            if(valid)memcpy(device.serial,text,41);
        }
        if(node)node->release();}
    // This one-configuration profile already exists in iOS 6's original
    // USBDeviceConfiguration.plist, so configd can use its normal API.
    setProperty("ConfigurationType","unknownHardware");
    setProperty("USBDeviceImplementation","Nexus7 source");
    {OSDictionary *description=defaultDescription(device);if(!description)goto failed;
        setProperty("DefaultDeviceDescription",description);setProperty("DeviceDescription",description);description->release();}
    for(unsigned i=0;i<2;i++){
        IOUSBDeviceInterface *child=new IOUSBDeviceInterface;
        if(!child)goto failed;
        if(!child->initWithNumber(i) || !child->attach(this) || !child->start(this)){if(child->getProvider())child->detach(this);child->release();goto failed;}
        interfaces[i]=child;child->registerService();
    }
    mux=new NexusUSBMux;
    if(!mux || !mux->initWithController(this))goto failed;
    // Like DeviceFamily, expose the functions first and attach only after
    // the original PTP driver has supplied/finalized its pipes in userland.
    return true;
failed:
    stop(p);return false;
}
void IOUSBDeviceController::stop(IOService *p){
    stopped=true;transport->goOnBus(false);endClass();configure(0);
    for(unsigned i=0;i<2;i++)if(interfaces[i]){interfaces[i]->terminate(kIOServiceSynchronous);interfaces[i]->stop(this);interfaces[i]->detach(this);interfaces[i]->release();interfaces[i]=0;}
    if(mux){mux->release();mux=0;}
    if(gate){if(gate->getWorkLoop())gate->getWorkLoop()->removeEventSource(gate);gate->release();gate=0;}IOService::stop(p);
}
void IOUSBDeviceController::free(){endClass();if(controlBuffer)controlBuffer->release();IOService::free();}
IOWorkLoop *IOUSBDeviceController::getWorkLoop() const {return getProvider()?getProvider()->getWorkLoop():0;}
void IOUSBDeviceController::returnRequest(NexusUSB::Request *r){
    IOMemoryDescriptor *memory=r->memory;
    if(r->completion)r->completion(r->target,r->parameter,r->status,r->actual);
    if(memory)memory->release();IOFree(r,sizeof(*r));
}
void IOUSBDeviceController::controlDone(void *,void *,IOReturn,unsigned){}
IOReturn IOUSBDeviceController::sendControl(const void *bytes,unsigned length,bool zlp){
    if(length>4096)return kIOReturnBadArgument;
    if(length)controlBuffer->writeBytes(0,bytes,length);
    NexusUSB::Request *r=(NexusUSB::Request *)IOMalloc(sizeof(*r));if(!r)return kIOReturnNoMemory;
    bzero(r,sizeof(*r));r->endpoint=128;r->memory=controlBuffer;controlBuffer->retain();
    r->length=length;r->timeout=5000;r->completion=controlDone;
    IOReturn status=submit(r);if(status){returnRequest(r);return status;}
    if(zlp && length && !(length&63))return sendControl(0,0);
    return kIOReturnSuccess;
}
IOReturn IOUSBDeviceController::finalizeInterface(){
    if(stopped)return kIOReturnNotAttached;
    interfaceFinalized=true;
    IOLog("[tegra-usb] PTP interface finalized; enabling device connection\n");
    return transport->goOnBus(!forcedOff);
}
IOReturn IOUSBDeviceController::configure(unsigned value){
    // Set the state before callbacks, so an aborted reader cannot requeue.
    device.configuration=0;activationDeadline=0;if(mux)mux->setConfigured(false);
    if(interfaces[1])interfaces[1]->setActivated(false);
    for(unsigned ep=1;ep<=3;ep++){transport->disableEndpoint(ep);transport->disableEndpoint(ep|128);}
    if(!value)return kIOReturnSuccess;
    unsigned packet=transport->busSpeed()==2?512:64;
    if(transport->enableEndpoint(1,2,packet) || transport->enableEndpoint(129,2,packet) ||
       (device.ptp && (transport->enableEndpoint(2,2,packet) || transport->enableEndpoint(130,2,packet) || transport->enableEndpoint(131,3,16))))return kIOReturnIOError;
    device.configuration=1;if(mux)mux->setConfigured(true);
    // Hosts may select 1/0/1 while claiming Mux. Let that settle before
    // activating ptpd: an old aborted read on its other runloop would otherwise
    // mark the newly activated transport disconnected.
    if(device.ptp){AbsoluteTime t;uint64_t ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);activationDeadline=ns+100000000ULL;}
    return kIOReturnSuccess;
}
void IOUSBDeviceController::handleControl(UInt8 *bytes,bool setup,unsigned length){
    if(stopped)return;
    if(setup){
        if(length!=8)return;endClass();controlReceived=0;setupPacket.decode(bytes);
        NexusUSB::Reply reply=device.control(setupPacket,transport->busSpeed()==2);
        IOReturn status=kIOReturnSuccess;
        switch(reply.kind){
        case NexusUSB::InReply:status=sendControl(reply.bytes,reply.length,reply.length<setupPacket.length);break;
        case NexusUSB::AddressReply:status=transport->setAddress(reply.value);if(!status)status=sendControl(0,0);break;
        case NexusUSB::ConfigurationReply:status=configure(reply.value);if(!status)status=sendControl(0,0);break;
        case NexusUSB::EndpointHaltReply:status=transport->haltEndpoint(reply.value,setupPacket.request==3);if(!status)status=sendControl(0,0);break;
        case NexusUSB::StatusReply:status=sendControl(0,0);break;
        case NexusUSB::ClassReply:
            classPending=true;
            {AbsoluteTime t;uint64_t ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);classDeadline=ns+5000000000ULL;}
            if(!interfaces[1]->classCommand(setupPacket,0,0))status=kIOReturnUnsupported;
            break;
        default:status=kIOReturnUnsupported;
        }
        if(status){endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);}
    }else if(classPending && !(setupPacket.type&128) && setupPacket.length){
        if(length>setupPacket.length-controlReceived){endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);return;}
        controlBuffer->writeBytes(controlReceived,bytes,length);controlReceived+=length;
        if(length<64 && controlReceived<setupPacket.length){endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);return;}
        finishClassOut();
    }
}
void IOUSBDeviceController::endClass(){classPending=controlAccepted=false;classDeadline=0;if(classMemory){classMemory->release();classMemory=0;}}
void IOUSBDeviceController::finishClassOut(){
    if(!classPending || !controlAccepted || controlReceived!=setupPacket.length)return;
    if(controlReceived && (!classMemory || classMemory->writeBytes(0,controlBuffer->getBytesNoCopy(),controlReceived)!=controlReceived)){
        endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);return;
    }
    endClass();sendControl(0,0);
}
IOReturn IOUSBDeviceController::classResponse(bool accepted,unsigned length,IOMemoryDescriptor *memory,IOReturn result){
    if(!classPending)return kIOReturnNotReady;
    if(!accepted || result){endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);return kIOReturnSuccess;}
    if(controlAccepted)return kIOReturnNotReady;
    if(!(setupPacket.type&128)){
        if(length<setupPacket.length || (setupPacket.length && (!memory || memory->getLength()<setupPacket.length)))return kIOReturnBadArgument;
        classMemory=memory;if(memory)memory->retain();controlAccepted=true;finishClassOut();return kIOReturnSuccess;
    }
    if(length>setupPacket.length)length=setupPacket.length;
    if(length>4096 || (length && (!memory || memory->getLength()<length)))return kIOReturnBadArgument;
    if(length && memory->readBytes(0,controlBuffer->getBytesNoCopy(),length)!=length)return kIOReturnVMError;
    endClass();return sendControl(controlBuffer->getBytesNoCopy(),length,length<setupPacket.length);
}
void IOUSBDeviceController::busEvent(unsigned event){
    if(event==NexusUSB::BusReset){device.reset();endClass();controlReceived=0;activationDeadline=0;if(mux)mux->setConfigured(false);if(interfaces[1])interfaces[1]->setActivated(false);}
    else {device.suspended=event==NexusUSB::BusSuspend;IOLog("[tegra-usb] bus %s\n",device.suspended?"suspended":"resumed");}
}
void IOUSBDeviceController::poll(){
    if(activationDeadline){AbsoluteTime t;uint64_t ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);
        if(ns>=activationDeadline){activationDeadline=0;if(device.configuration && device.ptp)interfaces[1]->setActivated(true);}}
    if(classPending){AbsoluteTime t;uint64_t ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);
        if(ns>=classDeadline){endClass();transport->haltEndpoint(0,true);transport->haltEndpoint(128,true);}}
    if(mux && device.configuration && !device.suspended)mux->poll();
}

IOReturn IOUSBDeviceController::propertyAction(OSObject *o,void *p,void *,void *,void *){IOUSBDeviceController *c=static_cast<IOUSBDeviceController *>(o);return c->stopped?kIOReturnNotAttached:c->changeProperties(static_cast<OSObject *>(p));}
IOReturn IOUSBDeviceController::setProperties(OSObject *object){return !stopped && gate?gate->runAction(propertyAction,object):kIOReturnNotReady;}
IOReturn IOUSBDeviceController::changeProperties(OSObject *object){
    OSDictionary *dict=OSDynamicCast(OSDictionary,object);if(!dict)return kIOReturnBadArgument;
    OSString *command=OSDynamicCast(OSString,dict->getObject("USBDeviceCommand"));if(!command)return kIOReturnBadArgument;
    if(command->isEqualTo("ForceOffBusEnable") || command->isEqualTo("DeviceDestroy")){forcedOff=true;return transport->goOnBus(false);}
    IOLog("[tegra-usb] userland command %s\n",command->getCStringNoCopy());
    if(command->isEqualTo("ForceOffBusDisable")){forcedOff=false;return transport->goOnBus(interfaceFinalized || !device.ptp);}
    if(command->isEqualTo("GoOffAndOnBus")){transport->goOnBus(false);configure(0);endClass();device.reset();return transport->goOnBus(!forcedOff && (interfaceFinalized || !device.ptp));}
    if(command->isEqualTo("SetDevicePreferredConfiguration")){
        OSNumber *n=OSDynamicCast(OSNumber,dict->getObject("USBDeviceCommandParameter"));
        return n && n->unsigned64BitValue()<=1?kIOReturnSuccess:kIOReturnUnsupported;
    }
    if(!command->isEqualTo("SetDeviceConfiguration") && !command->isEqualTo("SetDeviceDescription"))return kIOReturnUnsupported;
    OSDictionary *description=OSDynamicCast(OSDictionary,dict->getObject("USBDeviceCommandParameter"));if(!description)return kIOReturnBadArgument;
    NexusUSB::Device next=device;
    const char *numbers[]={"vendorID","productID","deviceID"};UInt16 *values[]={&next.vendor,&next.product,&next.revision};
    for(unsigned i=0;i<3;i++)if(OSObject *object=description->getObject(numbers[i])){
        OSNumber *n=OSDynamicCast(OSNumber,object);if(!n || n->unsigned64BitValue()>65535)return kIOReturnBadArgument;*values[i]=n->unsigned16BitValue();}
    const char *strings[]={"manufacturerString","productString","serialNumber"};char *destinations[]={next.manufacturer,next.name,next.serial};
    for(unsigned i=0;i<3;i++)if(OSObject *object=description->getObject(strings[i])){
        OSString *text=OSDynamicCast(OSString,object);if(!text || text->getLength()>63)return kIOReturnBadArgument;
        const char *bytes=text->getCStringNoCopy();for(unsigned n=0;n<text->getLength();n++)if((UInt8)bytes[n]>127)return kIOReturnUnsupported;
        strlcpy(destinations[i],bytes,64);}
    // One configuration, with Mux first and optional PTP. Do not claim arbitrary profiles.
    if(OSObject *object=description->getObject("ConfigurationDescriptors")){
        OSArray *configs=OSDynamicCast(OSArray,object);if(!configs || configs->getCount()!=1)return kIOReturnUnsupported;
        OSDictionary *config=OSDynamicCast(OSDictionary,configs->getObject(0));
        OSArray *functions=config?OSDynamicCast(OSArray,config->getObject("Interfaces")):0;
        if(!functions || functions->getCount()<1 || functions->getCount()>2)return kIOReturnUnsupported;
        OSString *first=OSDynamicCast(OSString,functions->getObject(0));
        OSString *second=functions->getCount()==2?OSDynamicCast(OSString,functions->getObject(1)):0;
        if(!first || !first->isEqualTo("AppleUSBMux") || (functions->getCount()==2 && (!second || !second->isEqualTo("PTP"))))return kIOReturnUnsupported;
        next.ptp=second!=0;
    }
    setProperty("DeviceDescription",description);
    transport->goOnBus(false);configure(0);endClass();device=next;device.reset();return transport->goOnBus(!forcedOff && (interfaceFinalized || !device.ptp));
}
bool IOUSBDeviceInterface::initWithNumber(unsigned n){if(!IOService::init())return false;number=n;client=0;active=false;return true;}
bool IOUSBDeviceInterface::start(IOService *p){
    if(!IOService::start(p))return false;setName("IOUSBDeviceInterface");
    setProperty("USBDeviceFunction",number?"PTP":"AppleUSBMux");setProperty("IOUserClientClass","IOUSBDeviceInterfaceUserClient");
    OSDictionary *plugins=OSDictionary::withCapacity(1);OSString *path=OSString::withCString("IOUSBDeviceFamily.kext/PlugIns/IOUSBDeviceLib.plugin");
    if(!plugins || !path){if(plugins)plugins->release();if(path)path->release();return false;}
    plugins->setObject("9E72217E-8A60-11DB-BF57-000D936D06D2",path);setProperty("IOCFPlugInTypes",plugins);
    path->release();plugins->release();return true;
}
IOWorkLoop *IOUSBDeviceInterface::getWorkLoop() const {return getProvider()?getProvider()->getWorkLoop():0;}
IOReturn IOUSBDeviceInterface::newUserClient(task_t task,void *token,UInt32 type,OSDictionary *properties,IOUserClient **out){
    if(!out || !number)return kIOReturnUnsupported;
    IOUSBDeviceInterfaceUserClient *c=new IOUSBDeviceInterfaceUserClient;if(!c)return kIOReturnNoMemory;
    if(!c->initWithTask(task,token,type,properties) || !c->attach(this) || !c->start(this)){c->detach(this);c->release();return kIOReturnNotReady;}
    *out=c;return kIOReturnSuccess;
}
bool IOUSBDeviceInterface::claim(IOUSBDeviceInterfaceUserClient *c){if(client && client!=c)return false;client=c;return true;}
void IOUSBDeviceInterface::unclaim(IOUSBDeviceInterfaceUserClient *c){if(client==c)client=0;}
void IOUSBDeviceInterface::setActivated(bool on){
    if(active==on)return;active=on;
    // DeviceLib forwards an argument buffer to ptpd, which dereferences its
    // first word. Eight bytes keep IOKit from treating it as a scalar pointer.
    uint64_t speed=on && controller()->hardware()->busSpeed()==2;
    messageClients(on?NexusUSB::InterfaceActivated:NexusUSB::InterfaceDeactivated,
        &speed,sizeof(speed));
}
bool IOUSBDeviceInterface::classCommand(const NexusUSB::Setup &s,const UInt8 *bytes,unsigned length){return client && client->classCommand(s,bytes,length);}
