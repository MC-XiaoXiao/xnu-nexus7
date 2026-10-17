/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef TEGRA30_USB_HARDWARE_H
#define TEGRA30_USB_HARDWARE_H
#include <stdint.h>

namespace TegraUSB {
enum {
    EndpointPairs=8, Channels=EndpointPairs*2, Chunk=16384,
    Command=0x130, Status=0x134, Interrupt=0x138, Address=0x144,
    EndpointList=0x148, Port=0x174, HostPort=0x1b4, Mode=0x1f8,
    SetupStatus=0x208, Prime=0x20c, Flush=0x210, EndpointStatus=0x214,
    Complete=0x218, Control=0x21c,
    Run=1, Reset=2, SetupTripwire=1<<13,
    USBInterrupt=1, USBError=2, PortChange=4, BusReset=1<<6, Suspend=1<<8,
    Active=1<<7, Halted=1<<6, BufferError=1<<5, TransactionError=1<<3,
    IOC=1<<15, Terminate=1
};
struct QueueHead {
    uint32_t capability,current,next,token,pages[5],reserved;
    uint8_t setup[8];
    uint32_t padding[4];
};
struct Transfer {uint32_t next,token,pages[5],reserved;};
typedef char QueueHeadSize[(sizeof(QueueHead)==64)?1:-1];
typedef char TransferSize[(sizeof(Transfer)==32)?1:-1];
inline unsigned channel(unsigned ep) {return (ep&15)*2+((ep>>7)&1);}
inline uint32_t bit(unsigned ep) {return 1U<<((ep&15)+((ep&128)?16:0));}
inline bool valid(unsigned ep) {return (ep&~0x8fU)==0 && (ep&15)<EndpointPairs;}
inline bool descriptor(volatile Transfer &td,uint32_t physical,unsigned length) {
    if(length>Chunk || uint64_t(physical)+length>0x100000000ULL)return false;
    td.next=Terminate;td.token=(length<<16)|IOC|Active;
    td.pages[0]=physical;
    for(unsigned i=1;i<5;i++)td.pages[i]=(physical&~4095U)+i*4096;
    td.reserved=0;return true;
}
inline bool completed(uint32_t token,unsigned programmed,unsigned &actual) {
    unsigned remaining=(token>>16)&0x7fff;
    if((token&(Active|Halted|BufferError|TransactionError)) || remaining>programmed)return false;
    actual=programmed-remaining;return true;
}
// MMIO and DMA publication need a real barrier on this non-coherent ARM SoC.
inline void barrier() {
#ifdef __arm__
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

// Clock backend uses CAR offsets. Tegra30 PLLU does not have a reliable LOCK
// indication: upstream clk-tegra30.c omits TEGRA_PLL_USE_LOCK for this PLL and
// waits 1000 us. The PHY's USB_PHY_CLK_VALID is the actual readiness check.
template<class IO> bool enablePLLU(IO &io) {
    const uint32_t enable=1U<<30,detect=1U<<22;
    io.write(0xcc,io.read(0xcc)|detect);
    if(!(io.read(0xc0)&enable)) {
        uint32_t misc=io.read(0xcc);
        io.write(0xcc,(misc&~((15U<<8)|(15U<<4)))|(12U<<8)|(1U<<4)|detect);
        io.write(0xc0,enable|(960U<<8)|12);
    }
    io.delay(1000);
    return (io.read(0xc0)&enable)!=0;
}

// Backend supplies MMIO and microsecond delay; the same code is checked offline.
template<class IO> bool waitClear(IO &io,unsigned reg,uint32_t mask,unsigned us=10000) {
    for(unsigned i=0;i<us;i++) {if(!(io.read(reg)&mask))return true;io.delay(1);}
    return false;
}
template<class IO> bool flush(IO &io,unsigned ep) {
    const uint32_t mask=bit(ep);
    // A prime can race a flush. Repeat until both PRIME and STAT are clear.
    for(unsigned attempt=0;attempt<4;attempt++) {
        io.write(Flush,mask);
        if(!waitClear(io,Flush,mask))return false;
        if(!((io.read(Prime)|io.read(EndpointStatus))&mask))return true;
    }
    return false;
}
template<class IO> bool reset(IO &io) {
    io.write(Interrupt,0);io.write(Command,io.read(Command)&~Run);
    io.write(Command,Reset);
    return waitClear(io,Command,Reset);
}
template<class IO> void address(IO &io,unsigned value) {
    // CI latches the new address after the EP0 IN status packet, not at SETUP.
    io.write(Address,((value&127)<<25)|(1U<<24));
}
template<class IO> bool setup(IO &io,volatile QueueHead &qh,uint8_t packet[8]) {
    for(unsigned i=0;i<16;i++) {
        io.write(Command,io.read(Command)|SetupTripwire);
        barrier();for(unsigned j=0;j<8;j++)packet[j]=qh.setup[j];barrier();
        bool stable=(io.read(Command)&SetupTripwire)!=0;
        io.write(Command,io.read(Command)&~SetupTripwire);
        if(stable) {io.write(SetupStatus,1);return true;}
    }
    return false;
}
}
#endif
