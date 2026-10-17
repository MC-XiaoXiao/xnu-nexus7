/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NEXUS7_TOUCH_EVENTS_H
#define NEXUS7_TOUCH_EVENTS_H
#include "Nexus7TouchProtocol.h"
#include "../AppleHIDTouch/IOHIDEvent.h"
#include "../AppleHIDTouch/IOHIDEventData.h"
typedef char Nexus7TouchPrefixSize[sizeof(IOHIDSystemQueueElement)==16?1:-1];
typedef char Nexus7TouchDigitizerSize[sizeof(IOHIDDigitizerEventData)==104?1:-1];
typedef char Nexus7TouchDeviceIDOffset[__builtin_offsetof(IOHIDDigitizerEventData,deviceID)==24?1:-1];
typedef char Nexus7TouchPositionOffset[__builtin_offsetof(IOHIDDigitizerEventData,position)==32?1:-1];
namespace NexusTouch {
inline IOHIDEvent *event(const Frame &before,const Frame &after,AbsoluteTime time,uint64_t device) {
    IOHIDEvent *hand=IOHIDEvent::digitizerEvent(time,0,kIOHIDDigitizerTransducerTypeHand,
        after.active!=0,0,0,0,0,0,0,0,kIOHIDEventOptionIsCollection);
    if (!hand) return 0;
    uint32_t aggregate=0;
    unsigned children=0;
    for (unsigned i=0;i<Fingers;++i) if ((before.active|after.active)&(1U<<i)) {
        bool down=(after.active&(1U<<i))!=0;
        const Contact &p=down?after.contacts[i]:before.contacts[i];
        IOHIDEvent *finger=IOHIDEvent::digitizerEvent(time,i+1,kIOHIDDigitizerTransducerTypeFinger,
            down,0,(uint32_t)p.x*65536U/RawY,(uint32_t)p.y*65536U/RawX,0,
            down?(uint32_t)p.pressure*65536U/255:0,0,0,down?kIOHIDTransducerTouch:0);
        if (!finger) { hand->release();return 0; }
        uint32_t mask=changeMask(before,after,i);aggregate|=mask;
        finger->setIntegerValue(kIOHIDEventFieldDigitizerEventMask,mask);
        finger->setIntegerValue(kIOHIDEventFieldDigitizerIdentity,i+1);
        finger->setDeviceID(device);hand->appendChild(finger);finger->release();
        ++children;
    }
    hand->setIntegerValue(kIOHIDEventFieldDigitizerEventMask,aggregate);
    hand->setIntegerValue(kIOHIDEventFieldDigitizerChildEventMask,aggregate);
    hand->setIntegerValue(kIOHIDEventFieldDigitizerTouch,after.active!=0);
    hand->setDeviceID(device);
    if (hand->getLength()!=sizeof(IOHIDSystemQueueElement)+(children+1)*sizeof(IOHIDDigitizerEventData)) {
        hand->release();return 0;
    }
    return hand;
}
}
#endif
