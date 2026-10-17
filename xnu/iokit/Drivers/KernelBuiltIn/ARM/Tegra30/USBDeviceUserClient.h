/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NEXUS_USB_USERCLIENT_H
#define NEXUS_USB_USERCLIENT_H
#include <IOKit/IOUserClient.h>
#include "USBDeviceFamily.h"
class IOCommandGate;
class IOUSBDeviceInterfaceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IOUSBDeviceInterfaceUserClient)
    struct Buffer {IOMemoryDescriptor *memory;IOMemoryMap *map;unsigned id,busy;} buffers[8];
    struct Pipe {unsigned id,endpoint,type,packet,interval;} pipes[3];
    IOUSBDeviceInterface *interface;
    task_t task;
    IOCommandGate *gate;
    IOWorkLoop *loop;
    OSAsyncReference64 classReference;
    unsigned nextBuffer,nextPipe;
    bool opened,closing,classRegistered;
    UInt8 descriptorClass,descriptorSubclass,descriptorProtocol;
    static IOReturn dispatch(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn closeGate(OSObject *,void *,void *,void *,void *);
    static IOReturn gated(OSObject *,void *,void *,void *,void *);
    IOReturn method(unsigned,IOExternalMethodArguments *);
    Buffer *buffer(unsigned);
    Pipe *pipe(unsigned);
    void releaseBuffers();
    struct Transfer;
    static void done(void *,void *,IOReturn,unsigned);
public:
    virtual bool initWithTask(task_t,void *,UInt32,OSDictionary *);
    virtual bool start(IOService *);
    virtual IOReturn clientClose();
    virtual void free();
    virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch * = 0,OSObject * = 0,void * = 0);
    bool classCommand(const NexusUSB::Setup &,const UInt8 *,unsigned);
};
#endif
