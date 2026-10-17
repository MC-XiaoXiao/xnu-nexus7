/* SPDX-License-Identifier: BSD-2-Clause
 * ELAN KTF3K wire protocol, independently implemented from the Grouper
 * android-tegra3-grouper-3.1-jb-mr1 driver observations. No firmware flashing.
 */
#ifndef NEXUS7_TOUCH_PROTOCOL_H
#define NEXUS7_TOUCH_PROTOCOL_H
#include <stdint.h>
namespace NexusTouch {
enum { Fingers=10, RawX=2111, RawY=1279, PowerPin=59, ReadyPin=60, ResetPin=62,
       SharedSupplyPin=177 }; // PW1: vdd_panel also supplies vdd_touch/vddio_ts.
struct Contact { uint16_t x,y; uint8_t size,pressure; };
struct Frame { uint16_t active; Contact contacts[Fingers]; };
inline bool decode(const uint8_t *bytes,unsigned length,Frame &frame) {
    if (!bytes || (length!=40 && length!=55) || bytes[0]!=0x62) return false;
    unsigned mask=bytes[1] | ((bytes[2]&0x30U)<<4),count=0;
    for (unsigned i=0;i<Fingers;++i) count+=(mask>>i)&1;
    if (count!=(bytes[2]&15)) return false;
    unsigned sum=0; for (unsigned i=0;i<34;++i) sum+=bytes[i];
    // ELAN legacy firmware does not supply a checksum for <3 fingers.
    if (count>=3 && (sum&255)!=bytes[34]) return false;
    Frame next={}; next.active=mask;
    for (unsigned i=0;i<Fingers;++i) if (mask&(1U<<i)) {
        unsigned p=3+i*3,rawX=((bytes[p]&0xf0U)<<4)|bytes[p+1];
        unsigned rawY=((bytes[p]&15U)<<8)|bytes[p+2];
        if (rawX>RawX || rawY>RawY) return false;
        // Board's ABS_MT_POSITION_X=rawY, Y=(abs_x_max-rawX).
        next.contacts[i].x=rawY; next.contacts[i].y=RawX-rawX;
        next.contacts[i].size=length==55 ? bytes[35+i] : (bytes[35+i/2]>>(i&1?0:4))&15;
        next.contacts[i].pressure=length==55 ? bytes[45+i] : next.contacts[i].size<<4;
    }
    frame=next; return true;
}
inline bool bufferHeader(const uint8_t header[4],unsigned &length,unsigned &count) {
    if (!header || (header[0]!=0x63 && header[0]!=0x66) || header[1]<1 || header[1]>3) return false;
    length=header[0]==0x63?40:55; count=header[1]; return true;
}
inline uint32_t changeMask(const Frame &before,const Frame &after,unsigned slot) {
    bool was=(before.active&(1U<<slot))!=0,now=(after.active&(1U<<slot))!=0;
    uint32_t mask=was!=now?3:0;
    if (now && (!was || before.contacts[slot].x!=after.contacts[slot].x || before.contacts[slot].y!=after.contacts[slot].y)) mask|=4;
    if (now && !was) mask|=32;
    if (now && was && (before.contacts[slot].pressure!=after.contacts[slot].pressure || before.contacts[slot].size!=after.contacts[slot].size)) mask|=64;
    return mask;
}
enum BufferResult { BufferGood,BufferInvalid,BufferLost };
template<class Transport,class Sink> BufferResult consume(Transport &transport,Sink &sink,unsigned &rejected) {
    uint8_t header[4];
    if (!transport.receiveTouch(header,4)) return BufferLost;
    if (header[0]==0x52) return BufferGood;
    unsigned length=0,count=0;
    if (!bufferHeader(header,length,count)) return BufferLost;
    bool valid=true;
    for (unsigned i=0;i<count;++i) {
        uint8_t bytes[55];Frame frame;
        if (!transport.receiveTouch(bytes,length)) return BufferLost;
        if (!decode(bytes,length,frame)) {++rejected;valid=false;}
        else if (!sink.report(frame)) valid=false;
        // Even invalid contact data consumes a complete packet. Drain the batch
        // so the next transaction starts at an ELAN header, not a finger record.
    }
    return valid?BufferGood:BufferInvalid;
}
}
#endif
