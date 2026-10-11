/* SPDX-License-Identifier: BSD-2-Clause
 * Tegra30 SDMMC4 eMMC transport for Apple's IOStorage family.
 * IODMACommand client scatter/gather ADMA2; SDMA/bounce and PIO fallback.
 * Board firmware supplies the pad mux and eMMC rail; DT supplies MMIO resources.
 */
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>

// OSSynchronizeIO is empty in this tree. Keep ordering local to this device.
// Guarded, inhibited-cache MMIO preserves FIFO access order; fence each block,
// rather than serializing every FIFO word with a system-wide DSB.
static inline void emmcBarrier() { __asm__ volatile("dsb sy" ::: "memory"); }
static const unsigned emmcChunkBlocks=128, emmcChunkBytes=emmcChunkBlocks*512;
static uint64_t emmcNanoseconds()
{
    AbsoluteTime now;
    uint64_t ns;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now,&ns);
    return ns;
}

class Tegra30EMMC : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(Tegra30EMMC)
    IOMemoryMap *hostMap, *carMap;
    volatile UInt8 *host;
    volatile UInt32 *car;
    IOLock *lock;
    UInt8 *transferBuffer;
    IOBufferMemoryDescriptor *dmaBuffer;
    UInt8 *dmaBytes;
    UInt32 dmaAddress;
    IODMACommand *clientDMA;
    IOMemoryDescriptor *clientMemory;
    bool clientRangePrepared, clientCacheOwned, scatterDMA;
    UInt64 clientOffset, clientLength, directBytes[2], fallbackBytes[2], sgSegments[2], fragmented[2];
    unsigned clientSegments;
    bool beginClientDMA(IOMemoryDescriptor *);
    bool planClientDMA(UInt64, unsigned, bool);
    IOReturn finishClientRange(bool);
    void endClientDMA();
    void logClientDMA(unsigned);
    UInt64 dmaReads, dmaWrites;
    bool dmaEnabled, dmaPrepared, dmaActive, admaEnabled;
    unsigned dmaSelection, activeDMABytes;
    bool directBounce, busySpin, normalDMA;
    UInt64 ioBytes[2], ioNs[2], clientCopyNs[2], bounceCopyNs[2], readyNs, busySleeps;
    UInt64 sectors;
    unsigned requestedMHz, actualHz, chunkBlocks;
    UInt64 multiReads, multiWrites;
    bool online, faulted, protectedCard, singleBlock, probeEnabled;
    UInt32 r32(unsigned o) { emmcBarrier(); UInt32 v=*(volatile UInt32 *)(host+o); emmcBarrier(); return v; }
    UInt16 r16(unsigned o) { emmcBarrier(); UInt16 v=*(volatile UInt16 *)(host+o); emmcBarrier(); return v; }
    UInt8 r8(unsigned o) { emmcBarrier(); UInt8 v=host[o]; emmcBarrier(); return v; }
    void w32(unsigned o, UInt32 v) { emmcBarrier(); *(volatile UInt32 *)(host+o)=v; emmcBarrier(); }
    void w16(unsigned o, UInt16 v) { emmcBarrier(); *(volatile UInt16 *)(host+o)=v; emmcBarrier(); }
    void w8(unsigned o, UInt8 v) { emmcBarrier(); host[o]=v; emmcBarrier(); }
    UInt32 carRead(unsigned o) { emmcBarrier(); UInt32 v=car[o/4]; emmcBarrier(); return v; }
    void carWrite(unsigned o, UInt32 v) { emmcBarrier(); car[o/4]=v; emmcBarrier(); }
    IOReturn wait(unsigned offset, UInt32 mask, UInt32 value, unsigned us);
    IOReturn status(UInt32 bit, unsigned spinUs=1000, unsigned timeoutMs=1000);
    bool setupDMA();
    bool fallbackSDMA(bool allowed);
    void releaseDMA();
    IOReturn command(unsigned cmd, UInt32 arg, UInt16 flags, UInt32 *response=0, UInt16 mode=0);
    IOReturn data(unsigned cmd, UInt32 arg, UInt8 *bytes, bool writing, unsigned blocks=1, bool client=false);
    void abortTransfer();
    IOReturn readProbe();
    IOReturn ready();
    IOReturn initializeCard();
    IOReturn clock(unsigned divisor);
    IOReturn selectTiming(const UInt8 *ext);
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
    virtual char *getAdditionalDeviceInfoString() { return (char *)(dmaEnabled?(admaEnabled?"SDMMC4 ADMA2":"SDMMC4 SDMA"):"SDMMC4 PIO"); }
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
    hostMap=carMap=0; host=0; car=0; lock=0; transferBuffer=0; sectors=0;
    clientDMA=0; clientMemory=0; clientRangePrepared=clientCacheOwned=false; scatterDMA=true;
    clientOffset=clientLength=0; clientSegments=0;
    for (unsigned i=0;i<2;++i) directBytes[i]=fallbackBytes[i]=sgSegments[i]=fragmented[i]=0;
    dmaBuffer=0; dmaBytes=0; dmaAddress=0; dmaReads=dmaWrites=0;
    dmaEnabled=dmaPrepared=dmaActive=admaEnabled=false; dmaSelection=0; activeDMABytes=0;
    directBounce=busySpin=normalDMA=true;
    for (unsigned i=0;i<2;++i) ioBytes[i]=ioNs[i]=clientCopyNs[i]=bounceCopyNs[i]=0;
    readyNs=busySleeps=0;
    online=faulted=protectedCard=singleBlock=false;
    chunkBlocks=emmcChunkBlocks;
    probeEnabled=false; multiReads=multiWrites=0; requestedMHz=52; actualHz=6000000;
    return true;
}
void Tegra30EMMC::releaseResources()
{
    if (dmaActive && host && car) abortTransfer();
    releaseDMA();
    if (hostMap) { hostMap->release(); hostMap=0; }
    if (carMap) { carMap->release(); carMap=0; }
    host=0; car=0;
    if (transferBuffer) { IOFree(transferBuffer,chunkBlocks*512); transferBuffer=0; }
    if (lock) { IOLockFree(lock); lock=0; }
}
void Tegra30EMMC::free() { releaseResources(); IOBlockStorageDevice::free(); }
void Tegra30EMMC::stop(IOService *p)
{
    IOLockLock(lock); online=false;
    if (dmaActive) abortTransfer();
    IOLockUnlock(lock);
    IOBlockStorageDevice::stop(p);
}
IOReturn Tegra30EMMC::wait(unsigned o, UInt32 mask, UInt32 value, unsigned us)
{
    uint64_t begin=emmcNanoseconds();
    for (;;) {
        if ((r32(o)&mask)==value) return kIOReturnSuccess;
        uint64_t elapsed=emmcNanoseconds()-begin;
        if (elapsed>=(uint64_t)us*1000) return kIOReturnTimeout;
        // IODelay(1) may schedule for a full tick on this ARM port. Spin for
        // short controller handshakes; sleep only after 1 ms, with a real
        // elapsed-time bound that also includes scheduler/preemption delays.
        if (elapsed>=1000000) IOSleep(1);
    }
}
IOReturn Tegra30EMMC::status(UInt32 bit, unsigned spinUs, unsigned timeoutMs)
{
    bool boundaryResumed=false;
    uint64_t begin=emmcNanoseconds();
    for (;;) {
        UInt32 s=r32(0x30);
        if (s&0xffff8000U) {
            IOLog("[tegra-emmc] status error=0x%x\n",(unsigned)s);
            if (admaEnabled && (s&(1U<<25)))
                IOLog("[tegra-emmc] ADMA2 error=0x%x descriptor=0x%x\n",r8(0x54),r32(0x58));
            return (s&0x00110000U)?kIOReturnTimeout:kIOReturnIOError;
        }
        // A buffer smaller than 512 KiB, aligned to 512 KiB cannot cross an SDMA boundary.
        // Completion wins if a controller reports boundary and end together.
        if (s&bit) { w32(0x30,bit|((dmaActive && bit==2)?(s&8):0)); return kIOReturnSuccess; }
        if (dmaActive && !admaEnabled && bit==2 && (s&8)) {
            if (activeDMABytes==524288 && !boundaryResumed) {
                // Linux SDMA boundary handling: acknowledge and provide the
                // next boundary address. Block count is exhausted at this
                // boundary, so hardware issues completion without more data.
                w32(0x30,8); w32(0x00,dmaAddress+524288);
                boundaryResumed=true; continue;
            }
            IOLog("[tegra-emmc] unexpected SDMA boundary status=0x%x bytes=%u resumed=%u address=0x%x remaining=%u\n",
                  s,activeDMABytes,boundaryResumed,r32(0x00),r16(0x06));
            return kIOReturnIOError;
        }
        uint64_t elapsed=emmcNanoseconds()-begin;
        // Keep the deadline independent of the busy-poll window. Large writes
        // can include long card programming/garbage-collection intervals.
        if (elapsed>=(uint64_t)timeoutMs*1000000ULL) {
            IOLog("[tegra-emmc] status timeout wanted=0x%x\n",(unsigned)bit);
            return kIOReturnTimeout;
        }
        if (elapsed>=(uint64_t)spinUs*1000) IOSleep(1);
    }
}
// Linux SDHCI SDMA uses one contiguous segment and a 32-bit address on Tegra30.
// Retain an explicit bounce buffer for SDMA and unaligned/unaddressable clients.
// Both buffer policies bypass inner/outer caches. Normal noncacheable RAM
// permits bursts; strongly ordered memory serializes stores as if to MMIO.
// WCOMB is TEX=100, C=B=0 in this ARM pmap; aliases inherit the same type.
// Device registers always remain guarded/inhibited. DSB transfers ownership.
bool Tegra30EMMC::fallbackSDMA(bool allowed)
{
    releaseDMA();
    if (!allowed) return false;
    unsigned saved=dmaSelection; dmaSelection=1;
    IOLog("[tegra-emmc] ADMA2 resource fallback to SDMA\n");
    bool result=setupDMA(); dmaSelection=saved;
    return result;
}
bool Tegra30EMMC::setupDMA()
{
    UInt32 caps=r32(0x40);
    bool useADMA=(caps&(1U<<19)) && dmaSelection!=1;
    bool useSDMA=(caps&(1U<<22)) && dmaSelection!=2;
    if (!useADMA && !useSDMA) {
        IOLog("[tegra-emmc] SDMA unavailable caps=0x%x; PIO fallback\n",caps);
        return false;
    }
    dmaBuffer=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryMapperNone|
            (normalDMA?kIOMapWriteCombineCache:kIOMapInhibitCache),
        chunkBlocks*512+(useADMA?4096:0),0xfff80000ULL);
    if (!dmaBuffer && useADMA && useSDMA) {
        useADMA=false;
        dmaBuffer=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryMapperNone|
                (normalDMA?kIOMapWriteCombineCache:kIOMapInhibitCache),
            chunkBlocks*512,0xfff80000ULL);
    }
    if (!dmaBuffer) return false;
    if (dmaBuffer->prepare(kIODirectionInOut)!=kIOReturnSuccess) return fallbackSDMA(useADMA && useSDMA);
    dmaPrepared=true;
    IOByteCount length=0;
    UInt64 address=dmaBuffer->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    dmaBytes=(UInt8 *)dmaBuffer->getBytesNoCopy();
    if (!dmaBytes || length<chunkBlocks*512+(useADMA?4096:0) || !address || address>0xffffffffULL-chunkBlocks*512-(useADMA?4096:0)+1 ||
        (address&0x7ffff)) return fallbackSDMA(useADMA && useSDMA);
    dmaAddress=(UInt32)address;
    // SDHCI DMA select: 00 SDMA, 10 32-bit ADMA2; retain bus/voltage policy.
    admaEnabled=useADMA;
    w8(0x28,(r8(0x28)&~0x18)|(admaEnabled?0x10:0));
    // Latch DMA boundary/error status, but retain polled completion (no IRQ).
    w32(0x34,r32(0x34)|8|(admaEnabled?(1U<<25):0));
    if (useADMA && scatterDMA) {
        // Cursor-only mode is essential: IODMACommand must never silently copy
        // into its own alignment/address bounce pages. Bypass any system mapper;
        // SDMMC4 consumes 32-bit physical addresses. Validate cache-line ownership
        // ourselves before transferring ownership on this noncoherent ARM SoC.
        clientDMA=IODMACommand::withSpecification(IODMACommand::OutputHost32,32,32768,
            (IODMACommand::MappingOptions)(IODMACommand::kBypassed|IODMACommand::kIterateOnly),
            chunkBlocks*512,1);
    }
    dmaEnabled=true;
    IOLog("[tegra-emmc] %s enabled caps=0x%x address=0x%x bytes=%u uncached=1 normal=%u\n",
          admaEnabled?"ADMA2":"SDMA",caps,dmaAddress,chunkBlocks*512,normalDMA);
    return true;
}
void Tegra30EMMC::releaseDMA()
{
    dmaEnabled=admaEnabled=false;
    // Never return pages to VM while a bus master might still own them.
    if (dmaActive) {
        IOLog("[tegra-emmc] DMA memory quarantined: reset not confirmed\n");
        return;
    }
    endClientDMA();
    if (clientDMA) { clientDMA->release(); clientDMA=0; }
    if (!dmaBuffer) return;
    if (dmaPrepared) dmaBuffer->complete(kIODirectionInOut);
    dmaBuffer->release(); dmaBuffer=0; dmaBytes=0; dmaAddress=0; dmaPrepared=false;
}
// IODMACommand retains the descriptor, but does not wire it in this IOKit
// version. Own a nested prepare until all bus-master activity has stopped.
bool Tegra30EMMC::beginClientDMA(IOMemoryDescriptor *memory)
{
    if (!clientDMA || !admaEnabled || !scatterDMA) return false;
    if (memory->prepare(memory->getDirection())) return false;
    if (clientDMA->setMemoryDescriptor(memory,false)) {
        memory->complete(memory->getDirection()); return false;
    }
    clientMemory=memory;
    return true;
}
bool Tegra30EMMC::planClientDMA(UInt64 offset, unsigned length, bool writing)
{
    if (!clientMemory) return false;
    clientOffset=offset; clientLength=length; clientSegments=0;
    if (clientDMA->prepare(offset,length,false,false)) {
        // prepare may have acquired internal state even when it fails.
        clientDMA->clearMemoryDescriptor();
        clientMemory->complete(clientMemory->getDirection()); clientMemory=0;
        return false;
    }
    clientRangePrepared=true;
    UInt64 cursor=0;
    UInt32 *table=(UInt32 *)(dmaBytes+chunkBlocks*512);
    UInt32 previousEnd=0; bool discontinuous=false;
    while (cursor<length) {
        IODMACommand::Segment32 segments[32]; UInt32 count=32;
        UInt64 previous=cursor;
        if (clientDMA->gen32IOVMSegments(&cursor,segments,&count) || !count ||
            count>32 || cursor<=previous || cursor>length) goto fallback;
        UInt64 covered=previous;
        for (unsigned i=0;i<count;++i) {
            UInt32 address=segments[i].fIOVMAddr, size=segments[i].fLength;
            // A9 lines are 32 bytes. Sharing a dirty partial line with another
            // owner makes read invalidation unsafe; use explicit bounce instead.
            if (!address || (address&31) || (size&31) || !size || size>32768 ||
                address>0xffffffffU-size+1 || clientSegments>=512 || covered+size>length)
                goto fallback;
            if (clientSegments && previousEnd!=address) discontinuous=true;
            previousEnd=address+size;
            table[clientSegments*2]=(size<<16)|0x21U;
            table[clientSegments*2+1]=address;
            ++clientSegments; covered+=size;
        }
        if (covered!=cursor) goto fallback;
    }
    if (cursor==length && clientSegments) {
        table[(clientSegments-1)*2]|=2; // END on the final transfer descriptor
        // The boot shim disables PL310. These operations cover the active A9
        // cache; CPU access to complete cache lines ceases until DMA finishes.
        if (!clientMemory->performOperation(writing?kIOMemoryIncoherentIOStore:
                kIOMemoryIncoherentIOFlush,(IOByteCount)offset,length)) {
            clientCacheOwned=true;
            if (discontinuous) ++fragmented[writing];
            emmcBarrier(); return true;
        }
    }
fallback:
    finishClientRange(writing);
    return false;
}
IOReturn Tegra30EMMC::finishClientRange(bool writing)
{
    if (dmaActive || !clientRangePrepared) return kIOReturnSuccess;
    IOReturn ret=kIOReturnSuccess;
    emmcBarrier();
    if (clientCacheOwned && !writing)
        ret=clientMemory->performOperation(kIOMemoryIncoherentIOFlush,
            (IOByteCount)clientOffset,(IOByteCount)clientLength);
    clientCacheOwned=false;
    IOReturn complete=clientDMA->complete(false,false);
    clientRangePrepared=false;
    return ret?ret:complete;
}
void Tegra30EMMC::endClientDMA()
{
    // A failed reset leaves both the command's retain and our nested wire in
    // place, including after the storage family's completion unwires its owner.
    if (dmaActive || !clientMemory) return;
    finishClientRange(clientMemory->getDirection()==kIODirectionOut);
    clientDMA->clearMemoryDescriptor(false);
    clientMemory->complete(clientMemory->getDirection()); clientMemory=0;
}
IOReturn Tegra30EMMC::clock(unsigned divisor)
{
    w16(0x2c,0);
    w16(0x2c,(divisor<<8)|1);
    uint64_t begin=emmcNanoseconds();
    for (;;) {
        if (r16(0x2c)&2) { w16(0x2c,(divisor<<8)|5); return kIOReturnSuccess; }
        uint64_t elapsed=emmcNanoseconds()-begin;
        if (elapsed>=10000000) return kIOReturnTimeout;
        if (elapsed>=1000000) IOSleep(1);
    }
}
// Linux mmc_select_hs + Tegra peripheral U7.1 divider semantics, implemented
// through IOKit MMIO. PLLP is shared: inspect it, never reprogram it here.
IOReturn Tegra30EMMC::selectTiming(const UInt8 *ext)
{
    actualHz=6000000;
    if (requestedMHz==6 || !(ext[196]&3)) {
        IOLog("[tegra-emmc] timing SDR hz=%u hs=0 card-type=0x%x\n",actualHz,ext[196]);
        return kIOReturnSuccess;
    }
    UInt32 osc=carRead(0x50), pll=carRead(0xa0);
    unsigned m=pll&31, n=(pll>>8)&1023, p=(pll>>20)&7;
    // This board boots with a 12 MHz oscillator. Require explicit, enabled,
    // locked PLLP configuration; unknown firmware settings retain the baseline.
    if ((osc>>28)!=8 || (osc&(3U<<26)) ||
        (pll&0x58000000U)!=0x58000000U || (pll&0x80000000U) || !m || !n) {
        IOLog("[tegra-emmc] clock fallback osc=0x%x pllp=0x%x\n",osc,pll);
        return kIOReturnSuccess;
    }
    unsigned parent=(unsigned)((12000000ULL*n)/(m*(1U<<p)));
    if (parent<200000000 || parent>600000000) return kIOReturnSuccess;
    unsigned target=requestedMHz*1000000;
    unsigned limit=(ext[196]&2)?52000000:26000000;
    if (target>limit) target=limit;
    unsigned denominator=(unsigned)((2ULL*parent+target-1)/target);
    if (denominator<2 || denominator>257) return kIOReturnUnsupported;
    UInt8 baseline[512], fast[512];
    IOReturn ret=data(17,0,baseline,false); if (ret) return ret;
    ret=command(6,(3U<<24)|(185U<<16)|(1U<<8),R1b);
    if (ret) return ret; // ambiguous switch/transport errors must not expose writes
    ret=ready(); if (ret) return ret;
    unsigned selectedHz=(unsigned)(2ULL*parent/denominator);
    // Linux __mmc_select_powerclass: 8-bit SDR, board VCC in 2.7-3.6 V
    // range (distinct from the retained 1.8 V VCCQ signaling rail).
    unsigned powerClass=ext[selectedHz>26000000?202:203]>>4;
    if (powerClass) {
        ret=command(6,(3U<<24)|(187U<<16)|(powerClass<<8),R1b);
        if (ret) return ret;
        ret=ready(); if (ret) return ret;
    }
    w16(0x2c,0); // stop card clock before source/tap changes
    // Grouper's downstream Linux SDMMC4 platform data uses tap_delay=0x0f.
    // Keep firmware's pad drive/voltage/trim; SDR HS requires no voltage switch.
    w32(0x100,(r32(0x100)&~0x00ff0000U)|(15U<<16));
    UInt32 source=(carRead(0x164)&~0xc00000ffU)|(denominator-2);
    carWrite(0x164,source); // parent 0 = PLLP, output = 2*parent/(divider+2)
    // Tegra30 Linux NO_HISPD_BIT: leave HOST_CONTROL high-speed bit clear.
    ret=clock(0); if (ret) return ret; // SDHCI divider bypass
    actualHz=selectedHz;
    UInt8 check[512]; ret=data(8,0,check,false); if (ret) return ret;
    // Linux compares immutable EXT_CSD fields after bus changes. Verify timing
    // and capacity too, before the device is registered with IOStorage.
    if ((powerClass && check[187]!=powerClass) || check[185]!=1 || check[192]!=ext[192] || check[196]!=ext[196]) return kIOReturnIOError;
    for (unsigned i=212;i<216;++i) if (check[i]!=ext[i]) return kIOReturnIOError;
    ret=data(17,0,fast,false); if (ret) return ret;
    for (unsigned i=0;i<512;++i) if (fast[i]!=baseline[i]) return kIOReturnIOError;
    IOLog("[tegra-emmc] timing SDR hz=%u hs=1 card-type=0x%x pllp=%u tap=15 power-class=%u\n",actualHz,ext[196],parent,powerClass);
    return ready();
}
IOReturn Tegra30EMMC::command(unsigned cmd, UInt32 arg, UInt16 flags, UInt32 *response, UInt16 mode)
{
    // An abort CMD12 must be allowed while DAT remains inhibited.
    IOReturn ret=wait(0x24,(cmd!=12 && ((flags&3)==3 || (flags&0x20)))?3:1,0,100000);
    if (ret) return ret;
    w32(0x30,0xffffffffU);
    // Tegra requires transfer mode and command in one 32-bit write.
    w32(0x08,arg);
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
void Tegra30EMMC::abortTransfer()
{
    // Reset host command/data circuits before best-effort card stop. The caller
    // still latches faulted: cleanup never makes an ambiguous write retryable.
    w8(0x2f,6);
    for (unsigned i=0;i<10000;++i) {
        if (!(r8(0x2f)&6)) {
            emmcBarrier(); dmaActive=false;
            IOReturn ret=command(12,0,R1b|0xc0);
            if (ret) IOLog("[tegra-emmc] abort failed=0x%x\n",ret);
            return;
        }
        IODelay(1);
    }
    IOLog("[tegra-emmc] abort reset timeout\n");
    if (dmaActive) {
        // A stuck SDHCI reset cannot authorize freeing DMA pages. Assert the
        // SDMMC4 peripheral reset, confirm it in CAR, and keep media faulted.
        carWrite(0x300,1U<<15);
        if (carRead(0x04)&(1U<<15)) { emmcBarrier(); dmaActive=false; }
    }
}
IOReturn Tegra30EMMC::data(unsigned cmd, UInt32 arg, UInt8 *bytes, bool writing, unsigned blocks, bool client)
{
    if (client && (!clientRangePrepared || !clientCacheOwned || !admaEnabled)) return kIOReturnBadArgument;
    if (!bytes || !blocks || blocks>chunkBlocks) return kIOReturnBadArgument;
    IOReturn ret=wait(0x24,3,0,100000); if (ret) return ret;
    // Counted eMMC transfer: both the card and host know the exact block count.
    // No Auto CMD12/23 dependency and no normal-path CMD12 after termination.
    if (blocks>1) { ret=command(23,blocks,R1); if (ret) return ret; }
    bool useDMA=dmaEnabled && (bytes==transferBuffer || bytes==dmaBytes);
    if (useDMA) {
        if (writing && bytes!=dmaBytes) {
            UInt64 begin=probeEnabled?emmcNanoseconds():0;
            bcopy(bytes,dmaBytes,blocks*512);
            if (probeEnabled) bounceCopyNs[1]+=emmcNanoseconds()-begin;
        }
        emmcBarrier();
        if (admaEnabled) {
          if (!client) {
            // Tegra30 BROKEN_ADMA_ZEROLEN_DESC: never encode 64 KiB as zero.
            // Aligned 32 KiB segments also preserve aligned subsequent addresses.
            UInt32 *table=(UInt32 *)(dmaBytes+chunkBlocks*512);
            unsigned remaining=blocks*512,offset=0,index=0;
            while (remaining) {
                unsigned length=remaining>32768?32768:remaining;
                remaining-=length;
                table[index++]=(length<<16)|0x21U|(remaining?0:2U);
                table[index++]=dmaAddress+offset; offset+=length;
            }
          }
            emmcBarrier();
            w32(0x58,dmaAddress+chunkBlocks*512);
        } else w32(0x00,dmaAddress);
    }
    w16(0x04,(useDMA?0x7000:0)|512); w16(0x06,(UInt16)blocks);
    UInt16 mode=2|(writing?0:0x10)|(blocks>1?0x20:0)|(useDMA?1:0);
    activeDMABytes=useDMA?blocks*512:0;
    dmaActive=useDMA;
    ret=command(cmd,arg,R1|0x20,0,mode);
    if (ret) { abortTransfer(); return ret; }
    if (!useDMA) for (unsigned block=0;block<blocks;++block) {
        ret=status(writing?0x10:0x20);
        if (ret) { abortTransfer(); return ret; }
        emmcBarrier();
        for (unsigned i=0;i<128;++i) {
            unsigned p=block*512+i*4;
            if (writing) *(volatile UInt32 *)(host+0x20)=(UInt32)bytes[p]|((UInt32)bytes[p+1]<<8)|((UInt32)bytes[p+2]<<16)|((UInt32)bytes[p+3]<<24);
            else { UInt32 word=*(volatile UInt32 *)(host+0x20); for (unsigned b=0;b<4;++b) bytes[p+b]=(UInt8)(word>>(8*b)); }
        }
        emmcBarrier();
    }
    // At 48 MHz a whole 64 KiB DMA takes >1 ms. A short bounded polling
    // window avoids a scheduler tick per chunk; long card busy still sleeps.
    ret=status(2,useDMA?(blocks>128?32000:8000):1000,10000);
    if (!ret && useDMA) ret=wait(0x24,2,0,100000);
    if (!ret && useDMA) {
        emmcBarrier(); dmaActive=false;
        if (!writing && bytes!=dmaBytes) {
            UInt64 begin=probeEnabled?emmcNanoseconds():0;
            bcopy(dmaBytes,bytes,blocks*512);
            if (probeEnabled) bounceCopyNs[0]+=emmcNanoseconds()-begin;
        }
        if (writing) ++dmaWrites; else ++dmaReads;
    }
    if (!ret && writing) ret=ready();
    if (ret) abortTransfer();
    return ret;
}
IOReturn Tegra30EMMC::ready()
{
    UInt64 begin=emmcNanoseconds();
    IOReturn ret=kIOReturnTimeout;
    for (;;) {
        UInt32 r;
        ret=command(13,1U<<16,R1,&r); if (ret) break;
        if ((r&0x100) && ((r>>9)&15)==4) break;
        UInt64 elapsed=emmcNanoseconds()-begin;
        if (elapsed>=1000000000ULL) { ret=kIOReturnTimeout; break; }
        // A short program-busy interval must not cost an entire scheduler tick.
        // Long program/erase busy still sleeps; the timeout is elapsed time.
        if (!busySpin || elapsed>=2000000) { IOSleep(1); ++busySleeps; }
    }
    if (probeEnabled) readyNs+=emmcNanoseconds()-begin;
    return ret;
}
IOReturn Tegra30EMMC::readProbe()
{
    // Explicit boot-argument-only, read-only comparison before publishing media.
    // No reserved-sector writes or automatic retries, even in diagnostic mode.
    UInt8 *reference=(UInt8 *)IOMalloc(emmcChunkBytes);
    if (!reference) return kIOReturnNoMemory;
    IOLog("[tegra-emmc] read-probe begin\n");
    uint64_t elapsed[2]={0,0};
    IOReturn ret=kIOReturnSuccess;
    for (unsigned round=0;round<16 && !ret;++round) {
        UInt32 base=round*emmcChunkBlocks;
        if ((UInt64)base+emmcChunkBlocks>sectors) { ret=kIOReturnBadArgument; break; }
        // Alternate ordering to reduce a systematic card read-cache advantage.
        for (unsigned pass=0;pass<2 && !ret;++pass) {
            unsigned multi=pass^(round&1);
            AbsoluteTime begin,end;
            uint64_t ns;
            clock_get_uptime(&begin);
            if (multi) ret=data(18,base,transferBuffer,false,emmcChunkBlocks);
            else for (unsigned b=0;b<emmcChunkBlocks && !ret;++b)
                ret=data(17,base+b,reference+b*512,false);
            clock_get_uptime(&end);
            SUB_ABSOLUTETIME(&end,&begin);
            absolutetime_to_nanoseconds(end,&ns);
            elapsed[multi]+=ns;
            if (!round) IOLog("[tegra-emmc] read-probe first-%s result=0x%x\n",multi?"multi":"single",ret);
        }
        if (!ret) for (unsigned i=0;i<emmcChunkBytes;++i)
            if (reference[i]!=transferBuffer[i]) { ret=kIOReturnIOError; break; }
    }
    IOFree(reference,emmcChunkBytes);
    IOLog("[tegra-emmc] read-probe bytes=%u single-us=%llu multi-us=%llu match=%u result=0x%x\n",
          16*emmcChunkBytes,(UInt64)(elapsed[0]/1000),(UInt64)(elapsed[1]/1000),!ret,ret);
    return ret;
}
IOReturn Tegra30EMMC::initializeCard()
{
    // CAR SDMMC4 gate/reset bit 15; CLK_M source, undivided (12 MHz on grouper).
    carWrite(0x300,1U<<15);
    carWrite(0x320,1U<<15);
    carWrite(0x164,3U<<30); IODelay(10);
    carWrite(0x304,1U<<15); IODelay(10);
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
    IOLog("[tegra-emmc] OCR=0x%x\n",(unsigned)ocr);
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
    ret=ready(); if (ret) return ret;
    return selectTiming(ext);
}
bool Tegra30EMMC::start(IOService *p)
{
    if (!IOBlockStorageDevice::start(p)) return false;
    lock=IOLockAlloc();
    // A full 512 KiB ADMA table reduces command/copy overhead for bulk I/O.
    // Smaller bursts remain available through n7b for comparison and recovery.
    int burst=512;
    PE_parse_boot_argn("n7b",&burst,sizeof(burst));
    if (burst!=64 && burst!=256 && burst!=512) return false;
    chunkBlocks=(unsigned)burst*2;
    transferBuffer=(UInt8 *)IOMalloc(chunkBlocks*512);
    // Advertise limits to IOBlockStorageDriver instead of changing its globals.
    setProperty(kIOMaximumBlockCountReadKey,(UInt64)chunkBlocks,64);
    setProperty(kIOMaximumBlockCountWriteKey,(UInt64)chunkBlocks,64);
    setProperty(kIOMaximumByteCountReadKey,(UInt64)(chunkBlocks*512),64);
    setProperty(kIOMaximumByteCountWriteKey,(UInt64)(chunkBlocks*512),64);
    int single=0;
    PE_parse_boot_argn("nexus7_emmc_single",&single,sizeof(single));
    singleBlock=single!=0;
    int mhz=52;
    PE_parse_boot_argn("nexus7_emmc_mhz",&mhz,sizeof(mhz));
    if (mhz!=6 && mhz!=12 && mhz!=24 && mhz!=48 && mhz!=52) return false;
    requestedMHz=(unsigned)mhz;
    int probe=0;
    PE_parse_boot_argn("nexus7_emmc_probe",&probe,sizeof(probe));
    probeEnabled=probe!=0;
    int direct=1,spin=1,normal=1;
    PE_parse_boot_argn("nexus7_emmc_direct",&direct,sizeof(direct));
    PE_parse_boot_argn("nexus7_emmc_busyspin",&spin,sizeof(spin));
    PE_parse_boot_argn("nexus7_emmc_normaldma",&normal,sizeof(normal));
    directBounce=direct!=0; busySpin=spin!=0;
    normalDMA=normal!=0;
    p->getResources();
    hostMap=p->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
    carMap=p->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
    if (!lock || !transferBuffer || !hostMap || !carMap) { releaseResources(); return false; }
    host=(volatile UInt8 *)(uintptr_t)hostMap->getAddress();
    car=(volatile UInt32 *)(uintptr_t)carMap->getAddress();
    IOReturn ret=initializeCard();
    if (!ret && probeEnabled) IOLog("[tegra-emmc] card initialized\n");
    int selection=0;
    PE_parse_boot_argn("n7d",&selection,sizeof(selection));
    if (selection<0 || selection>2) { releaseResources(); return false; }
    dmaSelection=(unsigned)selection;
    int scatter=1; PE_parse_boot_argn("n7s",&scatter,sizeof(scatter));
    scatterDMA=scatter!=0;
    int dma=1;
    PE_parse_boot_argn("nexus7_emmc_dma",&dma,sizeof(dma));
    if (!ret && dma) {
        if (setupDMA()) {
            // Independently compare PIO and DMA before publishing writable media.
            UInt8 reference[512];
            ret=data(17,0,reference,false);
            if (!ret) ret=data(17,0,transferBuffer,false);
            if (!ret && bcmp(reference,transferBuffer,512)) ret=kIOReturnIOError;
            IOLog("[tegra-emmc] SDMA readback result=0x%x\n",ret);
        } else IOLog("[tegra-emmc] SDMA allocation/capability fallback to PIO\n");
    }
    if (!ret && probeEnabled) ret=readProbe();
    if (!ret && probeEnabled && dmaEnabled && chunkBlocks>emmcChunkBlocks) {
        UInt64 begin=emmcNanoseconds();
        const unsigned totalBlocks=67108864/512;
        if (sectors<totalBlocks) ret=kIOReturnBadArgument;
        for (unsigned block=0;block<totalBlocks && !ret;block+=chunkBlocks)
            ret=data(18,block,dmaBytes,false,chunkBlocks);
        IOLog("[tegra-emmc] sequential-probe bytes=67108864 chunk=%u us=%llu result=0x%x\n",
              chunkBlocks*512,(emmcNanoseconds()-begin)/1000,ret);
    }
    if (ret) { IOLog("[tegra-emmc] init failed=0x%x\n",ret); releaseResources(); return false; }
    online=true;
    if (probeEnabled) {
        ret=doSynchronizeCache(); // Exercise explicit sync independently of HFS fsync policy.
        if (ret) { online=false; releaseResources(); return false; }
    }
    IOLog("[tegra-emmc] online sectors=%llu block=512 bus=8 %s writable=%u\n",sectors,dmaEnabled?(admaEnabled?"ADMA2":"SDMA"):"PIO",!protectedCard);
    IOLog("[tegra-emmc] counted %s max-blocks=%u ordered-mmio=1\n",dmaEnabled?(admaEnabled?"ADMA2":"SDMA"):"PIO",singleBlock?1:chunkBlocks);
    if (probeEnabled) IOLog("[tegra-emmc] policy direct=%u busyspin=%u normaldma=%u\n",directBounce,busySpin,normalDMA);
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
    UInt64 done=0; IOReturn ret=kIOReturnSuccess;
    IOLockLock(lock);
    if (!online || faulted) { IOLockUnlock(lock); return kIOReturnNotReady; }
    if (writing && protectedCard) { IOLockUnlock(lock); return kIOReturnNotWritable; }
    UInt64 begin=probeEnabled?emmcNanoseconds():0;
    UInt8 *ioBuffer=(dmaEnabled && directBounce)?dmaBytes:transferBuffer;
    UInt64 oldBytes=ioBytes[writing];
    beginClientDMA(buffer);
    for (UInt64 i=0;i<count;) {
        unsigned limit=singleBlock?1:chunkBlocks;
        unsigned blocks=(unsigned)((count-i)>limit?limit:count-i);
        unsigned length=blocks*512;
        bool client=planClientDMA(done,length,writing);
        if (writing && !client) {
            UInt64 copyBegin=probeEnabled?emmcNanoseconds():0;
            IOByteCount copiedBytes=buffer->readBytes((IOByteCount)done,ioBuffer,length);
            if (probeEnabled) clientCopyNs[1]+=emmcNanoseconds()-copyBegin;
            if (copiedBytes!=length) { ret=kIOReturnUnderrun; break; }
        }
        unsigned cmd=writing?(blocks>1?25:24):(blocks>1?18:17);
        ret=data(cmd,(UInt32)(block+i),client?dmaBytes:ioBuffer,writing,blocks,client);
        IOReturn cacheRet=finishClientRange(writing);
        if (!ret) ret=cacheRet;
        if (ret) { faulted=true; break; } // never reuse ambiguous command/FIFO state
        if (blocks>1) { if (writing) ++multiWrites; else ++multiReads; }
        if (probeEnabled && writing && multiWrites==1 && blocks>1)
            IOLog("[tegra-emmc] chunks read=%llu write=%llu ready=0x0\n",multiReads,multiWrites);
        if (probeEnabled && writing && dmaEnabled && dmaWrites==1)
            IOLog("[tegra-emmc] dma-chunks read=%llu write=%llu active=%u\n",dmaReads,dmaWrites,dmaActive);
        // Only fully completed chunks are reported. FIFO progress alone does
        // not prove that a failing multi-block write reached the medium.
        if (!writing && !client) {
            UInt64 copyBegin=probeEnabled?emmcNanoseconds():0;
            IOByteCount copiedBytes=buffer->writeBytes((IOByteCount)done,ioBuffer,length);
            if (probeEnabled) clientCopyNs[0]+=emmcNanoseconds()-copyBegin;
            if (copiedBytes!=length) { ret=kIOReturnUnderrun; break; }
        }
        if (client) { directBytes[writing]+=length; sgSegments[writing]+=clientSegments; }
        else fallbackBytes[writing]+=length;
        done+=length; i+=blocks;
    }
    endClientDMA();
    if (probeEnabled) {
        ioBytes[writing]+=done; ioNs[writing]+=emmcNanoseconds()-begin;
        if (oldBytes/67108864!=ioBytes[writing]/67108864) {
            logClientDMA(writing);
        }
    }
    IOLockUnlock(lock);
    IOStorage::complete(&copied,ret,done);
    return kIOReturnSuccess;
}
void Tegra30EMMC::logClientDMA(unsigned writing)
{
    IOLog("[tegra-emmc] client-dma dir=%u direct=%llu fallback=%llu segments=%llu fragmented=%llu active=%u\n",
        writing,directBytes[writing],fallbackBytes[writing],sgSegments[writing],fragmented[writing],dmaActive);
    IOLog("[tegra-emmc] profile dir=%u bytes=%llu io-us=%llu client-copy-us=%llu bounce-copy-us=%llu ready-us=%llu sleeps=%llu\n",
        writing,ioBytes[writing],ioNs[writing]/1000,clientCopyNs[writing]/1000,
        bounceCopyNs[writing]/1000,readyNs/1000,busySleeps);
}
IOReturn Tegra30EMMC::doSynchronizeCache()
{
    IOLockLock(lock);
    IOReturn ret=(!online || faulted)?kIOReturnNotReady:ready();
    if (ret) faulted=true;
    if (probeEnabled) IOLog("[tegra-emmc] chunks read=%llu write=%llu flush=0x%x\n",multiReads,multiWrites,ret);
    if (probeEnabled && dmaEnabled) IOLog("[tegra-emmc] dma-chunks read=%llu write=%llu active=%u\n",dmaReads,dmaWrites,dmaActive);
    if (probeEnabled && dmaEnabled) { logClientDMA(0); logClientDMA(1); }
    IOLockUnlock(lock); return ret;
}
