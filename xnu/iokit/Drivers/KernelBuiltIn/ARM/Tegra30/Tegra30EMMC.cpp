/* SPDX-License-Identifier: BSD-2-Clause
 * Tegra30 SDMMC4 eMMC transport for Apple's IOStorage family.
 * Polled single-block PIO deliberately avoids DMA/cache coherency assumptions.
 * Board firmware supplies the pad mux and eMMC rail; DT supplies MMIO resources.
 */
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLib.h>

class Tegra30EMMC : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(Tegra30EMMC)
    IOMemoryMap *hostMap, *carMap;
    volatile UInt8 *host;
    volatile UInt32 *car;
    IOLock *lock;
    UInt64 sectors;
    bool online, faulted, protectedCard;
    UInt32 r32(unsigned o) { OSSynchronizeIO(); return *(volatile UInt32 *)(host+o); }
    UInt16 r16(unsigned o) { OSSynchronizeIO(); return *(volatile UInt16 *)(host+o); }
    UInt8 r8(unsigned o) { OSSynchronizeIO(); return host[o]; }
    void w32(unsigned o, UInt32 v) { *(volatile UInt32 *)(host+o)=v; OSSynchronizeIO(); }
    void w16(unsigned o, UInt16 v) { *(volatile UInt16 *)(host+o)=v; OSSynchronizeIO(); }
    void w8(unsigned o, UInt8 v) { host[o]=v; OSSynchronizeIO(); }
    IOReturn wait(unsigned offset, UInt32 mask, UInt32 value, unsigned us);
    IOReturn status(UInt32 bit);
    IOReturn command(unsigned cmd, UInt32 arg, UInt16 flags, UInt32 *response=0);
    IOReturn data(unsigned cmd, UInt32 arg, UInt8 *bytes, bool writing);
    IOReturn ready();
    IOReturn initializeCard();
    IOReturn clock(unsigned divisor);
    void releaseResources();
public:
    virtual bool init(OSDictionary *properties=0);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
                                     IOStorageAttributes *, IOStorageCompletion *);
    virtual IOReturn doSynchronizeCache();
    virtual IOReturn doEjectMedia() { return kIOReturnUnsupported; }
    virtual IOReturn doFormatMedia(UInt64) { return kIOReturnUnsupported; }
    virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const { return 0; }
    virtual IOReturn doLockUnlockMedia(bool) { return kIOReturnUnsupported; }
    virtual char *getVendorString() { static char s[]="NVIDIA"; return s; }
    virtual char *getProductString() { static char s[]="Tegra30 eMMC"; return s; }
    virtual char *getRevisionString() { static char s[]="1.0"; return s; }
    virtual char *getAdditionalDeviceInfoString() { static char s[]="SDMMC4 PIO"; return s; }
    virtual IOReturn reportBlockSize(UInt64 *v) { if (!v) return kIOReturnBadArgument; *v=512; return kIOReturnSuccess; }
    virtual IOReturn reportMaxValidBlock(UInt64 *v) { if (!v || !sectors) return kIOReturnNotReady; *v=sectors-1; return kIOReturnSuccess; }
    virtual IOReturn reportEjectability(bool *v) { if (!v) return kIOReturnBadArgument; *v=false; return kIOReturnSuccess; }
    virtual IOReturn reportLockability(bool *v) { return reportEjectability(v); }
    virtual IOReturn reportRemovability(bool *v) { return reportEjectability(v); }
    virtual IOReturn reportMediaState(bool *v, bool *changed) { if (!v || !changed) return kIOReturnBadArgument; *v=online; *changed=false; return kIOReturnSuccess; }
    virtual IOReturn reportPollRequirements(bool *v, bool *expensive) { if (!v || !expensive) return kIOReturnBadArgument; *v=*expensive=false; return kIOReturnSuccess; }
    virtual IOReturn reportWriteProtection(bool *v) { if (!v) return kIOReturnBadArgument; *v=protectedCard; return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(Tegra30EMMC, IOBlockStorageDevice)
static const UInt32 r1Errors=0xfdffa080U; // includes SWITCH_ERROR, excludes state/ready
static const UInt16 R1=0x1a, R2=0x09, R3=0x02, R1b=0x1b;

bool Tegra30EMMC::init(OSDictionary *p)
{
    if (!IOBlockStorageDevice::init(p)) return false;
    hostMap=carMap=0; host=0; car=0; lock=0; sectors=0;
    online=faulted=protectedCard=false;
    return true;
}
void Tegra30EMMC::releaseResources()
{
    if (hostMap) { hostMap->release(); hostMap=0; }
    if (carMap) { carMap->release(); carMap=0; }
    host=0; car=0;
    if (lock) { IOLockFree(lock); lock=0; }
}
void Tegra30EMMC::free() { releaseResources(); IOBlockStorageDevice::free(); }
void Tegra30EMMC::stop(IOService *p)
{
    IOLockLock(lock); online=false; IOLockUnlock(lock);
    IOBlockStorageDevice::stop(p);
}
IOReturn Tegra30EMMC::wait(unsigned o, UInt32 mask, UInt32 value, unsigned us)
{
    for (unsigned i=0;i<us;++i) { if ((r32(o)&mask)==value) return kIOReturnSuccess; IODelay(1); }
    return kIOReturnTimeout;
}
IOReturn Tegra30EMMC::status(UInt32 bit)
{
    for (unsigned i=0;i<1000000;++i) {
        UInt32 s=r32(0x30);
        if (s&0xffff8000U) {
            IOLog("[tegra-emmc] status error=0x%x\n",(unsigned)s);
            return (s&0x00110000U)?kIOReturnTimeout:kIOReturnIOError;
        }
        if (s&bit) { w32(0x30,bit); return kIOReturnSuccess; }
        IODelay(1);
    }
    return kIOReturnTimeout;
}
IOReturn Tegra30EMMC::clock(unsigned divisor)
{
    w16(0x2c,0);
    w16(0x2c,(divisor<<8)|1);
    for (unsigned i=0;i<10000;++i) {
        if (r16(0x2c)&2) { w16(0x2c,(divisor<<8)|5); return kIOReturnSuccess; }
        IODelay(1);
    }
    return kIOReturnTimeout;
}
IOReturn Tegra30EMMC::command(unsigned cmd, UInt32 arg, UInt16 flags, UInt32 *response)
{
    IOReturn ret=wait(0x24,(flags==R1b || (flags&0x20))?3:1,0,100000);
    if (ret) return ret;
    w32(0x30,0xffffffffU);
    // Tegra requires transfer mode and command in one 32-bit write.
    w32(0x08,arg);
    UInt32 mode=(flags&0x20)?(cmd==24?2:0x12):0;
    w32(0x0c,(((cmd<<8)|flags)<<16)|mode);
    ret=status(1);
    if (ret) { IOLog("[tegra-emmc] CMD%u failed 0x%x\n",cmd,ret); return ret; }
    if (response) *response=r32(0x10);
    if ((flags&3)==2 || (flags&3)==3) {
        if (cmd!=1 && (r32(0x10)&r1Errors)) {
            IOLog("[tegra-emmc] CMD%u R1=0x%x\n",cmd,(unsigned)r32(0x10));
            return kIOReturnIOError;
        }
    }
    if ((flags&3)==3) return status(2);
    return kIOReturnSuccess;
}
IOReturn Tegra30EMMC::data(unsigned cmd, UInt32 arg, UInt8 *bytes, bool writing)
{
    IOReturn ret=wait(0x24,3,0,100000); if (ret) return ret;
    w16(0x04,512); w16(0x06,1);
    ret=command(cmd,arg,R1|0x20); if (ret) return ret;
    ret=status(writing?0x10:0x20); if (ret) return ret;
    for (unsigned i=0;i<128;++i) {
        unsigned p=i*4;
        if (writing) w32(0x20,(UInt32)bytes[p]|((UInt32)bytes[p+1]<<8)|((UInt32)bytes[p+2]<<16)|((UInt32)bytes[p+3]<<24));
        else { UInt32 word=r32(0x20); for (unsigned b=0;b<4;++b) bytes[p+b]=(UInt8)(word>>(8*b)); }
    }
    ret=status(2);
    if (!ret && writing) ret=ready();
    return ret;
}
IOReturn Tegra30EMMC::ready()
{
    for (unsigned i=0;i<1000;++i) {
        UInt32 r; IOReturn ret=command(13,1U<<16,R1,&r); if (ret) return ret;
        if ((r&0x100) && ((r>>9)&15)==4) return kIOReturnSuccess;
        IOSleep(1);
    }
    return kIOReturnTimeout;
}
IOReturn Tegra30EMMC::initializeCard()
{
    // CAR SDMMC4 gate/reset bit 15; CLK_M source, undivided (12 MHz on grouper).
    car[0x300/4]=1U<<15; OSSynchronizeIO();
    car[0x320/4]=1U<<15; OSSynchronizeIO();
    car[0x164/4]=3U<<30; OSSynchronizeIO(); IODelay(10);
    car[0x304/4]=1U<<15; OSSynchronizeIO(); IODelay(10);
    w8(0x2f,1);
    for (unsigned i=0;r8(0x2f)&1;++i) { if (i==10000) return kIOReturnTimeout; IODelay(1); }
    w32(0x34,0x007f0033U); w32(0x38,0); // polled status, no interrupt delivery
    w8(0x2e,0xe); w8(0x28,0); w8(0x29,0x0b); // 1-bit, 3.0 V VCC (board rail retained)
    IOReturn ret=clock(16); if (ret) return ret;
    IODelay(1000);
    ret=command(0,0,0); if (ret) return ret;
    UInt32 ocr=0;
    for (unsigned i=0;i<1000;++i) {
        ret=command(1,0x40ff8080U,R3,&ocr); if (ret) return ret;
        if (ocr&0x80000000U) break;
        IOSleep(1);
    }
    IOLog("[tegra-emmc] OCR=0x%x\n",ocr);
    if (!(ocr&0x80000000U)) return kIOReturnTimeout;
    if (!(ocr&0x40000000U)) return kIOReturnUnsupported; // sector-addressed eMMC only
    ret=command(2,0,R2); if (ret) return ret;
    ret=command(3,1U<<16,R1); if (ret) return ret;
    ret=command(9,1U<<16,R2); if (ret) return ret;
    // SDHCI strips CRC/end bit: CSD[13:12] maps to RESPONSE0[5:4].
    protectedCard=(r32(0x10)&0x30)!=0;
    ret=command(7,1U<<16,R1b); if (ret) return ret;
    UInt8 ext[512]; ret=data(8,0,ext,false); if (ret) return ret;
    sectors=(UInt32)ext[212]|((UInt32)ext[213]<<8)|((UInt32)ext[214]<<16)|((UInt32)ext[215]<<24);
    IOLog("[tegra-emmc] EXT_CSD rev=%u sectors=%llu partition=%u\n",ext[192],sectors,ext[179]);
    if (!sectors || ext[192]<2) return kIOReturnUnsupported;
    // CMD0 resets the card bus width, but explicitly select the user area and
    // disable optional volatile cache before exposing write-through semantics.
    ret=command(6,(3U<<24)|(179U<<16)|((ext[179]&0xf8U)<<8),R1b); if (ret) return ret;
    if (ext[192]>=6 && ext[33]) { ret=command(6,(3U<<24)|(33U<<16),R1b); if (ret) return ret; }
    ret=command(6,(3U<<24)|(183U<<16)|(2U<<8),R1b); if (ret) return ret;
    w8(0x28,0x20); // 8-bit SDR, no high-speed timing claim
    ret=clock(1); if (ret) return ret;
    return ready();
}
bool Tegra30EMMC::start(IOService *p)
{
    if (!IOBlockStorageDevice::start(p)) return false;
    lock=IOLockAlloc();
    p->getResources();
    hostMap=p->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
    carMap=p->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
    if (!lock || !hostMap || !carMap) { releaseResources(); return false; }
    host=(volatile UInt8 *)(uintptr_t)hostMap->getAddress();
    car=(volatile UInt32 *)(uintptr_t)carMap->getAddress();
    IOReturn ret=initializeCard();
    if (ret) { IOLog("[tegra-emmc] init failed=0x%x\n",ret); releaseResources(); return false; }
    online=true;
    IOLog("[tegra-emmc] online sectors=%llu block=512 bus=8 PIO writable=%u\n",sectors,!protectedCard);
    registerService();
    return true;
}
IOReturn Tegra30EMMC::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                    IOStorageAttributes *a, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count ||
        block>=sectors || count>sectors-block || count>buffer->getLength()/512)
        return kIOReturnBadArgument;
    IODirection direction=buffer->getDirection();
    if (direction!=kIODirectionIn && direction!=kIODirectionOut) return kIOReturnBadArgument;
    if (a && (a->options || a->reserved0032 || a->reserved0064)) return kIOReturnUnsupported;
    bool writing=direction==kIODirectionOut;
    IOStorageCompletion copied=*completion;
    UInt64 done=0; UInt8 bytes[512]; IOReturn ret=kIOReturnSuccess;
    IOLockLock(lock);
    if (!online || faulted) { IOLockUnlock(lock); return kIOReturnNotReady; }
    if (writing && protectedCard) { IOLockUnlock(lock); return kIOReturnNotWritable; }
    for (UInt64 i=0;i<count;++i) {
        if (writing && buffer->readBytes((IOByteCount)done,bytes,512)!=512) { ret=kIOReturnUnderrun; break; }
        ret=data(writing?24:17,(UInt32)(block+i),bytes,writing);
        if (ret) { faulted=true; break; } // never reuse ambiguous command/FIFO state
        if (!writing && buffer->writeBytes((IOByteCount)done,bytes,512)!=512) { ret=kIOReturnUnderrun; break; }
        done+=512;
    }
    IOLockUnlock(lock);
    IOStorage::complete(&copied,ret,done);
    return kIOReturnSuccess;
}
IOReturn Tegra30EMMC::doSynchronizeCache()
{
    IOLockLock(lock);
    IOReturn ret=(!online || faulted)?kIOReturnNotReady:ready();
    if (ret) faulted=true;
    IOLockUnlock(lock); return ret;
}
