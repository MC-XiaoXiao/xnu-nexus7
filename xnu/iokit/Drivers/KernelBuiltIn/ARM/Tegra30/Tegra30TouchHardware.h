/* SPDX-License-Identifier: BSD-2-Clause
 * Tegra30 I2C2 packet FIFO, CAR and GPIO operations. IO is injectable so the
 * identical implementation is exercised without physical MMIO by host tests.
 */
#ifndef TEGRA30_TOUCH_HARDWARE_H
#define TEGRA30_TOUCH_HARDWARE_H
#include "Nexus7TouchProtocol.h"
#include "Tegra30I2CGPIO.h"
namespace NexusTouch {
enum { I2C=0,CAR=1,GPIO=2,Pinmux=3,TransferError=0x3c,Complete=0x80 };
inline unsigned gpioOffset(unsigned pin,unsigned reg) { return (pin/32)*0x100+((pin/8)%4)*4+reg; }
template<class IO> void gpio(IO &io,unsigned pin,unsigned reg,bool value) {
    unsigned bit=pin%8; io.write(GPIO,gpioOffset(pin,reg+0x80),(1U<<(bit+8))|(value?1U<<bit:0));
}
template<class IO> bool ready(IO &io) { return !(io.read(GPIO,gpioOffset(ReadyPin,0x30))&(1U<<(ReadyPin%8))); }
// The LCD controller owns PW1. Observe its output; never change the shared
// panel/touch rail from this driver or attempt I2C while it is powered down.
template<class IO> bool supplyPowered(IO &io) {return (io.read(GPIO,gpioOffset(SharedSupplyPin,0x20))&(1U<<(SharedSupplyPin%8)))!=0;}
template<class IO> void resetBus(IO &io) {
    io.write(CAR,0x308,1U<<22); io.delayUS(2); io.write(CAR,0x30c,1U<<22); io.delayUS(2);
    io.write(I2C,0,0x2c00);io.write(I2C,0x6c,3);io.write(I2C,0x20,6);
    io.write(I2C,0x2c,0xfc);io.write(I2C,0x30,0);io.write(I2C,0x64,0);io.write(I2C,0x68,0xff);
}
template<class IO> bool transfer(IO &io,uint8_t *bytes,unsigned count,bool read) {
    if (!bytes || !count || count>55) return false;
    io.write(I2C,0x68,0xff); io.write(I2C,0x5c,3); // TX/RX FIFO flush
    unsigned budget=1000;
    while ((io.read(I2C,0x5c)&3) && budget) { --budget;io.delayUS(10); }
    if (!budget) { resetBus(io);return false; }
    io.write(I2C,0x50,0x11010); // I2C protocol, controller 1, packet 1
    io.write(I2C,0x50,count-1);
    io.write(I2C,0x50,0x20U|(1U<<17)|(read?1U<<19:0)); // slave0x10, packet completion, STOP
    unsigned done=0; budget=1000;
    while (budget--) {
        uint32_t status=io.read(I2C,0x68);
        if (status&TransferError) { resetBus(io);return false; }
        uint32_t fifo=io.read(I2C,0x60);
        if (done<count && (read?(fifo&15):((fifo>>4)&15))) {
            unsigned n=count-done; if (n>4) n=4;
            uint32_t word=0;
            if (read) { word=io.read(I2C,0x54);for (unsigned i=0;i<n;++i) bytes[done+i]=(word>>(8*i))&255; }
            else { for (unsigned i=0;i<n;++i) word|=(uint32_t)bytes[done+i]<<(8*i);io.write(I2C,0x50,word); }
            done+=n;
        }
        if (done==count && (status&Complete)) { io.write(I2C,0x68,status);return true; }
        io.delayUS(10);
    }
    resetBus(io);return false;
}
template<class IO> bool prepare(IO &io) {
    // Verify PLLP's actual rate against OSC_CTRL before selecting its divider.
    const uint32_t crystals[16]={13000000,16800000,0,0,19200000,38400000,0,0,12000000,48000000,0,0,26000000,0,0,0};
    uint32_t pll=io.read(CAR,0xa0),ref=crystals[io.read(CAR,0x50)>>28];
    unsigned m=pll&31,n=(pll>>8)&1023,p=(pll>>20)&7;
    if (!m || !ref || !(pll&(1U<<30)) || (pll&(1U<<31)) || (uint64_t)ref*n/m/(1U<<p)!=408000000) return false;
    io.write(CAR,0x308,1U<<22);io.write(CAR,0x328,1U<<22);
    io.write(CAR,0x198,127); // Grouper I2C2:408M/128, divisor3 ->398.4375kHz
    for (unsigned i=148;i<=149;++i) {
        uint32_t old=io.read(Pinmux,i*4); io.write(Pinmux,i*4,(old&~0x7fU)|0x60U); // I2C2, input, open-drain
    }
    gpio(io,157,0,false);gpio(io,158,0,false); // PT5/PT6 SFIO
    const unsigned pads[3]={135,136,138};
    for (unsigned i=0;i<3;++i) {
        uint32_t old=io.read(Pinmux,pads[i]*4);
        io.write(Pinmux,pads[i]*4,(old&~0x3fU)|(i==1?0x30U:0));
    }
    gpio(io,ReadyPin,0x10,false);gpio(io,ReadyPin,0,true);
    gpio(io,PowerPin,0x20,true);gpio(io,PowerPin,0x10,true);gpio(io,PowerPin,0,true);
    gpio(io,ResetPin,0x20,false);gpio(io,ResetPin,0x10,true);gpio(io,ResetPin,0,true);
    resetBus(io);return true;
}
template<class IO> bool receiveGPIO(IO &io,uint8_t *bytes,unsigned count) {
    if(!bytes || !count || count>55)return false;
    // Same I2C2 pins and slave as the packet controller, with open-drain
    // clock-stretch handling and exact STOP/restoration on every outcome.
    NexusI2CGPIO::Bus<IO,157,158> bus(io);
    bool ok=bus.start() && bus.send(0x21);
    for(unsigned i=0;ok && i<count;++i)ok=bus.receive(bytes[i],i+1<count);
    const bool stopped=bus.stop();return ok && stopped;
}
template<class IO> bool handshake(IO &io,bool gpioTransport=false) {
    // Grouper's ELAN firmware needs250ms after reset and sends a four-byte
    // hello (ektf3k.c __hello_packet_handler/elan_ktf3k_ts_hw_reset).
    gpio(io,ResetPin,0x20,false);io.delayUS(1000);gpio(io,ResetPin,0x20,true);io.delayUS(250000);
    // Match the board driver's __elan_ktf3k_ts_poll: up to150 observations
    // at20ms intervals, including a settling interval after ready goes low.
    // A1ms platform timer must not shorten the firmware boot allowance that
    // the old10ms tick accidentally supplied to the earlier1ms poll loop.
    bool available=false;
    for(unsigned wait=0;wait<150;++wait) {
        available=ready(io);io.delayUS(20000);
        if(available)break;
    }
    uint8_t hello[4];
    return available && ready(io) && (gpioTransport?receiveGPIO(io,hello,sizeof(hello)):transfer(io,hello,sizeof(hello),true)) &&
        hello[0]==0x55 && hello[1]==0x55 && hello[2]==0x55 && hello[3]==0x55;
}
}
#endif
