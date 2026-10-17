/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
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
#include <IOKit/IOLib.h>

#include "IOHIDEvent.h"
#include "IOHIDEventData.h"
#include "IOHIDUsageTables.h"

#define EXTERNAL ((unsigned int) -1)

#define super OSObject

OSDefineMetaClassAndStructors(IOHIDEvent, OSObject)



//==============================================================================
// IOHIDEvent::initWithCapacity
//==============================================================================
bool IOHIDEvent::initWithCapacity(IOByteCount capacity)
{
    if (!super::init())
        return false;

    if (_data && (!capacity || _capacity < capacity) ) {
        // clean out old data's storage if it isn't big enough
        IOFree(_data, _capacity);
        _data = 0;
    }

    _capacity = capacity;

    if ( !_capacity )
        return false;

    if ( !_data && !(_data = (IOHIDEventData *) IOMalloc(_capacity)))
        return false;

    bzero(_data, _capacity);
    _data->size = _capacity;
    _children = NULL;

    return true;
}

//==============================================================================
// IOHIDEvent::initWithType
//==============================================================================
bool IOHIDEvent::initWithType(IOHIDEventType type)
{
    size_t capacity = 0;

    IOHIDEventGetSize(type,capacity);

    if ( !initWithCapacity(capacity) )
        return false;

    _data->type = type;
    _typeMask   = IOHIDEventTypeMask(type);

    return true;
}

//==============================================================================
// IOHIDEvent::initWithTypeTimeStamp
//==============================================================================
bool IOHIDEvent::initWithTypeTimeStamp(IOHIDEventType type, AbsoluteTime timeStamp, IOOptionBits options)
{
    if ( !initWithType(type))
        return false;

    _timeStamp  = timeStamp;
    _options    = options;

    return true;
}

//==============================================================================
// IOHIDEvent::free
//==============================================================================
void IOHIDEvent::free()
{
    if (_capacity != EXTERNAL && _data && _capacity) {
        IOFree(_data, _capacity);
        _data = NULL;
        _capacity = 0;
    }

    if ( _children ) {
        // We should probably iterate over each child to clear the parent
        _children->release();
        _children = NULL;
    }
    super::free();
}

//==============================================================================
// IOHIDEvent::getTimeStamp
//==============================================================================
AbsoluteTime IOHIDEvent::getTimeStamp()
{
    return _timeStamp;
}

//==============================================================================
// IOHIDEvent::setTimeStamp
//==============================================================================
void IOHIDEvent::setTimeStamp( AbsoluteTime timeStamp )
{
    _timeStamp = timeStamp;
}

//==============================================================================
// IOHIDEvent::withType
//==============================================================================
IOHIDEvent * IOHIDEvent::withType(      IOHIDEventType          type,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithType(type)) {
        me->release();
        return 0;
    }

    me->_options = options;

    return me;
}

//==============================================================================
// IOHIDEvent::withEventData
//==============================================================================
IOHIDEvent * IOHIDEvent::keyboardEvent( AbsoluteTime            timeStamp,
                                        UInt32                  usagePage,
                                        UInt32                  usage,
                                        Boolean                 down,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeKeyboard, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDKeyboardEventData * keyboard = (IOHIDKeyboardEventData *)me->_data;
    keyboard->usagePage = usagePage;
    keyboard->usage = usage;
    keyboard->down = down;

    return me;
}

//==============================================================================
// IOHIDEvent::_axisEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::_axisEvent(    IOHIDEventType          type,
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 x,
                                        IOFixed                 y,
                                        IOFixed                 z,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(type, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDAxisEventData * event = (IOHIDAxisEventData *)me->_data;
    event->position.x = x;
    event->position.y = y;
    event->position.z = z;

    return me;
}

//==============================================================================
// IOHIDEvent::translationEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::translationEvent(
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 x,
                                        IOFixed                 y,
                                        IOFixed                 z,
                                        IOOptionBits            options)
{
    return IOHIDEvent::_axisEvent(      kIOHIDEventTypeTranslation,
                                        timeStamp,
                                        x,
                                        y,
                                        z,
                                        options);
}

//==============================================================================
// IOHIDEvent::scrollEvet
//==============================================================================
IOHIDEvent * IOHIDEvent::scrollEvent(
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 x,
                                        IOFixed                 y,
                                        IOFixed                 z,
                                        IOOptionBits            options)
{
    return IOHIDEvent::_axisEvent(      kIOHIDEventTypeScroll,
                                        timeStamp,
                                        x,
                                        y,
                                        z,
                                        options);
}

//==============================================================================
// IOHIDEvent::zoomEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::zoomEvent(
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 x,
                                        IOFixed                 y,
                                        IOFixed                 z,
                                        IOOptionBits            options)
{
    return IOHIDEvent::_axisEvent(      kIOHIDEventTypeZoom,
                                        timeStamp,
                                        x,
                                        y,
                                        z,
                                        options);
}


//==============================================================================
// IOHIDEvent::accelerometerEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::accelerometerEvent(
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 x,
                                        IOFixed                 y,
                                        IOFixed                 z,
                                        IOHIDAccelerometerType  type,
										IOHIDAccelerometerSubType  subType,
                                        IOOptionBits            options)
{
    IOHIDEvent *                    event;
    IOHIDAccelerometerEventData *   data;

    event = IOHIDEvent::_axisEvent( kIOHIDEventTypeAccelerometer,
                                    timeStamp,
                                    x,
                                    y,
                                    z,
                                    options);

    if ( event ) {
        data = (IOHIDAccelerometerEventData *)event->_data;
        data->acclType = type;
		data->acclSubType = subType;
    }

    return event;
}

//==============================================================================
// IOHIDEvent::gyroEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::gyroEvent(
											AbsoluteTime            timeStamp,
											IOFixed                 x,
											IOFixed                 y,
											IOFixed                 z,
											IOHIDGyroType           type,
											IOHIDGyroSubType		subType,
											IOOptionBits            options)
{
    IOHIDEvent *                    event;
    IOHIDGyroEventData *		data;

    event = IOHIDEvent::_axisEvent( kIOHIDEventTypeGyro,
								   timeStamp,
								   x,
								   y,
								   z,
								   options);

    if ( event ) {
        data = (IOHIDGyroEventData *)event->_data;
        data->gyroType = type;
        data->gyroSubType = subType;
    }

    return event;
}

//==============================================================================
// IOHIDEvent::compassEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::compassEvent(
                                   AbsoluteTime            timeStamp,
                                   IOFixed                 x,
                                   IOFixed                 y,
                                   IOFixed                 z,
                                   IOHIDCompassType		type,
                                   IOOptionBits            options)
{
    IOHIDEvent *                event;
    IOHIDCompassEventData *		data;

    event = IOHIDEvent::_axisEvent( kIOHIDEventTypeCompass,
								   timeStamp,
								   x,
								   y,
								   z,
								   options);

    if ( event ) {
        data = (IOHIDCompassEventData *)event->_data;
        data->compassType = type;
    }

    return event;
}

//==============================================================================
// IOHIDEvent::ambientLightSensorEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::ambientLightSensorEvent(
                                        AbsoluteTime            timeStamp,
                                        UInt32                  level,
                                        UInt32                  channel0,
                                        UInt32                  channel1,
										UInt32                  channel2,
										UInt32                  channel3,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeAmbientLightSensor, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDAmbientLightSensorEventData * event = (IOHIDAmbientLightSensorEventData *)me->_data;
    event->level = level;
    event->ch0 = channel0;
    event->ch1 = channel1;
    event->ch2 = channel2;
    event->ch3 = channel3;

    return me;
}

//==============================================================================
// IOHIDEvent::proximityEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::proximityEvent (
                                        AbsoluteTime				timeStamp,
                                        IOHIDProximityDetectionMask	mask,
										UInt32						level,
                                        IOOptionBits				options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeProximity, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDProximityEventData * event = (IOHIDProximityEventData *)me->_data;
    event->detectionMask	= mask;
	event->level			= level;

    return me;
}



//==============================================================================
// IOHIDEvent::temperatureEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::temperatureEvent(
                                        AbsoluteTime            timeStamp,
                                        IOFixed                 temperature,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeTemperature, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDTemperatureEventData * event = (IOHIDTemperatureEventData *)me->_data;
    event->level = temperature;

    return me;
}

//==============================================================================
// IOHIDEvent::buttonEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::buttonEvent(
                                        AbsoluteTime            timeStamp,
                                        UInt32                  buttonMask,
                                        IOOptionBits            options)
{
    return IOHIDEvent::buttonEvent(timeStamp, buttonMask, 0, options);
}

//==============================================================================
// IOHIDEvent::buttonEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::buttonEvent(
                                        AbsoluteTime            timeStamp,
                                        UInt32                  buttonMask,
                                        IOFixed                 pressure,
                                        IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeButton, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDButtonEventData * event = (IOHIDButtonEventData *)me->_data;
    event->button.buttonMask = buttonMask;
    event->button.pressure   = pressure;

    return me;
}

//====================================================================================================
// IOHIDEvent::absolutePointerEvent
//====================================================================================================
// Digitizer factory follows Apple's IOHIDFamily-503.1.13 implementation,
// using the iOS6-compatible 421.20.2 event data fields.
IOHIDEvent *IOHIDEvent::digitizerEvent(AbsoluteTime timeStamp, UInt32 index,
    IOHIDDigitizerTransducerType type, bool inRange, UInt32 buttons,
    IOFixed x, IOFixed y, IOFixed z, IOFixed pressure, IOFixed auxiliary,
    IOFixed twist, IOOptionBits options)
{
    IOHIDEvent *me=new IOHIDEvent;
    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypeDigitizer,timeStamp,options|kIOHIDEventOptionIsAbsolute)) {
        me->release();return 0;
    }
    IOHIDDigitizerEventData *event=(IOHIDDigitizerEventData *)me->_data;
    // The 421 initializer stores options only on the queue prefix. Digitizer
    // consumers read Touch/Absolute/Collection from each serialized record.
    event->options=options|kIOHIDEventOptionIsAbsolute;
    if (inRange) event->options|=kIOHIDTransducerRange;
    event->transducerIndex=index;event->transducerType=type;event->buttonMask=buttons;
    event->position.x=x;event->position.y=y;event->position.z=z;
    event->tipPressure=pressure;event->barrelPressure=auxiliary;event->twist=twist;
    if (type==kIOHIDDigitizerTransducerTypeFinger) {
        event->identity=2;event->orientationType=kIOHIDDigitizerOrientationTypeQuality;
        event->orientation.quality.majorRadius=event->orientation.quality.minorRadius=6<<16;
    }
    return me;
}

IOHIDEvent * IOHIDEvent::powerEvent(
                                   AbsoluteTime            timeStamp,
                                   IOFixed                 measurement,
                                   IOHIDPowerType          powerType,
                                   IOHIDPowerSubType       powerSubType,
                                   IOOptionBits            options)
{
    IOHIDEvent *me = new IOHIDEvent;

    if (!me) return 0;
    if (!me->initWithTypeTimeStamp(kIOHIDEventTypePower, timeStamp, options)) {
        me->release();
        return 0;
    }

    IOHIDPowerEventData *event = (IOHIDPowerEventData *)me->_data;

    event->measurement = measurement;
    event->powerType = powerType;
    event->powerSubType = powerSubType;

    return me;
}


//==============================================================================
// IOHIDEvent::appendChild
//==============================================================================
void IOHIDEvent::appendChild(IOHIDEvent *childEvent)
{
    if (!childEvent || childEvent==this) return;
    childEvent->_data->depth=_data->depth+1;
    if (!_children) {
        const OSObject *events[] = { childEvent };

        _children = OSArray::withObjects(events, 1);
    } else {
        _children->setObject(childEvent);
    }

}

//==============================================================================
// IOHIDEvent::getType
//==============================================================================
IOHIDEventType IOHIDEvent::getType()
{
    return _data->type;
}

//==============================================================================
// IOHIDEvent::setType
//==============================================================================
void IOHIDEvent::setType(IOHIDEventType type)
{
    initWithType(type);
}

//==============================================================================
// IOHIDEvent::getEvent
//==============================================================================
IOHIDEvent * IOHIDEvent::getEvent(      IOHIDEventType          type,
                                        IOOptionBits            options __unused)
{
    return (_data->type == type) ? this : NULL;
}

//==============================================================================
// IOHIDEvent::getIntegerValue
//==============================================================================
SInt32 IOHIDEvent::getIntegerValue(     IOHIDEventField         key,
                                        IOOptionBits            options)
{
    SInt32 value = 0;

    GET_EVENT_VALUE(this, key, value, options);

    return value;
}

//==============================================================================
// IOHIDEvent::getFixedValue
//==============================================================================
IOFixed IOHIDEvent::getFixedValue(      IOHIDEventField         key,
                                        IOOptionBits            options)
{
    IOFixed value = 0;

    GET_EVENT_VALUE_FIXED(this, key, value, options);

    return value;
}

//==============================================================================
// IOHIDEvent::setIntegerValue
//==============================================================================
void IOHIDEvent::setIntegerValue(       IOHIDEventField         key,
                                        SInt32                  value,
                                        IOOptionBits            options)
{
    SET_EVENT_VALUE(this, key, value, options);
}

//==============================================================================
// IOHIDEvent::setFixedValue
//==============================================================================
void IOHIDEvent::setFixedValue(         IOHIDEventField         key,
                                        IOFixed                 value,
                                        IOOptionBits            options)
{
    SET_EVENT_VALUE_FIXED(this, key, value, options);
}


//==============================================================================
// IOHIDEvent::getLength
//==============================================================================
size_t IOHIDEvent::getLength()
{
    UInt32 count=0;
    IOByteCount length=getLength(&count);
    _eventCount=count;
    return length;
}

//==============================================================================
// IOHIDEvent::getLength
//==============================================================================
IOByteCount IOHIDEvent::getLength(UInt32 * count)
{
    IOByteCount length = _data->size;

    length += sizeof(IOHIDSystemQueueElement);

    if ( _children )
    {
        UInt32          i, childCount;
        IOHIDEvent *    child;

        childCount = _children->getCount();

        for(i=0 ;i<childCount; i++) {
            if ( (child = (IOHIDEvent *)_children->getObject(i)) ) {
                length += child->getLength(count) - sizeof(IOHIDSystemQueueElement);
            }
        }
    }

    if ( count )
        *count = *count + 1;

    return length;
}

//==============================================================================
// IOHIDEvent::appendBytes
//==============================================================================
IOByteCount IOHIDEvent::appendBytes(UInt8 * bytes, IOByteCount withLength)
{
    IOByteCount size = 0;

    size = _data->size;

    if ( size > withLength )
        return 0;

    bcopy(_data, bytes, size);

    if ( _children )
    {
        UInt32          i, childCount;
        IOHIDEvent *    child;

        childCount = _children->getCount();

        for(i=0 ;i<childCount; i++) {
            if ( (child = (IOHIDEvent *)_children->getObject(i)) ) {
                size += child->appendBytes(bytes + size, withLength - size);
            }
        }
    }

    return size;
}

//==============================================================================
// IOHIDEvent::withBytes
//==============================================================================
// Only a single keyboard record or shallow digitizer trees are accepted.
IOHIDEvent *IOHIDEvent::withBytes(const void *bytes, IOByteCount size)
{
    if (!bytes || size<sizeof(IOHIDSystemQueueElement)) return 0;
    IOHIDSystemQueueElement header;
    bcopy(bytes,&header,sizeof(header));
    if (header.eventCount==1 && size==sizeof(header)+sizeof(IOHIDKeyboardEventData)) {
        IOHIDKeyboardEventData data;bcopy((const UInt8 *)bytes+sizeof(header),&data,sizeof(data));
        if (data.type!=kIOHIDEventTypeKeyboard || data.size!=sizeof(data) || data.depth ||
            (data.down!=0 && data.down!=1)) return 0;
        IOHIDEvent *event=withType(kIOHIDEventTypeKeyboard);
        if (!event) return 0;
        bcopy(&data,event->_data,sizeof(data));
        bcopy(&header.timeStamp,&event->_timeStamp,sizeof(header.timeStamp));event->_options=header.options;
        return event;
    }
    if (!header.eventCount || header.eventCount>11 ||
        size!=sizeof(header)+header.eventCount*sizeof(IOHIDDigitizerEventData)) return 0;
    IOHIDEvent *parent=0;
    const UInt8 *cursor=(const UInt8 *)bytes+sizeof(header);
    for (unsigned i=0;i<header.eventCount;++i,cursor+=sizeof(IOHIDDigitizerEventData)) {
        IOHIDDigitizerEventData data;bcopy(cursor,&data,sizeof(data));
        if (data.type!=kIOHIDEventTypeDigitizer || data.size!=sizeof(data) || data.depth!=(i?1:0)) {
            if (parent) parent->release();return 0;
        }
        IOHIDEvent *event=withType(kIOHIDEventTypeDigitizer);
        if (!event) { if (parent) parent->release();return 0; }
        bcopy(&data,event->_data,sizeof(data));
        bcopy(&header.timeStamp,&event->_timeStamp,sizeof(header.timeStamp));event->_options=header.options;
        if (!parent) parent=event;
        else { parent->appendChild(event);event->release(); }
    }
    return parent;
}

//==============================================================================
// IOHIDEvent::readBytes
//==============================================================================
IOByteCount IOHIDEvent::readBytes(void *bytes, IOByteCount withLength)
{
    if (!bytes || withLength<getLength()) return 0;
    // The queue payload is only 4-byte aligned on ARM32. Use a local header
    // so no uint64_t access assumes 8-byte alignment in shared memory.
    IOHIDSystemQueueElement header;
    bcopy(&_timeStamp,&header.timeStamp,sizeof(header.timeStamp));
    header.options=_options;header.eventCount=_eventCount;
    bcopy(&header,bytes,sizeof(header));
    return appendBytes((UInt8 *)bytes+sizeof(header),withLength-sizeof(header));
}

//==============================================================================
// IOHIDEvent::getPhase
//==============================================================================
IOHIDEventPhaseBits IOHIDEvent::getPhase()
{
    return (_data->options >> kIOHIDEventEventOptionPhaseShift) & kIOHIDEventEventPhaseMask;
}

//==============================================================================
// IOHIDEvent::setPhase
//==============================================================================
void IOHIDEvent::setPhase(IOHIDEventPhaseBits phase)
{
    _data->options &= ~(kIOHIDEventEventPhaseMask << kIOHIDEventEventOptionPhaseShift);
    _data->options |= ((phase & kIOHIDEventEventPhaseMask) << kIOHIDEventEventOptionPhaseShift);
}

//==============================================================================
// IOHIDEvent::setDeviceID
//==============================================================================
void IOHIDEvent::setDeviceID(uint64_t deviceID)
{
    _data->deviceID = deviceID;
}
