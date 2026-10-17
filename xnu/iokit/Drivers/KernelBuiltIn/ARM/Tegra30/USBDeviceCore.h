/* SPDX-License-Identifier: BSD-2-Clause
 * Small, source-built USB device protocol. No Apple executable or private ABI.
 */
#ifndef NEXUS_USB_CORE_H
#define NEXUS_USB_CORE_H
#include <stdint.h>
#include <string.h>
namespace NexusUSB {
inline unsigned le16(const uint8_t *p){return p[0]|unsigned(p[1])<<8;}
inline unsigned be16(const uint8_t *p){return unsigned(p[0])<<8|p[1];}
inline uint32_t be32(const uint8_t *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline void put16(uint8_t *p,unsigned v){p[0]=v>>8;p[1]=v;}
inline void put32(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
struct Setup {
    uint8_t type,request;uint16_t value,index,length;
    void decode(const uint8_t *p){type=p[0];request=p[1];value=le16(p+2);index=le16(p+4);length=le16(p+6);}
};
enum { StallReply,InReply,StatusReply,AddressReply,ConfigurationReply,EndpointHaltReply,ClassReply };
struct Reply {unsigned kind,length,value;uint8_t bytes[256];};
struct Device {
    unsigned configuration,address;bool ptp,halted[8],suspended;
    uint16_t vendor,product,revision;
    char manufacturer[64],name[64],serial[64];
    Device():configuration(0),address(0),ptp(true),suspended(false),vendor(0x05ac),product(0x12a5),revision(0x100){
        memset(halted,0,sizeof(halted));strcpy(manufacturer,"Apple Inc.");strcpy(name,"iPad");
        strcpy(serial,"0000000000000000000000000000000000000007");
    }
    void reset(){configuration=address=0;suspended=false;memset(halted,0,sizeof(halted));}
    bool endpoint(unsigned ep) const {return !(ep&~0x83U) && ((ep&15)==0 || (ep&15)==1 || (ptp && ((ep&15)==2 || ep==0x83)));}
    unsigned config(uint8_t *p,bool high,bool other=false) const {
        const uint8_t mux[]={9,4,0,0,2,255,254,2,0,7,5,0x81,2,0,0,0,7,5,1,2,0,0,0};
        const uint8_t camera[]={9,4,1,0,3,6,1,1,0,7,5,0x82,2,0,0,0,7,5,2,2,0,0,0,7,5,0x83,3,16,0,6};
        unsigned length=9+sizeof(mux)+(ptp?sizeof(camera):0);
        const uint8_t header[]={9,2,0,0,0,1,0,0x80,250};memcpy(p,header,9);
        p[1]=other?7:2;p[2]=length;p[3]=length>>8;p[4]=ptp?2:1;
        memcpy(p+9,mux,sizeof(mux));if(ptp)memcpy(p+9+sizeof(mux),camera,sizeof(camera));
        unsigned packet=high?512:64;
        for(unsigned offset=9;offset<length;offset+=p[offset])if(p[offset+1]==5 && p[offset+3]==2){p[offset+4]=packet;p[offset+5]=packet>>8;}
        return length;
    }
    unsigned string(unsigned index,uint8_t *p) const {
        p[1]=3;if(!index){p[0]=4;p[2]=9;p[3]=4;return 4;}
        const char *text=index==1?manufacturer:index==2?name:index==3?serial:0;if(!text)return 0;
        unsigned n=0;while(text[n] && n<63){p[2+n*2]=text[n];p[3+n*2]=0;n++;}p[0]=2+n*2;return p[0];
    }
    Reply control(const Setup &s,bool high) {
        Reply r;memset(&r,0,sizeof(r));unsigned recipient=s.type&31;
        if((s.type&0x60) && recipient==1 && configuration && ptp && s.index==1 && s.length<=4096){r.kind=ClassReply;return r;}
        if(s.type&0x60)return r;
        if(s.type==0x80 && s.request==6) {
            unsigned type=s.value>>8,index=s.value&255;
            if(type==1 && !index && !s.index){
                const uint8_t d[]={18,1,0,2,0,0,0,64,0,0,0,0,0,0,1,2,3,1};memcpy(r.bytes,d,18);
                r.bytes[8]=vendor;r.bytes[9]=vendor>>8;r.bytes[10]=product;r.bytes[11]=product>>8;
                r.bytes[12]=revision;r.bytes[13]=revision>>8;r.length=18;
            }else if((type==2 || type==7) && !index && !s.index)r.length=config(r.bytes,type==7?!high:high,type==7);
            else if(type==3 && (s.index==0 || s.index==0x409))r.length=string(index,r.bytes);
            else if(type==6 && !index && !s.index){const uint8_t q[]={10,6,0,2,0,0,0,64,1,0};memcpy(r.bytes,q,10);r.length=10;}
            if(r.length){r.kind=InReply;if(r.length>s.length)r.length=s.length;}return r;
        }
        if(s.type==0 && s.request==5 && !s.index && !s.length && s.value<128 && !configuration){r.kind=AddressReply;r.value=s.value;address=s.value;return r;}
        if(s.type==0 && s.request==9 && !s.index && !s.length && s.value<=1 && (address || !s.value)){
            r.kind=ConfigurationReply;r.value=s.value;configuration=s.value;memset(halted,0,sizeof(halted));return r;}
        if(s.type==0x80 && s.request==8 && !s.value && !s.index && s.length==1){r.kind=InReply;r.length=1;r.bytes[0]=configuration;return r;}
        if(s.request==0 && !s.value && s.length==2 && (s.type==0x80 || s.type==0x81 || s.type==0x82)){
            if(!recipient && s.index)return r;
            if(recipient==1 && (!configuration || s.index>unsigned(ptp)))return r;
            if(recipient==2){unsigned ep=s.index&15;if(!endpoint(s.index) || (ep && !configuration))return r;r.bytes[0]=halted[ep*2+((s.index>>7)&1)];}
            r.kind=InReply;r.length=2;return r;
        }
        if((s.request==1 || s.request==3) && s.type==2 && !s.value && !s.length && configuration){
            unsigned ep=s.index&15;if(!endpoint(s.index) || !ep)return r;
            halted[ep*2+((s.index>>7)&1)]=s.request==3;r.kind=EndpointHaltReply;r.value=s.index;return r;
        }
        if(s.type==0x81 && s.request==10 && configuration && !s.value && s.index<=unsigned(ptp) && s.length==1){r.kind=InReply;r.length=1;return r;}
        if(s.type==1 && s.request==11 && configuration && !s.value && s.index<=unsigned(ptp) && !s.length){r.kind=StatusReply;return r;}
        return r;
    }
};
}
#endif
