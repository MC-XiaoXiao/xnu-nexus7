#!/usr/bin/env python3
"""Linux serial automation for mainline U-Boot. No third-party Python modules.

inspect is read-only apart from console commands. deploy writes regular files;
--autoboot explicitly saves U-Boot environment. Never writes raw MMC sectors.
"""
from pathlib import Path
import argparse
import binascii
import contextlib
import fcntl
import json
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import termios
import time
import zlib
ROOT=Path(__file__).resolve().parents[1]
PROMPT=re.compile(rb'(?:^|[\r\n])(?:=>|Tegra30 \(Grouper\) #) ?$')

def occupied(port):
    target=os.stat(port).st_rdev
    for process in Path('/proc').iterdir():
        if not process.name.isdigit() or int(process.name)==os.getpid(): continue
        try:
            for fd in (process/'fd').iterdir():
                try:
                    if fd.stat().st_rdev==target:
                        return f'PID {process.name} ({(process/"comm").read_text().strip()})'
                except OSError: pass
        except OSError: pass
    return None

class Console:
    def __init__(self,port,log,baud=115200,check_busy=True):
        if check_busy and (owner:=occupied(port)):
            raise RuntimeError(f'{port} is already open by {owner}; close that console first')
        self.fd=os.open(port,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
        self.old=termios.tcgetattr(self.fd)
        fcntl.flock(self.fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        fcntl.ioctl(self.fd,termios.TIOCEXCL)
        attrs=termios.tcgetattr(self.fd)
        attrs[0]=attrs[1]=attrs[3]=0
        attrs[2]=termios.CLOCAL|termios.CREAD|termios.CS8
        speed=getattr(termios,f'B{baud}')
        attrs[4]=attrs[5]=speed
        attrs[6][termios.VMIN]=0; attrs[6][termios.VTIME]=0
        termios.tcsetattr(self.fd,termios.TCSANOW,attrs)
        self.log=log
    def close(self):
        with contextlib.suppress(OSError,termios.error): termios.tcsetattr(self.fd,termios.TCSANOW,self.old)
        with contextlib.suppress(OSError): fcntl.ioctl(self.fd,termios.TIOCNXCL)
        os.close(self.fd)
    def write(self,data):
        while data:
            _,ready,_=select.select([],[self.fd],[],10)
            if not ready: raise TimeoutError('serial write timed out')
            n=os.write(self.fd,data); data=data[n:]
    def read(self,timeout):
        ready,_,_=select.select([self.fd],[],[],timeout)
        if not ready: return b''
        data=os.read(self.fd,65536)
        self.log.write(data); self.log.flush()
        return data
    def line(self,text):
        # U-Boot echoes each input character; without RTS/CTS, a full burst
        # can overflow the Tegra UART RX FIFO while the CLI prints the echo.
        data=text.encode('ascii')+b'\r'
        for byte in data:
            self.write(bytes((byte,))); time.sleep(0.004)
    def wait_prompt(self,timeout=30):
        end=time.monotonic()+timeout; data=b''
        while time.monotonic()<end:
            data+=self.read(min(0.2,max(0,end-time.monotonic())))
            if PROMPT.search(data): return data
        raise TimeoutError('U-Boot prompt not found; see console log (no hardware reset was attempted)')
    def connect(self,interrupt=False):
        # Ctrl-C only affects U-Boot; keyed autoboot may need a configured key.
        # It cannot recover a hung kernel. Never toggle modem lines.
        self.write(b'\x03' if interrupt else b'\r')
        return self.wait_prompt()
    def command(self,text,timeout=30):
        self.line(text)
        return self.wait_prompt(timeout)
    def checked(self,text,timeout=30):
        # Unique marker makes command failure explicit; do not rely on prompt.
        marker='XNU_CMD_OK_9274'
        data=self.command(f'if {text}; then echo {marker}; else echo XNU_CMD_FAILED; fi',timeout)
        if not re.search(rb'(?:\r?\n)'+marker.encode()+rb'\r?\n',data):
            raise RuntimeError(f'U-Boot command failed: {text}\n{data.decode(errors="replace")}')
        return data
    def monitor(self,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            data=self.read(min(0.2,max(0,end-time.monotonic())))
            if data: sys.stdout.buffer.write(data); sys.stdout.buffer.flush()
    def upload(self,path,address=0x92000000):
        data=Path(path).read_bytes()
        self.line(f'loadx {address:x}')
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if b'C' in self.read(0.2): break
        else: raise TimeoutError('loadx did not request XMODEM-CRC')
        # 1 KiB XMODEM-CRC blocks; sequence wraps modulo 256. ACK-driven,
        # bounded retries and CAN cancellation. U-Boot accepts STX packets.
        for offset in range(0,len(data),1024):
            payload=data[offset:offset+1024].ljust(1024,b'\x1a')
            seq=(offset//1024+1)&255
            packet=bytes((2,seq,255-seq))+payload+struct.pack('>H',binascii.crc_hqx(payload,0))
            for attempt in range(10):
                self.write(packet)
                response=self.read(5)
                if b'\x18' in response: raise RuntimeError('U-Boot cancelled XMODEM')
                if b'\x06' in response: break
            else:
                self.write(b'\x18\x18'); raise TimeoutError('XMODEM block retry limit')
            if offset%(128*1024)==0:
                print(f'Uploading {Path(path).name}: {offset}/{len(data)}',flush=True)
        # XMODEM receivers may NAK the first EOT, then ACK the second.
        tail=b''
        for attempt in range(10):
            self.write(b'\x04'); tail=self.read(5)
            if b'\x06' in tail: break
        else: raise TimeoutError('XMODEM EOT retry limit')
        if not PROMPT.search(tail): self.wait_prompt()
        crc=zlib.crc32(data)
        self.checked(f'crc32 -v {address:x} {len(data):x} {crc:08x}')
        return len(data)

def watchdog_stop(c):
    for address,value in [(0x6000510c,0xc45a),(0x60005108,2),(0x60005060,0)]:
        c.checked(f'mw.l {address:x} {value:x} 1')

def watchdog_start(c,seconds):
    """Tegra30 WDT0 + TIMER5: reset after four expirations, IRQ-independent.

    TIMER5 ticks every 250 ms, so CFG's period field is the timeout in seconds.
    See Linux drivers/watchdog/tegra_wdt.c. TIMER1 belongs to XNU and is untouched.
    """
    if not 1<=seconds<=255: raise ValueError('watchdog seconds must be 1..255')
    watchdog_stop(c)
    for address,value in [(0x60005064,0x40000000),(0x60005060,0xc003d090),
                          (0x60005100,0x8005+(seconds<<4)),(0x60005108,1)]:
        c.checked(f'mw.l {address:x} {value:x} 1')

def wait_reboot(c,seconds):
    """Wait for a fresh boot banner and CLI, retaining the complete serial log."""
    end=time.monotonic()+seconds; data=b''
    while time.monotonic()<end:
        chunk=c.read(min(0.2,max(0,end-time.monotonic())))
        if chunk:
            sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()
            data=(data+chunk)[-262144:]
        if b'U-Boot ' in data and PROMPT.search(data):
            watchdog_stop(c)
            return {'watchdog_reset':b'Reset cause: WATCHDOG' in data,
                    'shim_started':b'XNU standalone shim' in data,
                    'kernel_jump':b'shim: jumping to XNU' in data,
                    'shim_error':b'shim: ERROR:' in data}
    raise TimeoutError('No U-Boot recovery prompt after watchdog deadline; see serial log')

def fastboot_devices(output):
    """Accept both platform-tools and android-tools device-list labels."""
    return re.findall(r'^([A-Za-z0-9_-]+)\s+(?:Android Fastboot|fastboot)\s*$',output,re.M)

def wait_fastboot_device(executable,timeout=30):
    """Allow host USB enumeration retries without starting an unbounded wait."""
    deadline=time.monotonic()+timeout
    detail='no USB device listed'
    while time.monotonic()<deadline:
        try:
            found=subprocess.run([executable,'devices'],capture_output=True,text=True,
                                 timeout=min(3,max(0.1,deadline-time.monotonic())))
        except subprocess.TimeoutExpired:
            detail='host USB enumeration timed out'
            continue
        devices=fastboot_devices(found.stdout)
        if len(devices)>1:
            raise RuntimeError(f'fastboot needs exactly one accessible device; found {len(devices)}')
        if len(devices)==1: return devices[0]
        detail=(found.stdout+'\n'+found.stderr).strip() or 'no USB device listed'
        time.sleep(0.2)
    raise RuntimeError(f'fastboot enumeration failed after {timeout}s: {detail}')

def upload_fastboot(c,path,address):
    """Download to RAM only, then return to the CLI; never flash a partition."""
    executable=shutil.which('fastboot')
    if not executable: raise RuntimeError('host fastboot not installed')
    # Tegra's EHCI controller is shared by host and gadget mode. A complete
    # host start/stop cycle recovers gadget enumeration after watchdog resets.
    # This tool targets Nexus7; none of these commands change persistent state.
    c.checked('usb reset',30)
    c.checked('usb stop',15)
    # Let the host retire the previous gadget before it enumerates again.
    time.sleep(1)
    c.line(f'fastboot -l {address:x} -s 2000000 usb 0')
    active=True
    try:
        device=wait_fastboot_device(executable)
        print(f'Uploading {Path(path).name} through fastboot to RAM only',flush=True)
        for command in [('stage',str(path)),('continue',)]:
            try:
                result=subprocess.run([executable,'-s',device,*command],
                                      capture_output=True,text=True,timeout=60)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f'fastboot {command[0]} timed out') from error
            if result.returncode:
                raise RuntimeError(f'fastboot {command[0]} failed: {result.stderr.strip()}')
        c.wait_prompt(15); active=False
        data=Path(path).read_bytes()
        c.checked(f'crc32 -v {address:x} {len(data):x} {zlib.crc32(data):08x}')
    finally:
        if active:
            c.write(b'\x03'); c.wait_prompt(15)

def ensure_upload(c,path,address,transport='serial'):
    data=Path(path).read_bytes()
    try:
        c.checked(f'crc32 -v {address:x} {len(data):x} {zlib.crc32(data):08x}')
        print(f'Reusing verified RAM copy: {Path(path).name}',flush=True)
    except RuntimeError:
        if transport in ('fastboot','auto'):
            for attempt in range(3):
                try:
                    upload_fastboot(c,path,address)
                    return len(data)
                except (RuntimeError,OSError,subprocess.TimeoutExpired) as error:
                    if attempt<2:
                        print(f'Fastboot attempt {attempt+1}/3 failed ({error}); recovering USB',flush=True)
                        continue
                    if transport=='fastboot': raise
                    print(f'Fastboot RAM upload unavailable ({error}); using XMODEM',flush=True)
        c.upload(path,address)
    return len(data)

def framebuffer_arguments(c, manifest):
    """Read the active Tegra DC window; never guess its physical layout."""
    values = manifest.get('framebuffer')
    if values is None:
        info = c.command('bdinfo').decode(errors='replace')
        match = re.search(r'Video\s*= dc@([0-9a-fA-F]+) active\s+FB base\s*= (0x[0-9a-fA-F]+)\s+FB size\s*= (\d+)x(\d+)x(\d+)', info)
        if not match:
            return ''
        dc, base = int(match[1],16), int(match[2],16)
        width, height, depth = map(int,match.group(3,4,5))
        if dc not in (0x54200000,0x54240000) or depth != 16:
            raise RuntimeError('unsupported active display format; specify a validated framebuffer explicitly')
        def reg(offset):
            address = dc + offset
            text = c.command(f'md.l {address:x} 1').decode(errors='replace')
            found = re.search(rf'{address:08x}:\s*([0-9a-fA-F]{{8}})',text,re.I)
            if not found: raise RuntimeError('cannot read display window')
            return int(found[1],16)
        if reg(0x2000) != base or reg(0x1c0c) != 6:
            raise RuntimeError('active DC window disagrees with bdinfo or is not RGB565')
        stride = reg(0x1c28) & 0xffff
        values = [base,stride,width,height,depth]
    if not isinstance(values,list) or len(values)!=5 or any(type(v) is not int or not 0<v<=0xffffffff for v in values):
        raise RuntimeError('invalid framebuffer metadata')
    print('Framebuffer handoff: ' + ', '.join(hex(v) for v in values),flush=True)
    return ''.join(f' {v:x}' for v in values)

def install_autoboot(c,device,partition):
    # Preserve the first original values on repeated deployment.
    c.checked('if env exists xnu_previous_bootcmd; then true; else setenv xnu_previous_bootcmd "${bootcmd}"; fi')
    c.checked('if env exists xnu_previous_bootdelay; then true; else setenv xnu_previous_bootdelay "${bootdelay}"; fi')
    c.checked(f"setenv xnu_bootcmd 'setenv devtype mmc; setenv devnum {device}; setenv distro_bootpart {partition}; if load mmc {device}:{partition} 90000000 /xnu-boot.scr; then source 90000000; fi'")
    c.checked("setenv bootcmd 'run xnu_bootcmd'")
    c.checked('setenv bootdelay 2')
    c.checked('saveenv',60)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['inspect','watchdog-test','debug','ram-boot','deploy','boot','install-autoboot','restore'])
    p.add_argument('--port',default='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0')
    p.add_argument('--bundle',type=Path,default=ROOT/'build/uboot-xnu')
    p.add_argument('--device',type=int,default=0)
    p.add_argument('--partition',type=int,default=1)
    p.add_argument('--fs',choices=['fat','ext4'],default='fat')
    p.add_argument('--interrupt',action='store_true',help='send Ctrl-C to interrupt U-Boot autoboot')
    p.add_argument('--autoboot',action='store_true',help='after deploy, save persistent automatic startup')
    p.add_argument('--run',action='store_true',help='after deploy/install, boot immediately')
    p.add_argument('--monitor',type=float,default=30)
    p.add_argument('--log',type=Path,default=ROOT/'build/serial-xnu.log')
    p.add_argument('--watchdog-seconds',type=int,default=30,help='absolute run deadline, 1..255 seconds; no kernel feeding')
    p.add_argument('--attempts',type=int,default=1,help='bounded debug runs, 1..10; reuses RAM only after CRC verification')
    p.add_argument('--transport',choices=['auto','serial','fastboot'],default='fastboot',
                   help='kernel RAM upload: fastboot stage/continue or XMODEM; never flash')
    a=p.parse_args()
    if a.device<0 or not 1<=a.partition<=128: p.error('invalid MMC device/partition')
    if a.autoboot and a.action!='deploy': p.error('--autoboot is for deploy')
    if not 1<=a.watchdog_seconds<=255: p.error('--watchdog-seconds must be 1..255')
    if not 1<=a.attempts<=10: p.error('--attempts must be 1..10')
    a.log.parent.mkdir(parents=True,exist_ok=True)
    with a.log.open('ab') as log:
        c=Console(a.port,log)
        try:
            print(c.connect(a.interrupt).decode(errors='replace'))
            if a.action=='watchdog-test':
                # Simulate a locked kernel: IRQ/FIQ masked, permanent WFI loop.
                # loadx flushes D/I caches before go; no MMC or environment writes.
                probe=ROOT/'build/watchdog-probe.bin'
                probe.write_bytes(struct.pack('<III',0xf10c00c0,0xe320f003,0xeafffffd))
                c.upload(probe,0x89000000)
                watchdog_start(c,a.watchdog_seconds)
                start=time.monotonic(); c.line('go 89000000')
                result=wait_reboot(c,a.watchdog_seconds+30)
                result['elapsed_seconds']=round(time.monotonic()-start,3)
                (ROOT/'build/watchdog-test.json').write_text(json.dumps(result,indent=2)+'\n')
                if not result['watchdog_reset']: raise RuntimeError('Reset occurred but cause was not WATCHDOG')
                print(json.dumps(result,indent=2)); return
            if a.action=='inspect':
                for cmd in ['version','bdinfo','printenv bootcmd bootdelay boot_targets scriptaddr',
                            'mmc list',f'part list mmc {a.device}', 'help loadx','help fatwrite','help ext4write']:
                    print(c.command(cmd).decode(errors='replace'))
                return
            if a.action in ('deploy','ram-boot','debug'):
                manifest=json.loads((a.bundle/'manifest.json').read_text())
                import hashlib
                for name in ('xnu-shim.bin','xnu-kernel','xnu-device-tree.adt','xnu-boot.scr'):
                    data=(a.bundle/name).read_bytes(); record=manifest['files'][name]
                    if len(data)!=record['size'] or hashlib.sha256(data).hexdigest()!=record['sha256']:
                        raise RuntimeError(f'bundle hash mismatch: {name}')
                if a.action in ('ram-boot','debug'):
                    command=manifest.get('bootargs','debug=0x8 keepsyms=1')
                    if not re.fullmatch(r'[A-Za-z0-9 /.,=_:+-]{0,255}',command):
                        raise RuntimeError('unsafe bundle bootargs')
                    results=[]
                    for attempt in range(a.attempts if a.action=='debug' else 1):
                        print(f'Run {attempt+1}: watchdog deadline {a.watchdog_seconds}s',flush=True)
                        ensure_upload(c,a.bundle/'xnu-shim.bin',0x88000000)
                        size=ensure_upload(c,a.bundle/'xnu-kernel',0x92000000,a.transport)
                        dt_size=ensure_upload(c,a.bundle/'xnu-device-tree.adt',0x91000000)
                        fb_arguments=framebuffer_arguments(c,manifest)
                        watchdog_start(c,a.watchdog_seconds)
                        c.line(f'go 88000000 92000000 {size:x} 08000000 "{command}" 91000000 {dt_size:x}{fb_arguments}')
                        result=wait_reboot(c,a.watchdog_seconds+30)
                        result['attempt']=attempt+1; results.append(result)
                        (ROOT/'build/debug-results.json').write_text(json.dumps(results,indent=2)+'\n')
                        if not result['watchdog_reset']: raise RuntimeError('Run ended with unexpected reset cause')
                    print(json.dumps(results,indent=2))
                    return
                # Writes go to regular files on the existing filesystem.
                for name in ('xnu-shim.bin','xnu-kernel','xnu-device-tree.adt','xnu-boot.scr'):
                    size=c.upload(a.bundle/name)
                    c.checked(f'{a.fs}write mmc {a.device}:{a.partition} 92000000 /{name} {size:x}',120)
                    c.checked(f'load mmc {a.device}:{a.partition} 92000000 /{name}',120)
                    c.checked(f'crc32 -v 92000000 {size:x} {manifest["files"][name]["crc32"]}')
                if a.autoboot: install_autoboot(c,a.device,a.partition)
            elif a.action=='install-autoboot':
                c.checked(f'load mmc {a.device}:{a.partition} 90000000 /xnu-boot.scr')
                install_autoboot(c,a.device,a.partition)
            elif a.action=='restore':
                c.checked('env exists xnu_previous_bootcmd')
                c.checked('setenv bootcmd "${xnu_previous_bootcmd}"')
                c.checked('setenv bootdelay "${xnu_previous_bootdelay}"')
                c.checked('saveenv',60)
            if a.action=='boot' or a.run:
                c.checked(f'load mmc {a.device}:{a.partition} 90000000 /xnu-boot.scr')
                c.command(f'setenv devtype mmc; setenv devnum {a.device}; setenv distro_bootpart {a.partition}')
                watchdog_start(c,a.watchdog_seconds)
                c.line('source 90000000')
                print(json.dumps(wait_reboot(c,a.watchdog_seconds+30),indent=2))
        finally: c.close()
if __name__=='__main__':
    try: main()
    except (OSError,RuntimeError,TimeoutError) as exc:
        print(f'Error: {exc}',file=sys.stderr); sys.exit(1)
