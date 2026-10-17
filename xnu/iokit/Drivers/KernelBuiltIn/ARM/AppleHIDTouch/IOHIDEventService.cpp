/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2009 Apple Computer, Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/* Embedded touch/button adaptation of Apple IOHIDFamily-421.20.2 callback dispatch.
 * Serialization and the shared queue use Apple IOHIDFamily-421.20.2 source.
 */
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include "IOHIDEventService.h"
#include "IOHIDEventServiceUserClient.h"
OSDefineMetaClassAndStructors(IOHIDEventService, IOService)
bool IOHIDEventService::init(OSDictionary *dictionary) {
    if (!IOService::init(dictionary)) return false;
    bzero(clients,sizeof(clients)); lastEvent=0; eventLoop=0;
    eventLock=IORecursiveLockAlloc(); return eventLock!=0;
}
bool IOHIDEventService::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    eventLoop=IOWorkLoop::workLoop();
    if (!eventLoop) return false;
    setProperty("HIDServiceSupport",true);
    setProperty("IOUserClientClass","IOHIDEventServiceUserClient");
    OSDictionary *plugins=OSDictionary::withCapacity(1);
    OSString *path=OSString::withCString("IOHIDFamily.kext/PlugIns/IOHIDLib.plugin");
    if (!plugins || !path) { if (plugins) plugins->release(); if (path) path->release(); return false; }
    plugins->setObject("7DDEECA8-A7B4-11DA-8A0E-0014519758EF",path);
    plugins->setObject("0516B563-B15B-11DA-96EB-0014519758EF",path);
    plugins->setObject("FA12FA38-6F1A-11D4-BA0C-0005028F18D5",path);
    setProperty("IOCFPlugInTypes",plugins); path->release(); plugins->release();
    return true;
}
void IOHIDEventService::free() {
    if (lastEvent) lastEvent->release();
    if (eventLoop) eventLoop->release();
    if (eventLock) IORecursiveLockFree(eventLock);
    IOService::free();
}
IOReturn IOHIDEventService::newUserClient(task_t task,void *security,UInt32 type,
                                        OSDictionary *,IOUserClient **handler) {
    if (!handler || type!=0) return kIOReturnBadArgument;
    *handler=0;
    IOHIDEventServiceUserClient *client=new IOHIDEventServiceUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task,security,type) || !client->attach(this) || !client->start(this)) {
        if (client->getProvider()) client->detach(this);
        client->release(); return kIOReturnNoResources;
    }
    *handler=client; return kIOReturnSuccess;
}
bool IOHIDEventService::open(IOService *client,IOOptionBits,void *context,Action action) {
    if (!client || !action || isInactive()) return false;
    IORecursiveLockLock(eventLock);
    unsigned index=MaximumClients;
    for (unsigned i=0;i<MaximumClients;++i) {
        if (clients[i].service==client) { index=i; break; }
        if (!clients[i].service && index==MaximumClients) index=i;
    }
    bool ok=index<MaximumClients;
    if (ok) { clients[index].service=client; clients[index].context=context; clients[index].action=(void *)action; }
    IORecursiveLockUnlock(eventLock); return ok;
}
void IOHIDEventService::close(IOService *client,IOOptionBits) {
    if (!eventLock) return;
    IORecursiveLockLock(eventLock);
    for (unsigned i=0;i<MaximumClients;++i) if (clients[i].service==client) bzero(&clients[i],sizeof(Client));
    IORecursiveLockUnlock(eventLock);
}
bool IOHIDEventService::handleIsOpen(const IOService *client) const {
    IORecursiveLockLock(eventLock); bool result=false;
    for (unsigned i=0;i<MaximumClients;++i) if (clients[i].service && (!client || clients[i].service==client)) result=true;
    IORecursiveLockUnlock(eventLock); return result;
}
void IOHIDEventService::dispatchEvent(IOHIDEvent *event,IOOptionBits options) {
    if (!event) return;
    IORecursiveLockLock(eventLock);
    event->setDeviceID(getRegistryEntryID());
    event->retain(); if (lastEvent) lastEvent->release(); lastEvent=event;
    for (unsigned i=0;i<MaximumClients;++i) if (clients[i].service) {
        Action action=(Action)clients[i].action;
        action(clients[i].service,this,clients[i].context,event,options);
    }
    IORecursiveLockUnlock(eventLock);
}
IOHIDEvent *IOHIDEventService::copyEvent(IOHIDEventType type,IOHIDEvent *matching,IOOptionBits) {
    if (matching) return 0; // Per-usage state filtering belongs to the button service.
    IORecursiveLockLock(eventLock);
    IOHIDEvent *event=lastEvent && lastEvent->getType()==type ? lastEvent : 0;
    if (event) event->retain(); IORecursiveLockUnlock(eventLock); return event;
}
