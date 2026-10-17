/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NEXUS_USB_MUX_H
#define NEXUS_USB_MUX_H
#include "USBDeviceFamily.h"
#include "USBDeviceMuxProtocol.h"
#include <sys/kpi_socket.h>
class NexusUSBMux : public OSObject {
    OSDeclareDefaultStructors(NexusUSBMux)
    struct Connection {
        socket_t socket;unsigned host,port,state,window,pending,offset,trace;
        uint32_t rx,tx,acked;uint64_t deadline;
        UInt8 data[NexusUSB::MuxMaximum];
    } connections[NexusUSB::MuxConnections];
    struct Frame {unsigned length;UInt8 bytes[NexusUSB::MuxMaximum];} queue[NexusUSB::MuxQueue];
    IOUSBDeviceController *controller;
    IOBufferMemoryDescriptor *input,*output;
    UInt8 stream[NexusUSB::MuxMaximum*2];unsigned received,first,count;
    bool configured,reading,writing,negotiated;
    static void readDone(void *,void *,IOReturn,unsigned);
    static void writeDone(void *,void *,IOReturn,unsigned);
    void read();void write();void consume();
    bool packet(const NexusUSB::MuxPacket &);
    Frame *frame();void commit(unsigned);
    bool send(Connection &,unsigned,const void * = 0,unsigned = 0);
    bool resetPeer(const NexusUSB::MuxPacket &);
    void close(Connection &);
public:
    bool initWithController(IOUSBDeviceController *);
    virtual void free();
    void setConfigured(bool);
    void poll();
};
#endif
