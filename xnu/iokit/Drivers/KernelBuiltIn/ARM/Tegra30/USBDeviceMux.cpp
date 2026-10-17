/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded USBMux v1 TCP proxy, using XNU's socket KPI and loopback services.
 * All methods and completions execute under the controller's workloop gate.
 */
#include "USBDeviceMux.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <netinet/in.h>
OSDefineMetaClassAndStructors(NexusUSBMux,OSObject)
static uint64_t muxNow(){AbsoluteTime t;uint64_t ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;}
bool NexusUSBMux::initWithController(IOUSBDeviceController *c){
    if(!OSObject::init())return false;controller=c;input=output=0;
    configured=reading=writing=negotiated=false;received=first=count=0;
    bzero(connections,sizeof(connections));bzero(queue,sizeof(queue));
    input=IOBufferMemoryDescriptor::withCapacity(NexusUSB::MuxMaximum,kIODirectionInOut);
    output=IOBufferMemoryDescriptor::withCapacity(NexusUSB::MuxMaximum,kIODirectionInOut);
    return input && output;
}
void NexusUSBMux::close(Connection &c){if(c.socket)sock_close(c.socket);bzero(&c,sizeof(c));}
void NexusUSBMux::free(){for(unsigned i=0;i<NexusUSB::MuxConnections;i++)close(connections[i]);if(input)input->release();if(output)output->release();OSObject::free();}
void NexusUSBMux::setConfigured(bool on){
    configured=false;
    controller->hardware()->cancel(1,kIOReturnAborted);controller->hardware()->cancel(129,kIOReturnAborted);
    for(unsigned i=0;i<NexusUSB::MuxConnections;i++)close(connections[i]);
    received=first=count=0;negotiated=false;configured=on;
    if(on)read();
}
NexusUSBMux::Frame *NexusUSBMux::frame(){return count<NexusUSB::MuxQueue?&queue[(first+count)%NexusUSB::MuxQueue]:0;}
void NexusUSBMux::commit(unsigned length){frame()->length=length;count++;}
bool NexusUSBMux::send(Connection &c,unsigned flags,const void *p,unsigned length){
    Frame *f=frame();if(!f)return false;
    unsigned n=NexusUSB::muxTCP(f->bytes,c.port,c.host,c.tx,c.rx,flags,p,length,c.pending?0:NexusUSB::MuxMaximum);
    if(!n)return false;
    if((flags&2 || length) && c.trace++<4)IOLog("[tegra-usb] Mux tx port=%u flags=%02x seq=%u ack=%u bytes=%u\n",c.port,flags,c.tx,c.rx,length);
    commit(n);c.tx+=length+((flags&3)?1:0);c.deadline=muxNow()+30000000000ULL;return true;
}
bool NexusUSBMux::resetPeer(const NexusUSB::MuxPacket &p){
    Frame *f=frame();if(!f)return false;
    unsigned n=NexusUSB::muxTCP(f->bytes,p.destination,p.source,(p.flags&16)?p.acknowledgment:0,
        p.sequence+p.payloadLength+((p.flags&3)?1:0),(p.flags&16)?4:20,0,0);commit(n);return true;
}
void NexusUSBMux::read(){
    if(!configured || reading || received>NexusUSB::MuxMaximum || count==NexusUSB::MuxQueue)return;
    NexusUSB::Request *r=(NexusUSB::Request *)IOMalloc(sizeof(*r));if(!r)return;bzero(r,sizeof(*r));
    r->memory=input;input->retain();r->endpoint=1;r->length=NexusUSB::MuxMaximum;r->target=this;r->completion=readDone;
    reading=true;retain();if(controller->submit(r)){reading=false;release();r->memory->release();IOFree(r,sizeof(*r));}
}
void NexusUSBMux::write(){
    if(!configured || writing || !count)return;
    Frame &f=queue[first];output->writeBytes(0,f.bytes,f.length);
    NexusUSB::Request *r=(NexusUSB::Request *)IOMalloc(sizeof(*r));if(!r)return;bzero(r,sizeof(*r));
    r->memory=output;output->retain();r->endpoint=129;r->length=f.length;r->terminate=true;r->target=this;r->completion=writeDone;
    writing=true;retain();if(controller->submit(r)){writing=false;release();r->memory->release();IOFree(r,sizeof(*r));}
}
void NexusUSBMux::readDone(void *target,void *,IOReturn status,unsigned actual){
    NexusUSBMux *m=(NexusUSBMux *)target;m->reading=false;
    if(m->configured && !status && actual && actual<=sizeof(m->stream)-m->received){
        m->input->readBytes(0,m->stream+m->received,actual);m->received+=actual;m->consume();
    }else if(m->configured && !status && !actual)m->read();
    else if(m->configured && status!=kIOReturnAborted)m->setConfigured(false);
    m->release();
}
void NexusUSBMux::writeDone(void *target,void *,IOReturn status,unsigned){
    NexusUSBMux *m=(NexusUSBMux *)target;m->writing=false;
    if(m->configured && !status && m->count){m->first=(m->first+1)%NexusUSB::MuxQueue;m->count--;m->write();m->consume();}
    else if(m->configured && status!=kIOReturnAborted)m->setConfigured(false);
    m->release();
}
void NexusUSBMux::consume(){
    while(configured && received){
        NexusUSB::MuxPacket p;int result=p.decode(stream,received);
        if(result<0){setConfigured(false);return;}if(!result || !packet(p))break;
        received-=p.length;memmove(stream,stream+p.length,received);
    }
    write();read();
}
bool NexusUSBMux::packet(const NexusUSB::MuxPacket &p){
    if(p.protocol==0){
        if(!frame())return false;
        for(unsigned i=0;i<NexusUSB::MuxConnections;i++)close(connections[i]);
        unsigned major=NexusUSB::be32(p.payload);if(major!=1 && major!=2){setConfigured(false);return false;}
        unsigned n=NexusUSB::muxVersion(frame()->bytes);negotiated=true;commit(n);return true;
    }
    if(!negotiated){setConfigured(false);return false;}
    Connection *c=0,*vacant=0;
    for(unsigned i=0;i<NexusUSB::MuxConnections;i++){
        if(connections[i].state && connections[i].host==p.source && connections[i].port==p.destination)c=&connections[i];
        if(!connections[i].state)vacant=&connections[i];
    }
    if(p.flags&4){if(c)close(*c);return true;}
    if((p.flags&2) && !c){
        if(!frame())return false;
        if(!vacant || !p.destination || p.payloadLength)return resetPeer(p);
        c=vacant;c->host=p.source;c->port=p.destination;c->rx=p.sequence+1;c->window=p.window;c->state=1;c->deadline=muxNow()+5000000000ULL;
        IOLog("[tegra-usb] Mux opening loopback port=%u\n",c->port);
        if(sock_socket(PF_INET,SOCK_STREAM,IPPROTO_TCP,0,0,&c->socket)){close(*c);return resetPeer(p);}
        sockaddr_in address;bzero(&address,sizeof(address));address.sin_len=sizeof(address);address.sin_family=AF_INET;
        address.sin_port=htons(c->port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        int error=sock_connect(c->socket,(sockaddr *)&address,MSG_DONTWAIT);
        IOLog("[tegra-usb] Mux connect port=%u status=%d\n",c->port,error);
        if(error && error!=EINPROGRESS){close(*c);return resetPeer(p);}
        c->state=error?1:2;return true;
    }
    if(!c)return resetPeer(p);
    if(c->state<3)return true;
    if((p.flags&16) && NexusUSB::sequenceBetween(p.acknowledgment,c->acked,c->tx))c->acked=p.acknowledgment;
    c->window=p.window;
    if(c->state==4){if(c->acked==c->tx)close(*c);return true;}
    if(c->pending)return false;
    if(p.sequence!=c->rx)return send(*c,16);
    if(p.payloadLength){
        if(c->trace++<4)IOLog("[tegra-usb] Mux rx port=%u seq=%u bytes=%u\n",c->port,p.sequence,p.payloadLength);
        if(p.payloadLength>sizeof(c->data))return resetPeer(p);
        memcpy(c->data,p.payload,p.payloadLength);c->pending=p.payloadLength;c->offset=0;
        // Hold FIN until the preceding bytes have reached the service.
        if(p.flags&1)c->state=5;return true;
    }
    if(p.flags&1){c->rx++;if(!send(*c,17)){c->rx--;return false;}
        c->state=4;sock_shutdown(c->socket,SHUT_WR);return true;}
    c->deadline=muxNow()+30000000000ULL;return true;
}
void NexusUSBMux::poll(){
    if(!configured)return;
    uint64_t now=muxNow();
    for(unsigned i=0;i<NexusUSB::MuxConnections;i++){
        Connection &c=connections[i];if(!c.state)continue;
        if(now>=c.deadline){IOLog("[tegra-usb] Mux timeout port=%u state=%u\n",c.port,c.state);if(send(c,4))close(c);continue;}
        if(c.state==1){int error=0;int size=sizeof(error);
            int status=sock_getsockopt(c.socket,SOL_SOCKET,SO_ERROR,&error,&size);
            if(status || error){IOLog("[tegra-usb] Mux connect failed port=%u status=%d error=%d\n",c.port,status,error);if(send(c,20))close(c);continue;}
            sockaddr_in peer;int sizePeer=sizeof(peer);
            if(sock_getpeername(c.socket,(sockaddr *)&peer,sizePeer))continue;c.state=2;
            IOLog("[tegra-usb] Mux connected port=%u\n",c.port);
        }
        if(c.state==2){if(send(c,18))c.state=3;continue;}
        if(c.pending){
            iovec vector={c.data+c.offset,c.pending-c.offset};msghdr message;bzero(&message,sizeof(message));message.msg_iov=&vector;message.msg_iovlen=1;
            size_t sent=0;int error=sock_send(c.socket,&message,MSG_DONTWAIT,&sent);
            if(sent>vector.iov_len || (error && error!=EWOULDBLOCK)){if(send(c,4))close(c);continue;}
            c.offset+=sent;
            if(c.offset==c.pending && frame()){
                c.rx+=c.pending;c.pending=c.offset=0;
                if(c.state==5){c.rx++;send(c,17);c.state=4;sock_shutdown(c.socket,SHUT_WR);}else send(c,16);
            }
            continue;
        }
        if(c.state!=3 || c.acked!=c.tx || !c.window || !frame())continue;
        unsigned maximum=c.window;if(maximum>NexusUSB::MuxMaximum-28)maximum=NexusUSB::MuxMaximum-28;
        iovec vector={c.data,maximum};msghdr message;bzero(&message,sizeof(message));message.msg_iov=&vector;message.msg_iovlen=1;
        size_t actual=0;int error=sock_receive(c.socket,&message,MSG_DONTWAIT,&actual);
        // Apple's USBMux TCP subset uses ACK for data; usbmuxd rejects PSH.
        if(actual)send(c,16,c.data,actual);
        else if(!error){if(send(c,17))c.state=4;}
        else if(error!=EWOULDBLOCK){if(send(c,4))close(c);}
    }
    write();consume();
}
