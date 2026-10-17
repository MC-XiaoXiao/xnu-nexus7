/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NEXUS_USB_MUX_PROTOCOL_H
#define NEXUS_USB_MUX_PROTOCOL_H
#include "USBDeviceCore.h"
namespace NexusUSB {
enum { MuxMaximum=16384,MuxConnections=4,MuxQueue=8 };
struct MuxPacket {
    unsigned protocol,length,header,payloadLength;
    unsigned source,destination,flags,window;uint32_t sequence,acknowledgment;
    const uint8_t *payload;
    // -1 invalid, 0 incomplete, 1 complete; USB is a byte stream.
    int decode(const uint8_t *bytes,unsigned available){
        if(available<8)return 0;protocol=be32(bytes);length=be32(bytes+4);
        if(length<8 || length>MuxMaximum)return -1;if(available<length)return 0;
        header=8;payload=bytes+8;payloadLength=length-8;
        if(protocol==0)return payloadLength==12?1:-1;
        if(protocol!=6 || payloadLength<20)return -1;
        const uint8_t *tcp=payload;unsigned h=(tcp[12]>>4)*4;
        if(h<20 || h>payloadLength)return -1;
        source=be16(tcp);destination=be16(tcp+2);sequence=be32(tcp+4);acknowledgment=be32(tcp+8);
        flags=tcp[13];window=be16(tcp+14)<<8;payloadLength-=h;payload+=h;header+=h;return 1;
    }
};
inline unsigned muxVersion(uint8_t *p){memset(p,0,20);put32(p,0);put32(p+4,20);put32(p+8,1);return 20;}
inline unsigned muxTCP(uint8_t *p,unsigned source,unsigned destination,uint32_t seq,uint32_t ack,unsigned flags,const void *bytes,unsigned length,unsigned window=MuxMaximum){
    if(length>MuxMaximum-28 || source>65535 || destination>65535)return 0;
    memset(p,0,28);put32(p,6);put32(p+4,length+28);put16(p+8,source);put16(p+10,destination);
    put32(p+12,seq);put32(p+16,ack);p[20]=0x50;p[21]=flags;put16(p+22,window>>8);
    if(length)memcpy(p+28,bytes,length);return length+28;
}
// Sequence arithmetic remains valid across the 32-bit wrap.
inline bool sequenceBetween(uint32_t v,uint32_t start,uint32_t end){return uint32_t(v-start)<=uint32_t(end-start);}
}
#endif
