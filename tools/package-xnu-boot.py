#!/usr/bin/env python3
"""Create a U-Boot boot.scr bundle without an SDK or mkimage dependency."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import struct
import time
import zlib
ROOT=Path(__file__).resolve().parents[1]
def script_image(text, timestamp=0):
    body=text.encode('ascii')
    payload=struct.pack('>II',len(body),0)+body
    fields=[0x27051956,0,timestamp,len(payload),0,0,zlib.crc32(payload),0x00020600]
    # OS=invalid, arch=ARM(2), type=script(6), compression=none(0).
    header=struct.pack('>8I32s',*fields,b'Nexus7 XNU standalone shim')
    fields[1]=zlib.crc32(header)
    return struct.pack('>8I32s',*fields,b'Nexus7 XNU standalone shim')+payload

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--kernel',type=Path,default=ROOT/'build/output/mach_kernel')
    p.add_argument('--shim',type=Path,default=ROOT/'build/shim-nexus7/xnu-shim.bin')
    p.add_argument('--device-tree',type=Path,help='Apple DT blob; defaults to the selected shim build')
    p.add_argument('--kernel-uart',choices=['UARTA','UARTD'],required=True,
                   help='UART compiled into the selected XNU image; mainline U-Boot uses UARTA')
    p.add_argument('--bootargs',default='debug=0x8 keepsyms=1')
    p.add_argument('--framebuffer', help='physical base,stride,width,height,depth (integers, decimal or 0x)')
    p.add_argument('--out',type=Path,default=ROOT/'build/uboot-xnu')
    a=p.parse_args()
    framebuffer = None
    if a.framebuffer:
        try:
            framebuffer = [int(value,0) for value in a.framebuffer.split(',')]
            assert len(framebuffer)==5 and all(0 < value <= 0xffffffff for value in framebuffer)
        except (ValueError, AssertionError): p.error('framebuffer requires five positive 32-bit integers')
    fb_arguments = ''.join(f' {value:x}' for value in framebuffer) if framebuffer else ''
    config=json.loads((a.shim.parent/'build-config.json').read_text())
    if config['platform']!='nexus7': p.error('requires a Nexus7 shim')
    uart={'UARTA':'0x70006000','UARTD':'0x70006300'}[a.kernel_uart]
    if config['uart_phys']!=uart: p.error('kernel and shim UART differ; rebuild the kernel or shim')
    kernel_config=a.kernel.parent/'build-config.json'
    kernel_signature=a.kernel.parent.parent/'nexus7-uart.txt'
    if kernel_config.exists():
        if json.loads(kernel_config.read_text()).get('uart_phys')!=uart:
            p.error('selected kernel build UART differs from --kernel-uart')
    elif kernel_signature.exists() and kernel_signature.read_text().strip()!=uart:
        p.error('selected kernel object directory UART differs from --kernel-uart')

    if not re.fullmatch(r'[A-Za-z0-9 /.,=_:+-]{0,255}',a.bootargs):
        p.error('bootargs must be <=255 safe ASCII characters')
    kernel=a.kernel.read_bytes(); shim=a.shim.read_bytes()
    tree=(a.device_tree or a.shim.parent/'xnu-device-tree.adt').read_bytes()
    if not 0<len(tree)<=4096: p.error('invalid Apple DT size')
    if not (28<=len(kernel)<=0x2000000) or struct.unpack_from('<4I',kernel)!=(0xfeedface,12,9,2):
        p.error('expected a <=32 MiB ARMv7 Mach-O kernel')
    if not (0<len(shim)<=0x20000): p.error('shim exceeds its reservation')
    a.out.mkdir(parents=True,exist_ok=True)
    for data,name in [(kernel,'xnu-kernel'),(shim,'xnu-shim.bin'),(tree,'xnu-device-tree.adt')]: (a.out/name).write_bytes(data)
    # Do not reuse scriptaddr=0x90000000 as the kernel staging address.
    text=f'''# Generated; run via mainline bootstd or source. No persistent env writes.
if test -z "${{devtype}}"; then setenv devtype mmc; fi
if test -z "${{devnum}}"; then setenv devnum 0; fi
if test -z "${{distro_bootpart}}"; then setenv distro_bootpart 1; fi
setenv xnu_bootargs '{a.bootargs}'
if load ${{devtype}} ${{devnum}}:${{distro_bootpart}} 88000000 /xnu-shim.bin {len(shim)+1:x}; then
    if test "${{filesize}}" = "{len(shim):x}"; then
        if crc32 -v 88000000 {len(shim):x} {zlib.crc32(shim):08x}; then
            if load ${{devtype}} ${{devnum}}:${{distro_bootpart}} 92000000 /xnu-kernel {len(kernel)+1:x}; then
                if test "${{filesize}}" = "{len(kernel):x}"; then
                    if crc32 -v 92000000 {len(kernel):x} {zlib.crc32(kernel):08x}; then
                        if load ${{devtype}} ${{devnum}}:${{distro_bootpart}} 91000000 /xnu-device-tree.adt {len(tree)+1:x}; then
                            if test "${{filesize}}" = "{len(tree):x}"; then
                                if crc32 -v 91000000 {len(tree):x} {zlib.crc32(tree):08x}; then
                                    echo XNU: launching standalone shim
                                    go 88000000 92000000 {len(kernel):x} 08000000 "${{xnu_bootargs}}" 91000000 {len(tree):x}{fb_arguments}
                                fi
                            fi
                        fi
                    fi
                fi
            fi
        fi
    fi
fi
echo 'XNU: boot failed before handoff; returning to U-Boot'
'''
    (a.out/'boot.cmd').write_text(text)
    image=script_image(text)
    (a.out/'boot.scr').write_bytes(image)
    (a.out/'xnu-boot.scr').write_bytes(image)
    manifest={name:{'size':len((a.out/name).read_bytes()),
                    'sha256':hashlib.sha256((a.out/name).read_bytes()).hexdigest(),
                    'crc32':f'{zlib.crc32((a.out/name).read_bytes()):08x}'}
              for name in ('xnu-shim.bin','xnu-kernel','xnu-device-tree.adt','xnu-boot.scr','boot.scr')}
    (a.out/'manifest.json').write_text(json.dumps({'uart':a.kernel_uart,'bootargs':a.bootargs,'framebuffer':framebuffer,'files':manifest},indent=2)+'\n')
    (a.out/'SHA256SUMS').write_text(''.join(v['sha256']+'  '+k+'\n' for k,v in manifest.items()))
    print(f'Packaged {a.out}: UART={a.kernel_uart}, kernel={len(kernel)} bytes')
if __name__=='__main__': main()
