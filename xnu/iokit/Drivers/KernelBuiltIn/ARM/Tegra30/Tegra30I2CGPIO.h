/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef TEGRA30_I2C_GPIO_H
#define TEGRA30_I2C_GPIO_H
#include "Tegra30GPIOHardware.h"
namespace NexusI2CGPIO {
// Power-I2C PZ6/PZ7. Open-drain GPIO never drives either line high.
template<class IO,unsigned ClockPin=206,unsigned DataPin=207> class Bus {
    IO &io;unsigned savedSCL,savedSDA;
    enum {SCL=ClockPin,SDA=DataPin};
    void low(unsigned pin,bool drive) {NexusGPIO::openDrain(io,pin,drive);}
public:
    explicit Bus(IO &access):io(access) {
        savedSCL=NexusGPIO::save(io,SCL);savedSDA=NexusGPIO::save(io,SDA);
        NexusGPIO::prepareOpenDrain(io,SCL);NexusGPIO::prepareOpenDrain(io,SDA);
        io.delayUS(5);
    }
    ~Bus() {
        NexusGPIO::restore(io,SCL,savedSCL);NexusGPIO::restore(io,SDA,savedSDA);
    }
    void data(bool bit) {low(SDA,!bit);io.delayUS(5);}
    void clockLow() {low(SCL,true);io.delayUS(5);}
    bool clockHigh() {
        low(SCL,false);
        for (unsigned n=0;n<1000;++n) {
            if (NexusGPIO::input(io,SCL)) {io.delayUS(5);return true;}
            io.delayUS(5);
        }
        return false;
    }
    bool start() {
        data(true);if (!clockHigh() || !NexusGPIO::input(io,SDA)) return false;
        data(false);clockLow();return true;
    }
    bool stop() {
        clockLow();data(false);bool ready=clockHigh();data(true);
        return ready && NexusGPIO::input(io,SCL) && NexusGPIO::input(io,SDA);
    }
    bool send(uint8_t value) {
        for (unsigned bit=0;bit<8;++bit) {
            data((value&0x80)!=0);if (!clockHigh()) return false;clockLow();value<<=1;
        }
        data(true);if (!clockHigh()) return false;
        bool ack=!NexusGPIO::input(io,SDA);clockLow();return ack;
    }
    bool receive(uint8_t &value,bool ack) {
        value=0;data(true);
        for (unsigned bit=0;bit<8;++bit) {
            if (!clockHigh()) return false;
            value=uint8_t((value<<1)|(NexusGPIO::input(io,SDA)?1:0));clockLow();
        }
        data(!ack);if (!clockHigh()) return false;clockLow();data(true);return true;
    }
};
template<class IO> bool readRegister(IO &io,unsigned slave,uint8_t reg,uint8_t *bytes,unsigned count,unsigned *failure=0) {
    if (failure) *failure=0;
    if (slave>127 || !bytes || !count || count>2) return false;
    Bus<IO> bus(io);uint8_t result[2]={0,0};unsigned stage=1;
    bool ok=bus.start();if (ok) {stage=2;ok=bus.send(uint8_t(slave<<1));}
    if (ok) {stage=3;ok=bus.send(reg);}
    if (ok) {stage=4;ok=bus.start();}
    if (ok) {stage=5;ok=bus.send(uint8_t((slave<<1)|1));}
    if (ok) {stage=6;for (unsigned n=0;n<count && ok;++n) ok=bus.receive(result[n],n+1<count);}
    bool stopped=bus.stop();if (ok && !stopped) stage=7;
    if (!ok || !stopped) {if (failure) *failure=stage;return false;}
    for (unsigned n=0;n<count;++n) bytes[n]=result[n];return true;
}
template<class IO> bool writeRegister(IO &io,unsigned slave,uint8_t reg,uint16_t value) {
    if (slave>127) return false;
    Bus<IO> bus(io);
    bool ok=bus.start() && bus.send(uint8_t(slave<<1)) && bus.send(reg) &&
        bus.send(uint8_t(value>>8)) && bus.send(uint8_t(value));
    bool stopped=bus.stop();return ok && stopped;
}
// PMIC registers are one byte. Never send the codec's second data byte.
template<class IO> bool writeByteRegister(IO &io,unsigned slave,uint8_t reg,uint8_t value) {
    if(slave>127)return false;
    Bus<IO> bus(io);
    bool ok=bus.start() && bus.send(uint8_t(slave<<1)) && bus.send(reg) && bus.send(value);
    bool stopped=bus.stop();return ok && stopped;
}

}
#endif
