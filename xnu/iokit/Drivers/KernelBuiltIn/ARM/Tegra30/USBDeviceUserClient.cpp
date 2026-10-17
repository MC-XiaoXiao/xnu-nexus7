/* SPDX-License-Identifier: BSD-2-Clause */
#include "USBDeviceUserClient.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSString.h>
OSDefineMetaClassAndStructors(IOUSBDeviceInterfaceUserClient,IOUserClient)

struct IOUSBDeviceInterfaceUserClient::Transfer {
    Buffer *buffer;OSAsyncReference64 reference;
    bool asynchronous,finished;IOReturn status;unsigned actual;
};
bool IOUSBDeviceInterfaceUserClient::initWithTask(task_t t,void *token,UInt32 type,OSDictionary *p){
    if(type || !IOUserClient::initWithTask(t,token,type,p))return false;
    task=t;interface=0;gate=0;loop=0;nextBuffer=nextPipe=1;
    opened=closing=classRegistered=false;descriptorClass=6;descriptorSubclass=descriptorProtocol=1;
    bzero(buffers,sizeof(buffers));bzero(pipes,sizeof(pipes));bzero(classReference,sizeof(classReference));return true;
}
bool IOUSBDeviceInterfaceUserClient::start(IOService *p){
    IOUSBDeviceInterface *provider=OSDynamicCast(IOUSBDeviceInterface,p);
    if(!provider || !IOUserClient::start(p))return false;
    interface=provider;interface->retain();
    loop=interface->getWorkLoop();if(!loop)return false;loop->retain();
    gate=IOCommandGate::commandGate(this);
    if(!gate || loop->addEventSource(gate))return false;
    return true;
}
IOUSBDeviceInterfaceUserClient::Buffer *IOUSBDeviceInterfaceUserClient::buffer(unsigned id){
    for(unsigned i=0;i<8;i++)if(buffers[i].memory && buffers[i].id==id)return &buffers[i];return 0;
}
IOUSBDeviceInterfaceUserClient::Pipe *IOUSBDeviceInterfaceUserClient::pipe(unsigned id){
    if(!id)return 0;
    for(unsigned i=0;i<3;i++)if(pipes[i].id==id)return &pipes[i];return 0;
}
void IOUSBDeviceInterfaceUserClient::releaseBuffers(){
    for(unsigned i=0;i<8;i++)if(buffers[i].memory && !buffers[i].busy){
        if(buffers[i].map)buffers[i].map->release();buffers[i].memory->release();bzero(&buffers[i],sizeof(Buffer));
    }
}
IOReturn IOUSBDeviceInterfaceUserClient::closeGate(OSObject *o,void *,void *,void *,void *){
    IOUSBDeviceInterfaceUserClient *c=static_cast<IOUSBDeviceInterfaceUserClient *>(o);
    c->closing=true;c->opened=false;
    if(c->interface && c->interface->controller() && c->interface->controller()->available()){
        c->interface->controller()->classResponse(false,0,0,kIOReturnAborted);
        for(unsigned i=0;i<3;i++)if(c->pipes[i].id)c->interface->controller()->hardware()->cancel(c->pipes[i].endpoint,kIOReturnAborted);
    }
    if(c->interface)c->interface->unclaim(c);
    if(c->classRegistered){releaseAsyncReference64(c->classReference);c->classRegistered=false;}
    c->releaseBuffers();bzero(c->pipes,sizeof(c->pipes));return kIOReturnSuccess;
}
IOReturn IOUSBDeviceInterfaceUserClient::clientClose(){
    if(gate)gate->runAction(closeGate);
    terminate();return kIOReturnSuccess;
}
void IOUSBDeviceInterfaceUserClient::free(){
    if(gate){gate->runAction(closeGate);if(gate->getWorkLoop())gate->getWorkLoop()->removeEventSource(gate);gate->release();gate=0;}
    if(loop){loop->release();loop=0;}releaseBuffers();
    if(interface){interface->release();interface=0;}IOUserClient::free();
}
IOReturn IOUSBDeviceInterfaceUserClient::dispatch(OSObject *o,void *selector,IOExternalMethodArguments *a){
    IOUSBDeviceInterfaceUserClient *c=static_cast<IOUSBDeviceInterfaceUserClient *>(o);
    return c->gate?c->gate->runAction(gated,selector,a):kIOReturnNotReady;
}
IOReturn IOUSBDeviceInterfaceUserClient::gated(OSObject *o,void *selector,void *a,void *,void *){
    return static_cast<IOUSBDeviceInterfaceUserClient *>(o)->method((uintptr_t)selector,static_cast<IOExternalMethodArguments *>(a));
}
IOReturn IOUSBDeviceInterfaceUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *){
    // iOS 6 DeviceLib v2's selector layout. Variable scalar selector 9 is checked below.
    static const unsigned counts[22][4]={
        {1,0,0,0},{0,0,0,0},{0,~0U,0,0},{2,0,0,0},{2,0,0,0},{2,0,0,0},
        {2,~0U,0,0},{2,~0U,0,0},{2,0,0,0},{~0U,0,0,0},{6,0,1,0},
        {0,0,0,0},{1,0,0,0},{3,0,1,0},{3,0,1,0},{1,0,0,0},{1,0,0,0},
        {1,0,1,0},{1,0,3,0},{1,0,0,0},{0,0,2,0},{2,0,1,0}
    };
    IOReturn result=kIOReturnUnsupported;
    if(a && selector<22){
        IOExternalMethodDispatch d={dispatch,counts[selector][0],counts[selector][1],counts[selector][2],counts[selector][3]};
        result=IOUserClient::externalMethod(selector,a,&d,this,(void *)(uintptr_t)selector);
    }
    // A successful asynchronous method owns exactly one wake-port reference.
    if(result && a && a->asyncReferenceCount && a->asyncReference){
        OSAsyncReference64 ref;bzero(ref,sizeof(ref));
        unsigned n=a->asyncReferenceCount;if(n>kOSAsyncRef64Count)n=kOSAsyncRef64Count;
        memcpy(ref,a->asyncReference,n*sizeof(io_user_reference_t));releaseAsyncReference64(ref);
    }
    if(result)IOLog("[tegra-usb] DeviceLib selector=%u failed=%08x scalars=%u/%u bytes=%u\n",selector,result,a?a->scalarInputCount:0,a?a->scalarOutputCount:0,a?a->structureInputSize:0);
    return result;
}
void IOUSBDeviceInterfaceUserClient::done(void *target,void *parameter,IOReturn status,unsigned actual){
    IOUSBDeviceInterfaceUserClient *c=static_cast<IOUSBDeviceInterfaceUserClient *>(target);Transfer *t=(Transfer *)parameter;
    t->buffer->busy--;t->status=status;t->actual=actual;t->finished=true;
    if(t->asynchronous){
        io_user_reference_t bytes=actual;sendAsyncResult64(t->reference,status,&bytes,1);
        releaseAsyncReference64(t->reference);IOFree(t,sizeof(*t));
    }else c->gate->commandWakeup(t);
    c->release();
}
IOReturn IOUSBDeviceInterfaceUserClient::method(unsigned selector,IOExternalMethodArguments *a){
    if(closing || !interface || !interface->controller() || !interface->controller()->available())return kIOReturnNotAttached;
    bool async=a->asyncReferenceCount!=0;
    if(async && ((selector!=8 && selector!=13 && selector!=14) || !a->asyncReference || a->asyncReferenceCount<3 || a->asyncReferenceCount>kOSAsyncRef64Count))return kIOReturnBadArgument;
    if(selector && selector!=1 && !opened)return kIOReturnNotOpen;
    const uint64_t *in=a->scalarInput;uint64_t *out=a->scalarOutput;
    IOUSBDeviceController *controller=interface->controller();NexusUSB::Transport *hw=controller->hardware();
    switch(selector){
    case 0:if(opened)return kIOReturnExclusiveAccess;if(!interface->claim(this))return kIOReturnExclusiveAccess;opened=true;return kIOReturnSuccess;
    case 1:closeGate(this,0,0,0,0);closing=false;return kIOReturnSuccess;
    case 2:{
        // DeviceLib passes UTF-8 bytes with strlen(), without a terminator.
        if(a->structureInputSize>255 || (a->structureInputSize && !a->structureInput))return kIOReturnBadArgument;
        char text[256];if(a->structureInputSize)memcpy(text,a->structureInput,a->structureInputSize);text[a->structureInputSize]=0;
        interface->setProperty("InterfaceDescription",text);return kIOReturnSuccess;}
    case 3:case 4:case 5:if(in[1] || in[0]!=(selector==3?6:1))return kIOReturnUnsupported;return kIOReturnSuccess;
    case 6:case 7:return (!in[0] && !a->structureInputSize)?kIOReturnSuccess:kIOReturnUnsupported;
    case 8:
        if(!async || in[0]!=1 || in[1]==1)return kIOReturnBadArgument;
        if(classRegistered)releaseAsyncReference64(classReference);
        bzero(classReference,sizeof(classReference));memcpy(classReference,a->asyncReference,a->asyncReferenceCount*sizeof(io_user_reference_t));classRegistered=true;return kIOReturnSuccess;
    case 9:{if(a->scalarInputCount!=4 || in[1]>4096 || in[2]>0xffffffffULL)return kIOReturnBadArgument;
        Buffer *b=in[2]?buffer(in[2]):0;if(in[2] && (!b || b->busy || in[1]>b->memory->getLength()))return kIOReturnBadArgument;
        return controller->classResponse(in[0]!=0,in[1],b?b->memory:0,(IOReturn)in[3]);}
    case 10:{
        // Type, direction (0 OUT, 1 IN), max packet, interval, options, alternate.
        if(in[0]>3 || in[1]>1 || in[5] || (in[0]!=2 && in[0]!=3) || (in[0]==3 && !in[1]))return kIOReturnUnsupported;
        unsigned ep=in[0]==3?131:in[1]?130:2;
        for(unsigned i=0;i<3;i++)if(!pipes[i].id){pipes[i].id=nextPipe++;pipes[i].endpoint=ep;pipes[i].type=in[0];
            pipes[i].packet=in[0]==3?16:512;pipes[i].interval=in[0]==3?6:0;out[0]=pipes[i].id;return kIOReturnSuccess;}
        return kIOReturnNoResources;
    }
    case 11:return controller->finalizeInterface();
    case 12:return in[0]?kIOReturnUnsupported:kIOReturnSuccess;
    case 13:case 14:{
        if(in[0]>0xffffffffULL || in[1]>0xffffffffULL || in[2]>1024*1024)return kIOReturnBadArgument;
        Pipe *p=pipe(in[0]);Buffer *b=buffer(in[1]);
        if(!p || !b || b->busy || in[2]>b->memory->getLength() || bool(p->endpoint&128)!=(selector==14))return kIOReturnBadArgument;
        if(!controller->isConfigured())return kIOReturnNotReady;
        Transfer *t=(Transfer *)IOMalloc(sizeof(*t));NexusUSB::Request *r=(NexusUSB::Request *)IOMalloc(sizeof(*r));
        if(!t || !r){if(t)IOFree(t,sizeof(*t));if(r)IOFree(r,sizeof(*r));return kIOReturnNoMemory;}
        bzero(t,sizeof(*t));bzero(r,sizeof(*r));t->buffer=b;t->asynchronous=async;
        if(async)memcpy(t->reference,a->asyncReference,a->asyncReferenceCount*sizeof(io_user_reference_t));
        r->memory=b->memory;r->memory->retain();r->endpoint=p->endpoint;r->length=in[2];
        // ptpd leaves its command reader pending while the host is idle.
        r->timeout=selector==14?30000:0;
        r->target=this;r->parameter=t;r->completion=done;b->busy++;retain();IOReturn result=controller->submit(r);
        if(result){b->busy--;release();r->memory->release();IOFree(r,sizeof(*r));IOFree(t,sizeof(*t));return result;}
        out[0]=0;if(async)return kIOReturnSuccess;
        while(!t->finished){IOReturn wake=gate->commandSleep(t,THREAD_ABORTSAFE);if(wake!=THREAD_AWAKENED && !t->finished)hw->cancel(p->endpoint,kIOReturnAborted);}
        result=t->status;out[0]=t->actual;IOFree(t,sizeof(*t));return result;
    }
    case 15:case 16:case 17:{if(in[0]>0xffffffffULL)return kIOReturnBadArgument;Pipe *p=pipe(in[0]);if(!p)return kIOReturnBadArgument;
        if(selector==15)return hw->haltEndpoint(p->endpoint,true);
        if(selector==16)return hw->cancel(p->endpoint,kIOReturnAborted);
        out[0]=p->type==3?16:hw->busSpeed()==2?512:64;return kIOReturnSuccess;}
    case 18:case 21:{
        unsigned slot;for(slot=0;slot<8 && buffers[slot].memory;slot++){}if(slot==8)return kIOReturnNoResources;
        uint64_t length=selector==18?in[0]:in[1];if(!length || length>1024*1024)return kIOReturnBadArgument;
        IOMemoryDescriptor *memory;IOMemoryMap *map=0;
        if(selector==18){if(length<4096)length=4096;
            // A pageable named object can be shared with DeviceLib. Ordinary
            // withCapacity() storage belongs to kernel_object and cannot be
            // exported to a task. USB DMA still uses the transport's bounce pages.
            IOBufferMemoryDescriptor *allocation=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut|kIOMemoryKernelUserShared|kIOMemoryPageable,length,4096);memory=allocation;
            if(allocation)bzero(allocation->getBytesNoCopy(),length);
            if(memory)map=memory->createMappingInTask(task,0,kIOMapAnywhere);if(!map){if(memory)memory->release();return kIOReturnNoMemory;}
            IOLog("[tegra-usb] PTP shared buffer=%u bytes\n",(unsigned)length);
        }else {if(in[0]>0xffffffffULL || in[0]+length>0x100000000ULL)return kIOReturnBadArgument;
            memory=IOMemoryDescriptor::withAddressRange(in[0],length,kIODirectionInOut,task);if(!memory)return kIOReturnNoMemory;}
        buffers[slot].memory=memory;buffers[slot].map=map;buffers[slot].id=nextBuffer++;buffers[slot].busy=0;
        if(selector==18){out[0]=map->getAddress();out[1]=length;out[2]=buffers[slot].id;}else out[0]=buffers[slot].id;return kIOReturnSuccess;
    }
    case 19:{if(in[0]>0xffffffffULL)return kIOReturnBadArgument;Buffer *b=buffer(in[0]);if(!b)return kIOReturnBadArgument;if(b->busy)return kIOReturnBusy;
        if(b->map)b->map->release();b->memory->release();bzero(b,sizeof(*b));return kIOReturnSuccess;}
    case 20:out[0]=interface->isActivated();out[1]=hw->busSpeed()==2;return kIOReturnSuccess;
    default:return kIOReturnUnsupported;
    }
}
bool IOUSBDeviceInterfaceUserClient::classCommand(const NexusUSB::Setup &s,const UInt8 *,unsigned){
    if(!opened || !classRegistered || closing)return false;
    io_user_reference_t args[4]={0,s.value|(uint32_t(s.type)<<24)|(uint32_t(s.request)<<16),s.length|(uint32_t(s.index)<<16),s.length};
    return sendAsyncResult64(classReference,kIOReturnSuccess,args,4)==kIOReturnSuccess;
}
