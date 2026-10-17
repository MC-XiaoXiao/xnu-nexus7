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

/* Embedded touch/button subset of Apple IOHIDFamily-421.20.2's service contract.
 * Event objects, serialization, queues and user client come from Apple source.
 * https://github.com/apple-oss-distributions/IOHIDFamily/tree/IOHIDFamily-421.20.2
 * Original APSL notices are preserved in each source file.
 */
#ifndef NEXUS7_APPLE_HID_EVENT_SERVICE_H
#define NEXUS7_APPLE_HID_EVENT_SERVICE_H
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include "IOHIDEvent.h"
class IOHIDEventService : public IOService {
    OSDeclareDefaultStructors(IOHIDEventService)
    enum { MaximumClients=8 };
    struct Client { IOService *service; void *context; void *action; } clients[MaximumClients];
    IORecursiveLock *eventLock;
    IOWorkLoop *eventLoop;
    IOHIDEvent *lastEvent;
protected:
    virtual bool handleIsOpen(const IOService *client) const;
public:
    typedef void (*Action)(OSObject *, IOHIDEventService *, void *, IOHIDEvent *, IOOptionBits);
    virtual bool init(OSDictionary *dictionary=0);
    virtual bool start(IOService *provider);
    virtual void free();
    virtual IOWorkLoop *getWorkLoop() const { return eventLoop; }
    virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
                                  OSDictionary *properties, IOUserClient **handler);
    virtual bool open(IOService *client, IOOptionBits options, void *context, Action action);
    virtual void close(IOService *client, IOOptionBits options=0);
    virtual void dispatchEvent(IOHIDEvent *event, IOOptionBits options=0);
    virtual IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options);
    virtual void setElementValue(UInt32, UInt32, UInt32) {}
};
#endif
