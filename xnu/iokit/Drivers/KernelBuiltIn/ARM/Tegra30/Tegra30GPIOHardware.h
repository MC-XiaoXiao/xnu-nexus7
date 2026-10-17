/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef TEGRA30_GPIO_HARDWARE_H
#define TEGRA30_GPIO_HARDWARE_H
namespace NexusGPIO {
enum { Pins=256 };
inline unsigned port(unsigned pin) {return (pin/32)*0x100+((pin%32)/8)*4;}
inline unsigned bit(unsigned pin) {return 1U<<(pin%8);}
// These electrical operations use the client's GPIO-provider adapter (bank 2).
// Protocol code only supplies pin numbers and logical levels.
template<class IO> unsigned save(IO &io,unsigned pin) {
    unsigned p=port(pin),b=bit(pin);
    return (io.read(2,p)&b?1:0)|(io.read(2,p+0x10)&b?2:0)|(io.read(2,p+0x20)&b?4:0);
}
template<class IO> void function(IO &io,unsigned pin,bool gpio) {
    unsigned b=bit(pin);io.write(2,port(pin)+0x80,(b<<8)|(gpio?b:0));
}
template<class IO> void openDrain(IO &io,unsigned pin,bool driveLow) {
    unsigned b=bit(pin);io.write(2,port(pin)+0x90,(b<<8)|(driveLow?b:0));
}
template<class IO> void prepareOpenDrain(IO &io,unsigned pin) {
    openDrain(io,pin,false);io.write(2,port(pin)+0xa0,bit(pin)<<8);function(io,pin,true);
}
template<class IO> bool input(IO &io,unsigned pin) {return (io.read(2,port(pin)+0x30)&bit(pin))!=0;}
template<class IO> void restore(IO &io,unsigned pin,unsigned state) {
    unsigned p=port(pin),b=bit(pin);openDrain(io,pin,false);
    io.write(2,p+0xa0,(b<<8)|(state&4?b:0));function(io,pin,(state&1)!=0);
    io.write(2,p+0x90,(b<<8)|(state&2?b:0));
}
template<class IO> void output(IO &io,unsigned pin,bool value) {
    unsigned p=port(pin),b=bit(pin);
    io.write(2,p+0xa0,(b<<8)|(value?b:0)); // establish latch before driving
    function(io,pin,true);io.write(2,p+0x90,(b<<8)|b);
}
// Ownership is independent of the function using the pins. A shared bus uses
// one owner token, with one lease per client; different owners cannot overlap.
class Leases {
    const void *owners[Pins];unsigned references[Pins];unsigned char saved[Pins];
public:
    Leases() {for (unsigned n=0;n<Pins;++n) {owners[n]=0;references[n]=0;saved[n]=0;}}
    template<class IO> bool claim(IO &io,const void *owner,const unsigned *pins,unsigned count) {
        if (!owner || !pins || !count || count>Pins) return false;
        for (unsigned i=0;i<count;++i) {
            unsigned n=pins[i];if (n>=Pins || (owners[n] && owners[n]!=owner) || references[n]==~0U) return false;
            for (unsigned j=0;j<i;++j) if (pins[j]==n) return false;
        }
        for (unsigned i=0;i<count;++i) {
            unsigned n=pins[i],p=port(n),b=bit(n);
            if (!references[n]) {
                saved[n]=(io.read32(p)&b?1:0)|(io.read32(p+0x10)&b?2:0)|(io.read32(p+0x20)&b?4:0);
                owners[n]=owner;
            }
            ++references[n];
        }
        return true;
    }
    template<class IO> void release(IO &io,const void *owner,const unsigned *pins,unsigned count) {
        if (!owner || !pins || count>Pins) return;
        for (unsigned i=0;i<count;++i)
            for (unsigned j=0;j<i;++j) if (pins[i]==pins[j]) return;
        for (unsigned i=0;i<count;++i) {
            unsigned n=pins[i];if (n>=Pins || owners[n]!=owner || !references[n]) continue;
            if (--references[n]) continue;
            unsigned p=port(n),b=bit(n),v=saved[n];
            io.write32(p+0x90,b<<8); // release before restoring the output latch
            io.write32(p+0xa0,(b<<8)|(v&4?b:0));
            io.write32(p+0x80,(b<<8)|(v&1?b:0));
            io.write32(p+0x90,(b<<8)|(v&2?b:0));
            owners[n]=0;
        }
    }
    bool allows(const void *owner,unsigned offset,unsigned value) const {
        if (!owner || offset>=0x800 || (offset&3) || (value&~0xffffU)) return false;
        unsigned reg=offset&0xf0;
        if (reg!=0x80 && reg!=0x90 && reg!=0xa0) return false;
        unsigned mask=value>>8,base=(offset/0x100)*32+((offset&15)/4)*8;
        if (!mask || (value&255&~mask)) return false;
        for (unsigned i=0;i<8;++i) if ((mask&(1U<<i)) && owners[base+i]!=owner) return false;
        return true;
    }
};
}
#endif
